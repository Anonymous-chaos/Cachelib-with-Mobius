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

#pragma once

#include <atomic>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#pragma GCC diagnostic pop
#include <folly/container/Array.h>
#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/Mobius.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook {
namespace cachelib {

// CacheLib's modified LRU policy.
// In classic LRU, the items form a queue according to the last access time.
// Items are inserted to the head of the queue and removed from the tail of the
// queue. Items accessed (used) are moved (promoted) to the head of the queue.
// CacheLib made two variations on top of the classic LRU:
// 1. Insertion point. The items are inserted into a configured insertion point
// instead of always to the head.
// 2. Delayed promotion. Items get promoted at most once in any lru refresh time
// window. lru refresh time and lru refresh ratio controls this internval
// length.
class MMMobius {
 public:
  // unique identifier per MMType
  static const int kId;

  // forward declaration;
  template <typename T>
  // using Hook = DListHook<T>;
  using Hook = MobiusHook<T>;
  using SerializationType = serialization::MMMobiusObject;
  using SerializationConfigType = serialization::MMMobiusConfig;
  using SerializationTypeContainer = serialization::MMMobiusCollection;

  // This is not applicable for MMMobius, just for compile of cache allocator
  enum LruType { NumTypes };

  // Config class for MMMobius
  struct Config {
    // create from serialized config
    explicit Config(SerializationConfigType configState)
        : Config(*configState.updateOnWrite(), *configState.updateOnRead()) {}

    // @param time        the LRU refresh time in seconds.
    //                    An item will be promoted only once in each lru refresh
    //                    time depite the number of accesses it gets.
    // @param udpateOnW   whether to promote the item on write
    // @param updateOnR   whether to promote the item on read
    Config(bool updateOnW, bool updateOnR) : Config(updateOnW, updateOnR, 0) {}

    // @param time        the LRU refresh time in seconds.
    //                    An item will be promoted only once in each lru refresh
    //                    time depite the number of accesses it gets.
    // @param ratio       the lru refresh ratio. The ratio times the
    //                    oldest element's lifetime in warm queue
    //                    would be the minimum value of LRU refresh time.
    // @param udpateOnW   whether to promote the item on write
    // @param updateOnR   whether to promote the item on read
    // @param mmReconfigureInterval   Time interval for recalculating lru
    //                                refresh time according to the ratio.
    // useCombinedLockForIterators    Whether to use combined locking for
    //                                withEvictionIterator
    Config(bool updateOnW, bool updateOnR, uint32_t mmReconfigureInterval)
        : updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          mmReconfigureIntervalSecs(
              std::chrono::seconds(mmReconfigureInterval)) {}

    Config() = default;
    Config(const Config& rhs) = default;
    Config(Config&& rhs) = default;

    Config& operator=(const Config& rhs) = default;
    Config& operator=(Config&& rhs) = default;

    template <typename... Args>
    void addExtraConfig(Args...) {}

    // threshold value in seconds to compare with a node's update time to
    // determine if we need to update the position of the node in the linked
    // list. By default this is 60s to reduce the contention on the lru lock.
    // uint32_t defaultLruRefreshTime{60};
    // uint32_t lruRefreshTime{defaultLruRefreshTime};

    // ratio of LRU refresh time to the tail age. If a refresh time computed
    // according to this ratio is larger than lruRefreshtime, we will adopt
    // this one instead of the lruRefreshTime set.
    // double lruRefreshRatio{0.};

    // whether the lru needs to be updated on writes for recordAccess. If
    // false, accessing the cache for writes does not promote the cached item
    // to the head of the lru.
    bool updateOnWrite{false};

    // whether the lru needs to be updated on reads for recordAccess. If
    // false, accessing the cache for reads does not promote the cached item
    // to the head of the lru.
    bool updateOnRead{true};

    // whether to tryLock or lock the lru lock when attempting promotion on
    // access. If set, and tryLock fails, access will not result in promotion.
    bool tryLockUpdate{false};

    // Minimum interval between reconfigurations. If 0, reconfigure is never
    // called.
    std::chrono::seconds mmReconfigureIntervalSecs{};

