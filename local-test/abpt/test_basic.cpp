#include "test_utils.hpp"

// Global instances for this test file
BTreeTest btree_global_basic; // Renamed to avoid clashes if linked, though intent is separate compilation
RefModelTest ref_model_global_basic;

void test_SimpleRead_EmptyTree() {
    std::cout << "\n--- Running test_SimpleRead_EmptyTree ---" << std::endl;
    setup_test_environment(btree_global_basic, ref_model_global_basic, true, true); 
    auto task = [&]() -> wutong::Task<void> {
        co_await verify_range_query(btree_global_basic, ref_model_global_basic, 0, BTreeTest::val_min, 100, BTreeTest::val_max, "EmptyTree_Range");
        co_await verify_all_query(btree_global_basic, ref_model_global_basic, 10, "EmptyTree_All");
    }();
    run_task_sync(std::move(task), btree_global_basic);
    teardown_test_environment(true); 
    std::cout << "--- test_SimpleRead_EmptyTree PASSED ---" << std::endl;
}

void test_SimpleRead_OnlyWriteMap() {
    std::cout << "\n--- Running test_SimpleRead_OnlyWriteMap ---" << std::endl;
    setup_test_environment(btree_global_basic, ref_model_global_basic);
    auto task = [&]() -> wutong::Task<void> {
        co_await apply_insert_op(btree_global_basic, ref_model_global_basic, 10, 100);
        co_await apply_insert_op(btree_global_basic, ref_model_global_basic, 10, 101);
        co_await apply_insert_op(btree_global_basic, ref_model_global_basic, 20, 200);

        co_await verify_all_query(btree_global_basic, ref_model_global_basic, 10, "WM_Key10");
        co_await verify_all_query(btree_global_basic, ref_model_global_basic, 20, "WM_Key20");
        co_await verify_all_query(btree_global_basic, ref_model_global_basic, 30, "WM_Key30_Empty");
        co_await verify_range_query(btree_global_basic, ref_model_global_basic, 0, BTreeTest::val_min, 100, BTreeTest::val_max, "WM_Range_All_Items");
    }();
    run_task_sync(std::move(task), btree_global_basic);
    teardown_test_environment();
    std::cout << "--- test_SimpleRead_OnlyWriteMap PASSED ---" << std::endl;
}

void test_InitializeWithData() {
    std::cout << "\n--- Running test_InitializeWithData ---" << std::endl;
    setup_test_environment(btree_global_basic, ref_model_global_basic, true, false); 

    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_items = 50; 
        for (int i = 0; i < num_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i, i * 10));
            ref_model_global_basic.insert(norb::make_pair<idx_t_test, val_t_test>(i, i * 10));
        }
        std::sort(initial_data_std.begin(), initial_data_std.end());

        co_await btree_global_basic.initialize_from_vector_async(to_sjtu_vector(initial_data_std));
        
        co_await verify_range_query(btree_global_basic, ref_model_global_basic, 0, BTreeTest::val_min, num_items, BTreeTest::val_max, "Init_Range_All");
        if (num_items > 0) co_await verify_all_query(btree_global_basic, ref_model_global_basic, 0, "Init_Key0");
        if (num_items > 10) co_await verify_all_query(btree_global_basic, ref_model_global_basic, 10, "Init_Key10");
    }();
    run_task_sync(std::move(task), btree_global_basic);
    teardown_test_environment();
    std::cout << "--- test_InitializeWithData PASSED ---" << std::endl;
}


void run_basic_operations_tests() {
    std::cout << "\n========== Running Basic Operations Tests ==========" << std::endl;
    test_SimpleRead_EmptyTree();
    test_SimpleRead_OnlyWriteMap();
    test_InitializeWithData();
}

int main() {
    std::cout << "=============== Starting Basic B+Tree Tests ===============" << std::endl;
    run_basic_operations_tests();
    std::cout << "\n*********************************" << std::endl;
    std::cout << "*** Basic B+Tree Tests COMPLETED SUCCESSFULLY ***" << std::endl;
    std::cout << "*********************************" << std::endl;
    return 0;
}