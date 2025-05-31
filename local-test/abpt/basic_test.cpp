#include "async_bpt.h" // The class to test


#include <set>
#include <vector>
#include <algorithm> // For std::sort
#include <random>
#include <iostream>
#include <filesystem> // For file cleanup
#include <string>
#include <stdexcept> // For runtime_error
#include <chrono>    // For timing
#include <thread>    // For std::this_thread::yield

// --- Global Test State & Configuration ---
norb::AsyncBPlusTree<int, int> btree_global;
std::set<norb::Pair<int, int>> ref_model_global; // Still used for ref in correctness tests

// Define PMA/FiledConfig file names for cleanup.
const std::string PMA_FILE_NAME = "pma_test_file.dat";
const std::string FILED_CONFIG_FILE_NAME = "default_filed_config.dat";


// --- Typedefs for Test ---
#ifndef TEMPLATE_CHECK
using idx_t_test = int;
using val_t_test = int;
#else
using idx_t_test = idx_t;
using val_t_test = val_t;
#endif

using storage_pair_test_t = norb::Pair<idx_t_test, val_t_test>;
using BTreeTest = norb::AsyncBPlusTree<idx_t_test, val_t_test>;
using RefModelTest = std::set<storage_pair_test_t>;


// --- Task Execution Helpers ---
void run_task_sync(wutong::Task<void> task) {
    if (!task.handle) {
        return;
    }
    while (task.handle && !task.handle.done()) {
        btree_global.poll();
    }
    if (task.handle && task.handle.promise().exception_ptr_) {
        std::rethrow_exception(task.handle.promise().exception_ptr_);
    }
}

template <typename T>
T run_task_sync_with_result(wutong::Task<T> task) {
    if (!task.handle) {
        throw std::runtime_error("Attempted to run an invalid or moved-from task for result.");
    }
    while (task.handle && !task.handle.done()) {
        btree_global.poll();
    }
    if (!task.handle) {
         throw std::runtime_error("Task handle became null unexpectedly during execution.");
    }
    return task.await_resume();
}

// --- Utility and Assertion Helpers ---

std::vector<storage_pair_test_t> to_std_vector(const sjtu::vector<storage_pair_test_t>& sjtu_vec) {
    std::vector<storage_pair_test_t> std_vec;
    std_vec.reserve(sjtu_vec.size());
    for (const auto& item : sjtu_vec) {
        std_vec.push_back(item);
    }
    std::sort(std_vec.begin(), std_vec.end());
    return std_vec;
}

sjtu::vector<storage_pair_test_t> to_sjtu_vector(const std::vector<storage_pair_test_t>& std_vec) {
    sjtu::vector<storage_pair_test_t> sjtu_vec;
    for (const auto& item : std_vec) {
        sjtu_vec.push_back(item);
    }
    return sjtu_vec;
}

std::vector<storage_pair_test_t> to_std_vector(const RefModelTest& ref_set) {
    std::vector<storage_pair_test_t> std_vec(ref_set.begin(), ref_set.end());
    return std_vec;
}

void custom_assert(bool condition, const std::string& message, const char* file, int line) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED [" << file << ":" << line << "]: " << message << std::endl;
        std::exit(1);
    }
}

#define ASSERT_TRUE(cond, msg) custom_assert(cond, msg, __FILE__, __LINE__)

void ASSERT_EQ_VEC(const std::vector<storage_pair_test_t>& actual,
                   const std::vector<storage_pair_test_t>& expected,
                   const std::string& context_message) {
    ASSERT_TRUE(actual.size() == expected.size(),
                context_message + " - Size mismatch. Actual: " + std::to_string(actual.size()) +
                ", Expected: " + std::to_string(expected.size()));

    for (size_t i = 0; i < actual.size(); ++i) {
        ASSERT_TRUE(actual[i].first == expected[i].first && actual[i].second == expected[i].second,
                    context_message + " - Element mismatch at index " + std::to_string(i) +
                    ". Actual: (" + std::to_string(actual[i].first) + "," + std::to_string(actual[i].second) +
                    "), Expected: (" + std::to_string(expected[i].first) + "," + std::to_string(expected[i].second) + ")");
    }
}


