/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/network_path.h"

#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include <atomic>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"

namespace xe::net {
namespace {

std::atomic<NetworkPathState> g_state{NetworkPathState::kAvailable};
std::mutex g_listeners_mutex;
std::map<uint64_t, std::function<void()>> g_listeners;
uint64_t g_next_listener = 1;
// Held while listeners run (not while |g_listeners_mutex| is), so Remove can
// wait for a call in flight.
std::mutex g_call_mutex;
thread_local bool t_in_listener = false;

dispatch_queue_t g_queue = nullptr;
dispatch_semaphore_t g_first_answer = nullptr;
bool g_answered = false;  // On |g_queue|.

const char* Name(NetworkPathState state) {
  switch (state) {
    case NetworkPathState::kAvailable:
      return "available";
    case NetworkPathState::kNoRoute:
      return "no route";
    case NetworkPathState::kNotAllowed:
      return "not allowed";
  }
  return "?";
}

NetworkPathState StateOf(nw_path_t path) {
  // "Satisfiable" means a connection can bring the route up (on-demand VPN).
  if (nw_path_get_status(path) != nw_path_status_unsatisfied) {
    return NetworkPathState::kAvailable;
  }
  switch (nw_path_get_unsatisfied_reason(path)) {
    case nw_path_unsatisfied_reason_cellular_denied:
    case nw_path_unsatisfied_reason_wifi_denied:
      return NetworkPathState::kNotAllowed;
    default:
      return NetworkPathState::kNoRoute;
  }
}

void Apply(nw_path_t path) {
  const NetworkPathState state = StateOf(path);
  if (!std::exchange(g_answered, true)) {
    g_state = state;
    XELOGI("Network: {}", Name(state));
    dispatch_semaphore_signal(g_first_answer);
    if (state == NetworkPathState::kAvailable) {
      return;
    }
  } else if (g_state.exchange(state) == state) {
    return;
  } else {
    XELOGI("Network: {}", Name(state));
  }
  std::lock_guard call_lock(g_call_mutex);
  std::vector<uint64_t> ids;
  {
    std::lock_guard lock(g_listeners_mutex);
    for (const auto& [id, listener] : g_listeners) {
      ids.push_back(id);
    }
  }
  t_in_listener = true;
  for (uint64_t id : ids) {
    std::function<void()> listener;
    {
      std::lock_guard lock(g_listeners_mutex);
      auto it = g_listeners.find(id);
      if (it == g_listeners.end()) {
        continue;  // Removed since the snapshot.
      }
      listener = it->second;
    }
    listener();
  }
  t_in_listener = false;
}

// Starts on |g_queue|.
void StartMonitor(nw_path_monitor_t monitor) {
  nw_path_monitor_set_queue(monitor, g_queue);
  nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
    Apply(path);
  });
  nw_path_monitor_start(monitor);
}

void EnsureStarted() {
  static std::once_flag once;
  std::call_once(once, [] {
    g_queue = dispatch_queue_create("xenia.net.network_path", DISPATCH_QUEUE_SERIAL);
    g_first_answer = dispatch_semaphore_create(0);
    dispatch_async(g_queue, ^{
      StartMonitor(nw_path_monitor_create());
    });
    dispatch_semaphore_wait(g_first_answer,
                            dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
  });
}

}  // namespace

NetworkPathState GetNetworkPathState() {
  EnsureStarted();
  return g_state.load();
}

uint64_t AddNetworkPathListener(std::function<void()> listener) {
  EnsureStarted();
  std::lock_guard lock(g_listeners_mutex);
  const uint64_t id = g_next_listener++;
  g_listeners.emplace(id, std::move(listener));
  return id;
}

void RemoveNetworkPathListener(uint64_t id) {
  {
    std::lock_guard lock(g_listeners_mutex);
    g_listeners.erase(id);
  }
  if (!t_in_listener) {
    // Wait out a call that may have copied the listener already.
    std::lock_guard call_lock(g_call_mutex);
  }
}

}  // namespace xe::net
