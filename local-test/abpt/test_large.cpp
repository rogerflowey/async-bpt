#define PMA_DEBUG

#include "test_utils.hpp"

// Global instances for this test file
BTreeTest btree_global_large;
RefModelTest ref_model_global_large;


void test_AsyncRead_FromInitializedDisk_WithFlush() {
    std::cout << "\n--- Running test_AsyncRead_FromInitializedDisk_WithFlush ---" << std::endl;
    setup_test_environment(btree_global_large, ref_model_global_large, true, false); 

    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_initial_items = 120; 
        for (int i = 0; i < num_initial_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 2, i * 10));
            if (i % 5 == 0) {
                 initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 2, i * 10 + 1));
            }
        }
        std::sort(initial_data_std.begin(), initial_data_std.end());
        initial_data_std.erase(std::unique(initial_data_std.begin(), initial_data_std.end()), initial_data_std.end());

        co_await btree_global_large.initialize_from_vector_async(to_sjtu_vector(initial_data_std));
        for(const auto& p : initial_data_std) { 
            ref_model_global_large.insert(p);
        }
        std::cout << "  Disk initialized with " << ref_model_global_large.size() << " distinct pairs." << std::endl;
        
        co_await verify_all_query(btree_global_large, ref_model_global_large, 0, "DiskReadL_Key0");
        co_await verify_all_query(btree_global_large, ref_model_global_large, 10, "DiskReadL_Key10");
        co_await verify_range_query(btree_global_large, ref_model_global_large, 0, BTreeTest::val_min, num_initial_items * 2, BTreeTest::val_max, "DiskReadL_Range_All");
    }();
    run_task_sync(std::move(task), btree_global_large);
    teardown_test_environment();
    std::cout << "--- test_AsyncRead_FromInitializedDisk_WithFlush PASSED ---" << std::endl;
}

void test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush() {
    std::cout << "\n--- Running test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush ---" << std::endl;
    setup_test_environment(btree_global_large, ref_model_global_large, true, false);

    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_initial_items = 110;
        for (int i = 0; i < num_initial_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 3, i + 1000));
        }
        std::sort(initial_data_std.begin(), initial_data_std.end());
        initial_data_std.erase(std::unique(initial_data_std.begin(), initial_data_std.end()), initial_data_std.end());
        
        co_await btree_global_large.initialize_from_vector_async(to_sjtu_vector(initial_data_std));
        for(const auto& p : initial_data_std) {
            ref_model_global_large.insert(p);
        }
        std::cout << "  Disk initialized with " << ref_model_global_large.size() << " pairs." << std::endl;

        co_await apply_insert_op(btree_global_large, ref_model_global_large, 20, 2000); 
        co_await apply_remove_op(btree_global_large, ref_model_global_large, 0, 1000);  
        co_await apply_insert_op(btree_global_large, ref_model_global_large, 0, 1001);  
        
        co_await verify_all_query(btree_global_large, ref_model_global_large, 0, "Mix_Key0_BeforeFlush");

        std::cout << "  Flushing first batch of map operations..." << std::endl;
        co_await btree_global_large.flush();
        
        co_await verify_all_query(btree_global_large, ref_model_global_large, 0, "Mix_Key0_AfterFlush1");
        co_await verify_all_query(btree_global_large, ref_model_global_large, 20, "Mix_Key20_AfterFlush1");

        co_await apply_remove_op(btree_global_large, ref_model_global_large, initial_data_std[1].first, initial_data_std[1].second); 
        co_await apply_insert_op(btree_global_large, ref_model_global_large, num_initial_items * 3 + 10, 5000); 

        std::cout << "  Flushing second batch of map operations..." << std::endl;
        co_await btree_global_large.flush();
        
        co_await verify_range_query(btree_global_large, ref_model_global_large, 0, BTreeTest::val_min, num_initial_items * 3 + 20, BTreeTest::val_max, "Mix_Range_All_AfterFlush2");
    }();
    run_task_sync(std::move(task), btree_global_large);
    teardown_test_environment();
    std::cout << "--- test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush PASSED ---" << std::endl;
}

