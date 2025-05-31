#include "test_utils.hpp"

// Define USE_SMALL_BATCH if you want to test threshold-based flushes with fewer ops
// and observe splits/merges with smaller node capacities.
// #define USE_SMALL_BATCH

// Global instances for this test file
BTreeTest btree_global_flush;
RefModelTest ref_model_global_flush;

// Helper to print a separator for clarity
void print_flush_separator(const std::string& flush_id) {
    std::cout << "\n--- FLUSH " << flush_id << " ---" << std::endl;
}
void print_test_step(const std::string& step_description) {
    std::cout << "\n>>> Test Step: " << step_description << " <<<" << std::endl;
}
void print_capacities() {
    std::cout << "  INFO: Leaf Capacity: " << BTreeTest::LeafNode::node_capacity
              << ", Leaf Split Threshold: " << BTreeTest::LeafNode::split_threshold
              << ", Leaf Merge Threshold: " << BTreeTest::LeafNode::merge_threshold << std::endl;
    std::cout << "  INFO: Index Capacity (keys): " << BTreeTest::IndexNode::node_capacity
              << ", Index Split Threshold (keys): " << BTreeTest::IndexNode::split_threshold
              << ", Index Merge Threshold (keys): " << BTreeTest::IndexNode::merge_threshold << std::endl;
}

// --- Test Cases ---

void test_Flush_BuildUp_SingleLeaf_To_RootSplit_And_More() {
    std::cout << "\n--- Running test_Flush_BuildUp_SingleLeaf_To_RootSplit_And_More ---" << std::endl;
    setup_test_environment(btree_global_flush, ref_model_global_flush);
    print_capacities();

    const size_t leaf_cap = BTreeTest::LeafNode::node_capacity;
    const size_t leaf_split_thresh = BTreeTest::LeafNode::split_threshold;

    auto task = [&]() -> wutong::Task<void> {
        // Step 1: Insert one item, flush.
        print_test_step("1.1: Insert first item (10,100)");
        co_await apply_insert_op(btree_global_flush, ref_model_global_flush, 10, 100);
        print_flush_separator("1.A");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);
        co_await verify_all_query(btree_global_flush, ref_model_global_flush, 10, "Flush 1.A");
        ASSERT_TRUE(btree_global_flush.tree_height_.val == 1, "Height should be 1 (single leaf node)");

        // Step 2: Insert items up to (but not exceeding) leaf split threshold.
        print_test_step("1.2: Insert items up to leaf split threshold");
        // Assuming split_threshold is N, we insert N items total.
        // If leaf_split_thresh is 0, this loop won't run, which is fine.
        for (size_t i = 1; i < leaf_split_thresh; ++i) { // Already have 1 item
            if (ref_model_global_flush.size() >= leaf_split_thresh) break;
            co_await apply_insert_op(btree_global_flush, ref_model_global_flush, 10 + i * 2, 100 + i * 20);
        }
        print_flush_separator("1.B");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);
        co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, 100, BTreeTest::val_max, "Flush 1.B (leaf near full)");
        ASSERT_TRUE(btree_global_flush.tree_height_.val == 1, "Height still 1 (leaf not split yet)");

        // Step 3: Insert one more item to trigger leaf split (and root split from leaf to index).
        print_test_step("1.3: Insert item to trigger leaf split / root split (leaf->index)");
        idx_t_test key_to_split = 10 + leaf_split_thresh * 2; // A key that should cause split
        val_t_test val_to_split = 100 + leaf_split_thresh * 20;
        std::cout << "  Inserting (" << key_to_split << "," << val_to_split << ") to cause split. Current size: " << ref_model_global_flush.size() << std::endl;
        co_await apply_insert_op(btree_global_flush, ref_model_global_flush, key_to_split, val_to_split);

        print_flush_separator("1.C");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);
        co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, key_to_split + 10, BTreeTest::val_max, "Flush 1.C (first split)");
        ASSERT_TRUE(btree_global_flush.tree_height_.val == 2, "Height should be 2 (root is index, 2 leaf children)");

        // Step 4: Insert more items to potentially fill one of the new leaves and cause another leaf split.
        // This might or might not change the root index node's keys, depending on where items go.
        print_test_step("1.4: Insert more items to cause another leaf split (under new root)");
        // Insert leaf_cap + 1 items into a new key range to ensure a split in a "fresh" area
        idx_t_test next_key_base = 500;
        for (size_t i = 0; i <= leaf_cap; ++i) { // leaf_cap + 1 items
            co_await apply_insert_op(btree_global_flush, ref_model_global_flush, next_key_base + i * 3, next_key_base + i * 30);
        }
        print_flush_separator("1.D");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);
        co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, next_key_base + leaf_cap * 3 + 10, BTreeTest::val_max, "Flush 1.D (another leaf split)");
        ASSERT_TRUE(btree_global_flush.tree_height_.val == 2, "Height likely still 2 (root index may have more keys/children)");
        // To verify root index node structure, one would need to parse traverse_async output or have access to root node's size.
    }();
    run_task_sync(std::move(task), btree_global_flush);
    teardown_test_environment();
    std::cout << "--- test_Flush_BuildUp_SingleLeaf_To_RootSplit_And_More PASSED ---" << std::endl;
}