// --- Test Environment Setup/Teardown ---
void setup_test_environment(bool initialize_with_empty = true) {
    std::cout << "Setting up test environment..." << std::endl;
    std::filesystem::remove(PMA_FILE_NAME);
    std::filesystem::remove(FILED_CONFIG_FILE_NAME);

    norb::PersistentMemoryAsync::get_instance();

    if (initialize_with_empty) {
        run_task_sync(btree_global.initialize_from_vector_async({}));
    }
    ref_model_global.clear(); // Cleared for correctness tests; perf test might not use it.
    std::cout << "Test environment set up." << std::endl;
}

void teardown_test_environment() {
    std::cout << "Tearing down test environment..." << std::endl;
    std::filesystem::remove(PMA_FILE_NAME);
    std::filesystem::remove(FILED_CONFIG_FILE_NAME);
    std::cout << "Test environment torn down." << std::endl;
}

// --- Operation Helpers for simpler tests ---
wutong::Task<void> apply_insert_op_for_simple_tests(idx_t_test k, val_t_test v) {
    storage_pair_test_t p = norb::make_pair(k, v);
    co_await btree_global.insert_async(k, v);
    ref_model_global.insert(p);
}

wutong::Task<void> apply_remove_op_for_simple_tests(idx_t_test k, val_t_test v) {
    storage_pair_test_t p = norb::make_pair(k, v);
    co_await btree_global.remove_async(k, v);
    ref_model_global.erase(p);
}

// --- Verification Helpers (use global btree and ref_model) ---
wutong::Task<void> verify_range_query(idx_t_test k_start, val_t_test v_start, idx_t_test k_end, val_t_test v_end, const std::string& query_context) {
    storage_pair_test_t range_s = norb::make_pair(k_start, BTreeTest::val_min);
    if (v_start != BTreeTest::val_min) range_s.second = v_start;

    storage_pair_test_t range_e = norb::make_pair(k_end, BTreeTest::val_max);
    if (v_end != BTreeTest::val_max) range_e.second = v_end;

    sjtu::vector<storage_pair_test_t> btree_res_sjtu = co_await btree_global.find_range_async(range_s, range_e);
    std::vector<storage_pair_test_t> btree_res = to_std_vector(btree_res_sjtu);

    RefModelTest expected_in_range_set;
    for (const auto& p_ref : ref_model_global) {
        if (p_ref >= range_s && p_ref < range_e) {
            expected_in_range_set.insert(p_ref);
        }
    }
    std::vector<storage_pair_test_t> ref_res = to_std_vector(expected_in_range_set);

    ASSERT_EQ_VEC(btree_res, ref_res, "Range Query: " + query_context);
}

wutong::Task<void> verify_all_query(idx_t_test key, const std::string& query_context) {
    sjtu::vector<storage_pair_test_t> btree_res_sjtu = co_await btree_global.find_all_async(key);
    std::vector<storage_pair_test_t> btree_res = to_std_vector(btree_res_sjtu);

    RefModelTest expected_for_key_set;
    for (const auto& p_ref : ref_model_global) {
        if (p_ref.first == key) {
            expected_for_key_set.insert(p_ref);
        }
    }
    std::vector<storage_pair_test_t> ref_res = to_std_vector(expected_for_key_set);

    ASSERT_EQ_VEC(btree_res, ref_res, "Find_All Query for key " + std::to_string(key) + ": " + query_context);
}

// --- Test Cases (Correctness) ---

