#include "b_plus_tree.hpp"
#include <cassert>

constexpr char PHASE = 'a';
using norb::array::equals;
using bpt_type = norb::BPlusTree<char, int>;
using norb::PersistentMemory;

void phase_a(bpt_type &bpt) {
  assert(bpt.size() == 0);
  bpt.insert('b', 3);
  bpt.insert('a', 4);
  bpt.insert('a', 2);
  bpt.insert('a', 3);
  bpt.insert('a', 1);
  const auto handle = PersistentMemory::fetch_handle<bpt_type::LeafNode>(0);
  bpt.insert('a', 5);
  bpt.insert('b', 1);
  bpt.insert('b', 4);
  bpt.insert('b', 2);
  bpt.insert('b', 5);
  bpt.insert('c', 1);
  bpt.insert('c', 3);
  bpt.insert('c', 4);
  bpt.insert('c', 2);
  bpt.insert('d', 10);
  bpt.insert('c', 5);
  bpt.traverse(true);
  assert(bpt.size() == 3 * 5 + 1);
  assert(equals(bpt.find_all('a'), {1, 2, 3, 4, 5}));
  assert(equals(bpt.find_all('b'), {1, 2, 3, 4, 5}));
  assert(equals(bpt.find_all('c'), {1, 2, 3, 4, 5}));
  assert(equals(bpt.find_all('d'), {10}));
  bpt.insert('d', 5);
  bpt.insert('d', 1);
  bpt.insert('d', 4);
  bpt.insert('d', 2);
  // bpt.insert('d', 3);
  // bpt.insert('e', 3);
  // bpt.insert('e', 2);

  bpt.traverse();
  assert(equals(bpt.find_all('a'), {1, 2, 3, 4, 5}));
  assert(equals(bpt.find_all('b'), {1, 2, 3, 4, 5}));
  assert(equals(bpt.find_all('c'), {1, 2, 3, 4, 5}));
  std::cout << bpt.find_all('d') << '\n';
  assert(equals(bpt.find_all('d'), {1, 2, 4, 5, 10}));

  // test remove
  assert(bpt.remove('d', 0) == false);
  assert(bpt.remove('d', 1) == true);
  bpt.traverse(true);
  assert(bpt.remove('d', 2) == true);
  bpt.traverse(true);
  assert(bpt.remove('a', 1) == true);
  bpt.traverse();

  bpt.insert('a', 0);
  bpt.traverse();
  bpt.insert('a', -1);
  bpt.traverse();
  bpt.remove('b', 1);
  bpt.traverse(true);
}

void phase_b(bpt_type &bpt) {
  bpt.traverse(true);
  // assert(bpt.size() == 3 * 5 + 1);
  assert(equals(bpt.find_all('a'), {1, 2, 3, 4, 5}));
}

int main() {
  if constexpr (PHASE == 'a') {
    norb::chore::remove_associated();
    bpt_type bpt;
    phase_a(bpt);
    std::cout << "Congratulations, you passed Phase A!" << std::endl;
  } else if constexpr (PHASE == 'b') {
    bpt_type bpt;
    phase_b(bpt);
    std::cout << "Congratulations, you passed Phase B!" << std::endl;
  }
  return 0;
}