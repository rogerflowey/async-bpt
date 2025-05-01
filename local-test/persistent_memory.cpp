#include "persistent_memory.hpp"

#include <iostream>

using norb::PersistentMemory;

void test_basic() {
  // auto a = PersistentMemory::create<int>();
  // *a.ref() = 1;
  std::cout << '+' << '\n';
  auto a = PersistentMemory::fetch_handle<int>(0);
  std::cout << PersistentMemory::get_page_count() << std::endl;
  assert(*a.const_ref() == 1);
}

void test_reference() {
  struct node {
    int id;
    PersistentMemory::Handle<node> next;

    explicit node(const int &id = 0) : id(id) {}
  };

  // const auto root = PersistentMemory::create_and_init<node>(0);
  // const auto node1 = root.ref()->next = PersistentMemory::create_and_init<node>(1);
  // const auto node2 = node1.ref()->next = PersistentMemory::create_and_init<node>(2);
  // const auto node3 = node2.ref()->next = PersistentMemory::create_and_init<node>(3);
  // std::cout << "root page-id = " << root.page_id << '\n';
  auto handle = PersistentMemory::fetch_handle<node>(8);

  int i = 0;
  while (!handle.is_nullptr() && i++ < 6) {
    std::cout << handle.const_ref()->id << std::endl;
    handle = handle.const_ref()->next;
  }
}

int main() {
  test_reference();
}