void test_SimpleRead_EmptyTree() {
    std::cout << "\n--- Running test_SimpleRead_EmptyTree ---" << std::endl;
    setup_test_environment();
    auto task = [&]() -> wutong::Task<void> {
        co_await verify_range_query(0, BTreeTest::val_min, 100, BTreeTest::val_max, "EmptyTree_Range");
        co_await verify_all_query(10, "EmptyTree_All");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_SimpleRead_EmptyTree PASSED ---" << std::endl;
}

void test_SimpleRead_OnlyWriteMap() {
    std::cout << "\n--- Running test_SimpleRead_OnlyWriteMap ---" << std::endl;
    setup_test_environment();
    auto task = [&]() -> wutong::Task<void> {
        co_await apply_insert_op_for_simple_tests(10, 100);
        co_await apply_insert_op_for_simple_tests(10, 101);
        co_await apply_insert_op_for_simple_tests(20, 200);

        co_await verify_all_query(10, "WM_Key10");
        co_await verify_all_query(20, "WM_Key20");
        co_await verify_all_query(30, "WM_Key30_Empty");

        co_await verify_range_query(10, BTreeTest::val_min, 11, BTreeTest::val_max, "WM_Range_Key10_Items");
        co_await verify_range_query(0, BTreeTest::val_min, 100, BTreeTest::val_max, "WM_Range_All_Items");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_SimpleRead_OnlyWriteMap PASSED ---" << std::endl;
}

void test_SimpleMix_OnlyWriteMap() {
    std::cout << "\n--- Running test_SimpleMix_OnlyWriteMap ---" << std::endl;
    setup_test_environment();
    auto task = [&]() -> wutong::Task<void> {
        co_await apply_insert_op_for_simple_tests(10, 100);
        co_await apply_insert_op_for_simple_tests(10, 101);
        co_await apply_insert_op_for_simple_tests(20, 200);
        co_await verify_range_query(0, BTreeTest::val_min, 100, BTreeTest::val_max, "WM_Mix_Initial");

        co_await apply_remove_op_for_simple_tests(10, 101);
        co_await verify_all_query(10, "WM_Mix_After_Remove_10_101");

        co_await apply_remove_op_for_simple_tests(20, 200);
        co_await verify_all_query(20, "WM_Mix_After_Remove_20_200");
        ASSERT_TRUE(ref_model_global.find(norb::make_pair<idx_t_test, val_t_test>(20,200)) == ref_model_global.end(), "Ref model check for (20,200)");

        co_await apply_insert_op_for_simple_tests(20, 201);
        co_await verify_all_query(20, "WM_Mix_After_Insert_20_201");

        co_await apply_remove_op_for_simple_tests(10, 100);
        co_await apply_remove_op_for_simple_tests(10, 100);
        co_await verify_all_query(10, "WM_Mix_After_Remove_10_100_Twice");

        co_await verify_range_query(0, BTreeTest::val_min, 100, BTreeTest::val_max, "WM_Mix_Final");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_SimpleMix_OnlyWriteMap PASSED ---" << std::endl;
}

void test_AsyncRead_FromInitializedDisk_Larger() {
    std::cout << "\n--- Running test_AsyncRead_FromInitializedDisk_Larger ---" << std::endl;
    setup_test_environment(false);
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

        for(const auto& p : initial_data_std) {
            ref_model_global.insert(p);
        }
        std::cout << "Initializing disk with " << ref_model_global.size() << " distinct pairs for AsyncRead_Larger." << std::endl;
        co_await btree_global.initialize_from_vector_async(to_sjtu_vector(initial_data_std));

        co_await verify_all_query(0, "DiskReadL_Key0");
        co_await verify_all_query(10, "DiskReadL_Key10");
        co_await verify_all_query(num_initial_items -1, "DiskReadL_KeyNearEnd");
        co_await verify_all_query(num_initial_items * 2 + 10, "DiskReadL_KeyNonExistent");
        co_await verify_range_query(0, BTreeTest::val_min, num_initial_items * 2, BTreeTest::val_max, "DiskReadL_Range_All");
        co_await verify_range_query(20, BTreeTest::val_min, 40, BTreeTest::val_max, "DiskReadL_Range_20_40");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_AsyncRead_FromInitializedDisk_Larger PASSED ---" << std::endl;
}

void test_AsyncMix_DiskAndWriteMap_Larger() {
    std::cout << "\n--- Running test_AsyncMix_DiskAndWriteMap_Larger ---" << std::endl;
    setup_test_environment(false);
    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_initial_items = 110;
        for (int i = 0; i < num_initial_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 3, i + 1000));
        }
        std::sort(initial_data_std.begin(), initial_data_std.end());

        for(const auto& p : initial_data_std) {
            ref_model_global.insert(p);
        }
        co_await btree_global.initialize_from_vector_async(to_sjtu_vector(initial_data_std));
        std::cout << "Disk initialized with " << ref_model_global.size() << " pairs for AsyncMix_Larger." << std::endl;

        co_await apply_insert_op_for_simple_tests(20, 2000);
        co_await apply_remove_op_for_simple_tests(0, 1000);
        co_await apply_insert_op_for_simple_tests(0, 1001);
        co_await apply_remove_op_for_simple_tests(6, 1002);
        co_await apply_insert_op_for_simple_tests(num_initial_items * 3 + 10, 5000);

        std::cout << "Ref model size after map ops: " << ref_model_global.size() << std::endl;
        co_await verify_all_query(0, "DiskWML_Mix_Key0");
        co_await verify_all_query(20, "DiskWML_Mix_Key20");
        co_await verify_all_query(6, "DiskWML_Mix_Key6");
        co_await verify_all_query(num_initial_items * 3 + 10, "DiskWML_Mix_KeyFarOut");
        co_await verify_range_query(0, BTreeTest::val_min, num_initial_items * 3 + 20, BTreeTest::val_max, "DiskWML_Mix_Range_All");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_AsyncMix_DiskAndWriteMap_Larger PASSED ---" << std::endl;
}

void test_MergeLogic_Comprehensive() {
    std::cout << "\n--- Running test_MergeLogic_Comprehensive ---" << std::endl;
    setup_test_environment(false);
    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(10, 1));
        initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(30, 1));
        initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(50, 1));
        std::sort(initial_data_std.begin(), initial_data_std.end());
        for(const auto& p : initial_data_std) ref_model_global.insert(p);
        co_await btree_global.initialize_from_vector_async(to_sjtu_vector(initial_data_std));

        co_await apply_remove_op_for_simple_tests(10, 1);
        co_await apply_insert_op_for_simple_tests(5, 1);
        co_await apply_insert_op_for_simple_tests(30, 2);
        co_await apply_remove_op_for_simple_tests(50, 1);
        co_await apply_insert_op_for_simple_tests(60, 1);

        co_await verify_range_query(0, BTreeTest::val_min, 100, BTreeTest::val_max, "MergeComprehensive_Range_All");
        co_await verify_all_query(30, "MergeComprehensive_Key30");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_MergeLogic_Comprehensive PASSED ---" << std::endl;
}

