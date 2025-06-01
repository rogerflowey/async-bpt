#include "test_utils.hpp"

// Define USE_SMALL_BATCH if you want to test with smaller node capacities
// and potentially enable more verbose tree traversal during these large tests.
// #define USE_SMALL_BATCH

// Global instances for this test file
BTreeTest btree_global_large;
RefModelTest ref_model_global_large;

// --- Test Cases ---

void test_AsyncRead_FromInitializedDisk_WithFlush_Detailed() {
    std::cout << "\n--- Running test_AsyncRead_FromInitializedDisk_WithFlush_Detailed ---" << std::endl;
    setup_test_environment(btree_global_large, ref_model_global_large, true, false);


    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_initial_items = 120;
        std::cout << "  LOG: Populating initial_data_std with " << num_initial_items << " base items." << std::endl;
        for (int i = 0; i < num_initial_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 2, i * 10));
            if (i % 5 == 0) {
                 initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 2, i * 10 + 1));
            }
        }
        std::sort(initial_data_std.begin(), initial_data_std.end());
        initial_data_std.erase(std::unique(initial_data_std.begin(), initial_data_std.end()), initial_data_std.end());
        std::cout << "  LOG: initial_data_std has " << initial_data_std.size() << " unique items." << std::endl;

        std::cout << "  LOG: Calling initialize_from_vector_async..." << std::endl;
        co_await btree_global_large.initialize_from_vector_async(to_sjtu_vector(initial_data_std));
        for(const auto& p : initial_data_std) {
            ref_model_global_large.insert(p);
        }
        std::cout << "  LOG: Disk initialized. RefModel size: " << ref_model_global_large.size()
                  << ", BTree height: " << btree_global_large.tree_height_.val << std::endl;

        #ifdef USE_SMALL_BATCH
        std::cout << "  LOG: Traversing tree after init (USE_SMALL_BATCH defined):" << std::endl;
        co_await btree_global_large.traverse_async(true);
        #endif

        std::cout << "  LOG: Verifying queries post-init..." << std::endl;
        co_await verify_all_query(btree_global_large, ref_model_global_large, 0, "DiskReadL_Key0_PostInit");
        co_await verify_all_query(btree_global_large, ref_model_global_large, 10, "DiskReadL_Key10_PostInit");
        co_await verify_range_query(btree_global_large, ref_model_global_large, 0, BTreeTest::val_min, num_initial_items * 2, BTreeTest::val_max, "DiskReadL_Range_All_PostInit");
        std::cout << "  LOG: Post-init verifications complete." << std::endl;
    }();
    run_task_sync(std::move(task), btree_global_large);
    teardown_test_environment();
    std::cout << "--- test_AsyncRead_FromInitializedDisk_WithFlush_Detailed PASSED ---" << std::endl;
}

