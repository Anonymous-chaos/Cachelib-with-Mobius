/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

namespace facebook {
namespace cachelib {

/* Container Interface Implementation */
template <typename T, MMClock::Hook<T> T::*HookPtr>
MMClock::Container<T, HookPtr>::Container(serialization::MMClockObject object,
                                        PtrCompressor compressor)
    : compressor_(std::move(compressor)),
      fifo_(*object.fifo(), compressor_),
      config_(*object.config()) {
  nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                             ? std::numeric_limits<Time>::max()
                             : static_cast<Time>(util::getCurrentTimeSec()) +
                                   config_.mmReconfigureIntervalSecs.count();
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
bool MMClock::Container<T, HookPtr>::recordAccess(T& node,
                                                AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
  // check if the node is still being memory managed
  if (node.isInMMContainer()) {
    if (!isAccessed(node)) {
      markAccessed(node);
    }

    // setUpdateTime(node, currTime);
    return true;
  }
  return false;
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat MMClock::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return lruMutex_->lock_combine([this, projectedLength]() {
    return getEvictionAgeStatLocked(projectedLength);
  });
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat
MMClock::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t projectedLength) const noexcept {
  EvictionAgeStat stat{};
  // const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  // const T* node = fifo_.getTail();
  // stat.warmQueueStat.oldestElementAge =
  //     node ? currTime - getUpdateTime(*node) : 0;
  // for (size_t numSeen = 0; numSeen < projectedLength && node != nullptr;
  //      numSeen++, node = fifo_.getPrev(*node)) {
  // }
  // stat.warmQueueStat.projectedAge = node ? currTime - getUpdateTime(*node)
  //                                        : stat.warmQueueStat.oldestElementAge;
  return stat;
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
void MMClock::Container<T, HookPtr>::setConfig(const Config& newConfig) {
  lruMutex_->lock_combine([this, newConfig]() {
    config_ = newConfig;
    nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                               ? std::numeric_limits<Time>::max()
                               : static_cast<Time>(util::getCurrentTimeSec()) +
                                     config_.mmReconfigureIntervalSecs.count();
  });
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
typename MMClock::Config MMClock::Container<T, HookPtr>::getConfig() const {
  return lruMutex_->lock_combine([this]() { return config_; });
}


template <typename T, MMClock::Hook<T> T::*HookPtr>
bool MMClock::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  if (node.isInMMContainer()) {
    return false;
  }

  // fifo_.linkAtHead(node);
  fifo_.add(node);
  unmarkAccessed(node);
  markValid(node);
  node.markInMMContainer();
  setUpdateTime(node, currTime);

  return true;
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
typename MMClock::Container<T, HookPtr>::LockedIterator
MMClock::Container<T, HookPtr>::getEvictionIterator() noexcept {
  // LockHolder l(*lruMutex_);
  return LockedIterator{&fifo_};
}

// template <typename T, MMClock::Hook<T> T::*HookPtr>
// template <typename F>
// void MMClock::Container<T, HookPtr>::withEvictionIterator(F&& fun) {
//   if (config_.useCombinedLockForIterators) {
//     lruMutex_->lock_combine([this, &fun]() { fun(Iterator{fifo_.rbegin()}); });
//   } else {
//     LockHolder lck{*lruMutex_};
//     fun(Iterator{fifo_.rbegin()});
//   }
// }


template <typename T, MMClock::Hook<T> T::*HookPtr>
void MMClock::Container<T, HookPtr>::removeLocked(T& node) {
  fifo_.remove(node);
  unmarkAccessed(node);
  node.unmarkInMMContainer();
  return;
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
bool MMClock::Container<T, HookPtr>::remove(T& node) noexcept {
  // return lruMutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  // });
}

// template <typename T, MMClock::Hook<T> T::*HookPtr>
// void MMClock::Container<T, HookPtr>::remove(Iterator& it) noexcept {
//   T& node = *it;
//   XDCHECK(node.isInMMContainer());
//   ++it;
//   removeLocked(node);
// }

template <typename T, MMClock::Hook<T> T::*HookPtr>
void MMClock::Container<T, HookPtr>::remove(LockedIterator& it) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  // ++it;
  // removeLocked(node);
  node.unmarkInMMContainer();
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
bool MMClock::Container<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  return lruMutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);
    fifo_.replace(oldNode, newNode);
    oldNode.unmarkInMMContainer();
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    if (isAccessed(oldNode)) {
      markAccessed(newNode);
    } else {
      unmarkAccessed(newNode);
    }
    return true;
  });
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
serialization::MMClockObject MMClock::Container<T, HookPtr>::saveState()
    const noexcept {
  serialization::MMClockConfig configObject;
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.tryLockUpdate() = config_.tryLockUpdate;

  serialization::MMClockObject object;
  *object.config() = configObject;
  *object.fifo() = fifo_.saveState();
  return object;
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
MMContainerStat MMClock::Container<T, HookPtr>::getStats() const noexcept {
  auto stat = lruMutex_->lock_combine([this]() {
    auto* tail = fifo_.getTail();

    // we return by array here because DistributedMutex is fastest when the
    // output data fits within 48 bytes.  And the array is exactly 48 bytes, so
    // it can get optimized by the implementation.
    //
    // the rest of the parameters are 0, so we don't need the critical section
    // to return them
    return folly::make_array(fifo_.size(),
                             tail == nullptr ? 0 : getUpdateTime(*tail)
                             );
  });
  return {stat[0] /* lru size */,
          stat[1] /* tail time */,
          // 0,
          0,
          0,
          0,
          0,
          0};
}

template <typename T, MMClock::Hook<T> T::*HookPtr>
void MMClock::Container<T, HookPtr>::reconfigureLocked(const Time& currTime) {
  if (currTime < nextReconfigureTime_) {
    return;
  }
  nextReconfigureTime_ = currTime + config_.mmReconfigureIntervalSecs.count();

  // // update LRU refresh time
  // auto stat = getEvictionAgeStatLocked(0);
  // auto lruRefreshTime = std::min(
  //     std::max(config_.defaultLruRefreshTime,
  //              static_cast<uint32_t>(stat.warmQueueStat.oldestElementAge *
  //                                    config_.lruRefreshRatio)),
  //     kLruRefreshTimeCap);
  // lruRefreshTime_.store(lruRefreshTime, std::memory_order_relaxed);
}

} // namespace cachelib
} // namespace facebook
