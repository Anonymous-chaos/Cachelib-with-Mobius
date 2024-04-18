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

/* Linked list implemenation */
template <typename T, ClockListHook<T> T::*HookPtr>
void ClockList<T, HookPtr>::linkAtHead(T& node) noexcept {
  std::shared_lock lock(head_mutex);
  
  setPrev(node, nullptr);

  T* oldHead = head_.load();
  setNext(node, oldHead);

  while (!head_.compare_exchange_weak(oldHead, &node)) {
    setNext(node, oldHead);
  }

  if (oldHead == nullptr) {
    // this is the thread that first makes head_ points to the node
    // other threads must follow this, o.w. oldHead will be nullptr
    XDCHECK_EQ(tail_, nullptr);

    T* tail = nullptr;
    tail_.compare_exchange_weak(tail, &node);
  } else {
    setPrev(*oldHead, &node);
  }

  size_++;
}


template <typename T, ClockListHook<T> T::*HookPtr>
void ClockList<T, HookPtr>::unlink(const T& node) noexcept {
  XDCHECK_GT(size_, 0u);

  {
    std::unique_lock lock(head_mutex);
    
    auto* const next = getNext(node);
    
    if (&node == head_) {
      head_ = next;
    }
    
    if (next != nullptr) {
      setPrevFrom(*next, node);
    }
  }

  auto* const prev = getPrev(node);
  
  if (&node == tail_) {
    tail_ = prev;
  }

  if (prev != nullptr) {
    setNextFrom(*prev, node);
  }

  size_--;
}

template <typename T, ClockListHook<T> T::*HookPtr>
void ClockList<T, HookPtr>::remove(T& node) noexcept {
  auto* const prev = getPrev(node);
  auto* const next = getNext(node);
  if (prev == nullptr && next == nullptr) {
    return;
  }

  LockHolder l(*mtx_);
  unlink(node);
  setNext(node, nullptr);
  setPrev(node, nullptr);
}


/* note that the next of the tail may not be nullptr  */
template <typename T, ClockListHook<T> T::*HookPtr>
T* ClockList<T, HookPtr>::removeTail() noexcept {
  T* tail = tail_.load();
  if (tail == nullptr) {
    // empty list
    return nullptr;
  }
  T* prev = getPrev(*tail);

  // if tail has not changed, the prev is correct
  while (!tail_.compare_exchange_weak(tail, prev)) {
    prev = getPrev(*tail);
  }

  // if the tail was also the head
  if (head_ == tail) {
    T* oldHead = tail;
    head_.compare_exchange_weak(oldHead, nullptr);
  }

  setNext(*tail, nullptr);
  setPrev(*tail, nullptr);

  size_--;

  return tail;
}


template <typename T, ClockListHook<T> T::*HookPtr>
void ClockList<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  LockHolder l(*mtx_);

  // Update head and tail links if needed
  if (&oldNode == head_) {
    head_ = &newNode;
  }
  if (&oldNode == tail_) {
    tail_ = &newNode;
  }

  // Make the previous and next nodes point to the new node
  auto* const prev = getPrev(oldNode);
  auto* const next = getNext(oldNode);
  if (prev != nullptr) {
    setNext(*prev, &newNode);
  }
  if (next != nullptr) {
    setPrev(*next, &newNode);
  }

  // Make the new node point to the previous and next nodes
  setPrev(newNode, prev);
  setNext(newNode, next);

  // Cleanup the old node
  setPrev(oldNode, nullptr);
  setNext(oldNode, nullptr);
}


/* Iterator Implementation */
template <typename T, ClockListHook<T> T::*HookPtr>
void ClockList<T, HookPtr>::Iterator::goForward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = ClockList_->getPrev(*curr_);
  } else {
    curr_ = ClockList_->getNext(*curr_);
  }
}

template <typename T, ClockListHook<T> T::*HookPtr>
void ClockList<T, HookPtr>::Iterator::goBackward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = ClockList_->getNext(*curr_);
  } else {
    curr_ = ClockList_->getPrev(*curr_);
  }
}

template <typename T, ClockListHook<T> T::*HookPtr>
typename ClockList<T, HookPtr>::Iterator&
ClockList<T, HookPtr>::Iterator::operator++() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, ClockListHook<T> T::*HookPtr>
typename ClockList<T, HookPtr>::Iterator&
ClockList<T, HookPtr>::Iterator::operator--() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goBackward();
  }
  return *this;
}

template <typename T, ClockListHook<T> T::*HookPtr>
typename ClockList<T, HookPtr>::Iterator ClockList<T, HookPtr>::begin()
    const noexcept {
  return ClockList<T, HookPtr>::Iterator(head_, Iterator::Direction::FROM_HEAD,
                                         *this);
}

template <typename T, ClockListHook<T> T::*HookPtr>
typename ClockList<T, HookPtr>::Iterator ClockList<T, HookPtr>::rbegin()
    const noexcept {
  return ClockList<T, HookPtr>::Iterator(tail_, Iterator::Direction::FROM_TAIL,
                                         *this);
}

template <typename T, ClockListHook<T> T::*HookPtr>
typename ClockList<T, HookPtr>::Iterator ClockList<T, HookPtr>::end()
    const noexcept {
  return ClockList<T, HookPtr>::Iterator(nullptr,
                                         Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, ClockListHook<T> T::*HookPtr>
typename ClockList<T, HookPtr>::Iterator ClockList<T, HookPtr>::rend()
    const noexcept {
  return ClockList<T, HookPtr>::Iterator(nullptr,
                                         Iterator::Direction::FROM_TAIL, *this);
}

template <typename T, ClockListHook<T> T::*HookPtr>
T* ClockList<T, HookPtr>::getEvictionCandidate() noexcept {
  T* curr = nullptr;

  while (true) {
    curr = removeTail();
    if (curr == nullptr) {
      return nullptr;
    }
    if (isAccessed(*curr)) {
      unmarkAccessed(*curr);
      linkAtHead(*curr);
    } 
    else {
      return curr;
    }
  }
}

} // namespace cachelib
}  // namespace facebook
