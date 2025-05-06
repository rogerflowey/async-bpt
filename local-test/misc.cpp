#include <cassert>
#include <iostream>

using key_t = int;
using size_t = unsigned long;
constexpr size_t size = 5;
key_t data[size] = {2,3,3,5,6};

static size_t lower_bound(const key_t &key) {
  size_t left = 0, right = size;
  while (left < right) {
    const size_t mid = (left + right) / 2;
    if (data[mid] >= key)
      right = mid;
    else
      left = mid + 1;
  }
  return (left > 1) ? (left - 1) : 0;
}

int main() {
  assert(lower_bound(0) == 0);
  assert(lower_bound(1) == 0);
  assert(lower_bound(2) == 0);
  assert(lower_bound(3) == 0);
  assert(lower_bound(4) == 2);
  assert(lower_bound(5) == 2);
  assert(lower_bound(6) == 3);
  assert(lower_bound(10) == 4);
  std::cout << "You passed the unit test!" << std::endl;
  return 0;
}