void test_LargeNumberOfOperations_WriteMapHeavy() {
    std::cout << "\n--- Running test_LargeNumberOfOperations_WriteMapHeavy ---" << std::endl;
    setup_test_environment();
    auto task = [&]() -> wutong::Task<void> {
        const int num_ops = 500;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> key_dist(0, 100);
        std::uniform_int_distribution<> val_dist(0, 5);
        std::uniform_int_distribution<> op_dist(0, 2);

        for (int i = 0; i < num_ops; ++i) {
            idx_t_test k = key_dist(gen);
            val_t_test v = val_dist(gen);
            int current_op_type = op_dist(gen);

            if (current_op_type == 1 && !ref_model_global.empty()) {
                if (std::uniform_int_distribution<>(0,2)(gen) != 0 && !ref_model_global.empty()) {
                    int N = ref_model_global.size();
                    int victim_idx = std::uniform_int_distribution<>(0,N-1)(gen);
                    auto it = ref_model_global.begin();
                    std::advance(it, victim_idx);
                    k = it->first;
                    v = it->second;
                }
                co_await apply_remove_op_for_simple_tests(k, v);
            } else {
                co_await apply_insert_op_for_simple_tests(k, v);
            }

            if (i > 0 && (i % 100 == 0 || i == num_ops -1) ) {
                 std::cout << "  LargeOps Iter " << i << ", RefModel size: " << ref_model_global.size() << std::endl;
                 co_await verify_range_query(0, BTreeTest::val_min, 101, BTreeTest::val_max, "LargeOps_Periodic_Range_Iter" + std::to_string(i));
                 if(!ref_model_global.empty()){
                    auto it_rand = ref_model_global.begin();
                    if (ref_model_global.size() > 1) {
                         std::advance(it_rand, std::uniform_int_distribution<>(0, (int)ref_model_global.size()-1)(gen));
                    }
                    co_await verify_all_query(it_rand->first, "LargeOps_Periodic_All_RandomKey_Iter" + std::to_string(i));
                 }
            }
        }
        std::cout << "Final RefModel size for LargeOps: " << ref_model_global.size() << std::endl;
        ASSERT_TRUE(ref_model_global.size() > 50, "LargeOps should result in a significant number of distinct entries");
        co_await verify_range_query(0, BTreeTest::val_min, 101, BTreeTest::val_max, "LargeOps_Final_Range_All");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_LargeNumberOfOperations_WriteMapHeavy PASSED ---" << std::endl;
}


void test_InitializeWithSplitsIfApplicable() {
    std::cout << "\n--- Running test_InitializeWithSplitsIfApplicable ---" << std::endl;
    setup_test_environment(false);
    auto task = [&]() -> wutong::Task<void> {
        std::vector<storage_pair_test_t> initial_data_std;
        const int num_items = 600;
        std::cout << "Initializing BTree with " << num_items << " items for split test." << std::endl;

        for (int i = 0; i < num_items; ++i) {
            initial_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i, i * 10));
        }
        for(const auto& p : initial_data_std) {
            ref_model_global.insert(p);
        }
        co_await btree_global.initialize_from_vector_async(to_sjtu_vector(initial_data_std));

        co_await verify_range_query(0, BTreeTest::val_min, num_items, BTreeTest::val_max, "InitSplit_Range_All");
        if (num_items > 0) co_await verify_all_query(0, "InitSplit_Key0");
        if (num_items > 10) co_await verify_all_query(10, "InitSplit_Key10");
        if (num_items > 0) co_await verify_all_query(num_items / 2, "InitSplit_KeyMid");
        if (num_items > 0) co_await verify_all_query(num_items -1, "InitSplit_KeyLast");
    }();
    run_task_sync(std::move(task));
    teardown_test_environment();
    std::cout << "--- test_InitializeWithSplitsIfApplicable PASSED ---" << std::endl;
}

