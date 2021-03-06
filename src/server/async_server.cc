// Copyright 2017 Panagiotis Ktistakis <panktist@gmail.com>
//
// This file is part of crocks.
//
// crocks is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// crocks is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with crocks.  If not, see <http://www.gnu.org/licenses/>.

#include "src/server/async_server.h"

#include <assert.h>
#include <stdlib.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <unordered_map>
#include <utility>

#include <grpc++/grpc++.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/status.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/write_batch.h>

#include <crocks/status.h>
#include "gen/crocks.pb.h"
#include "src/server/iterator.h"
#include "src/server/migrate_util.h"
#include "src/server/shards.h"
#include "src/server/util.h"

std::atomic<bool> shutdown(false);

namespace crocks {

// gRPC status indicating that the shard belongs to another node
const grpc::Status invalid_status(grpc::StatusCode::INVALID_ARGUMENT,
                                  "Not responsible for this shard");

// Simple POD struct used as an argument wrapper for calls
struct CallData {
  pb::RPC::AsyncService* service;
  grpc::ServerCompletionQueue* cq;
  rocksdb::DB* db;
  Info* info;
  Shards* shards;
};

// Base class used to cast the void* tags we get from
// the completion queue and call Proceed() on them.
class Call {
 public:
  virtual void Proceed(bool ok) = 0;
};

class PingCall final : public Call {
 public:
  explicit PingCall(CallData* data)
      : data_(data), responder_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data_->service->RequestPing(&ctx_, &request_, &responder_, data_->cq,
                                data_->cq, &proceed);
  }

