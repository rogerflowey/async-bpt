#include "persistent_memory.hpp"

#include <iostream>
#include <vector>

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

  const auto root = PersistentMemory::create_and_init<node>(0);
  const auto node1 = root.ref()->next = PersistentMemory::create_and_init<node>(1);
  const auto node2 = node1.ref()->next = PersistentMemory::create_and_init<node>(2);
  const auto node3 = node2.ref()->next = PersistentMemory::create_and_init<node>(3);
  std::cout << "root page-id = " << root.page_id << '\n';
  auto handle = PersistentMemory::fetch_handle<node>(root.page_id);

  int i = 0;
  while (!handle.is_nullptr() && i++ < 6) {
    std::cout << handle.const_ref()->id << std::endl;
    handle = handle.const_ref()->next;
  }
}
using norb::PersistentMemory;
using norb::page_id_t;

// Helper to print status (optional, good for debugging)
void print_status(const std::string& msg) {
    std::cout << "[STATUS] " << msg << std::endl;
}

void test_lru_k_and_remove() {
    print_status("Starting LRU-K (K=2, Slots=3) and Remove Test...");
    assert(norb::MEMORY_SIZE / norb::PAGE_SIZE == 3);
    assert(norb::LRU_K_INDEX == 2);

    // Keep track of handles
    std::vector<PersistentMemory::Handle<int>> handles;

    // --- Phase 1: Fill the buffer ---
    print_status("Phase 1: Filling the buffer (3 slots)");
    // Create p0, p1, p2. Buffer should be full.
    // Access history (K=2): p0={ts1}, p1={ts2}, p2={ts3} -> K-th timestamp is infinity for all
    handles.push_back(PersistentMemory::create_and_init<int>(0)); // page_id 0 -> slot 0
    handles.push_back(PersistentMemory::create_and_init<int>(1)); // page_id 1 -> slot 1
    handles.push_back(PersistentMemory::create_and_init<int>(2)); // page_id 2 -> slot 2

    assert(*handles[0].const_ref() == 0);
    assert(*handles[1].const_ref() == 1);
    assert(*handles[2].const_ref() == 2);
    print_status("Buffer filled. p0, p1, p2 loaded.");

    // --- Phase 2: Trigger first eviction ---
    print_status("Phase 2: Triggering first eviction");
    // Create p3. Buffer is full. Need to evict.
    // All p0, p1, p2 have K-th timestamp = infinity (accessed only once).
    // Eviction policy for K-th=inf tie: usually standard LRU (oldest overall timestamp).
    // Expected eviction: p0 (page_id 0, accessed at ts1, oldest).
    handles.push_back(PersistentMemory::create_and_init<int>(3)); // page_id 3 -> replaces p0 in slot 0

    assert(*handles[1].const_ref() == 1); // p1 should still be accessible
    assert(*handles[2].const_ref() == 2); // p2 should still be accessible
    assert(*handles[3].const_ref() == 3); // p3 is newly created
    print_status("p3 created. Expected eviction: p0 (page_id 0). Buffer: p3, p1, p2.");

    // Access p0 again - this should load it back, potentially evicting p1 (now oldest K-th=inf)
    print_status("Accessing p0 (expecting reload and eviction of p1)");
    assert(*handles[0].const_ref() == 0); // Access p0, loads page 0 back.
    // Expected eviction: p1 (page_id 1, K-th=inf, oldest timestamp ts2)
    // Buffer should now contain: p3 (slot 0), p0 (slot 1), p2 (slot 2)

    assert(*handles[0].const_ref() == 0); // p0 accessible
    assert(*handles[2].const_ref() == 2); // p2 accessible
    assert(*handles[3].const_ref() == 3); // p3 accessible
    print_status("p0 accessed. Expected eviction: p1 (page_id 1). Buffer: p3, p0, p2.");


    // --- Phase 3: Establish K=2 history and trigger K-based eviction ---
    print_status("Phase 3: Establishing K=2 history");
    // Access pages currently in buffer (p3, p0, p2) twice to give them finite K-th timestamps
    // Access order matters for timestamps.
    *handles[3].ref() = 33; // Access p3 (tsX). History[slot0]={ts4, tsX} -> K-th=ts4
    *handles[0].ref() = 10; // Access p0 (tsY). History[slot1]={ts1, tsZ, tsY} -> K-th=tsZ (tsZ was when p0 was reloaded)
    *handles[2].ref() = 22; // Access p2 (tsW). History[slot2]={ts3, tsW} -> K-th=ts3

    // Access again to ensure K=2 history
    *handles[3].ref() = 333; // Access p3 again. K-th still ts4 (or later if tsX > ts4)
    *handles[0].ref() = 100; // Access p0 again. K-th still tsZ (or later)
    *handles[2].ref() = 222; // Access p2 again. K-th still ts3 (or later)

    assert(*handles[3].const_ref() == 333);
    assert(*handles[0].const_ref() == 100);
    assert(*handles[2].const_ref() == 222);
    print_status("Accessed p3, p0, p2 multiple times.");

    print_status("Triggering K-based eviction");
    // Create p4. Buffer full (p3, p0, p2). Need eviction.
    // Compare K-th timestamps (2nd last access).
    // Assuming timestamps increase: ts3 (p2) < tsZ (p0) < ts4 (p3) -> rough order
    // Expected eviction: p2 (page_id 2) because it has the oldest K-th timestamp (ts3).
    handles.push_back(PersistentMemory::create_and_init<int>(4)); // page_id 4 -> replaces p2 in slot 2

    assert(*handles[3].const_ref() == 333); // p3 should be accessible
    assert(*handles[0].const_ref() == 100); // p0 should be accessible
    assert(*handles[4].const_ref() == 4);   // p4 is newly created
    print_status("p4 created. Expected eviction: p2 (page_id 2). Buffer: p3, p0, p4.");

    // --- Phase 4: Test remove ---
    print_status("Phase 4: Testing remove");
    page_id_t p0_page_id = handles[0].page_id; // Should be 0
    page_id_t p1_page_id = handles[1].page_id; // Should be 1 (evicted)
    page_id_t p3_page_id = handles[3].page_id; // Should be 3 (in buffer)

    PersistentMemory::remove(handles[3]); // Remove p3 (currently in buffer)
    PersistentMemory::remove(handles[1]); // Remove p1 (currently evicted)
    print_status("Removed p3 (page_id " + std::to_string(p3_page_id) + ") and p1 (page_id " + std::to_string(p1_page_id) + ").");
    // Note: Accessing removed handles might still work if the page hasn't been overwritten yet,
    // but the pages should be marked for garbage collection.

    // --- Phase 5: Test recycling ---
    print_status("Phase 5: Testing recycling");
    // Create p5. Garbage collector should have page_ids 3 and 1 available.
    // Assume it recycles the last one added (page_id 1).
    handles.push_back(PersistentMemory::create_and_init<int>(5)); // Should reuse page_id 1
    assert(handles[5].page_id == p1_page_id); // Verify page ID reuse
    // Buffer needs space. Current: p0 (slot 1), p4 (slot 2). Slot 0 might be free or hold evicted p3.
    // Need eviction. Compare K-th timestamps:
    // p0: finite K-th timestamp (tsZ or later)
    // p4: accessed only once. K-th = infinity.
    // Expected eviction: p4 (K-th=inf is older or prioritized).
    // p5 (recycled page 1) loaded into slot 2.
    assert(*handles[5].const_ref() == 5);
    assert(*handles[0].const_ref() == 100); // p0 should still be accessible
    print_status("p5 created, recycled page_id " + std::to_string(handles[5].page_id) + ". Expected eviction: p4. Buffer: p0, p5.");


    // Create p6. Garbage collector should have page_id 3 available.
    handles.push_back(PersistentMemory::create_and_init<int>(6)); // Should reuse page_id 3
    assert(handles[6].page_id == p3_page_id); // Verify page ID reuse
    // Buffer needs space. Current: p0 (slot 1), p5 (slot 2). Slot 0 might be free.
    // Need eviction. Compare K-th timestamps:
    // p0: finite K-th timestamp
    // p5: accessed only once. K-th = infinity.
    // Expected eviction: p5 (K-th=inf).
    // p6 (recycled page 3) loaded into slot 2.
    assert(*handles[6].const_ref() == 6);
    assert(*handles[0].const_ref() == 100); // p0 should still be accessible
    print_status("p6 created, recycled page_id " + std::to_string(handles[6].page_id) + ". Expected eviction: p5. Buffer: p0, p6.");

    // --- Phase 6: Final checks ---
    print_status("Phase 6: Final checks");
    assert(*handles[0].const_ref() == 100); // p0 (page_id 0)
    assert(*handles[6].const_ref() == 6);   // p6 (recycled page_id 3)

    // Accessing p5 (evicted) should load it back, evicting p0 (oldest K-th)
    assert(*handles[5].const_ref() == 5);   // p5 (recycled page_id 1)
    // Expected eviction: p0
    // Buffer: p6, p5
    print_status("Accessed p5. Expected eviction: p0. Buffer: p6, p5.");
    assert(*handles[6].const_ref() == 6);
    assert(*handles[5].const_ref() == 5);

    print_status("LRU-K and Remove Test Completed Successfully!");
}

int main() {
  test_lru_k_and_remove();
  // test_reference();
}