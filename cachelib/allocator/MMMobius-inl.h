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
template <typename T, MMMobius::Hook<T> T::*HookPtr>
MMMobius::Container<T, HookPtr>::Container(serialization::MMMobiusObject object,
                                        PtrCompressor compressor)
    : compressor_(std::move(compressor)),
      mlist_(*object.mlist(), compressor_),
      config_(*object.config()) {
  nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                             ? std::numeric_limits<Time>::max()
                             : static_cast<Time>(util::getCurrentTimeSec()) +
                                   config_.mmReconfigureIntervalSecs.count();
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
bool MMMobius::Container<T, HookPtr>::recordAccess(T& node,
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
    setUpdateTime(node, currTime);
    return true;
  }
  return false;
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat MMMobius::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return lruMutex_->lock_combine([this, projectedLength]() {
    return getEvictionAgeStatLocked(projectedLength);
  });
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
cachelib::EvictionAgeStat
MMMobius::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t projectedLength) const noexcept {
  EvictionAgeStat stat{};
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
  printf("getEvictionAgeStatLocked not implemented yet\n");
  return stat;
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
void MMMobius::Container<T, HookPtr>::setConfig(const Config& newConfig) {
  lruMutex_->lock_combine([this, newConfig]() {
    config_ = newConfig;
    nextReconfigureTime_ = config_.mmReconfigureIntervalSecs.count() == 0
                               ? std::numeric_limits<Time>::max()
                               : static_cast<Time>(util::getCurrentTimeSec()) +
                                     config_.mmReconfigureIntervalSecs.count();
  });
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
typename MMMobius::Config MMMobius::Container<T, HookPtr>::getConfig() const {
  return lruMutex_->lock_combine([this]() { return config_; });
}


template <typename T, MMMobius::Hook<T> T::*HookPtr>
bool MMMobius::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
  if(node.markInMMContainer()) {
    unmarkAccessed(node);
    markValid(node);
    setUpdateTime(node, currTime);
    mlist_.add(node);
    return true;   
  }
  else
    return false;
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
typename MMMobius::Container<T, HookPtr>::LockedIterator
MMMobius::Container<T, HookPtr>::getEvictionIterator() noexcept {
  // LockHolder l(*lruMutex_);
  return LockedIterator{&mlist_};
}


template <typename T, MMMobius::Hook<T> T::*HookPtr>
void MMMobius::Container<T, HookPtr>::removeLocked(T& node) noexcept {
  mlist_.remove(node);
  unmarkAccessed(node);
  // node.unmarkInMMContainer();
  return;
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
bool MMMobius::Container<T, HookPtr>::remove(T& node) noexcept {
  return lruMutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  });
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
void MMMobius::Container<T, HookPtr>::remove(LockedIterator& it) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  // removeLocked(node);
  node.unmarkInMMContainer();
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
bool MMMobius::Container<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  return lruMutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    unmarkAccessed(newNode);
    markValid(newNode);

    mlist_.replace(oldNode, newNode);
    oldNode.unmarkInMMContainer();

    return true;
  });
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
serialization::MMMobiusObject MMMobius::Container<T, HookPtr>::saveState()
    const noexcept {
  serialization::MMMobiusConfig configObject;
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.tryLockUpdate() = config_.tryLockUpdate;

  serialization::MMMobiusObject object;
  *object.config() = configObject;
  *object.mlist() = mlist_.saveState();
  return object;
}

template <typename T, MMMobius::Hook<T> T::*HookPtr>
MMContainerStat MMMobius::Container<T, HookPtr>::getStats() const noexcept {
  auto stat = lruMutex_->lock_combine([this]() {
    auto* tail = mlist_.getTail0();

    // we return by array here because DistributedMutex is fastest when the
    // output data fits within 48 bytes.  And the array is exactly 48 bytes, so
    // it can get optimized by the implementation.
    //
    // the rest of the parameters are 0, so we don't need the critical section
    // to return them
    return folly::make_array(mlist_.size(),
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

template <typename T, MMMobius::Hook<T> T::*HookPtr>
void MMMobius::Container<T, HookPtr>::reconfigureLocked(const Time& currTime) {
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
