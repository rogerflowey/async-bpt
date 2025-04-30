#include "stlite/looped_queue.hpp"

#include <cassert>

int main() {
  norb::LoopedQueue<int, 3> lq;
  assert(lq.back() == 0);
  lq.insert(1); // 1 * *
  assert(lq.back() == 1);
  lq.insert(2); // 1 2 *
  assert(lq.back() == 1);
  lq.insert(3); // 1 2 3
  assert(lq.back() == 1);
  lq.insert(4); // 2 3 4
  assert(lq.back() == 2);
  lq.insert(5); // 3 4 5
  assert(lq.back() == 3);
  return 0;
}