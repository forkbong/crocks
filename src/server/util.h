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

#ifndef CROCKS_SERVER_UTIL_H
#define CROCKS_SERVER_UTIL_H

#include <rocksdb/options.h>
#include <rocksdb/status.h>

namespace rocksdb {
class WriteBatch;
}

namespace crocks {

namespace pb {
class BatchUpdate;
}

int RocksdbStatusCodeToInt(const rocksdb::Status::Code& status);

// Exit unsuccessfully in case of RocksDB failure
void EnsureRocksdb(const std::string& what, const rocksdb::Status& status);

void ApplyBatchUpdate(rocksdb::WriteBatch* batch,
                      const pb::BatchUpdate& batch_update);

rocksdb::Options DefaultRocksdbOptions();

}  // namespace crocks

#endif  // CROCKS_SERVER_UTIL_H
