#include "test_utils.hpp"

// These tests use local BTree instances to simulate program restarts.
// They will use the PMA_FILE_NAME and FILED_CONFIG_FILE_NAME from test_utils.hpp

void test_Persistence_Simple() {
    std::cout << "\n--- Running test_Persistence_Simple ---" << std::endl;
    RefModelTest ref_model_session; 

    // Session 1: Create, populate, flush, shutdown
    {
        BTreeTest btree_s1; 
        // Clean files for this session, btree starts empty
        // Pass local ref_model_session
        setup_test_environment(btree_s1, ref_model_session, true, true); 

        auto populate_task = [&]() -> wutong::Task<void> {
            co_await apply_insert_op(btree_s1, ref_model_session, 10, 100);
            co_await apply_insert_op(btree_s1, ref_model_session, 20, 200);
            co_await apply_insert_op(btree_s1, ref_model_session, 5, 50);
        }();
        run_task_sync(std::move(populate_task), btree_s1);

        run_task_sync(btree_s1.flush(), btree_s1);
        run_task_sync(btree_s1.shutdown(), btree_s1);
    }

    // Session 2: Simulate restart
    {
        BTreeTest btree_s2; // New instance, should load from files
        // IMPORTANT: For persistence tests, ensure singletons (PMA, FiledConfig)
        // are truly re-initialized or load from file if a new BTreeTest is created.
        // This might require explicit reset calls for your singletons if they cache aggressively.
        // e.g., norb::PersistentMemoryAsync::reset_instance_for_testing();
        //      norb::FiledConfig::reset_instance_for_testing();
        // before creating btree_s2. For now, assuming constructor handles it.

        std::cout << "  BTree S2 height after load: " << btree_s2.tree_height_.val << std::endl;
        ASSERT_TRUE(btree_s2.tree_height_.val > 0, "Tree height should be > 0 after loading persisted state");

        auto verify_task = [&]() -> wutong::Task<void> {
            // Use the same ref_model_session from S1 for verification
            co_await verify_all_query(btree_s2, ref_model_session, 10, "Persistence S2 Key 10");
            co_await verify_all_query(btree_s2, ref_model_session, 20, "Persistence S2 Key 20");
            co_await verify_all_query(btree_s2, ref_model_session, 5, "Persistence S2 Key 5");
            co_await verify_all_query(btree_s2, ref_model_session, 100, "Persistence S2 Key 100 (Non-existent)");
        }();
        run_task_sync(std::move(verify_task), btree_s2);
        run_task_sync(btree_s2.shutdown(), btree_s2); // Shutdown S2 as well
    }

    teardown_test_environment(true); 
    std::cout << "--- test_Persistence_Simple PASSED ---" << std::endl;
}

void test_Persistence_AfterLeafSplit() {
    std::cout << "\n--- Running test_Persistence_AfterLeafSplit ---" << std::endl;
    RefModelTest ref_model_session;

    int items_to_cause_split = norb::AsyncBPlusTree<idx_t_test, val_t_test>::LeafNode::node_capacity + 2;
    if (items_to_cause_split < 6) items_to_cause_split = 6;

    // Session 1
    {
        BTreeTest btree_s1;
        setup_test_environment(btree_s1, ref_model_session, true, true);

        auto populate_task = [&]() -> wutong::Task<void> {
            for (int i = 0; i < items_to_cause_split; ++i) {
                co_await apply_insert_op(btree_s1, ref_model_session, i, i * 10);
            }
        }();
        run_task_sync(std::move(populate_task), btree_s1);
        run_task_sync(btree_s1.flush(), btree_s1);
        run_task_sync(btree_s1.shutdown(), btree_s1);
    }

    // Session 2
    {
        BTreeTest btree_s2;
        // Again, ensure PMA/FiledConfig singletons behave correctly for reloading.
        std::cout << "  BTree S2 height after split and load: " << btree_s2.tree_height_.val << std::endl;
        ASSERT_TRUE(btree_s2.tree_height_.val >= 1, "Tree height should be >= 1 after loading persisted split state");
        
        auto verify_task = [&]() -> wutong::Task<void> {
            co_await verify_range_query(btree_s2, ref_model_session, 0, BTreeTest::val_min, items_to_cause_split, BTreeTest::val_max, "Persistence S2 After Split");
        }();
        run_task_sync(std::move(verify_task), btree_s2);
        run_task_sync(btree_s2.shutdown(), btree_s2);
    }
    teardown_test_environment(true);
    std::cout << "--- test_Persistence_AfterLeafSplit PASSED ---" << std::endl;
}


void run_persistence_tests() {
    std::cout << "\n========== Running Persistence Tests ==========" << std::endl;
    test_Persistence_Simple();
    test_Persistence_AfterLeafSplit();
}

int main() {
    std::cout << "=============== Starting Persistence B+Tree Tests ===============" << std::endl;
    run_persistence_tests();
    std::cout << "\n*********************************" << std::endl;
    std::cout << "*** Persistence B+Tree Tests COMPLETED SUCCESSFULLY ***" << std::endl;
    std::cout << "*********************************" << std::endl;
    return 0;
}