    // Whether to use combined locking for withEvictionIterator.
    bool useCombinedLockForIterators{false};
  };

  // The container object which can be used to keep track of objects of type
  // T. T must have a public member of type Hook. This object is wrapper
  // around DList, is thread safe and can be accessed from multiple threads.
  // The current implementation models an LRU using the above DList
  // implementation.
  template <typename T, Hook<T> T::*HookPtr>
  struct Container {
   private:
    using MobiusList = Mobius<T, HookPtr>;
    using Mutex = folly::DistributedMutex;
    using LockHolder = std::unique_lock<Mutex>;
    using PtrCompressor = typename T::PtrCompressor;
    using Time = typename Hook<T>::Time;
    using CompressedPtr = typename T::CompressedPtr;
    using RefFlags = typename T::Flags;

   public:
    Container() = default;
    Container(Config c, PtrCompressor compressor)
        : compressor_(std::move(compressor)),
          mlist_(std::move(compressor)),
          config_(std::move(c)) {
      nextReconfigureTime_ =
          config_.mmReconfigureIntervalSecs.count() == 0
              ? std::numeric_limits<Time>::max()
              : static_cast<Time>(util::getCurrentTimeSec()) +
                    config_.mmReconfigureIntervalSecs.count();
    }
    Container(serialization::MMMobiusObject object, PtrCompressor compressor);

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    // context for iterating the MM container. At any given point of time,
    // there can be only one iterator active since we need to lock the LRU for
    // iteration. we can support multiple iterators at same time, by using a
    // shared ptr in the context for the lock holder in the future.
    class LockedIterator {
     public:
      // noncopyable but movable.
      LockedIterator(const LockedIterator&) = delete;
      LockedIterator& operator=(const LockedIterator&) = delete;

      LockedIterator(LockedIterator&&) noexcept = default;

      // moves the LockedIterator forward and backward. Calling ++ once the
      // LockedIterator has reached the end is undefined.
      LockedIterator& operator++() {
        candidate_ = mlist_->getEvictionCandidate1();
        return *this;
      }
      LockedIterator& operator--() { throw std::logic_error("Not implemented"); }

      T* operator->() const noexcept { return candidate_; }
      T& operator*() const noexcept { return *candidate_; }

      explicit operator bool() const noexcept { return candidate_ != nullptr; }

      T* get() const noexcept { return candidate_; }

      // Invalidates this iterator
      void reset() noexcept {
        // Set index to before first list
        // index_ = kInvalidIndex;
        // Point iterator to first list's rend
        // currIter_ = mlist_.lists_[0]->rend();
      }

      // 1. Invalidate this iterator
      // 2. Unlock
      void destroy() {
        // Iterator::reset();
        // if (l_.owns_lock()) {
        //   l_.unlock();
        // }
      }

      // Reset this iterator to the beginning
      void resetToBegin() noexcept {
        // if (!l_.owns_lock()) {
        //   l_.lock();
        // }
        // Iterator::resetToBegin();
      }

     private:
      // private because it's easy to misuse and cause deadlock for MMMobius
      LockedIterator& operator=(LockedIterator&&) noexcept = default;

      // create an lru iterator with the lock being held.
      LockedIterator(MobiusList* mlist)
      : mlist_(mlist),
        candidate_(mlist_->getEvictionCandidate1()) {}

      MobiusList* mlist_;

      T* candidate_;
      // lock protecting the validity of the iterator
      // LockHolder l_;
      // only the container can create iterators
      friend Container<T, HookPtr>;
    };

    // records the information that the node was accessed. This could bump up
    // the node to the head of the lru depending on the time when the node was
    // last updated in lru and the kLruRefreshTime. If the node was moved to
    // the head in the lru, the node's updateTime will be updated
    // accordingly.
    //
    // @param node  node that we want to mark as relevant/accessed
    // @param mode  the mode for the access operation.
    //
    // @return      True if the information is recorded and bumped the node
    //              to the head of the lru, returns false otherwise
    bool recordAccess(T& node, AccessMode mode) noexcept;

    // adds the given node into the container and marks it as being present in
    // the container. The node is added to the head of the lru.
    //
    // @param node  The node to be added to the container.
    // @return  True if the node was successfully added to the container. False
    //          if the node was already in the contianer. On error state of node
    //          is unchanged.
    bool add(T& node) noexcept;

