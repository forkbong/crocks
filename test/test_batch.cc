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

#include <assert.h>
#include <stdio.h>

#include <iostream>
#include <string>

#include <crocks/cluster.h>
#include <crocks/status.h>
#include <crocks/write_batch.h>
#include "src/common/util.h"

#include "util.h"

// This tests that there is no problem with some servers having no request
inline void TestSingle(crocks::Cluster* db) {
  crocks::WriteBatch batch(db);
  std::cout << "Starting a single batch put" << std::endl;
  batch.Put("yo", "yoyoyoyo");
  EnsureRpc(batch.Write());

  std::string value;
  EnsureRpc(db->Get("yo", &value));
  assert(value == "yoyoyoyo");
}

inline void TestBatch(crocks::Cluster* db) {
  std::cout << "Starting 1.000.000 sequential batch puts" << std::endl;
  Generator gen(SEQUENTIAL, 0, 800);
  for (int j = 0; j < 10; j++) {
    crocks::WriteBatch batch(db);
    for (int i = 0; i < 100000; i++)
      batch.Put(gen.NextKey(), gen.NextValue());
    EnsureRpc(batch.Write());
    std::cout << j << " done" << std::endl;
  }
}

inline void TestRandom(crocks::Cluster* db) {
  std::cout << "Starting 1.000.000 random batch puts" << std::endl;
  Generator gen(RANDOM, 1000000, 800);
  for (int j = 0; j < 10; j++) {
    crocks::WriteBatch batch(db);
    for (int i = 0; i < 100000; i++)
      batch.Put(gen.NextKey(), gen.NextValue());
    EnsureRpc(batch.Write());
    std::cout << j << " done" << std::endl;
  }
}

int main() {
  crocks::Cluster* db = crocks::DBOpen(crocks::GetEtcdEndpoint());

  Measure(TestSingle, db);
  std::cout << std::endl;

  Measure(TestBatch, db);
  std::cout << std::endl;

  Measure(TestRandom, db);
  std::cout << std::endl;

  delete db;

  return 0;
}
