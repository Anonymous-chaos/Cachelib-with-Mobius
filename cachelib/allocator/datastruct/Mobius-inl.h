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

/* when linkedAtHead uses atomic op,
 * it is possible to conflict with the node that unlink the head when using
 */

/* Linked list implemenation */
template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::linkAtTail(std::atomic<T*>& head_, std::atomic<T*>& tail_, T& beginNode, T& endNode) noexcept {
  setNext(endNode, nullptr);
  T* oldTail = tail_.load();

  while (!tail_.compare_exchange_weak(oldTail, &endNode)) {}

  // this is the thread that first makes Tail_ points to the node
  // other threads must follow this, o.w. oldHead will be nullptr
  if(oldTail == nullptr){
    head_ = &beginNode;
  }
  else{
    setNext(*oldTail, &beginNode);
  }
}

/*
  beginNode and endNode must be at the same linklist. 
 */
template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::unmarkAccessedBatch(T* beginNode, T* endNode) noexcept {
  T *curr = beginNode;
  while(true) {
    if(curr == nullptr) return;
    unmarkAccessed(*curr);
    if(curr == endNode)
      break;
    curr = getNext(*curr);
  }
}

template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::add(T& node) noexcept {
  bool wQ = whichQ_.load();
  linkAtTail(head_[wQ], tail_[wQ], node, node);
  size_.fetch_add(1);
}


template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::remove(T& node) noexcept {
  XDCHECK_GT(size_, 0u);
  if (isValid(node)) {
    unmarkValid(node);
    unmarkAccessed(node);
  }
}

template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  remove(oldNode);
  add(newNode);
}

// The common eviciton of Mobius using CAS for thread safety. 
template <typename T, MobiusHook<T> T::*HookPtr>
T* Mobius<T, HookPtr>::getEvictionCandidate0() noexcept {
  bool wQ = whichQ_.load();

  auto &activeHead_ = head_[wQ];
  auto &activeTail_ = tail_[wQ];

  auto &dormantHead_ = head_[!wQ];
  auto &dormantTail_ = tail_[!wQ];

  T *curr;

  while(true){
    curr = activeHead_.load();
    do{
      // When the active Q has only 1 elements, switch the active Q.
      if(getNext(*curr) == nullptr && activeHead_.load() == curr){
        whichQ_.compare_exchange_weak(wQ, !wQ);
        return getEvictionCandidate0();
      }
    }while(!activeHead_.compare_exchange_weak(curr, getNext(*curr)));

    if(isValid(*curr) && isAccessed(*curr)){
      unmarkAccessed(*curr);
      linkAtTail(dormantHead_, dormantTail_, *curr, *curr);
    }
    else{
      size_.fetch_sub(1);
      return curr;
    }
  }
}

// The Eviction with consecutive detection using CAS for thread safety. 
template <typename T, MobiusHook<T> T::*HookPtr>
T* Mobius<T, HookPtr>::getEvictionCandidate1() noexcept {

  bool wQ = whichQ_.load();

  auto &activeHead_ = head_[wQ];
  auto &activeTail_ = tail_[wQ];

  auto &dormantHead_ = head_[!wQ];
  auto &dormantTail_ = tail_[!wQ];

  T *prevHead, *prev, *curr = nullptr;
  int i;
  while(true){
    if(curr == nullptr){
      curr = prevHead = activeHead_.load();
      prev = nullptr;
      i = 0;
    }
    // Active Q has only no more elements, switch the active Q.
    if(curr == nullptr || (getNext(*curr) == nullptr && prevHead == activeHead_.load())){
        if(whichQ_.compare_exchange_weak(wQ, !wQ)){
          unmarkAccessedBatch(prevHead, curr);
        }
        return getEvictionCandidate1();
    }

    if(isValid(*curr) && isAccessed(*curr)){
      prev = curr;
      curr = getNext(*curr);
      i++;
      continue;     
    }

    bool updateHead = activeHead_.compare_exchange_weak(prevHead, getNext(*curr));

    if(updateHead){
      if(prev){
        unmarkAccessedBatch(prevHead, prev); // Set the state of the nodes between prevHead and curr to unAccessed.
        linkAtTail(dormantHead_, dormantTail_, *prevHead, *prev);
      }
      size_.fetch_sub(1+i);
      return curr;
    }
    curr = nullptr;
  }
}

/* Iterator Implementation */
template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::Iterator::goForward() noexcept {
    curr_ = Mobius_->getNext(*curr_);
}

template <typename T, MobiusHook<T> T::*HookPtr>
void Mobius<T, HookPtr>::Iterator::goBackward() {
   throw std::logic_error("Not implemented"); 
}

template <typename T, MobiusHook<T> T::*HookPtr>
typename Mobius<T, HookPtr>::Iterator&
Mobius<T, HookPtr>::Iterator::operator++() noexcept { 
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, MobiusHook<T> T::*HookPtr>
typename Mobius<T, HookPtr>::Iterator&
Mobius<T, HookPtr>::Iterator::operator--() {
   throw std::logic_error("Not implemented"); 
}

template <typename T, MobiusHook<T> T::*HookPtr>
typename Mobius<T, HookPtr>::Iterator
Mobius<T, HookPtr>::begin() const noexcept {
  return Mobius<T, HookPtr>::Iterator(
      head_[0], Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, MobiusHook<T> T::*HookPtr>
typename Mobius<T, HookPtr>::Iterator
Mobius<T, HookPtr>::rbegin() const {
  throw std::logic_error("Not implemented"); 
}

template <typename T, MobiusHook<T> T::*HookPtr>
typename Mobius<T, HookPtr>::Iterator
Mobius<T, HookPtr>::end() const noexcept {
  return Mobius<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, MobiusHook<T> T::*HookPtr>
typename Mobius<T, HookPtr>::Iterator
Mobius<T, HookPtr>::rend() const {
   throw std::logic_error("Not implemented"); 
}

}  // namespace cachelib
}  // namespace facebook