void test_Flush_TearDown_Merge_And_RootUnderflow() {
    std::cout << "\n--- Running test_Flush_TearDown_Merge_And_RootUnderflow ---" << std::endl;
    setup_test_environment(btree_global_flush, ref_model_global_flush, true, false); // Don't init empty, we'll use initialize_from_vector
    print_capacities();

    const size_t leaf_merge_thresh = BTreeTest::LeafNode::merge_threshold;

    auto task = [&]() -> wutong::Task<void> {
        // Step 1: Initialize a tree with a root index node and a few leaf children.
        // Aim for 3-4 leaves to have enough room for merges without immediate root collapse.
        // Each leaf should have slightly more than merge_threshold items.
        print_test_step("2.1: Initialize a 2-level tree with multiple leaves");
        std::vector<storage_pair_test_t> initial_data;
        int num_leaves_to_create = 3; // Target 3 leaves
        int items_per_leaf_init = leaf_merge_thresh + 2; // A bit above merge threshold
        if (items_per_leaf_init < 2) items_per_leaf_init = 2; // Min 2 items
        if (items_per_leaf_init > (int)BTreeTest::LeafNode::node_capacity) items_per_leaf_init = BTreeTest::LeafNode::node_capacity;


        for (int l = 0; l < num_leaves_to_create; ++l) {
            for (int i = 0; i < items_per_leaf_init; ++i) {
                idx_t_test key = 1000 * l + i * 10;
                val_t_test val = key + 5000;
                initial_data.push_back(norb::make_pair(key, val));
            }
        }
        std::sort(initial_data.begin(), initial_data.end());
        co_await btree_global_flush.initialize_from_vector_async(to_sjtu_vector(initial_data));
        for(const auto& p : initial_data) ref_model_global_flush.insert(p);

        std::cout << "  Initialized with " << ref_model_global_flush.size() << " items." << std::endl;
        co_await btree_global_flush.traverse_async(true); // Initial state after bulk load
        ASSERT_TRUE(btree_global_flush.tree_height_.val == 2 || (initial_data.size() <= BTreeTest::LeafNode::node_capacity && btree_global_flush.tree_height_.val == 1),
                    "Initial height should be 2 (or 1 if few items)");


        // Step 2: Delete items from one leaf to cause it to underflow and merge with a sibling.
        print_test_step("2.2: Delete items from first conceptual leaf to trigger merge");
        // Delete items from keys 0-based range
        int items_to_delete_from_first_leaf = items_per_leaf_init - leaf_merge_thresh + 1; // Delete enough to go below threshold
        if (items_to_delete_from_first_leaf <= 0) items_to_delete_from_first_leaf = 1;

        std::cout << "  Attempting to delete " << items_to_delete_from_first_leaf << " items from the 'first' leaf." << std::endl;
        for (int i = 0; i < items_to_delete_from_first_leaf; ++i) {
            idx_t_test key_to_delete = i * 10; // Assumes keys 0, 10, 20... were in the first leaf
            // Find the actual value from ref_model to ensure correct deletion if values differ
            auto it_ref = ref_model_global_flush.lower_bound(norb::make_pair(key_to_delete, BTreeTest::val_min));
            if (it_ref != ref_model_global_flush.end() && it_ref->first == key_to_delete) {
                 co_await apply_remove_op(btree_global_flush, ref_model_global_flush, it_ref->first, it_ref->second);
            } else {
                std::cout << "  Skipping delete for key " << key_to_delete << " as it's not in ref_model (or already deleted)." << std::endl;
            }
        }
        print_flush_separator("2.A (After Deletes for Merge)");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);
        co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, 1000 * num_leaves_to_create, BTreeTest::val_max, "Flush 2.A (leaf merge)");
        // Height might still be 2, but one less child under root, or fewer keys in root.

        // Step 3: Delete more items to cause further merges, potentially leading to root underflow (index -> leaf).
        print_test_step("2.3: Delete more items to trigger root underflow (index -> leaf)");
        // Delete all items from the "second" conceptual leaf
        std::cout << "  Attempting to delete all items from the 'second' leaf (keys around 1000s)." << std::endl;
        std::vector<storage_pair_test_t> items_in_second_leaf_range;
        for(const auto& p : ref_model_global_flush) {
            if (p.first >= 1000 && p.first < 2000) { // Heuristic for second leaf
                items_in_second_leaf_range.push_back(p);
            }
        }
        for(const auto& p_del : items_in_second_leaf_range) {
            co_await apply_remove_op(btree_global_flush, ref_model_global_flush, p_del.first, p_del.second);
        }

        print_flush_separator("2.B (After Deletes for Root Underflow)");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);
        co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, 1000 * num_leaves_to_create, BTreeTest::val_max, "Flush 2.B (root underflow index->leaf attempt)");

        // If enough items were deleted and merges propagated, height might become 1.
        if (!ref_model_global_flush.empty()) { // Only assert height change if items remain
             ASSERT_TRUE(btree_global_flush.tree_height_.val == 1 || btree_global_flush.tree_height_.val == 2,
                        "Height should be 1 (or 2 if merges didn't fully collapse root yet). Actual: " + std::to_string(btree_global_flush.tree_height_.val));
        }


        // Step 4: Delete all remaining items to make the tree empty.
        print_test_step("2.4: Delete all remaining items (root underflow leaf -> empty)");
        std::cout << "  Deleting all remaining " << ref_model_global_flush.size() << " items." << std::endl;
        std::vector<storage_pair_test_t> items_to_delete_all(ref_model_global_flush.begin(), ref_model_global_flush.end());
        for (const auto& pair_to_delete : items_to_delete_all) {
            co_await apply_remove_op(btree_global_flush, ref_model_global_flush, pair_to_delete.first, pair_to_delete.second);
        }

        print_flush_separator("2.C (All Deleted)");
        co_await btree_global_flush.flush();
        co_await btree_global_flush.traverse_async(true);

        if (!ref_model_global_flush.empty() || !(co_await is_bpt_empty_after_flush(btree_global_flush))) {
            std::cout << "  Ref model not empty or BPT not empty. Performing additional flush for final collapse." << std::endl;
            print_flush_separator("2.D (Final Collapse Flush)");
            co_await btree_global_flush.flush();
            co_await btree_global_flush.traverse_async(true);
        }

        co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, 10000, BTreeTest::val_max, "Flush 2.D (all items deleted)");
        ASSERT_TRUE(ref_model_global_flush.empty(), "Reference model should be empty.");
        ASSERT_TRUE(co_await is_bpt_empty_after_flush(btree_global_flush), "B+Tree on disk should be empty.");
        ASSERT_TRUE(btree_global_flush.tree_height_.val == 0, "Height should be 0 (empty tree).");

    }();
    run_task_sync(std::move(task), btree_global_flush);
    teardown_test_environment();
    std::cout << "--- test_Flush_TearDown_Merge_And_RootUnderflow PASSED ---" << std::endl;
}