void test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush_Detailed() {
    std::cout << "\n--- Running test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush_Detailed ---" << std::endl;
    setup_test_environment(btree_global_large, ref_model_global_large, true, false);

    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_initial_items = 110;
        std::cout << "  LOG: Populating initial_data_std with " << num_initial_items << " base items for mix test." << std::endl;
        for (int i = 0; i < num_initial_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 3, i + 1000));
        }
        std::sort(initial_data_std.begin(), initial_data_std.end());
        initial_data_std.erase(std::unique(initial_data_std.begin(), initial_data_std.end()), initial_data_std.end());
        std::cout << "  LOG: initial_data_std for mix test has " << initial_data_std.size() << " unique items." << std::endl;

        std::cout << "  LOG: Calling initialize_from_vector_async for mix test..." << std::endl;
        co_await btree_global_large.initialize_from_vector_async(to_sjtu_vector(initial_data_std));
        for(const auto& p : initial_data_std) {
            ref_model_global_large.insert(p);
        }
        std::cout << "  LOG: Disk initialized for mix test. RefModel size: " << ref_model_global_large.size()
                  << ", BTree height: " << btree_global_large.tree_height_.val << std::endl;

        co_await btree_global_large.traverse_async();

        // Operations that go into write_map_
        std::cout << "  LOG: Applying first batch of map operations..." << std::endl;
        co_await apply_insert_op(btree_global_large, ref_model_global_large, 20, 2000);
        std::cout << "    LOG: After insert (20,2000) - RefModel size: " << ref_model_global_large.size() << std::endl;
        co_await apply_remove_op(btree_global_large, ref_model_global_large, 0, 1000);
        std::cout << "    LOG: After remove (0,1000) - RefModel size: " << ref_model_global_large.size() << std::endl;
        co_await apply_insert_op(btree_global_large, ref_model_global_large, 0, 1001);
        std::cout << "    LOG: After insert (0,1001) - RefModel size: " << ref_model_global_large.size() << std::endl;

        std::cout << "  LOG: Verifying queries before first flush..." << std::endl;
        co_await verify_all_query(btree_global_large, ref_model_global_large, 0, "Mix_Key0_BeforeFlush1");

        std::cout << "  LOG: Flushing first batch of map operations..." << std::endl;
        co_await btree_global_large.flush();
        std::cout << "  LOG: Flush 1 complete. BTree height: " << btree_global_large.tree_height_.val << std::endl;
        #ifdef USE_SMALL_BATCH
        std::cout << "  LOG: Traversing tree after flush 1 (USE_SMALL_BATCH defined):" << std::endl;
        co_await btree_global_large.traverse_async(true);
        #endif

        std::cout << "  LOG: Verifying queries after first flush..." << std::endl;
        co_await verify_all_query(btree_global_large, ref_model_global_large, 0, "Mix_Key0_AfterFlush1");
        co_await verify_all_query(btree_global_large, ref_model_global_large, 20, "Mix_Key20_AfterFlush1");

        // More operations
        std::cout << "  LOG: Applying second batch of map operations..." << std::endl;
        storage_pair_test_t item_to_delete_from_init = initial_data_std[initial_data_std.size() / 2]; // Pick an item from middle of init data
        co_await apply_remove_op(btree_global_large, ref_model_global_large, item_to_delete_from_init.first, item_to_delete_from_init.second);
        std::cout << "    LOG: After remove (" << item_to_delete_from_init.first << "," << item_to_delete_from_init.second
                  << ") - RefModel size: " << ref_model_global_large.size() << std::endl;
        co_await apply_insert_op(btree_global_large, ref_model_global_large, num_initial_items * 3 + 10, 5000); // Far out insert
        std::cout << "    LOG: After insert (" << num_initial_items * 3 + 10 << ",5000) - RefModel size: " << ref_model_global_large.size() << std::endl;

        std::cout << "  LOG: Flushing second batch of map operations..." << std::endl;
        co_await btree_global_large.flush();
        std::cout << "  LOG: Flush 2 complete. BTree height: " << btree_global_large.tree_height_.val << std::endl;
        #ifdef USE_SMALL_BATCH
        std::cout << "  LOG: Traversing tree after flush 2 (USE_SMALL_BATCH defined):" << std::endl;
        co_await btree_global_large.traverse_async(true);
        #endif

        std::cout << "  LOG: Verifying final range query..." << std::endl;
        co_await verify_range_query(btree_global_large, ref_model_global_large, 0, BTreeTest::val_min, num_initial_items * 3 + 20, BTreeTest::val_max, "Mix_Range_All_AfterFlush2");
        std::cout << "  LOG: Final verification complete." << std::endl;
    }();
    run_task_sync(std::move(task), btree_global_large);
    teardown_test_environment();
    std::cout << "--- test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush_Detailed PASSED ---" << std::endl;
}