void test_AsyncWorkload_WithPermitAndPoll() {
    std::cout << "\n--- Running test_AsyncWorkload_WithPermitAndPoll ---" << std::endl;
    setup_test_environment(false);

    std::vector<storage_pair_test_t> initial_disk_data_std;
    const int NUM_INITIAL_DISK_ITEMS = 150;
    const int INITIAL_MAX_KEY = NUM_INITIAL_DISK_ITEMS * 2;
    std::cout << "Populating initial disk data with " << NUM_INITIAL_DISK_ITEMS << " items..." << std::endl;
    for (int i = 0; i < NUM_INITIAL_DISK_ITEMS; ++i) {
        initial_disk_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 2, i * 100));
        if (i % 10 == 0) {
            initial_disk_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(i * 2, i * 100 + 1));
        }
    }
    std::sort(initial_disk_data_std.begin(), initial_disk_data_std.end());
    for(const auto& p : initial_disk_data_std) {
        ref_model_global.insert(p);
    }
    run_task_sync(btree_global.initialize_from_vector_async(to_sjtu_vector(initial_disk_data_std)));
    std::cout << "B+Tree initialized with " << ref_model_global.size() << " items on disk. Write map is empty." << std::endl;

    const int TOTAL_OPERATIONS = 700;
    const int MAX_KEY_WORKLOAD = INITIAL_MAX_KEY + 50;
    const int FIND_ALL_RATIO = 5;
    int operations_issued_count = 0;

    std::vector<wutong::Task<void>> modification_tasks;
    modification_tasks.reserve(TOTAL_OPERATIONS);
    std::vector<wutong::Task<sjtu::vector<storage_pair_test_t>>> find_all_tasks;
    std::vector<std::vector<storage_pair_test_t>> expected_find_all_results;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> key_dist(0, MAX_KEY_WORKLOAD);
    std::uniform_int_distribution<> val_dist(0, 200);
    std::uniform_int_distribution<> op_choice_dist(0, FIND_ALL_RATIO);

    std::cout << "Starting to issue " << TOTAL_OPERATIONS << " workload operations (including find_all)..." << std::endl;
    while (operations_issued_count < TOTAL_OPERATIONS) {
        if (btree_global.permit_operation()) {
            idx_t_test k = key_dist(gen);
            val_t_test v = val_dist(gen);
            int op_choice = op_choice_dist(gen);

            if (op_choice == 0) {
                modification_tasks.push_back(btree_global.insert_async(k, v));
                ref_model_global.insert(norb::make_pair(k, v));
                operations_issued_count++;
            } else if (op_choice == 1) {
                storage_pair_test_t p_to_delete = norb::make_pair(k,v);
                if (!ref_model_global.empty() && std::uniform_int_distribution<>(0,1)(gen) == 0) {
                    int N = ref_model_global.size();
                    int victim_idx = std::uniform_int_distribution<>(0,N-1)(gen);
                    auto it = ref_model_global.begin(); std::advance(it, victim_idx); p_to_delete = *it;
                }
                modification_tasks.push_back(btree_global.remove_async(p_to_delete.first, p_to_delete.second));
                ref_model_global.erase(p_to_delete);
                operations_issued_count++;
            } else {
                idx_t_test find_key = key_dist(gen);
                if (!ref_model_global.empty() && std::uniform_int_distribution<>(0,1)(gen) == 0) {
                    int N = ref_model_global.size();
                    int victim_idx = std::uniform_int_distribution<>(0,N-1)(gen);
                    auto it = ref_model_global.begin(); std::advance(it, victim_idx); find_key = it->first;
                }
                find_all_tasks.push_back(btree_global.find_all_async(find_key));
                RefModelTest expected_set_for_key;
                for (const auto& p_ref : ref_model_global) { if (p_ref.first == find_key) expected_set_for_key.insert(p_ref); }
                expected_find_all_results.push_back(to_std_vector(expected_set_for_key));
                operations_issued_count++;
            }
            if (operations_issued_count % 50 == 0) {
                std::cout << "  Issued " << operations_issued_count << "/" << TOTAL_OPERATIONS << " ops. Ref size: " << ref_model_global.size() << std::endl;
            }
        }
        btree_global.poll();
    }
    std::cout << "All " << operations_issued_count << " workload operations have been issued." << std::endl;
    std::cout << "Waiting for " << modification_tasks.size() << " mod tasks and " << find_all_tasks.size() << " find_all tasks..." << std::endl;
    for (size_t i = 0; i < modification_tasks.size(); ++i) {
        run_task_sync(std::move(modification_tasks[i]));
        if ((i + 1) % 50 == 0) std::cout << "  Completed " << (i + 1) << "/" << modification_tasks.size() << " mod tasks." << std::endl;
    }
    std::cout << "All modification tasks complete." << std::endl;
    ASSERT_TRUE(find_all_tasks.size() == expected_find_all_results.size(), "Find_all task/expected count mismatch.");
    for (size_t i = 0; i < find_all_tasks.size(); ++i) {
        sjtu::vector<storage_pair_test_t> actual_sjtu_res = run_task_sync_with_result(std::move(find_all_tasks[i]));
        std::vector<storage_pair_test_t> actual_std_res = to_std_vector(actual_sjtu_res);
        ASSERT_EQ_VEC(actual_std_res, expected_find_all_results[i], "AsyncWorkload Find_All Verif task " + std::to_string(i));
        if ((i + 1) % 20 == 0) std::cout << "  Verified " << (i + 1) << "/" << expected_find_all_results.size() << " find_all results." << std::endl;
    }
    std::cout << "All find_all tasks complete and verified." << std::endl;
    auto final_verification_task = [&]() -> wutong::Task<void> {
        std::cout << "Starting final overall range verification. RefModel size: " << ref_model_global.size() << std::endl;
        co_await verify_range_query(0, BTreeTest::val_min, MAX_KEY_WORKLOAD + 1, BTreeTest::val_max, "AsyncWorkload_Final_Overall_Range_All");
    }();
    run_task_sync(std::move(final_verification_task));
    teardown_test_environment();
    std::cout << "--- test_AsyncWorkload_WithPermitAndPoll PASSED ---" << std::endl;
}

