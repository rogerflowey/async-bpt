#include <cassert>
#include <iostream>

using key_t = int;
using size_t = unsigned long;
constexpr size_t size = 5;
key_t data[size] = {2, 3, 4, 5, 8};

static size_t lower_bound(const key_t &key) {
  size_t left = 0, right = size - 1;
  while (left < right) {
    const size_t mid = (left + right + 1) / 2;
    if (data[mid] > key)
      right = mid - 1;
    else
      left = mid;
  }
  return left;
}

int main() {
  assert(lower_bound(0) == 0);
  assert(lower_bound(1) == 0);
  assert(lower_bound(2) == 0);
  assert(lower_bound(3) == 1);
  assert(lower_bound(4) == 2);
  assert(lower_bound(5) == 3);
  assert(lower_bound(6) == 3);
  assert(lower_bound(8) == 4);
  assert(lower_bound(10) == 4);
  std::cout << "You passed the unit test!" << std::endl;
  return 0;
}