void test_Performance_AsyncWorkload_Detailed() {
    std::cout << "\n--- Running test_Performance_AsyncWorkload_Detailed ---" << std::endl;
    setup_test_environment(btree_global_large, ref_model_global_large, true, false);

    const long long NUM_INITIAL_DISK_ITEMS = 10000;
    const long long TOTAL_OPERATIONS = 500000;
    const int MAX_KEY_RANGE = NUM_INITIAL_DISK_ITEMS * 2;
    const int FIND_ALL_RATIO_PERF = 10;
    const long long PROGRESS_INTERVAL_OPS = TOTAL_OPERATIONS / 20; // Log progress ~20 times

    auto start_time_total = std::chrono::high_resolution_clock::now();

    std::vector<storage_pair_test_t> initial_disk_data_std;
    initial_disk_data_std.reserve(NUM_INITIAL_DISK_ITEMS);
    std::cout << "PERF_LOG: Populating initial disk data with approx " << NUM_INITIAL_DISK_ITEMS << " items..." << std::endl;
    std::mt19937 perf_gen(std::chrono::system_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<> initial_key_dist(0, MAX_KEY_RANGE);
    std::uniform_int_distribution<> initial_val_dist(0, 10000);

    for (long long i = 0; i < NUM_INITIAL_DISK_ITEMS; ++i) {
        initial_disk_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(initial_key_dist(perf_gen), initial_val_dist(perf_gen)));
    }
    std::sort(initial_disk_data_std.begin(), initial_disk_data_std.end());
    initial_disk_data_std.erase(std::unique(initial_disk_data_std.begin(), initial_disk_data_std.end()), initial_disk_data_std.end());

    std::cout << "PERF_LOG: Calling initialize_from_vector_async for perf test..." << std::endl;
    run_task_sync(btree_global_large.initialize_from_vector_async(to_sjtu_vector(initial_disk_data_std)), btree_global_large);
    // For performance test, we don't typically populate ref_model_global_large for the workload phase
    // as it would slow down the test itself. We rely on the BTree's internal consistency.
    // However, for debugging, one might choose to.
    // for(const auto& p : initial_disk_data_std) { ref_model_global_large.insert(p); }

    auto init_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> init_duration = init_end_time - start_time_total;
    std::cout << "PERF_LOG: B+Tree initialized with " << initial_disk_data_std.size() << " unique items on disk. BTree height: "
              << btree_global_large.tree_height_.val << ". Init Time: " << init_duration.count() << " seconds." << std::endl;

    long long operations_issued_count = 0;
    long long modification_ops_issued = 0;
    long long find_ops_issued = 0;
    std::vector<wutong::Task<void>> modification_tasks_perf;
    modification_tasks_perf.reserve(TOTAL_OPERATIONS);
    std::vector<wutong::Task<sjtu::vector<storage_pair_test_t>>> find_all_tasks_perf;
    find_all_tasks_perf.reserve(TOTAL_OPERATIONS / FIND_ALL_RATIO_PERF + 1);

    std::uniform_int_distribution<> workload_key_dist(0, MAX_KEY_RANGE);
    std::uniform_int_distribution<> workload_val_dist(0, 20000);
    std::uniform_int_distribution<> op_choice_dist_perf(0, FIND_ALL_RATIO_PERF);

    std::cout << "PERF_LOG: Starting to issue " << TOTAL_OPERATIONS << " workload operations..." << std::endl;
    auto workload_issue_start_time = std::chrono::high_resolution_clock::now();
    auto last_print_time = workload_issue_start_time;

    while (operations_issued_count < TOTAL_OPERATIONS) {
        if (btree_global_large.permit_operation()) {
            idx_t_test k = workload_key_dist(perf_gen);
            val_t_test v = workload_val_dist(perf_gen);
            int op_choice = op_choice_dist_perf(perf_gen);

            if (op_choice == 0) {
                modification_tasks_perf.push_back(btree_global_large.insert_async(k, v));
                modification_ops_issued++;
            } else if (op_choice == 1) {
                modification_tasks_perf.push_back(btree_global_large.remove_async(k, v));
                modification_ops_issued++;
            } else {
                idx_t_test find_key = workload_key_dist(perf_gen);
                find_all_tasks_perf.push_back(btree_global_large.find_all_async(find_key));
                find_ops_issued++;
            }
            operations_issued_count++;

            if (operations_issued_count > 0 && operations_issued_count % PROGRESS_INTERVAL_OPS == 0) {
                auto current_time_progress = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed_segment = current_time_progress - last_print_time;
                std::cout << "  PERF_LOG: Issued " << operations_issued_count << "/" << TOTAL_OPERATIONS
                          << " ops (Mod: " << modification_ops_issued << ", Find: " << find_ops_issued
                          << "). BTree height: " << btree_global_large.tree_height_.val
                          << ". (Last segment issue time: " << elapsed_segment.count() << "s)" << std::endl;
                last_print_time = current_time_progress;

                // Optional: very infrequent flush and traverse if USE_SMALL_BATCH for debugging perf issues
                #if defined(USE_SMALL_BATCH)
                if (operations_issued_count % (PROGRESS_INTERVAL_OPS * 5) == 0) { // Even less frequent
                    std::cout << "  PERF_LOG_DBG: Flushing for debug..." << std::endl;
                    run_task_sync(btree_global_large.flush(), btree_global_large);
                    std::cout << "  PERF_LOG_DBG: Traversing tree (USE_SMALL_BATCH defined):" << std::endl;
                    run_task_sync(btree_global_large.traverse_async(true), btree_global_large);
                }
                #endif
            }
        }
        btree_global_large.poll();
    }

    auto workload_issue_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> issue_duration = workload_issue_end_time - workload_issue_start_time;
    std::cout << "PERF_LOG: All " << operations_issued_count << " workload operations issued. Issue phase time: "
              << issue_duration.count() << " seconds." << std::endl;

    std::cout << "PERF_LOG: Waiting for " << modification_tasks_perf.size() << " modification tasks and "
              << find_all_tasks_perf.size() << " find_all tasks to complete..." << std::endl;

    for (size_t i = 0; i < modification_tasks_perf.size(); ++i) {
        run_task_sync(std::move(modification_tasks_perf[i]), btree_global_large);
        if ((i + 1) % (modification_tasks_perf.size() / 10 + 1) == 0) {
             std::cout << "  PERF_LOG: Completed " << (i + 1) << "/" << modification_tasks_perf.size()
                       << " mod tasks. BTree height: " << btree_global_large.tree_height_.val << std::endl;
        }
    }
    std::cout << "PERF_LOG: All modification tasks complete. BTree height: " << btree_global_large.tree_height_.val << std::endl;

    for (size_t i = 0; i < find_all_tasks_perf.size(); ++i) {
        [[maybe_unused]] auto res = run_task_sync_with_result(std::move(find_all_tasks_perf[i]), btree_global_large);
         if ((i + 1) % (find_all_tasks_perf.size() / 10 + 1) == 0) {
             std::cout << "  PERF_LOG: Completed " << (i + 1) << "/" << find_all_tasks_perf.size()
                       << " find_all tasks." << std::endl;
        }
    }
    std::cout << "PERF_LOG: All find_all tasks complete." << std::endl;

    std::cout << "PERF_LOG: Performing final flush..." << std::endl;
    auto flush_start_time = std::chrono::high_resolution_clock::now();
    run_task_sync(btree_global_large.flush(), btree_global_large);
    auto flush_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> flush_duration = flush_end_time - flush_start_time;
    std::cout << "PERF_LOG: Final flush completed in " << flush_duration.count()
              << " seconds. BTree height: " << btree_global_large.tree_height_.val << std::endl;

    #ifdef USE_SMALL_BATCH
    std::cout << "PERF_LOG_DBG: Traversing tree after final flush (USE_SMALL_BATCH defined):" << std::endl;
    run_task_sync(btree_global_large.traverse_async(true), btree_global_large);
    #endif

    auto total_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_duration = total_end_time - start_time_total;
    std::chrono::duration<double> workload_plus_completion_duration = total_end_time - workload_issue_start_time;

    std::cout << "PERF_SUMMARY: Total workload (issuing + completion + final flush) time: " << workload_plus_completion_duration.count() << " seconds." << std::endl;
    if (workload_plus_completion_duration.count() > 0) {
        std::cout << "PERF_SUMMARY: Approximate operations per second (workload phase): "
                  << TOTAL_OPERATIONS / workload_plus_completion_duration.count() << std::endl;
    }
    std::cout << "PERF_SUMMARY: Total test duration (including init): " << total_duration.count() << " seconds." << std::endl;
    std::cout << "PERF_SUMMARY: Final BTree height: " << btree_global_large.tree_height_.val << std::endl;


    run_task_sync(btree_global_large.shutdown(), btree_global_large);
    teardown_test_environment();
    std::cout << "--- test_Performance_AsyncWorkload_Detailed FINISHED ---" << std::endl;
}


