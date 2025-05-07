#include "b_plus_tree.hpp"
#include <cassert>

int main() {
  norb::chore::remove_associated();
  norb::BPlusTree<int, int> bpt;
  bpt.insert(2, 2);
  bpt.traverse();
  bpt.insert(4, 4);
  bpt.traverse();
  bpt.insert(3, 3);
  bpt.traverse();
  bpt.insert(5, 5);
  bpt.traverse();
  bpt.insert(1, 1);
  bpt.traverse();
  bpt.insert(6, 6);
  bpt.traverse();
  assert(bpt.size() == 6);
  assert(norb::array::equals(bpt.find_all(1), {1}));
  assert(bpt.remove(1, 1) == true);
  bpt.traverse();
  assert(norb::array::equals(bpt.find_all(2), {2}));
  assert(norb::array::equals(bpt.find_all(1), {}));
  assert(bpt.size() == 5);
}