void test_Performance_AsyncWorkload() {
    std::cout << "\n--- Running test_Performance_AsyncWorkload ---" << std::endl;
    setup_test_environment(btree_global_large, ref_model_global_large, true, false); 

    const long long NUM_INITIAL_DISK_ITEMS = 10000; 
    const long long TOTAL_OPERATIONS = 50000;       
    const int MAX_KEY_RANGE = NUM_INITIAL_DISK_ITEMS * 2; 
    const int FIND_ALL_RATIO_PERF = 10; 

    auto start_time_total = std::chrono::high_resolution_clock::now();

    std::vector<storage_pair_test_t> initial_disk_data_std;
    initial_disk_data_std.reserve(NUM_INITIAL_DISK_ITEMS); 
    std::cout << "PERF: Populating initial disk data with approx " << NUM_INITIAL_DISK_ITEMS << " items..." << std::endl;
    std::mt19937 perf_gen(std::chrono::system_clock::now().time_since_epoch().count()); 
    std::uniform_int_distribution<> initial_key_dist(0, MAX_KEY_RANGE);
    std::uniform_int_distribution<> initial_val_dist(0, 10000);

    for (long long i = 0; i < NUM_INITIAL_DISK_ITEMS; ++i) {
        initial_disk_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(initial_key_dist(perf_gen), initial_val_dist(perf_gen)));
    }
    std::sort(initial_disk_data_std.begin(), initial_disk_data_std.end());
    initial_disk_data_std.erase(std::unique(initial_disk_data_std.begin(), initial_disk_data_std.end()), initial_disk_data_std.end()); 

    run_task_sync(btree_global_large.initialize_from_vector_async(to_sjtu_vector(initial_disk_data_std)), btree_global_large);
    auto init_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> init_duration = init_end_time - start_time_total;
    std::cout << "PERF: B+Tree initialized with " << initial_disk_data_std.size() << " unique items on disk. Time: "
              << init_duration.count() << " seconds." << std::endl;
    
    long long operations_issued_count = 0;
    std::vector<wutong::Task<void>> modification_tasks_perf;
    modification_tasks_perf.reserve(TOTAL_OPERATIONS); 
    std::vector<wutong::Task<sjtu::vector<storage_pair_test_t>>> find_all_tasks_perf;
    find_all_tasks_perf.reserve(TOTAL_OPERATIONS / FIND_ALL_RATIO_PERF + 1);

    std::uniform_int_distribution<> workload_key_dist(0, MAX_KEY_RANGE);
    std::uniform_int_distribution<> workload_val_dist(0, 20000);
    std::uniform_int_distribution<> op_choice_dist_perf(0, FIND_ALL_RATIO_PERF); 

    std::cout << "PERF: Starting to issue " << TOTAL_OPERATIONS << " workload operations..." << std::endl;
    auto workload_issue_start_time = std::chrono::high_resolution_clock::now();
    auto last_print_time = workload_issue_start_time;

    while (operations_issued_count < TOTAL_OPERATIONS) {
        if (btree_global_large.permit_operation()) { 
            idx_t_test k = workload_key_dist(perf_gen);
            val_t_test v = workload_val_dist(perf_gen);
            int op_choice = op_choice_dist_perf(perf_gen);

            if (op_choice == 0) { 
                modification_tasks_perf.push_back(btree_global_large.insert_async(k, v));
            } else if (op_choice == 1) { 
                modification_tasks_perf.push_back(btree_global_large.remove_async(k, v));
            } else { 
                idx_t_test find_key = workload_key_dist(perf_gen);
                find_all_tasks_perf.push_back(btree_global_large.find_all_async(find_key));
            }
            operations_issued_count++;

            if (operations_issued_count % (TOTAL_OPERATIONS / 20) == 0) { 
                auto current_time_progress = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed_segment = current_time_progress - last_print_time;
                std::cout << "  PERF: Issued " << operations_issued_count << "/" << TOTAL_OPERATIONS
                          << " ops. (Last segment issue time: " << elapsed_segment.count() << "s)" << std::endl;
                last_print_time = current_time_progress;
            }
        }
        btree_global_large.poll(); 
    }
    
    auto workload_issue_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> issue_duration = workload_issue_end_time - workload_issue_start_time;
    std::cout << "PERF: All " << operations_issued_count << " workload operations issued. Issue phase time: "
              << issue_duration.count() << " seconds." << std::endl;

    std::cout << "PERF: Waiting for " << modification_tasks_perf.size() << " modification tasks and "
              << find_all_tasks_perf.size() << " find_all tasks to complete..." << std::endl;
    
    for (size_t i = 0; i < modification_tasks_perf.size(); ++i) {
        run_task_sync(std::move(modification_tasks_perf[i]), btree_global_large);
    }
    std::cout << "PERF: All modification tasks complete." << std::endl;

    for (size_t i = 0; i < find_all_tasks_perf.size(); ++i) {
        [[maybe_unused]] auto res = run_task_sync_with_result(std::move(find_all_tasks_perf[i]), btree_global_large);
    }
    std::cout << "PERF: All find_all tasks complete." << std::endl;

    std::cout << "PERF: Performing final flush..." << std::endl;
    auto flush_start_time = std::chrono::high_resolution_clock::now();
    run_task_sync(btree_global_large.flush(), btree_global_large);
    auto flush_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> flush_duration = flush_end_time - flush_start_time;
    std::cout << "PERF: Final flush completed in " << flush_duration.count() << " seconds." << std::endl;

    auto total_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_duration = total_end_time - start_time_total;
    std::chrono::duration<double> workload_plus_completion_duration = total_end_time - workload_issue_start_time;

    std::cout << "PERF: Total workload (issuing + completion + final flush) time: " << workload_plus_completion_duration.count() << " seconds." << std::endl;
    if (workload_plus_completion_duration.count() > 0) {
        std::cout << "PERF: Approximate operations per second (workload phase): "
                  << TOTAL_OPERATIONS / workload_plus_completion_duration.count() << std::endl;
    }
    std::cout << "PERF: Total test duration (including init): " << total_duration.count() << " seconds." << std::endl;

    run_task_sync(btree_global_large.shutdown(), btree_global_large); 
    teardown_test_environment();
    std::cout << "--- test_Performance_AsyncWorkload FINISHED ---" << std::endl;
}


void run_large_workloads_tests() {
    std::cout << "\n========== Running Large Workloads & Performance Tests ==========" << std::endl;
    test_AsyncRead_FromInitializedDisk_WithFlush();
    test_AsyncMix_DiskAndWriteMap_WithPeriodicFlush();
    test_Performance_AsyncWorkload();
}

int main() {
    std::cout << "=============== Starting Large Workloads & Performance B+Tree Tests ===============" << std::endl;
    run_large_workloads_tests();
    std::cout << "\n*********************************" << std::endl;
    std::cout << "*** Large Workloads & Performance B+Tree Tests COMPLETED SUCCESSFULLY ***" << std::endl;
    std::cout << "*********************************" << std::endl;
    return 0;
}