void run_large_workloads_tests() {
    std::cout << "\n========== Running Large Workloads & Performance Tests (Detailed Logging) ==========" << std::endl;
    //test_AsyncRead_FromInitializedDisk_WithFlush_Detailed();
    test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush_Detailed();
    test_Performance_AsyncWorkload_Detailed();
}

int main() {
    std::cout << "=============== Starting Large Workloads & Performance B+Tree Tests (Detailed Logging) ===============" << std::endl;
    run_large_workloads_tests();
    std::cout << "\n*********************************" << std::endl;
    std::cout << "*** Large Workloads & Performance B+Tree Tests (Detailed Logging) COMPLETED SUCCESSFULLY ***" << std::endl;
    std::cout << "*********************************" << std::endl;
    return 0;
}

// Helper for print_capacities if not in test_utils.hpp
#ifndef PRINT_CAPACITIES_DEFINED_IN_UTILS
#define PRINT_CAPACITIES_DEFINED_IN_UTILS
void print_capacities() { // Define it locally if not in test_utils.hpp
    std::cout << "  INFO: Leaf Capacity: " << BTreeTest::LeafNode::node_capacity
              << ", Leaf Split Threshold: " << BTreeTest::LeafNode::split_threshold
              << ", Leaf Merge Threshold: " << BTreeTest::LeafNode::merge_threshold << std::endl;
    std::cout << "  INFO: Index Capacity (keys): " << BTreeTest::IndexNode::node_capacity
              << ", Index Split Threshold (keys): " << BTreeTest::IndexNode::split_threshold
              << ", Index Merge Threshold (keys): " << BTreeTest::IndexNode::merge_threshold << std::endl;
}
#endif