    // removes the node from the lru and sets it previous and next to nullptr.
    //
    // @param node  The node to be removed from the container.
    // @return  True if the node was successfully removed from the container.
    //          False if the node was not part of the container. On error, the
    //          state of node is unchanged.
    bool remove(T& node) noexcept;

    // same as the above but uses an iterator context. The iterator is updated
    // on removal of the corresponding node to point to the next node. The
    // iterator context is responsible for locking.
    //
    // iterator will be advanced to the next node after removing the node
    //
    // @param it    Iterator that will be removed
    void remove(LockedIterator& it) noexcept;

    // replaces one node with another, at the same position
    //
    // @param oldNode   node being replaced
    // @param newNode   node to replace oldNode with
    //
    // @return true  If the replace was successful. Returns false if the
    //               destination node did not exist in the container, or if the
    //               source node already existed.
    bool replace(T& oldNode, T& newNode) noexcept;

    // Obtain an iterator that start from the tail and can be used
    // to search for evictions. This iterator holds a lock to this
    // container and only one such iterator can exist at a time
    LockedIterator getEvictionIterator() noexcept;

    // Execute provided function under container lock. Function gets
    // iterator passed as parameter.
    // template <typename F>
    // void withEvictionIterator(F&& f);

    // get copy of current config
    Config getConfig() const;

    // override the existing config with the new one.
    void setConfig(const Config& newConfig);

    bool isEmpty() const noexcept { return size() == 0; }

    // reconfigure the MMContainer: update refresh time according to current
    // tail age
    void reconfigureLocked(const Time& currTime);

    // returns the number of elements in the container
    size_t size() const noexcept {
      return lruMutex_->lock_combine([this]() { return mlist_.size(); });
    }

    // Returns the eviction age stats. See CacheStats.h for details
    EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;

    // for saving the state of the lru
    //
    // precondition:  serialization must happen without any reader or writer
    // present. Any modification of this object afterwards will result in an
    // invalid, inconsistent state for the serialized data.
    //
    serialization::MMMobiusObject saveState() const noexcept;

    // return the stats for this container.
    MMContainerStat getStats() const noexcept;

    LruType getLruType(const T& node) noexcept {
      return LruType::NumTypes;
    }

   private:
    EvictionAgeStat getEvictionAgeStatLocked(
        uint64_t projectedLength) const noexcept;

    static Time getUpdateTime(const T& node) noexcept {
      return (node.*HookPtr).getUpdateTime();
    }

    static void setUpdateTime(T& node, Time time) noexcept {
      (node.*HookPtr).setUpdateTime(time);
    }

    // remove node from lru and adjust insertion points
    //
    // @param node          node to remove
    // @param doRebalance     whether to do rebalance in this remove
    void removeLocked(T& node) noexcept;


    // Bit MM_BIT_1 is used to record if the item has been accessed since
    // being written in cache. Unaccessed items are ignored when determining
    // projected update time.
    void markAccessed(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag1>();
    }

    void unmarkAccessed(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag1>();
    }

    bool isAccessed(const T& node) const noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag1>();
    }

    // Bit MM_BIT_2 is used to record if the item is valid.
    void markValid(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag2>();
    }

    void unmarkValid(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag2>();
    }

    bool isValid(const T& node) const noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag2>();
    }

    const PtrCompressor compressor_{};

    // protects all operations on the lru. We never really just read the state
    // of the LRU. Hence we dont really require a RW mutex at this point of
    // time.
    mutable folly::cacheline_aligned<Mutex> lruMutex_;

    // the mobius queue.
    MobiusList mlist_{};

    // The next time to reconfigure the container.
    std::atomic<Time> nextReconfigureTime_{};

    // Config for this lru.
    // Write access to the MMMobius Config is serialized.
    // Reads may be racy.
    Config config_{};

    FRIEND_TEST(MMMobiusTest, Reconfigure);
  };
};
}  // namespace cachelib
}  // namespace facebook

#include "cachelib/allocator/MMMobius-inl.h"
