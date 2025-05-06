#include "b_plus_tree.hpp"
#include <cassert>

int main() {
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
  bpt.insert(6, 6); // todo: check why the separation point is set as 2 not 1
  bpt.traverse();
  assert(bpt.size() == 6);
  assert(norb::array::equals(bpt.find_all(1), {1}));
}