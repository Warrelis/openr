/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/fbmeshd/pinger/PeerPinger.h"

#include <chrono>
#include <numeric>
#include <string>

#include <folly/Subprocess.h>
#include <openr/common/NetworkUtil.h>
#include <openr/fbmeshd/common/Constants.h>

using namespace openr::fbmeshd;

DEFINE_int32(ping_interval_s, 30, "peer ping interval");

PeerPinger::PeerPinger(folly::EventBase* evb, Nl80211Handler& nlHandler)
    : evb_(evb), nlHandler_(nlHandler) {
  LOG(INFO) << "PeerPinger created";
  attachEventBase(evb_);
}

PeerPinger::~PeerPinger() {
  stop();
}

void
PeerPinger::run() {
  LOG(INFO) << "starting PeerPinger loop";
  scheduleTimeout(0);
  evb_->loopForever();
}

void
PeerPinger::stop() {
  LOG(INFO) << "stopping PeerPinger";
  evb_->terminateLoopSoon();
  LOG(INFO) << "PeerPinger got stopped.";
}

void
PeerPinger::parsePingOutput(folly::StringPiece line, folly::MacAddress peer) {
  std::vector<std::string> col;
  folly::split(" ", line, col);
  if (col.size() != 8 || col[1] != "bytes") {
    return;
  }
  std::vector<std::string> v;
  folly::split("=", col[6], v);
  if (v.size() > 1) {
    try {
      float pingLatency = stof(v[1]);
      pingData_[peer].push_back(pingLatency);
    } catch (std::exception& e) {
      LOG(ERROR) << "error parsing ping output " << line << ": " << e.what();
    }
  }
}

void
PeerPinger::pingPeer(const folly::MacAddress& peer) {
  std::string cmd = "ping6 ";
  folly::IPAddressV6 ipv6(folly::IPAddressV6::LINK_LOCAL, peer);
  cmd = cmd + ipv6.str() + "%mesh0 -i 0.1 -c 50 -n -s 1024";

  folly::Subprocess proc(cmd, folly::Subprocess::Options().pipeStdout());

  auto callback = folly::Subprocess::readLinesCallback(
      [&](int /*fd*/, folly::StringPiece line) {
        parsePingOutput(line, peer);
        return false;
      });

  proc.communicate(std::ref(callback), [](int, int) { return true; });
  auto rc = proc.wait();

  if (rc.exitStatus() != 0) {
    throw folly::CalledProcessError(rc);
  }
}

void
PeerPinger::syncPeers() {
  VLOG(3) << folly::sformat("PeerPinger::{}()", __func__);
  std::vector<StationInfo> newStations = nlHandler_.getStationsInfo();

  // remove inactive stations, and keep macAddresses of the active ones
  std::vector<folly::MacAddress> activePeers;
  for (const auto& station : newStations) {
    if (station.inactiveTime < Constants::kMaxPeerInactiveTime) {
      activePeers.push_back(station.macAddress);
    }
  }

  // add new peers
  for (const auto& peer : activePeers) {
    if (peers_.find(peer) == peers_.end()) {
      VLOG(3) << "adding peer " << peer;
      peers_.emplace(peer);
    }
  }

  // remove neighbors that are not in the new set of peers
  std::unordered_set<folly::MacAddress> newPeerSet(
      activePeers.begin(), activePeers.end());
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (newPeerSet.find(*it) == newPeerSet.end()) {
      VLOG(3) << "removing peer " << *it;
      it = peers_.erase(it);
    } else {
      it++;
    }
  }
}

void
PeerPinger::processPingData() {
  std::vector<float> data;
  for (auto it : pingData_) {
    if (it.second.size() == 0) {
      continue;
    }
    auto peer = it.first;
    data = it.second;
    std::sort(data.begin(), data.end());
    // remove the largest 5% data points from avg calculation
    int size = 95 * data.size() / 100;
    VLOG(5) << "data size reduced from " << data.size() << " to " << size;
    float average =
        std::accumulate(data.begin(), data.begin() + size - 1, 0.0) / size;
    VLOG(5) << peer << " average ping " << average;
    linkMetric_[peer] = average;
  }
}

void
PeerPinger::timeoutExpired() noexcept {
  std::chrono::duration<int> pingInterval{FLAGS_ping_interval_s};
  syncPeers();
  if (peers_.size() == 0) {
    VLOG(1) << "no targets to ping.";
    scheduleTimeout(pingInterval);
    return;
  }

  auto start = std::chrono::steady_clock::now();

  for (const auto& peer : peers_) {
    pingData_[peer].clear();
    try {
      pingPeer(peer);
    } catch (folly::CalledProcessError& e) {
      LOG(ERROR) << "error pinging " << peer << ": " << e.what();
    }
  }

  processPingData();

  // schedule next run for ping
  auto end = std::chrono::steady_clock::now();
  auto diff = std::chrono::duration_cast<std::chrono::seconds>(end - start);
  VLOG(3) << "ping iteration took " << diff.count() << "s.";
  if (diff >= pingInterval) {
    scheduleTimeout(0);
  } else {
    scheduleTimeout(pingInterval - diff);
  }
}
