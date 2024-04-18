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

// #include "cachelib/allocator/MMSieve.h"


namespace facebook {
namespace cachelib {

/* Container Interface Implementation */
template <typename T, MMSieve::Hook<T> T::*HookPtr>
MMSieve::Container<T, HookPtr>::Container(
    serialization::MMSieveObject object, PtrCompressor compressor)
    : compressor_(std::move(compressor)),
      fifo_(*object.fifo(), compressor_),
      config_(*object.config()) {
  nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                             ? std::numeric_limits<Time>::max()
                             : static_cast<Time>(util::getCurrentTimeSec()) +
                                   config_.mmReconfigureIntervalSecs.count();
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
bool MMSieve::Container<T, HookPtr>::recordAccess(
    T& node, AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto curr = static_cast<Time>(util::getCurrentTimeSec());
  // check if the node is still being memory managed
  if (node.isInMMContainer()) {
    if (!isAccessed(node)) {
      markAccessed(node);
    }

    setUpdateTime(node, curr);
    return true;
  }
  return false;
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat
MMSieve::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return lruMutex_->lock_combine([this, projectedLength]() {
    return getEvictionAgeStatLocked(projectedLength);
  });
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat
MMSieve::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t projectedLength) const noexcept {
  EvictionAgeStat stat{};
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  const T* node = fifo_.getTail();
  stat.warmQueueStat.oldestElementAge =
      node ? currTime - getUpdateTime(*node) : 0;
  for (size_t numSeen = 0; numSeen < projectedLength && node != nullptr;
       numSeen++, node = fifo_.getPrev(*node)) {
  }
  stat.warmQueueStat.projectedAge = node ? currTime - getUpdateTime(*node)
                                         : stat.warmQueueStat.oldestElementAge;
  return stat;
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
void MMSieve::Container<T, HookPtr>::setConfig(const Config& newConfig) {
  // lruMutex_->lock_combine([this, newConfig]() {
  //   config_ = newConfig;
  //   if (config_.lruInsertionPointSpec == 0 && insertionPoint_ != nullptr) {
  //     auto curr = insertionPoint_;
  //     while (tailSize_ != 0) {
  //       XDCHECK(curr != nullptr);
  //       unmarkTail(*curr);
  //       tailSize_--;
  //       curr = fifo_.getNext(*curr);
  //     }
  //     insertionPoint_ = nullptr;
  //   }
  //   nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
  //                              ? std::numeric_limits<Time>::max()
  //                              : static_cast<Time>(util::getCurrentTimeSec())
  //                              +
  //                                    config_.mmReconfigureIntervalSecs.count();
  // });
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
typename MMSieve::Config MMSieve::Container<T, HookPtr>::getConfig()
    const {
  return lruMutex_->lock_combine([this]() { return config_; });
}


template <typename T, MMSieve::Hook<T> T::*HookPtr>
bool MMSieve::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  if (node.isInMMContainer()) {
    return false;
  }
  fifo_.linkAtHead(node);
  unmarkAccessed(node);
  node.markInMMContainer();
  setUpdateTime(node, currTime);
  return true;
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
typename MMSieve::Container<T, HookPtr>::LockedIterator
MMSieve::Container<T, HookPtr>::getEvictionIterator() noexcept {
  return LockedIterator{&fifo_};
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
template <typename F>
void MMSieve::Container<T, HookPtr>::withEvictionIterator(F&& fun) {
  if (config_.useCombinedLockForIterators) {
    lruMutex_->lock_combine([this, &fun]() { fun(Iterator{fifo_.rbegin()}); });
  } else {
    LockHolder lck{*lruMutex_};
    fun(Iterator{fifo_.rbegin()});
  }
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
void MMSieve::Container<T, HookPtr>::removeLocked(T& node) {
  fifo_.remove(node);
  unmarkAccessed(node);
  node.unmarkInMMContainer();
  // updateLruInsertionPoint();
  return;
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
bool MMSieve::Container<T, HookPtr>::remove(T& node) noexcept {
  return lruMutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  });
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
void MMSieve::Container<T, HookPtr>::remove(LockedIterator& it) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  // ++it;
  // removeLocked(node);
  node.unmarkInMMContainer();
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
bool MMSieve::Container<T, HookPtr>::replace(T& oldNode,
                                                   T& newNode) noexcept {
  return lruMutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);
    // fifo_.replace(oldNode, newNode);
    fifo_.remove(oldNode);
    fifo_.linkAtHead(newNode);

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

template <typename T, MMSieve::Hook<T> T::*HookPtr>
serialization::MMSieveObject
MMSieve::Container<T, HookPtr>::saveState() const noexcept {
  serialization::MMSieveConfig configObject;
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.tryLockUpdate() = config_.tryLockUpdate;
  *configObject.lruInsertionPointSpec() = config_.lruInsertionPointSpec;

  serialization::MMSieveObject object;
  *object.config() = configObject;
  *object.fifo() = fifo_.saveState();
  return object;
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
MMContainerStat MMSieve::Container<T, HookPtr>::getStats()
    const noexcept {
  auto stat = lruMutex_->lock_combine([this]() {
    auto* tail = fifo_.getTail();

    // we return by array here because DistributedMutex is fastest when the
    // output data fits within 48 bytes.  And the array is exactly 48 bytes, so
    // it can get optimized by the implementation.
    //
    // the rest of the parameters are 0, so we don't need the critical section
    // to return them
    return folly::make_array(fifo_.size(),
                             tail == nullptr ? 0 : getUpdateTime(*tail));
  });
  return {stat[0] /* lru size */, stat[1] /* tail time */,
          // 0,
          0, 0, 0, 0, 0};
}

template <typename T, MMSieve::Hook<T> T::*HookPtr>
void MMSieve::Container<T, HookPtr>::reconfigureLocked(
    const Time& currTime) {
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

}  // namespace cachelib
}  // namespace facebook
