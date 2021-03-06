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

#include "src/common/info.h"

#include <assert.h>
#include <stdlib.h>

#include <iostream>
#include <sstream>
#include <vector>

namespace crocks {

// Helper function to print a list of shards in a compact way
template <class T>
std::string ListToString(const T& list) {
  // T must be an iterable that contains integers. Works with std::vector<int>
  // and RepeatedField<int32> as defined in google::protobuf. The returned
  // string is a comma separated list of ranges, represented as from-to
  // inclusive. For example ListToString([1,2,3,5,7,8,9]) returns "1-3,5,7-9".
  std::ostringstream stream;
  bool in_range = false;
  int last = list[0];
  for (int n : list) {
    if (n == list[0]) {
      // Start with the first integer
      stream << n;
    } else if (!in_range) {
      if (n == last + 1) {
        // Not in a range and the current integer is larger
        // than the last by one. Open a new range and continue.
        in_range = true;
      } else {
        stream << "," << n;
      }
    } else {
      if (n != last + 1) {
        // The range is broken. Finish it and append the current integer.
        stream << "-" << last << "," << n;
        in_range = false;
      }
    }
    last = n;
  }
  // Finish the last open range
  if (in_range)
    stream << "-" << last;
  return stream.str();
}

Info::Info(const std::string& address) : etcd_(address) {}

void Info::Get() {
  std::string info;
  etcd_.Get(kInfoKey, &info);
  Parse(info);
}

void Info::Add(const std::string& address, int num_shards) {
  bool succeeded;
  do {
    std::string old_info;
    if (etcd_.Get(kInfoKey, &old_info)) {
      Parse(old_info);
      int id = info_.IndexOf(address);
      if (id >= 0) {
        if (info_.IsAvailable(id)) {
          std::cerr << "There is another node listening on " << address
                    << std::endl
                    << "If you are trying to recover from crashing "
                    << "run \"crocksctl health\" first" << std::endl;
          exit(EXIT_FAILURE);
        }
        id_ = id;
      } else if (info_.IsInit()) {
        id_ = info_.AddNodeWithNewShards(address, num_shards);
      } else if (info_.IsRunning()) {
        id_ = info_.AddNode(address);
      } else if (info_.IsMigrating()) {
        std::cout << "Migrating. Try again later." << std::endl;
        exit(EXIT_FAILURE);
      }
      succeeded =
          etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
    } else {
      id_ = info_.AddNodeWithNewShards(address, num_shards);
      succeeded = etcd_.TxnPutIfKeyMissing(kInfoKey, info_.Serialize());
    }
  } while (!succeeded);
  map_ = info_.map();
  address_ = address;
}

void Info::Remove(int id) {
  bool succeeded;
  do {
    Get();
    assert(IsRunning());
    std::string old_info = info_.Serialize();
    info_.MarkRemoveNode(id);
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
}

void Info::Remove() {
  bool succeeded;
  do {
    Get();
    std::string old_info = info_.Serialize();
    info_.RemoveNode(id_);
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
  // There's no need to update the map
}

void Info::Run() {
  if (IsRunning() || !NoMigrations())
    return;
  bool succeeded;
  do {
    std::string old_info;
    if (!etcd_.Get(kInfoKey, &old_info))
      return;
    Parse(old_info);
    if (IsRunning() || !NoMigrations())
      return;
    info_.SetRunning();
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
}

void Info::Migrate() {
  bool succeeded;
  do {
    std::string old_info;
    if (!etcd_.Get(kInfoKey, &old_info))
      return;
    Parse(old_info);
    info_.RedistributeShards();
    if (info_.NoMigrations()) {
      std::cout << "There was nothing to migrate" << std::endl;
      return;
    }
    info_.SetMigrating();
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
}

void* Info::Watch() {
  std::string info;
  void* call = etcd_.Watch(kInfoKey, &info);
  Parse(info);
  return call;
}

bool Info::WatchNext(void* call) {
  std::string info;
  bool canceled = etcd_.WatchNext(call, &info);
  if (!canceled)
    Parse(info);
  return canceled;
}

void Info::WatchCancel(void* call) {
  etcd_.WatchCancel(call);
}

void Info::WatchEnd(void* call) {
  etcd_.WatchEnd(call);
}

std::unordered_map<int, std::vector<int>> Info::Tasks() const {
  return info_.Tasks(id_);
}

void Info::GiveShard(int shard) {
  bool succeeded;
  do {
    Get();
    std::string old_info = info_.Serialize();
    info_.GiveShard(id_, shard);
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
  {
    write_lock lock(mutex_);
    map_ = info_.map();
  }
}

void Info::MigrationOver(int shard) {
  bool succeeded;
  do {
    Get();
    std::string old_info = info_.Serialize();
    info_.MigrationOver(shard);
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
}

bool Info::IsAvailable(int id) const {
  return info_.IsAvailable(id);
}

void Info::SetAvailable(int id, bool available) {
  bool succeeded;
  do {
    Get();
    if (info_.IsAvailable(id) == available)
      return;
    std::string old_info = info_.Serialize();
    info_.SetAvailable(id, available);
    succeeded =
        etcd_.TxnPutIfValueEquals(kInfoKey, info_.Serialize(), old_info);
  } while (!succeeded);
}

void Info::Print() {
  if (info_.IsInit())
    std::cout << "state: INIT" << std::endl;
  else if (info_.IsRunning())
    std::cout << "state: RUNNING" << std::endl;
  else if (info_.IsMigrating())
    std::cout << "state: MIGRATING" << std::endl;
  else
    assert(false);
  std::cout << "nodes: " << num_nodes() << std::endl;
  std::cout << "shards: " << num_shards() << std::endl;
  for (int i = 0; i < num_nodes(); i++) {
    std::string address = info_.Address(i);
    if (address.empty())
      continue;
    std::cout << "node " << i << ":" << std::endl;
    std::cout << "  address: " << address << std::endl;
    auto shards = info_.shards(i);
    if (shards.size() > 0)
      std::cout << "  shards: " << ListToString(shards) << " (" << shards.size()
                << ")" << std::endl;
    auto future = info_.future(i);
    if (future.size() > 0)
      std::cout << "  future: " << ListToString(future) << " (" << future.size()
                << ")" << std::endl;
    if (!info_.IsAvailable(i))
      std::cout << "  available: false" << std::endl;
    if (info_.IsRemoved(i))
      std::cout << "  remove: true" << std::endl;
  }
}

void Info::WaitUntilHealthy() {
  void* call = Watch();
  while (!info_.IsHealthy())
    WatchNext(call);
  WatchCancel(call);
}

}  // namespace crocks
