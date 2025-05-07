#include "b_plus_tree.hpp"
#include <cassert>

using norb::array::equals;

int main() {
  norb::BPlusTree<char, int> bpt;
  bpt.insert('a', 1);
  bpt.insert('b', 7);
  bpt.insert('a', 2);
  bpt.traverse();
  assert(bpt.remove('a', 1) == true);
  bpt.traverse();
  assert(equals(bpt.find_all('a'), {2}));
  bpt.insert('a', 8);
  bpt.traverse();
  bpt.insert('a', 12);
  bpt.traverse();
  bpt.insert('b', 3);
  bpt.traverse();
  assert(equals(bpt.find_all('a'), {2, 8, 12}));
  bpt.remove('b', 6);
  bpt.traverse();
  assert(bpt.size() == 5);
  assert(equals(bpt.find_all('a'), {2, 8, 12}));
  assert(equals(bpt.find_all('b'), {3, 7}));
  std::cout << "Congratulations, you passed this unit test!" << std::endl;
  return 0;
}