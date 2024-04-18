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

#include <folly/MPMCQueue.h>
#include <folly/logging/xlog.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop
#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include <atomic>
#include <algorithm>

#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook {
namespace cachelib {

// node information for the double linked list modelling the lru. It has the
// previous, next information with the last time the item was updated in the
// LRU.
template <typename T>
struct CACHELIB_PACKED_ATTR MobiusHook {
  using Time = uint32_t;
  using CompressedPtr = typename T::CompressedPtr;
  using PtrCompressor = typename T::PtrCompressor;

  void setNext(T* const n, const PtrCompressor& compressor) noexcept {
    next_ = compressor.compress(n);
  }

  void setNext(CompressedPtr next) noexcept { next_ = next; }


  CompressedPtr getNext() const noexcept { return CompressedPtr(next_); }

  T* getNext(const PtrCompressor& compressor) const noexcept {
    return compressor.unCompress(next_);
  }

  // set and get the time when the node was updated in the lru.
  void setUpdateTime(Time time) noexcept { updateTime_ = time; }

  Time getUpdateTime() const noexcept {
    // Suppress TSAN here because we don't care if an item is promoted twice by
    // two get operations running concurrently. It should be very rarely and is
    // just a minor inefficiency if it happens.
    folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
    return updateTime_;
  }

 private:
  CompressedPtr next_{};  // next node in the linked list
  // timestamp when this was last updated to the head of the list
  CompressedPtr prev_{};  // next node in the linked list. Unused in Mobius.
  Time updateTime_{0};
};

// uses a double linked list to implement an LRU. T must be have a public
// member of type Hook and HookPtr must point to that.
template <typename T, MobiusHook<T> T::*HookPtr>
class Mobius {
 public:
  using Mutex = folly::DistributedMutex;
  using LockHolder = std::unique_lock<Mutex>;
  using RefFlags = typename T::Flags;
  using CompressedPtr = typename T::CompressedPtr;
  using PtrCompressor = typename T::PtrCompressor;
  using MobiusObject = serialization::MobiusObject;

  Mobius() = default;
  Mobius(const Mobius&) = delete;
  Mobius& operator=(const Mobius&) = delete;

  explicit Mobius(PtrCompressor compressor) noexcept
      : compressor_(std::move(compressor)) {}

  // Restore Mobius from saved state.
  //
  // @param object              Save Mobius object
  // @param compressor          PtrCompressor object
  Mobius(const MobiusObject& object, PtrCompressor compressor)
      : compressor_(std::move(compressor)),
        head_({compressor_.unCompress(CompressedPtr{*object.compressedHead0()}),
          compressor_.unCompress(CompressedPtr{*object.compressedHead1()})}),
        tail_({compressor_.unCompress(CompressedPtr{*object.compressedTail0()}),
          compressor_.unCompress(CompressedPtr{*object.compressedTail1()})}),
        size_(*object.size()) {
        }

  /**
   * Exports the current state as a thrift object for later restoration.
   */
  MobiusObject saveState() const {
    MobiusObject state;
    *state.compressedHead0() = compressor_.compress(head_[0].load()).saveState();
    *state.compressedTail0() = compressor_.compress(tail_[0].load()).saveState();
    *state.compressedHead1() = compressor_.compress(head_[1].load()).saveState();
    *state.compressedTail1() = compressor_.compress(tail_[1].load()).saveState();
    *state.size() = size_.load();
    return state;
  }

  T* getNext(const T& node) const noexcept {
    return (node.*HookPtr).getNext(compressor_);
  }

  void setNext(T& node, T* next) noexcept {
    (node.*HookPtr).setNext(next, compressor_);
  }

  void setNextFrom(T& node, const T& other) noexcept {
    (node.*HookPtr).setNext((other.*HookPtr).getNext());
  }

  void add(T& node) noexcept;

  // Add node before nextNode.
  //
  // @param nextNode    node before which to insert
  // @param node        node to insert
  // @note nextNode must be in the list and node must not be in the list
  // void insertBefore(T& nextNode, T& node) noexcept;

