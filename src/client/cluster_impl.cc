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

#include "src/client/cluster_impl.h"

#include <assert.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

#include <grpc++/grpc++.h>

#include <crocks/cluster.h>
#include "src/client/node.h"

namespace crocks {

Cluster::Cluster(const Options& options, const std::string& address)
    : impl_(new ClusterImpl(options, address)) {}

Cluster::Cluster(const std::string& address)
    : impl_(new ClusterImpl(Options(), address)) {}

Cluster::~Cluster() {
  delete impl_;
}

Status Cluster::Get(const std::string& key, std::string* value) {
  return impl_->Get(key, value);
}

Status Cluster::Put(const std::string& key, const std::string& value) {
  return impl_->Put(key, value);
}

Status Cluster::Delete(const std::string& key) {
  return impl_->Delete(key);
}

Status Cluster::SingleDelete(const std::string& key) {
  return impl_->SingleDelete(key);
}

Status Cluster::Merge(const std::string& key, const std::string& value) {
  return impl_->Merge(key, value);
}

void Cluster::WaitUntilHealthy() {
  impl_->WaitUntilHealthy();
}

Cluster* DBOpen(const std::string& address) {
  return new Cluster(address);
}

// Cluster implementation
ClusterImpl::ClusterImpl(const Options& options, const std::string& address)
    : options_(options), info_(address) {
  info_.Get();
  info_.Run();
  int id = 0;
  for (const auto& address : info_.Addresses()) {
    if (!address.empty())
      nodes_[id] = new Node(address);
    id++;
  }
}

ClusterImpl::~ClusterImpl() {
  for (const auto& pair : nodes_)
    delete pair.second;
}

Status ClusterImpl::Get(const std::string& key, std::string* value) {
  auto op = std::bind(&Node::Get, std::placeholders::_1, key, value);
  return Operation(op, key);
}

Status ClusterImpl::Put(const std::string& key, const std::string& value) {
  auto op = std::bind(&Node::Put, std::placeholders::_1, key, value);
  return Operation(op, key);
}

Status ClusterImpl::Delete(const std::string& key) {
  auto op = std::bind(&Node::Delete, std::placeholders::_1, key);
  return Operation(op, key);
}

Status ClusterImpl::SingleDelete(const std::string& key) {
  auto op = std::bind(&Node::SingleDelete, std::placeholders::_1, key);
  return Operation(op, key);
}

Status ClusterImpl::Merge(const std::string& key, const std::string& value) {
  auto op = std::bind(&Node::Merge, std::placeholders::_1, key, value);
  return Operation(op, key);
}

void ClusterImpl::WaitUntilHealthy() {
  info_.WaitUntilHealthy();
}

int ClusterImpl::IndexForShard(int shard, bool update) {
  if (update)
    Update();
  return info_.IndexForShard(shard);
}

int ClusterImpl::ShardForKey(const std::string& key) {
  return info_.ShardForKey(key);
}

int ClusterImpl::IndexForKey(const std::string& key) {
  return info_.IndexForKey(key);
}

Node* ClusterImpl::NodeForKey(const std::string& key) {
  int idx = info_.IndexForKey(key);
  return nodes_[idx];
}

Node* ClusterImpl::NodeByIndex(int idx) {
  return nodes_[idx];
}

Status ClusterImpl::Operation(const std::function<Status(Node*)>& op,
                              const std::string& key) {
  Status status = op(NodeForKey(key));
  while (status.IsUnavailable() ||
         (status.grpc_code() == grpc::StatusCode::INVALID_ARGUMENT)) {
    int id = IndexForKey(key);
    if (status.grpc_code() == grpc::StatusCode::INVALID_ARGUMENT) {
      std::cerr << "Got status INVALID_ARGUMENT from node " << id << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      Update();
      std::cerr << "Retrying with the new master (node " << IndexForKey(key)
                << ")... ";
      status = op(NodeForKey(key));
      std::cerr << "OK (status " << status.grpc_code() << ": "
                << status.error_message() << ")" << std::endl;
      continue;
    }

    // We got status UNAVAILABLE. There are three possibilities:
    //   1. The node has shut down cleanly
    //   2. The node crashed but is back up and we need to reconnect
    //   3. The node crashed and we need to wait for recovery
    // In any case we close the current connection
    std::cerr << "Got status UNAVAILABLE from node " << id << std::endl;

    if (status.error_message() == "The former master has crashed") {
      std::cerr << "The former master has crashed" << std::endl;
    } else {
      delete nodes_[id];
      nodes_[id] = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      Update();
      if (IndexForKey(key) != id) {
        // Case 1. Retry with the new master
        std::cerr << "He has shut down. Retrying with the new master (node "
                  << IndexForKey(key) << ")... ";
        status = op(NodeForKey(key));
        std::cerr << "OK" << std::endl;
        continue;
      }
      std::cerr << "Pinging node " << id << "..." << std::endl;
      assert(IndexForKey(key) == id);
      Status ping_status = NodeForKey(key)->Ping();
      if (ping_status.grpc_code() == grpc::StatusCode::OK) {
        // Case 2. Do nothing, we'll just retry
        std::cerr << "He is back online" << std::endl;
      } else {
        // Case 3. Wait until the cluster is healthy again
        // FIXME: It crashed once, cannot reproduce
        if (!ping_status.IsUnavailable())
          EnsureRpc(ping_status);
        while (info_.IsHealthy() &&
               (ping_status.grpc_code() != grpc::StatusCode::OK)) {
          id = IndexForKey(key);
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
          delete nodes_[id];
          nodes_[id] = nullptr;
          Update();
          ping_status = NodeForKey(key)->Ping();
          std::cerr << "He has crashed but etcd is not aware" << std::endl;
          if (options_.inform_on_unavailable) {
            std::cerr << "Informing etcd" << std::endl;
            info_.SetAvailable(id, false);
          }
        }
      }
    }

    if (!info_.IsHealthy()) {
      id = IndexForKey(key);
      if (!options_.wait_on_unhealthy)
        return status;
      std::cerr << "Cluster is unhealthy. Waiting... ";
      info_.WaitUntilHealthy();
      std::cerr << "OK" << std::endl;
      delete nodes_[id];
      nodes_[id] = nullptr;
      Update();
    }

    std::cerr << "Retrying with node " << IndexForKey(key) << "...";
    status = op(NodeForKey(key));
    std::cerr << "OK (status " << status.grpc_code() << ": "
              << status.error_message() << ")" << std::endl;
  }
  return status;
}

void ClusterImpl::Update() {
  info_.Get();
  int id = 0;
  for (const auto& address : info_.Addresses()) {
    if (address.empty()) {
      delete nodes_[id];
      nodes_[id] = nullptr;
    } else if (nodes_[id] == nullptr) {
      std::cerr << "New connection with node " << id << std::endl;
      nodes_[id] = new Node(address);
    } else {
      assert(nodes_[id]->address() == address);
    }
    id++;
  }
}

}  // namespace crocks