void test_Flush_Alternating_InsertDelete() {
    std::cout << "\n--- Running test_Flush_Alternating_InsertDelete ---" << std::endl;
    setup_test_environment(btree_global_flush, ref_model_global_flush);
    print_capacities();

    const int operations_per_flush_cycle = BTreeTest::LeafNode::node_capacity / 2 + 1;
    const int num_cycles = 5;

    auto task = [&]() -> wutong::Task<void> {
        idx_t_test key_counter = 0;
        std::vector<storage_pair_test_t> inserted_in_cycle;

        for (int cycle = 0; cycle < num_cycles; ++cycle) {
            print_test_step("Cycle " + std::to_string(cycle + 1) + ": Inserts");
            inserted_in_cycle.clear();
            for (int i = 0; i < operations_per_flush_cycle; ++i) {
                idx_t_test current_key = key_counter++;
                val_t_test current_val = current_key * 10;
                co_await apply_insert_op(btree_global_flush, ref_model_global_flush, current_key, current_val);
                inserted_in_cycle.push_back(norb::make_pair(current_key, current_val));
            }
            print_flush_separator("Cycle " + std::to_string(cycle + 1) + ".A (After Inserts)");
            co_await btree_global_flush.flush();
            co_await btree_global_flush.traverse_async(true);
            co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, key_counter + 10, BTreeTest::val_max, "Cycle " + std::to_string(cycle+1) + ".A");

            if (cycle % 2 == 0 && !inserted_in_cycle.empty()) { // Delete some items on even cycles
                print_test_step("Cycle " + std::to_string(cycle + 1) + ": Deletes");
                size_t items_to_delete_this_cycle = inserted_in_cycle.size() / 2;
                if (items_to_delete_this_cycle == 0 && !inserted_in_cycle.empty()) items_to_delete_this_cycle = 1;

                for (size_t i = 0; i < items_to_delete_this_cycle; ++i) {
                    // Delete the first few items inserted in this cycle
                    storage_pair_test_t to_delete = inserted_in_cycle[i];
                    co_await apply_remove_op(btree_global_flush, ref_model_global_flush, to_delete.first, to_delete.second);
                }
                print_flush_separator("Cycle " + std::to_string(cycle + 1) + ".B (After Deletes)");
                co_await btree_global_flush.flush();
                co_await btree_global_flush.traverse_async(true);
                co_await verify_range_query(btree_global_flush, ref_model_global_flush, 0, BTreeTest::val_min, key_counter + 10, BTreeTest::val_max, "Cycle " + std::to_string(cycle+1) + ".B");
            }
        }
        std::cout << "  Final ref_model size: " << ref_model_global_flush.size() << std::endl;
    }();
    run_task_sync(std::move(task), btree_global_flush);
    teardown_test_environment();
    std::cout << "--- test_Flush_Alternating_InsertDelete PASSED ---" << std::endl;
}


void run_flush_correctness_tests() {
    std::cout << "\n========== Running Enhanced Flush Correctness Tests ==========" << std::endl;
    test_Flush_BuildUp_SingleLeaf_To_RootSplit_And_More();
    test_Flush_TearDown_Merge_And_RootUnderflow();
    test_Flush_Alternating_InsertDelete();
    // The test_Flush_IndexNodeSplit_Scenario from previous version can be added back if refined.
    // It's harder to make it robustly trigger non-root index splits without very specific knowledge
    // of capacities and B+tree split logic (e.g. which child gets more items on split).
}

int main() {
    std::cout << "=============== Starting Enhanced Flush Correctness B+Tree Tests ===============" << std::endl;
    run_flush_correctness_tests();
    std::cout << "\n*********************************" << std::endl;
    std::cout << "*** Enhanced Flush Correctness B+Tree Tests COMPLETED SUCCESSFULLY ***" << std::endl;
    std::cout << "*********************************" << std::endl;
    return 0;
}