  // removes the node completely from the linked list and cleans up the node
  // appropriately by setting its next and prev as nullptr.
  void remove(T& node) noexcept;

  // Unlinks the destination node and replaces it with the source node
  //
  // @param oldNode   destination node
  // @param newNode   source node
  void replace(T& oldNode, T& newNode) noexcept;

  T* getHead0() const noexcept { return head_[0].load(); }
  T* getTail0() const noexcept { return tail_[0].load(); }

  T* getHead1() const noexcept { return head_[1].load(); }
  T* getTail1() const noexcept { return tail_[1].load(); }

  size_t size() const noexcept { return size_.load(); }

  // Iterator interface for the double linked list. Supports both iterating
  // from the tail and head.
  class Iterator {
   public:
    enum class Direction { FROM_HEAD };

    Iterator(T* p, Direction d,
             const Mobius<T, HookPtr>& Mobius) noexcept
        : curr_(p), dir_(d), Mobius_(&Mobius) {}
    virtual ~Iterator() = default;

    // copyable and movable
    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;
    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    // moves the iterator forward and backward. Calling ++ once the iterator
    // has reached the end is undefined.
    Iterator& operator++() noexcept;
    Iterator& operator--();

    T* operator->() const noexcept { return curr_; }
    T& operator*() const noexcept { return *curr_; }

    bool operator==(const Iterator& other) const noexcept {
      return Mobius_ == other.Mobius_ &&
             curr_ == other.curr_ && dir_ == other.dir_;
    }

    bool operator!=(const Iterator& other) const noexcept {
      return !(*this == other);
    }

    explicit operator bool() const noexcept {
      return curr_ != nullptr && Mobius_ != nullptr;
    }

    T* get() const noexcept { return curr_; }

    // Invalidates this iterator
    void reset() noexcept { curr_ = nullptr; }

    // Reset the iterator back to the beginning
    void resetToBegin() noexcept {
      curr_ = Mobius_->head_[0].load();
      dir_ = Direction::FROM_HEAD;
    }

   protected:
    void goForward() noexcept;
    void goBackward();

    // the current position of the iterator in the list
    T* curr_{nullptr};
    // the direction we are iterating.
    Direction dir_{Direction::FROM_HEAD};
    const Mobius<T, HookPtr>* Mobius_{nullptr};
  };

  // provides an iterator starting from the head of the linked list.
  Iterator begin() const noexcept;

  // provides an iterator starting from the tail of the linked list.
  Iterator rbegin() const;

  // Iterator to compare against for the end.
  Iterator end() const noexcept;
  Iterator rend() const;

  T* getEvictionCandidate0() noexcept;

  T* getEvictionCandidate1() noexcept;

 private:

  // Links the passed nodes to the tail of the linked list
  // @param node node to be linked at the tail
  void linkAtTail(std::atomic<T*>& head_, std::atomic<T*>& tail_, T& beginNode, T& endNode) noexcept;

  void unmarkAccessedBatch(T* beginNode, T* endNode) noexcept;

  void markAccessed(T& node) noexcept {
    node.template setFlag<RefFlags::kMMFlag1>();
  }

  void unmarkAccessed(T& node) noexcept {
    node.template unSetFlag<RefFlags::kMMFlag1>();
  }

  bool isAccessed(const T& node) const noexcept {
    return node.template isFlagSet<RefFlags::kMMFlag1>();
  }

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

  mutable folly::cacheline_aligned<Mutex> mtx_;

  // head of the linked list
  std::atomic<T*> head_[2]{nullptr, nullptr};

  // tail of the linked list
  std::atomic<T*> tail_[2]{nullptr, nullptr};

  std::atomic<bool> whichQ_{0};

  // size of the list
  std::atomic<size_t> size_{0};

  std::atomic<size_t> countRequest_{0};
};
}  // namespace cachelib
}  // namespace facebook

#include "cachelib/allocator/datastruct/Mobius-inl.h"
