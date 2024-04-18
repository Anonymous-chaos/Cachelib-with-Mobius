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

#include <list>
#include <algorithm>
#define CC_UNMARK   0
#define CC_MOVABLE    1
#define CC_EVICTABLE  2

namespace facebook {
namespace cachelib {


/*
  beginNode and endNode must be at the same linklist. 
 */
template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::clearStates(T* beginNode, T* endNode) noexcept {
  T *curr = beginNode;
  while(true) {
    if(curr == nullptr) return;
    clearState(*curr);
    if(curr == endNode)
      break;
    curr = getNext(*curr);
  }
}

/*
  beginNode and endNode must be at the same linklist. 
 */
template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::unmarkAccessedBatch(T* beginNode, T* endNode) noexcept {
  T *curr = beginNode;
  while(true) {
    if(curr == nullptr) return;
    unmarkAccessed(*curr);
    if(curr == endNode)
      break;
    curr = getNext(*curr);
  }
}

/* Linked list implemenation */
template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::linkAtTail(T& beginNode, T& endNode) noexcept {
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

template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::add(T& node) noexcept {
  // printf("Add, size=%lu\n", size_.load());
  linkAtTail(node, node);
  markValid(node);
  unmarkAccessed(node);
  size_++;
  // countRequest_++;
  // if(countRequest_.load() % 10000000 == 0)
  //   printf("Request count: %lu, size: %lu\n", countRequest_.load(), size_.load());
}


template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::remove(T& node) noexcept {
  XDCHECK_GT(size_, 0u);
  if (isValid(node)) {
    unmarkValid(node);
    unmarkAccessed(node);
  }
}

template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  remove(oldNode);
  add(newNode);
}

template <typename T, CClockListHook<T> T::*HookPtr>
T* CClockList<T, HookPtr>::getEvictionCandidate0() noexcept {
  T* curr;

  while (true) {
    curr = head_.load();
    if (curr == nullptr)
      return nullptr;
    while(!head_.compare_exchange_weak(curr, getNext(*curr))){}
    // If the node is valid and has been accessed, move it to the tail
    if(isValid(*curr) && isAccessed(*curr)){
      unmarkAccessed(*curr);
      linkAtTail(*curr, *curr); 
    }
    // Else, unlink and return
    else{
      // printf("Evict0, size=%lu\n", size_.load());
      size_--;
      return curr;
    }
  }
}

template <typename T, CClockListHook<T> T::*HookPtr>
T* CClockList<T, HookPtr>::getEvictionCandidate1() noexcept {
  if (size_.load() == 0)
    return nullptr;

  LockHolder l(*mtx_);

  T* oldHead = head_.load();
  T* prev = nullptr;
  T* curr = oldHead;

  while (true) {
    if(isValid(*curr) && isAccessed(*curr)){
      unmarkAccessed(*curr);
      prev = curr;
      curr = getNext(*curr);
    }
    else{
      head_ = getNext(*curr);
      if (l.owns_lock()) {
        l.unlock();
      }
      if(prev){
        setNext(*prev, nullptr);
        linkAtTail(*oldHead, *prev);        
      }
      // printf("Evict1, size=%d\n", size_.load());
      setNext(*curr, nullptr);
      size_--;
      return curr;
    }
  }
}

template <typename T, CClockListHook<T> T::*HookPtr>
T* CClockList<T, HookPtr>::getEvictionCandidate2() noexcept {
  T* prevHead, *prev, *curr = nullptr;

  while (true) {
    if(curr == nullptr){
      curr = prevHead = head_.load();
      prev = nullptr;
    }

    if(isValid(*curr) && isAccessed(*curr)){
      prev = curr;
      curr = getNext(*curr); 
      continue;     
    }

    bool updateHead = head_.compare_exchange_weak(prevHead, getNext(*curr));
    if(updateHead){
      if(prev){
        unmarkAccessedBatch(prevHead, prev); // Set the state of the nodes between prevHead and curr to unAccessed.
        linkAtTail(*prevHead, *prev);
      }
      size_--;
      return curr;
    }

    curr = prevHead;
    prev = nullptr;
  }
}

template <typename T, CClockListHook<T> T::*HookPtr>
T* CClockList<T, HookPtr>::getEvictionCandidate3() noexcept {
  T *curr = head_.load();
  T *prev = nullptr;
  std::list<T*> historyHeads;
  historyHeads.push_back(curr);

  while (true) {
    // printf("Evict2.0, curr:%x\n", curr);

    if(curr == nullptr || getNext(*curr) == nullptr){
      curr = head_.load();
      historyHeads.clear();
      historyHeads.push_back(curr);
      prev = nullptr;
      continue;
    }

    // If the next node is nullptr, wait other thread inserts nodes at the tail.

    // printf("Evict2.1, curr:%x, next:%x\n", curr, getNext(*curr));

    uint32_t currState = getState(*curr);
    uint32_t desiredState = CC_UNMARK;

    bool markable = (currState == CC_UNMARK);

    if(markable){
      if(isValid(*curr) && isAccessed(*curr)){
        desiredState = CC_MOVABLE;
      }
      else{
        desiredState = CC_EVICTABLE;
      }
      markable = markState(*curr, &currState, desiredState);
    }

    // If the current head is not in history heads, it means the head has been changed by other threads.
    if(markable && std::find(historyHeads.begin(), historyHeads.end(), head_.load()) == historyHeads.end()){
      clearState(*curr);
      // printf("Evict2.2, curr%x\n", curr);
      curr = head_.load();
      historyHeads.clear();
      historyHeads.push_back(curr);
      prev = nullptr;
      continue;
    }

    if(!markable && currState == CC_EVICTABLE){
      historyHeads.push_back(getNext(*curr));
      if(historyHeads.size() > 64){
        historyHeads.pop_front();
      }
    }

    // printf("Evict2.3, desire:%d, success?:%d, curr:%x\n", desiredState, markable, curr);

    // If current node is movable, decrease the accessed time.
    if(markable && (desiredState == CC_MOVABLE)){
      // printf("Evict2.4, curr:%x\n", curr);
      unmarkAccessed(*curr);     
    }

    // If current node is evictable, try to evict it.
    if(markable && (desiredState == CC_EVICTABLE)){
      // Fetch the previous head from the historyHeads.
      T* prevHead = historyHeads.back();

      // Blocking until the head_ is the same as the previous head.
      // printf("Evict2.5, curr:%x\n", curr);
      while(head_.load() != prevHead){}

      // Set the head_ to the next node of the current node.
      // printf("prevHead:%x\n", prevHead);

      head_.store(getNext(*curr));

      clearStates(prevHead, curr);

      if(prevHead != curr){
        linkAtTail(*prevHead, *prev);
      }
      // setNext(*curr, nullptr);
      // printf("newHead:%x, state:%d\n", head_.load(), getState(*head_.load()));
      // printf("Evict2.6, curr:%x\n", curr);
      size_--;
      return curr;
    }
    prev = curr;
    curr = getNext(*curr); 
  }
}

template <typename T, CClockListHook<T> T::*HookPtr>
T* CClockList<T, HookPtr>::getEvictionCandidate4() noexcept {
  if (size_.load() == 0)
    return nullptr;

  T *oldHead, *newHead, *newTail = nullptr;
  T *curr, *next, *ret = nullptr;

  size_t spanSize = 4;

  while(true){
    do{
      newHead = oldHead = head_.load();
      for(int i = 0; i < spanSize; i++){
        if(getNext(*newHead) == nullptr){
          if(oldHead == head_.load()){
            // printf("Evict3.0, oldHead:%x\n", oldHead);
            i--;
          }
          else{
            // printf("Evict3.1, oldHead:%x\n", oldHead);
            break;
          }
        }
        else
        {
          newTail = newHead;
          newHead = getNext(*newHead);
          // printf("Evict3.3, oldHead:%x, newHead:%x\n", oldHead, newHead);
        }
      }
    }while(!head_.compare_exchange_weak(oldHead, newHead));

    // printf("Evict3.3, newHead:%x\n", newHead);

    curr = oldHead;
    while(curr != newHead){
      next = getNext(*curr);
      if(isValid(*curr) && isAccessed(*curr)){
        unmarkAccessed(*curr);
        linkAtTail(*curr, *curr); 
        printf("Evict3.4, move, curr:%x\n", curr);
        curr = next;
      }
      else{
        if(curr != newTail)
          linkAtTail(*next, *newTail);
        size_--;
        return curr;
      }
    }
  }
}

/* Iterator Implementation */
template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::Iterator::goForward() noexcept {
    curr_ = CClockList_->getNext(*curr_);
}

template <typename T, CClockListHook<T> T::*HookPtr>
void CClockList<T, HookPtr>::Iterator::goBackward() {
   throw std::logic_error("Not implemented"); 
}

template <typename T, CClockListHook<T> T::*HookPtr>
typename CClockList<T, HookPtr>::Iterator&
CClockList<T, HookPtr>::Iterator::operator++() noexcept { 
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, CClockListHook<T> T::*HookPtr>
typename CClockList<T, HookPtr>::Iterator&
CClockList<T, HookPtr>::Iterator::operator--() {
   throw std::logic_error("Not implemented"); 
}

template <typename T, CClockListHook<T> T::*HookPtr>
typename CClockList<T, HookPtr>::Iterator
CClockList<T, HookPtr>::begin() const noexcept {
  return CClockList<T, HookPtr>::Iterator(
      head_, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, CClockListHook<T> T::*HookPtr>
typename CClockList<T, HookPtr>::Iterator
CClockList<T, HookPtr>::rbegin() const {
  throw std::logic_error("Not implemented"); 
}

template <typename T, CClockListHook<T> T::*HookPtr>
typename CClockList<T, HookPtr>::Iterator
CClockList<T, HookPtr>::end() const noexcept {
  return CClockList<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, CClockListHook<T> T::*HookPtr>
typename CClockList<T, HookPtr>::Iterator
CClockList<T, HookPtr>::rend() const {
   throw std::logic_error("Not implemented"); 
}

}  // namespace cachelib
}  // namespace facebook