  void Proceed(bool ok) {
    switch (status_) {
      case REQUEST:
        if (!ok) {
          // Not ok in REQUEST means the server has been Shutdown
          // before the call got matched to an incoming RPC.
          delete this;
          break;
        }
        new PingCall(data_);
        responder_.Finish(response_, grpc::Status::OK, &proceed);
        status_ = FINISH;
        break;

      case FINISH:
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    if (ctx_.IsCancelled())
      std::cerr << data_->info->id() << ": Ping call cancelled" << std::endl;
    on_done_called_ = true;
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncResponseWriter<pb::Empty> responder_;
  pb::Empty request_;
  pb::Empty response_;
  enum CallStatus { REQUEST, FINISH };
  CallStatus status_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

class GetCall final : public Call {
 public:
  explicit GetCall(CallData* data)
      : data_(data), responder_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data->service->RequestGet(&ctx_, &request_, &responder_, data_->cq,
                              data_->cq, &proceed);
  }

  void Proceed(bool ok) {
    rocksdb::Status s;
    std::string value;
    int shard_id;
    bool ask;

    switch (status_) {
      case REQUEST:
        if (!ok) {
          delete this;
          break;
        }
        new GetCall(data_);
        shard_id = data_->info->ShardForKey(request_.key());
        if (data_->info->WrongShard(shard_id) && !request_.force()) {
          responder_.FinishWithError(invalid_status, &proceed);
          status_ = FINISH;
          break;
        }
        shard_ = data_->shards->at(shard_id);
        if (!shard_) {
          responder_.FinishWithError(invalid_status, &proceed);
          status_ = FINISH;
          break;
        }
        s = shard_->Get(request_.key(), &value, &ask);
        if (ask) {
          std::cerr << data_->info->id() << ": Asking the former master"
                    << std::endl;
          std::unique_ptr<pb::RPC::Stub> stub(
              pb::RPC::NewStub(grpc::CreateChannel(
                  shard_->old_address(), grpc::InsecureChannelCredentials())));
          request_.set_force(true);
          std::unique_ptr<grpc::ClientAsyncResponseReader<pb::Response>> rpc(
              stub->AsyncGet(&force_get_context_, request_, data_->cq));
          rpc->Finish(&response_, &force_get_status_, &proceed);
          status_ = GET;
          break;
        }
        response_.set_status(RocksdbStatusCodeToInt(s.code()));
        response_.set_value(value);
        responder_.Finish(response_, grpc::Status::OK, &proceed);
        status_ = FINISH;
        break;

      case GET:
        // If gRPC failed with status UNAVAILABLE, but the
        // node is still in the info, he must have crashed.
        if (force_get_status_.error_code() == grpc::StatusCode::UNAVAILABLE) {
          auto addresses = data_->info->Addresses();
          if (std::find(addresses.begin(), addresses.end(),
                        shard_->old_address()) != addresses.end()) {
            std::cerr << data_->info->id() << ": The former master crashed"
                      << std::endl;
            responder_.FinishWithError(
                grpc::Status(grpc::StatusCode::UNAVAILABLE,
                             "The former master has crashed"),
                &proceed);
            status_ = FINISH;
            break;
          }
        }
        // If gRPC failed, the server must have shut down and if
        // RocksDB status is INVALID_ARGUMENT, he has deleted
        // the shard. Either way, we must have ingested by now.
        if (!force_get_status_.ok() ||
            response_.status() == rocksdb::StatusCode::INVALID_ARGUMENT) {
          std::cerr << data_->info->id() << ": Meanwhile importing finished"
                    << std::endl;
          s = shard_->Get(request_.key(), &value, &ask);
          assert(!ask);
          response_.set_status(RocksdbStatusCodeToInt(s.code()));
          response_.set_value(value);
        }
        // If he responded successfully we just forward his response
        responder_.Finish(response_, grpc::Status::OK, &proceed);
        status_ = FINISH;
        break;

      case FINISH:
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    on_done_called_ = true;
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncResponseWriter<pb::Response> responder_;
  pb::Key request_;
  pb::Response response_;
  // We need to keep the shared_ptr in scope for the whole
  // lifetime of GetCall to make sure that the shard
  // doesn't get deleted while a get rpc is in progress.
  std::shared_ptr<Shard> shard_;
  grpc::ClientContext force_get_context_;
  grpc::Status force_get_status_;
  enum CallStatus { REQUEST, GET, FINISH };
  CallStatus status_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

class PutCall final : public Call {
 public:
  explicit PutCall(CallData* data)
      : data_(data), responder_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data_->service->RequestPut(&ctx_, &request_, &responder_, data_->cq,
                               data_->cq, &proceed);
  }

  void Proceed(bool ok) {
    rocksdb::Status s;
    int shard_id;
    // We need to keep the shared_ptr in scope at least
    // until shard->Ref() is called. If shard->Ref()
    // succeeds we know that the shard won't be deleted.
    std::shared_ptr<Shard> shard;

    switch (status_) {
      case REQUEST:
        if (!ok) {
          delete this;
          break;
        }
        new PutCall(data_);
        shard_id = data_->info->ShardForKey(request_.key());
        shard = data_->shards->at(shard_id);
        if (!shard || !shard->Ref()) {
          responder_.FinishWithError(invalid_status, &proceed);
        } else {
          s = shard->Put(request_.key(), request_.value());
          shard->Unref();
          response_.set_status(RocksdbStatusCodeToInt(s.code()));
          responder_.Finish(response_, grpc::Status::OK, &proceed);
        }
        status_ = FINISH;
        break;

      case FINISH:
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    on_done_called_ = true;
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncResponseWriter<pb::Response> responder_;
  pb::KeyValue request_;
  pb::Response response_;
  enum CallStatus { REQUEST, FINISH };
  CallStatus status_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

class DeleteCall final : public Call {
 public:
  explicit DeleteCall(CallData* data)
      : data_(data), responder_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data_->service->RequestDelete(&ctx_, &request_, &responder_, data_->cq,
                                  data_->cq, &proceed);
  }

  void Proceed(bool ok) {
    rocksdb::Status s;
    int shard_id;
    std::shared_ptr<Shard> shard;

    switch (status_) {
      case REQUEST:
        if (!ok) {
          delete this;
          break;
        }
        new DeleteCall(data_);
        shard_id = data_->info->ShardForKey(request_.key());
        shard = data_->shards->at(shard_id);
        if (!shard || !shard->Ref()) {
          responder_.FinishWithError(invalid_status, &proceed);
        } else {
          s = shard->Delete(request_.key());
          shard->Unref();
          response_.set_status(RocksdbStatusCodeToInt(s.code()));
          responder_.Finish(response_, grpc::Status::OK, &proceed);
        }
        status_ = FINISH;
        break;

      case FINISH:
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    on_done_called_ = true;
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncResponseWriter<pb::Response> responder_;
  pb::Key request_;
  pb::Response response_;
  enum CallStatus { REQUEST, FINISH };
  CallStatus status_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

// TODO: Add SingleDelete() and Merge()

class BatchCall final : public Call {
 public:
  explicit BatchCall(CallData* data)
      : data_(data), stream_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data_->service->RequestBatch(&ctx_, &stream_, data_->cq, data_->cq,
                                 &proceed);
  }

  void Proceed(bool ok) {
    rocksdb::Status s;

    switch (status_) {
      case REQUEST:
        if (!ok) {
          delete this;
          break;
        }
        new BatchCall(data_);
        stream_.Read(&request_, &proceed);
        status_ = READ;
        assert(request_.updates_size() == 0);
        break;

      case READ:
        if (ok) {
          int shard_id = data_->info->ShardForKey(request_.updates(0).key());
          std::shared_ptr<Shard> shard = data_->shards->at(shard_id);
          if (!got_ref_[shard_id]) {
            // We got the first buffer
            if (!shard || !shard->Ref()) {
              auto code = rocksdb::Status::Code::kInvalidArgument;
              response_.set_status(RocksdbStatusCodeToInt(code));
              stream_.Write(response_, &proceed);
              status_ = WRITE;
              // break early to avoid applying the buffer
              break;
            } else {
              got_ref_[shard_id] = true;
              auto code = rocksdb::Status::Code::kOk;
              response_.set_status(RocksdbStatusCodeToInt(code));
              stream_.Write(response_, &proceed);
              status_ = WRITE;
            }
          } else {
            stream_.Read(&request_, &proceed);
            assert(status_ == READ);
          }
          for (const pb::BatchUpdate& batch_update : request_.updates())
            ApplyBatchUpdate(&batch_, shard->cf(), batch_update);
        } else {
          s = data_->db->Write(rocksdb::WriteOptions(), &batch_);
          response_.set_status(RocksdbStatusCodeToInt(s.code()));
          stream_.Write(response_, &proceed);
          status_ = WRITE;
          finish_ = true;
        }
        break;

      case WRITE:
        // assert(ok);
        if (!finish_ && ok) {
          stream_.Read(&request_, &proceed);
          status_ = READ;
        } else {
          stream_.Finish(grpc::Status::OK, &proceed);
          status_ = FINISH;
        }
        break;

      case FINISH:
        // Unreference every referenced shard
        for (auto pair : got_ref_)
          if (pair.second)
            data_->shards->at(pair.first)->Unref();
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    if (ctx_.IsCancelled())
      std::cerr << data_->info->id() << ": Batch call cancelled" << std::endl;
    on_done_called_ = true;
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncReaderWriter<pb::Response, pb::BatchBuffer> stream_;
  pb::BatchBuffer request_;
  pb::Response response_;
  std::unordered_map<int, bool> got_ref_;
  bool finish_ = false;
  enum CallStatus { REQUEST, READ, WRITE, FINISH };
  CallStatus status_;
  rocksdb::WriteBatch batch_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

class IteratorCall final : public Call {
 public:
  explicit IteratorCall(CallData* data)
      : data_(data), stream_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data_->service->RequestIterator(&ctx_, &stream_, data_->cq, data_->cq,
                                    &proceed);
  }

  void Proceed(bool ok) {
    switch (status_) {
      case REQUEST:
        if (!ok) {
          delete this;
          break;
        }
        new IteratorCall(data_);
        it_ = std::unique_ptr<MultiIterator>(
            new MultiIterator(data_->db, data_->shards->ColumnFamilies()));
        stream_.Read(&request_, &proceed);
        status_ = READ;
        break;

      case READ:
        if (ok) {
          response_.Clear();
          ApplyIteratorRequest(it_.get(), request_, &response_);
          stream_.Write(response_, &proceed);
          status_ = WRITE;
        } else {
          stream_.Finish(grpc::Status::OK, &proceed);
          status_ = FINISH;
        }
        break;

      case WRITE:
        if (ok) {
          stream_.Read(&request_, &proceed);
          status_ = READ;
        } else {
          stream_.Finish(grpc::Status::OK, &proceed);
          status_ = FINISH;
        }
        break;

      case FINISH:
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    if (ctx_.IsCancelled())
      std::cerr << data_->info->id() << ": Iterator call cancelled"
                << std::endl;
    on_done_called_ = true;
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncReaderWriter<pb::IteratorResponse, pb::IteratorRequest>
      stream_;
  pb::IteratorRequest request_;
  pb::IteratorResponse response_;
  enum CallStatus { REQUEST, READ, WRITE, FINISH };
  CallStatus status_;
  std::unique_ptr<MultiIterator> it_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

class MigrateCall final : public Call {
 public:
  explicit MigrateCall(CallData* data)
      : data_(data), stream_(&ctx_), status_(REQUEST) {
    on_done = [&](bool ok) { OnDone(ok); };
    proceed = [&](bool ok) { Proceed(ok); };
    ctx_.AsyncNotifyWhenDone(&on_done);
    data_->service->RequestMigrate(&ctx_, &stream_, data_->cq, data_->cq,
                                   &proceed);
  }

  void Proceed(bool ok) {
    rocksdb::Status s;
    pb::MigrateResponse response;
    bool retval;
    int shard_id;
    std::shared_ptr<Shard> shard;

    switch (status_) {
      case REQUEST:
        if (!ok) {
          delete this;
          break;
        }
        new MigrateCall(data_);
        stream_.Read(&request_, &proceed);
        status_ = READ;
        break;

      case READ:
        shard_id = request_.shard();
        std::cerr << data_->info->id() << ": Migrating shard " << shard_id
                  << std::endl;
        shard = data_->shards->at(shard_id);
        if (!shard) {
          std::cerr << data_->info->id() << ": Already given and deleted"
                    << std::endl;
          stream_.Finish(invalid_status, &proceed);
          status_ = FINISH;
          break;
        }
        retval = shard->Unref(true);
        if (!retval)
          std::cerr << data_->info->id() << ": Resuming from SST "
                    << request_.start_from() << std::endl;
        // From now on requests for the shard are rejected
        data_->info->GiveShard(shard_id);
        // Inform the new node that he may proceed
        stream_.Write(response, &proceed);
        status_ = WRITE;
        migrator_ = std::unique_ptr<ShardMigrator>(
            new ShardMigrator(data_->db, shard_id, request_.start_from()));
        // DumpShard() creates SST files by iterating on the shard. We can't
        // modify the database after the iterator snapshot is taken, and
        // there may be some unfinished requests. So we have to wait for
        // the reference counter to reach 0 before calling DumpShard().
        // If we took into account batches, we would have do to that even
        // before calling GiveShard(). This is necessary, because the batch is
        // committed on the server that referenced the shard and any writes
        // that happen from the moment the shard is given until the commit,
        // will appear to have happened after the commit. However waiting for
        // the references before giving the shard might cause a deadlock.
        if (retval)
          shard->WaitRefs();
        migrator_->DumpShard(shard->cf());
        break;

      case WRITE:
        if (migrator_->ReadChunk(&response)) {
          stream_.Write(response, &proceed);
          status_ = WRITE;
        } else {
          stream_.Read(&request_, &proceed);
          status_ = DONE;
        }
        break;

      case DONE:
        stream_.Finish(grpc::Status::OK, &proceed);
        status_ = FINISH;
        break;

      case FINISH:
        finish_called_ = true;
        if (on_done_called_)
          delete this;
        break;
    }
  }

  void OnDone(bool ok) {
    assert(ok);
    on_done_called_ = true;
    if (ctx_.IsCancelled()) {
      std::cerr << data_->info->id() << ": Migrate call cancelled" << std::endl;
      auto metadata = ctx_.client_metadata();
      auto pair = metadata.find("id");
      assert(pair != metadata.end());
      int node_id = std::stoi(pair->second.data());
      std::cerr << data_->info->id() << ": Setting node " << node_id
                << " as unavailable" << std::endl;
      data_->info->SetAvailable(node_id, false);
    } else {
      data_->shards->Remove(request_.shard());
      if (migrator_)
        migrator_->ClearState();
    }
    if (data_->shards->empty()) {
      data_->info->Remove();
      shutdown.store(true);
    }
    if (finish_called_)
      delete this;
    else
      status_ = FINISH;
  }

  std::function<void(bool)> proceed;
  std::function<void(bool)> on_done;

 private:
  CallData* data_;
  grpc::ServerContext ctx_;
  grpc::ServerAsyncReaderWriter<pb::MigrateResponse, pb::MigrateRequest>
      stream_;
  pb::MigrateRequest request_;
  pb::MigrateResponse response_;
  enum CallStatus { REQUEST, READ, WRITE, DONE, FINISH };
  CallStatus status_;
  std::unique_ptr<ShardMigrator> migrator_;
  bool finish_called_ = false;
  bool on_done_called_ = false;
};

AsyncServer::AsyncServer(const std::string& etcd_address,
                         const std::string& dbpath,
                         const std::string& options_path, int num_threads)
    : dbpath_(dbpath), info_(etcd_address), num_threads_(num_threads) {
  if (options_path == "") {
    options_ = DefaultRocksdbOptions();
  } else {
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors;
    rocksdb::Status s = rocksdb::LoadOptionsFromFile(
        options_path, rocksdb::Env::Default(), &options_, &cf_descriptors);
    EnsureRocksdb("LoadOptionsFromFile", s);
  }
}

AsyncServer::~AsyncServer() {
  std::cerr << "Shutting down..." << std::endl;
  for (auto cq = cqs_.begin(); cq != cqs_.end(); ++cq)
    (*cq)->Shutdown();
  void* tag;
  bool ok;
  for (auto cq = cqs_.begin(); cq != cqs_.end(); ++cq) {
    while ((*cq)->Next(&tag, &ok)) {
      auto proceed = static_cast<std::function<void(bool)>*>(tag);
      (*proceed)(ok);
    }
  }
  migrate_cq_->Shutdown();
  while (migrate_cq_->Next(&tag, &ok)) {
    auto proceed = static_cast<std::function<void(bool)>*>(tag);
    (*proceed)(ok);
  }
  info_.WatchCancel(call_);
  watcher_.join();
  info_.WatchEnd(call_);
  delete shards_;
  delete default_cf_;
  delete db_;
  rocksdb::DestroyDB(dbpath_, options_);
}

void AsyncServer::Init(const std::string& listening_address,
                       const std::string& hostname, int num_shards) {
  // Initialize gRPC
  grpc::ServerBuilder builder;
  int selected_port;
  builder.AddListeningPort(listening_address, grpc::InsecureServerCredentials(),
                           &selected_port);
  builder.RegisterService(&service_);
  for (int i = 0; i < num_threads_; i++)
    cqs_.emplace_back(builder.AddCompletionQueue());
  migrate_cq_ = builder.AddCompletionQueue();
  server_ = builder.BuildAndStart();
  if (selected_port == 0) {
    std::cerr << "Could not bind to a port" << std::endl;
    exit(EXIT_FAILURE);
  }

  // Announce server to etcd
  std::string port = std::to_string(selected_port);
  std::string node_address = hostname + ":" + port;
  // TODO: This knows if we are resuming. We could return a relevant
  // bool, and if resuming check that we have the right column families.
  info_.Add(node_address, num_shards);

  // Open RocksDB database
  std::vector<std::string> column_families;
  db_->ListColumnFamilies(options_, dbpath_, &column_families);
  if (!column_families.empty()) {
    std::cerr << info_.id() << ": Recovering from crash" << std::endl;
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors;
    for (auto name : column_families) {
      rocksdb::ColumnFamilyDescriptor descriptor(name,
                                                 DefaultColumnFamilyOptions());
      cf_descriptors.push_back(descriptor);
    }
    std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
    rocksdb::Status s =
        rocksdb::DB::Open(options_, dbpath_, cf_descriptors, &cf_handles, &db_);
    EnsureRocksdb("Open", s);
    shards_ = new Shards(db_, cf_handles);
    std::cout << std::endl;
    for (auto cf : cf_handles) {
      if (cf->GetName() == "default") {
        // XXX: We just keep a copy to delete it on shutdown
        // and exit cleanly. Apparently it's only needed for
        // this DB::Open constructor and not for the default.
        default_cf_ = cf;
        continue;
      }
      std::cout << "      Shard " << cf->GetName() << std::endl;
      std::string stats;
      if (!db_->GetProperty(cf, "rocksdb.levelstats", &stats))
        stats = "(failed)";
      std::cout << stats.c_str() << std::endl;
    }
  } else {
    rocksdb::Status s = rocksdb::DB::Open(options_, dbpath_, &db_);
    EnsureRocksdb("Open", s);
    shards_ = new Shards(db_, info_.shards());
  }

  // Watch etcd for changes to the cluster
  call_ = info_.Watch();

  // Set existing shards that are not yet imported as importing
  for (const auto& task : info_.Tasks()) {
    for (int shard_id : task.second) {
      Shard* shard = shards_->at(shard_id).get();
      if (shard) {
        shard->set_importing(true);
        std::string key = Key(shard_id, "largest_key");
        std::string value;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &value);
        if (s.ok())
          shard->set_largest_key(value);
      }
    }
  }

  // Create a thread that watches the "info" key and repeatedly
  // reads for updates. Gets cleaned up by the destructor.
  watcher_ = std::thread(&AsyncServer::WatchThread, this);
  std::cerr << "Asynchronous server listening on port " << port << std::endl;
}

void AsyncServer::Run() {
  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads_; i++) {
    CallData data{&service_, cqs_[i].get(), db_, &info_, shards_};
    new PingCall(&data);
    new GetCall(&data);
    new PutCall(&data);
    new DeleteCall(&data);
    new BatchCall(&data);
    new IteratorCall(&data);
    threads.emplace_back(std::thread(&AsyncServer::ServeThread, this, i));
  }
  CallData migrate_data{&service_, migrate_cq_.get(), db_, &info_, shards_};
  new MigrateCall(&migrate_data);
  info_.SetAvailable(info_.id(), true);
  void* tag;
  bool ok;
  // For the meaning of the return value of Next, and ok see:
  // https://groups.google.com/d/msg/grpc-io/qtZya6AuGAQ/Umepla-GAAAJ
  // http://www.grpc.io/grpc/cpp/classgrpc_1_1_completion_queue.html
  while (migrate_cq_->Next(&tag, &ok)) {
    auto proceed = static_cast<std::function<void(bool)>*>(tag);
    (*proceed)(ok);
    if (shutdown.load())
      break;
  }
  server_->Shutdown(std::chrono::system_clock::now());
  for (auto thr = threads.begin(); thr != threads.end(); thr++)
    thr->join();
}

void AsyncServer::ServeThread(int i) {
  void* tag;
  bool ok;
  while (cqs_[i]->Next(&tag, &ok)) {
    auto proceed = static_cast<std::function<void(bool)>*>(tag);
    (*proceed)(ok);
    if (shutdown.load())
      break;
  }
}

void AsyncServer::WatchThread() {
  do {
    for (const auto& task : info_.Tasks()) {
      int node_id = task.first;
      std::string address = info_.Address(node_id);
      for (int shard_id : task.second) {
        if (!info_.IsAvailable(node_id)) {
          std::cerr << info_.id() << ": Node " << node_id
                    << " is unavailable. Skipping request for shard "
                    << shard_id << "." << std::endl;
          continue;
        }
        std::cerr << info_.id() << ": Requesting shard " << shard_id
                  << " from node " << node_id << std::endl;
        Shard* shard;
        // If it does not belong to us, we may
        // or may not have it and we must check.
        if (info_.IndexForShard(shard_id) != info_.id()) {
          if (!shards_->at(shard_id))
            // We don't have it so create it
            shard = shards_->Add(shard_id, address).get();
          else
            // We managed to create it before crashing. We
            // should ensure that it is empty as it should.
            shard = shards_->at(shard_id).get();
        } else {
          shard = shards_->at(shard_id).get();
        }

        ShardImporter importer(db_, shard_id);
        // If we are recovering from a crash there might be a file
        // that we didn't manage to ingest. Try to do that. If
        // there isn't such a file, Ingest() will silently fail.
        if (!importer.filename().empty())
          shard->Ingest(importer.filename(), importer.largest_key());

        pb::MigrateRequest request;
        pb::MigrateResponse response;
        grpc::ClientContext context;
        context.AddMetadata("id", std::to_string(info_.id()));
        std::unique_ptr<pb::RPC::Stub> stub(pb::RPC::NewStub(
            grpc::CreateChannel(address, grpc::InsecureChannelCredentials())));

        // Send a request for the shard
        request.set_shard(shard_id);
        request.set_start_from(importer.num());
        auto stream = stub->Migrate(&context);
        if (!stream->Write(request)) {
          std::cerr << info_.id() << ": Error on first write" << std::endl;
          grpc::Status status = stream->Finish();
          HandleError(status, node_id);
          continue;
        }

        // Once the old master gets the request, he is supposed to
        // pass ownership to us by informing etcd and then send
        // an empty response as a confirmation. We wait for these
        // events in reverse order to avoid a deadlock, and start
        // serving requests for that shard as soon as possible.
        if (!stream->Read(&response)) {
          std::cerr << info_.id() << ": Error on second read" << std::endl;
          grpc::Status status = stream->Finish();
          if (status.error_code() == grpc::StatusCode::INVALID_ARGUMENT) {
            std::cerr << "Migration was already finished but didn't manage to "
                         "announce it before crashing"
                      << std::endl;
            MigrationOver(importer, shard_id);
          } else {
            HandleError(status, node_id);
          }
          continue;
        }
        while (info_.IndexForShard(shard_id) != info_.id()) {
          bool ret = info_.WatchNext(call_);
          assert(!ret);
        }
        // From now on requests for the shard are accepted

        // The second read should be ok. Even if the shard is empty,
        // one message will be sent. So if it is not ok, it means he
        // crashed. We cannot know if he managed to give the shard.
        if (!stream->Read(&response)) {
          std::cerr << info_.id() << ": Error on second read" << std::endl;
          grpc::Status status = stream->Finish();
          HandleError(status, node_id);
          continue;
        }

        do {
          if (response.finished())
            break;
          // If true an SST is ready to be imported
          if (importer.WriteChunk(response))
            shard->Ingest(importer.filename(), importer.largest_key());
        } while (stream->Read(&response));

        stream->Write(request);
        grpc::Status status = stream->Finish();
        if (!status.ok()) {
          std::cerr << info_.id() << ": Error on finish" << std::endl;
          HandleError(status, node_id);
          continue;
        }

        MigrationOver(importer, shard_id);
        shard->set_importing(false);
        std::cerr << info_.id() << ": Imported shard " << shard_id << std::endl;
      }
    }
  } while (!info_.WatchNext(call_));
}

void AsyncServer::MigrationOver(ShardImporter& importer, int shard_id) {
  info_.MigrationOver(shard_id);
  // FIXME: If we crash here the state never gets cleared
  importer.ClearState();
  // Wait for the confirmation from etcd
  do {
    bool ret = info_.WatchNext(call_);
    assert(!ret);
  } while (info_.IsMigrating(shard_id));
}

void AsyncServer::HandleError(const grpc::Status& status, int node_id) {
  if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
    std::cerr << info_.id() << ": Setting node " << node_id << " as unavailable"
              << std::endl;
    info_.SetAvailable(node_id, false);
  } else if (!status.ok()) {
    // For every error other than UNAVAILABLE, exit
    EnsureRpc(status);
  }
}

}  // namespace crocks
