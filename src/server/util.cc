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

#include "src/server/util.h"

#include <assert.h>
#include <stdlib.h>

#include <iostream>
#include <string>

#include <rocksdb/advanced_options.h>
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/write_batch.h>

#include <crocks/status.h>
#include "gen/crocks.pb.h"
#include "src/server/iterator.h"

namespace crocks {

int RocksdbStatusCodeToInt(const rocksdb::Status::Code& code) {
  int value = static_cast<int>(code);
  assert(value >= 0 && value <= 13);
  return value;
}

void EnsureRocksdb(const std::string& what, const rocksdb::Status& status) {
  if (!status.ok()) {
    std::cerr << "RocksDB " << what << " failed with status " << status.code()
              << " (" << status.getState() << ")" << std::endl;
    exit(EXIT_FAILURE);
  }
}

void ApplyBatchUpdate(rocksdb::WriteBatch* batch,
                      rocksdb::ColumnFamilyHandle* cf,
                      const pb::BatchUpdate& batch_update) {
  switch (batch_update.op()) {
    case pb::BatchUpdate::PUT:
      batch->Put(cf, batch_update.key(), batch_update.value());
      break;
    case pb::BatchUpdate::DELETE:
      batch->Delete(cf, batch_update.key());
      break;
    case pb::BatchUpdate::SINGLE_DELETE:
      batch->SingleDelete(cf, batch_update.key());
      break;
    case pb::BatchUpdate::MERGE:
      batch->Merge(cf, batch_update.key(), batch_update.value());
      break;
    case pb::BatchUpdate::CLEAR:
      batch->Clear();
      break;
    default:
      assert(false);
  }
}

const int kIteratorBatchSize = 10;

void MakeNextBatch(MultiIterator* it, pb::IteratorResponse* response) {
  for (int i = 0; i < kIteratorBatchSize && it->Valid(); i++) {
    pb::KeyValue* kv = response->add_kvs();
    kv->set_key(it->key().ToString());
    kv->set_value(it->value().ToString());
    it->Next();
  }
  response->set_done(it->Valid() ? false : true);
  response->set_status(it->status().rocksdb_code());
}

void MakePrevBatch(MultiIterator* it, pb::IteratorResponse* response) {
  for (int i = 0; i < kIteratorBatchSize && it->Valid(); i++) {
    pb::KeyValue* kv = response->add_kvs();
    kv->set_key(it->key().ToString());
    kv->set_value(it->value().ToString());
    it->Prev();
  }
  response->set_done(it->Valid() ? false : true);
  response->set_status(it->status().rocksdb_code());
}

// Once we have a seek request, guess which way the client will
// iterate and send a bunch of key-value pairs and whatever
// else is needed. When the iterator becomes invalid, iteration
// stops and the done field of the response is set to true.
void ApplyIteratorRequest(MultiIterator* it, const pb::IteratorRequest& request,
                          pb::IteratorResponse* response) {
  switch (request.op()) {
    case pb::IteratorRequest::SEEK_TO_FIRST:
      it->SeekToFirst();
      MakeNextBatch(it, response);
      break;
    case pb::IteratorRequest::SEEK_TO_LAST:
      it->SeekToLast();
      MakePrevBatch(it, response);
      break;
    case pb::IteratorRequest::SEEK:
      it->Seek(request.target());
      MakeNextBatch(it, response);
      break;
    case pb::IteratorRequest::SEEK_FOR_PREV:
      it->SeekForPrev(request.target());
      MakePrevBatch(it, response);
      break;
    case pb::IteratorRequest::NEXT:
      MakeNextBatch(it, response);
      break;
    case pb::IteratorRequest::PREV:
      MakePrevBatch(it, response);
      break;
    default:
      assert(false);
  }
}

rocksdb::Options DefaultRocksdbOptions() {
  rocksdb::Options options;

  options.create_if_missing = true;
  options.IncreaseParallelism(4);
  options.OptimizeLevelStyleCompaction();
  options.compaction_pri = rocksdb::kOldestSmallestSeqFirst;
  options.level_compaction_dynamic_level_bytes = true;
  options.allow_ingest_behind = true;

  return options;
}

void AddColumnFamilies(
    std::vector<int> shards, rocksdb::DB* db,
    std::unordered_map<int, rocksdb::ColumnFamilyHandle*>* cfs) {
  std::vector<std::string> names;
  std::vector<rocksdb::ColumnFamilyHandle*> handles;
  for (int shard : shards)
    names.push_back(std::to_string(shard));
  rocksdb::Status s =
      db->CreateColumnFamilies(rocksdb::ColumnFamilyOptions(), names, &handles);
  EnsureRocksdb("CreateColumnFamilies", s);
  int i = 0;
  for (int shard : shards)
    (*cfs)[shard] = handles[i++];
}

}  // namespace crocks