// --- Performance Test ---
void test_Performance_AsyncWorkload() {
    std::cout << "\n--- Running test_Performance_AsyncWorkload ---" << std::endl;
    setup_test_environment(false); // Manual initialization for performance test

    const long long NUM_INITIAL_DISK_ITEMS = 10000; // As requested
    const long long TOTAL_OPERATIONS = 50000;       // As requested
    const int MAX_KEY_RANGE = NUM_INITIAL_DISK_ITEMS * 2; // Key range for operations
    const int FIND_ALL_RATIO_PERF = 10; // Issue a find_all less frequently to focus on modifications

    auto start_time = std::chrono::high_resolution_clock::now();
    auto last_print_time = start_time;

    // --- Initial Data Population ---
    std::vector<storage_pair_test_t> initial_disk_data_std;
    initial_disk_data_std.reserve(NUM_INITIAL_DISK_ITEMS * 1.1); // Pre-allocate
    std::cout << "PERF: Populating initial disk data with approx " << NUM_INITIAL_DISK_ITEMS << " items..." << std::endl;
    std::mt19937 perf_gen(std::chrono::system_clock::now().time_since_epoch().count()); // Separate generator for perf
    std::uniform_int_distribution<> initial_key_dist(0, MAX_KEY_RANGE);
    std::uniform_int_distribution<> initial_val_dist(0, 10000);

    for (long long i = 0; i < NUM_INITIAL_DISK_ITEMS; ++i) {
        initial_disk_data_std.push_back(norb::make_pair<idx_t_test, val_t_test>(initial_key_dist(perf_gen), initial_val_dist(perf_gen)));
    }
    std::sort(initial_disk_data_std.begin(), initial_disk_data_std.end());
    initial_disk_data_std.erase(std::unique(initial_disk_data_std.begin(), initial_disk_data_std.end()), initial_disk_data_std.end()); // Ensure unique for defined initial state

    run_task_sync(btree_global.initialize_from_vector_async(to_sjtu_vector(initial_disk_data_std)));
    auto init_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> init_duration = init_end_time - start_time;
    std::cout << "PERF: B+Tree initialized with " << initial_disk_data_std.size() << " unique items on disk. Time: "
              << init_duration.count() << " seconds." << std::endl;
    // --- End Initial Data Population ---

    long long operations_issued_count = 0;
    std::vector<wutong::Task<void>> modification_tasks_perf;
    modification_tasks_perf.reserve(TOTAL_OPERATIONS);
    std::vector<wutong::Task<sjtu::vector<storage_pair_test_t>>> find_all_tasks_perf;
    // No expected results stored for performance

    std::uniform_int_distribution<> workload_key_dist(0, MAX_KEY_RANGE);
    std::uniform_int_distribution<> workload_val_dist(0, 20000);
    std::uniform_int_distribution<> op_choice_dist_perf(0, FIND_ALL_RATIO_PERF);

    std::cout << "PERF: Starting to issue " << TOTAL_OPERATIONS << " workload operations..." << std::endl;
    auto workload_issue_start_time = std::chrono::high_resolution_clock::now();

    while (operations_issued_count < TOTAL_OPERATIONS) {
        if (btree_global.permit_operation()) {
            idx_t_test k = workload_key_dist(perf_gen);
            val_t_test v = workload_val_dist(perf_gen);
            int op_choice = op_choice_dist_perf(perf_gen);

            if (op_choice == 0) { // Insert
                modification_tasks_perf.push_back(btree_global.insert_async(k, v));
            } else if (op_choice == 1) { // Delete
                // For perf, just delete a random k,v. No need to ensure it exists.
                modification_tasks_perf.push_back(btree_global.remove_async(k, v));
            } else { // Find All
                idx_t_test find_key = workload_key_dist(perf_gen);
                find_all_tasks_perf.push_back(btree_global.find_all_async(find_key));
            }
            operations_issued_count++;

            if (operations_issued_count % (TOTAL_OPERATIONS / 10) == 0) { // Print progress 10 times
                auto current_time = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed_since_last_print = current_time - last_print_time;
                std::cout << "  PERF: Issued " << operations_issued_count << "/" << TOTAL_OPERATIONS
                          << " ops. (Last segment: " << elapsed_since_last_print.count() << "s)" << std::endl;
                last_print_time = current_time;
            }
        }
        btree_global.poll();
    }
    auto workload_issue_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> issue_duration = workload_issue_end_time - workload_issue_start_time;
    std::cout << "PERF: All " << operations_issued_count << " workload operations issued. Time: "
              << issue_duration.count() << " seconds." << std::endl;

    std::cout << "PERF: Waiting for " << modification_tasks_perf.size() << " modification tasks and "
              << find_all_tasks_perf.size() << " find_all tasks to complete..." << std::endl;
    auto completion_start_time = std::chrono::high_resolution_clock::now();
    last_print_time = completion_start_time;

    for (size_t i = 0; i < modification_tasks_perf.size(); ++i) {
        run_task_sync(std::move(modification_tasks_perf[i]));
        if ((i + 1) % (modification_tasks_perf.size() / 5 + 1) == 0) { // Print progress ~5 times
             auto current_time = std::chrono::high_resolution_clock::now();
             std::chrono::duration<double> elapsed_since_last_print = current_time - last_print_time;
             std::cout << "  PERF: Completed " << (i + 1) << "/" << modification_tasks_perf.size()
                       << " mod tasks. (Last segment: " << elapsed_since_last_print.count() << "s)" << std::endl;
             last_print_time = current_time;
        }
    }
    std::cout << "PERF: All modification tasks complete." << std::endl;

    for (size_t i = 0; i < find_all_tasks_perf.size(); ++i) {
        // We need to consume the result to ensure the task runs
        [[maybe_unused]] sjtu::vector<storage_pair_test_t> res = run_task_sync_with_result(std::move(find_all_tasks_perf[i]));
        if ((i + 1) % (find_all_tasks_perf.size() / 5 + 1) == 0) { // Print progress ~5 times
             auto current_time = std::chrono::high_resolution_clock::now();
             std::chrono::duration<double> elapsed_since_last_print = current_time - last_print_time;
             std::cout << "  PERF: Completed " << (i + 1) << "/" << find_all_tasks_perf.size()
                       << " find_all tasks. (Last segment: " << elapsed_since_last_print.count() << "s)" << std::endl;
             last_print_time = current_time;
        }
    }
    std::cout << "PERF: All find_all tasks complete." << std::endl;
    auto completion_end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> completion_duration = completion_end_time - completion_start_time;
    std::cout << "PERF: Task completion phase time: " << completion_duration.count() << " seconds." << std::endl;

    std::chrono::duration<double> total_workload_duration = completion_end_time - workload_issue_start_time;
    std::cout << "PERF: Total workload (issuing + completion) time: " << total_workload_duration.count() << " seconds." << std::endl;
    if (total_workload_duration.count() > 0) {
        std::cout << "PERF: Approximate operations per second (workload phase): "
                  << TOTAL_OPERATIONS / total_workload_duration.count() << std::endl;
    }

    // Optional: A single final sanity check (can be slow)
    // auto sanity_check_start_time = std::chrono::high_resolution_clock::now();
    // auto sanity_check_task = [&]() -> wutong::Task<void> {
    //     std::cout << "PERF: Performing a final sanity range query..." << std::endl;
    //     [[maybe_unused]] auto res_vec = co_await btree_global.find_range_async(
    //         norb::make_pair<idx_t_test, val_t_test>(0, BTreeTest::val_min),
    //         norb::make_pair<idx_t_test, val_t_test>(MAX_KEY_RANGE + 1, BTreeTest::val_max)
    //     );
    //     // std::cout << "PERF: Sanity query returned " << res_vec.size() << " items." << std::endl;
    // }();
    // run_task_sync(std::move(sanity_check_task));
    // auto sanity_check_end_time = std::chrono::high_resolution_clock::now();
    // std::chrono::duration<double> sanity_duration = sanity_check_end_time - sanity_check_start_time;
    // std::cout << "PERF: Sanity check query time: " << sanity_duration.count() << " seconds." << std::endl;


    teardown_test_environment();
    std::cout << "--- test_Performance_AsyncWorkload FINISHED ---" << std::endl;
}


// --- Main Test Runner ---
int main() {
        // Correctness Tests
        //test_SimpleRead_EmptyTree();
        //test_SimpleRead_OnlyWriteMap();
        //test_SimpleMix_OnlyWriteMap();
        //test_AsyncRead_FromInitializedDisk_Larger();
        //test_AsyncMix_DiskAndWriteMap_Larger();
        //test_MergeLogic_Comprehensive();
        //test_LargeNumberOfOperations_WriteMapHeavy();
        //test_InitializeWithSplitsIfApplicable();
        //test_AsyncWorkload_WithPermitAndPoll();

        // Performance Test
        test_Performance_AsyncWorkload();


        std::cout << "\n*********************************" << std::endl;
        std::cout << "*** ALL TESTS COMPLETED ***" << std::endl;
        std::cout << "(Correctness tests passed, performance test ran)" << std::endl;
        std::cout << "*********************************" << std::endl;

    return 0;
}