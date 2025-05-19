#include "b_plus_tree.hpp"
#include "async_bpt.h"

const auto hash_method = norb::hash::fnv1a_hash;
using norb::hash::hashed_t_;

int main() {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::cout.tie(nullptr);

  norb::BPlusTree<hashed_t_, int> tree;

  int n;
  std::cin >> n;
  for (int i = 0; i < n; i++) {
    std::string mode, index;
    int value;
    std::cin >> mode >> index;
    const auto hashed_index = hash_method(index);
    if (mode == "insert") {
      std::cin >> value;
      tree.insert(hashed_index, value);
    } else if (mode == "delete") {
      std::cin >> value;
      tree.remove(hashed_index, value);
    } else {
      bool found_any = false;
      tree.find_all_do(hashed_index, [&found_any](const int &x) {
        std::cout << x << ' ';
        found_any = true;
      });
      if (!found_any) {
        std::cout << "null";
      }
      std::cout << '\n';
    }
  }
  return 0;
}