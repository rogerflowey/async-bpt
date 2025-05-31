#pragma once

#include "async_bpt.h" // Assuming this is the path to your B+ tree header
#include "stlite/filed_config.hpp"
#include "stlite/map.hpp"
#include "stlite/pair.hpp"
#include "stlite/vector.hpp"
#include "tasks.h" // wutong::Task
#include "utils.hpp" // norb::make_pair

#include <algorithm> // For std::sort, std::unique
#include <chrono>    // For timing
#include <filesystem> // For file cleanup
#include <iostream>
#include <random>
#include <set>
#include <stdexcept> // For runtime_error
#include <string>
#include <vector>
#include <thread> // For std::this_thread::yield


// --- Global Test Configuration ---
// Defined as inline const for C++17+ to allow definition in header
// and avoid multiple definition errors if files were ever linked together.
// For separate compilation into different executables, this is also fine.
inline const std::string PMA_FILE_NAME = "pma_test_file.dat";
inline const std::string FILED_CONFIG_FILE_NAME = "default_filed_config.dat";

// --- Typedefs for Test ---
#ifndef TEMPLATE_CHECK
using idx_t_test = int;
using val_t_test = int;
#else
// Use types from TEMPLATE_CHECK if defined elsewhere
using idx_t_test = idx_t;
using val_t_test = val_t;
#endif

using storage_pair_test_t = norb::Pair<idx_t_test, val_t_test>;
using BTreeTest = norb::AsyncBPlusTree<idx_t_test, val_t_test>;
using RefModelTest = std::set<storage_pair_test_t>;


// --- Task Execution Helpers ---
inline void run_task_sync(wutong::Task<void> task, BTreeTest& btree_instance) {
    if (!task.handle) {
        return;
    }
    while (task.handle && !task.handle.done()) {
        btree_instance.poll();
    }
    if (task.handle && task.handle.promise().exception_ptr_) {
        std::rethrow_exception(task.handle.promise().exception_ptr_);
    }
}

template <typename T>
T run_task_sync_with_result(wutong::Task<T> task, BTreeTest& btree_instance) {
    if (!task.handle) {
        throw std::runtime_error("Attempted to run an invalid or moved-from task for result.");
    }
    while (task.handle && !task.handle.done()) {
        btree_instance.poll();
    }
    if (!task.handle) {
         throw std::runtime_error("Task handle became null unexpectedly during execution.");
    }
    if (task.handle.promise().exception_ptr_) {
        std::rethrow_exception(task.handle.promise().exception_ptr_);
    }
    return task.await_resume();
}


// --- Utility and Assertion Helpers (remain the same) ---
inline std::vector<storage_pair_test_t> to_std_vector(const sjtu::vector<storage_pair_test_t>& sjtu_vec) {
    std::vector<storage_pair_test_t> std_vec;
    std_vec.reserve(sjtu_vec.size());
    for (const auto& item : sjtu_vec) {
        std_vec.push_back(item);
    }
    std::sort(std_vec.begin(), std_vec.end());
    return std_vec;
}

inline sjtu::vector<storage_pair_test_t> to_sjtu_vector(const std::vector<storage_pair_test_t>& std_vec) {
    sjtu::vector<storage_pair_test_t> sjtu_vec;
    for (const auto& item : std_vec) {
        sjtu_vec.push_back(item);
    }
    return sjtu_vec;
}

inline std::vector<storage_pair_test_t> to_std_vector(const RefModelTest& ref_set) {
    std::vector<storage_pair_test_t> std_vec(ref_set.begin(), ref_set.end());
    return std_vec;
}

inline void custom_assert(bool condition, const std::string& message, const char* file, int line) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED [" << file << ":" << line << "]: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

#define ASSERT_TRUE(cond, msg) custom_assert(cond, msg, __FILE__, __LINE__)

inline void ASSERT_EQ_VEC(const std::vector<storage_pair_test_t>& actual,
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


// --- Test Environment Setup/Teardown (remain the same) ---
inline void setup_test_environment(BTreeTest& btree, RefModelTest& ref_model, bool clean_files = true, bool initialize_btree_empty = true) {
    std::cout << "Setting up test environment..." << std::endl;
    if (clean_files) {
        std::filesystem::remove(PMA_FILE_NAME);
        std::filesystem::remove(FILED_CONFIG_FILE_NAME);
    }
    // PMA and FiledConfig singletons are expected to be fresh or re-initialize correctly
    // when a new BTreeTest instance is created or when files are cleaned.

    if (initialize_btree_empty) {
        run_task_sync(btree.initialize_from_vector_async({}), btree);
    }
    ref_model.clear();
    std::cout << "Test environment set up." << std::endl;
}

inline void teardown_test_environment(bool clean_files = true) {
    std::cout << "Tearing down test environment..." << std::endl;
    if (clean_files) {
        std::filesystem::remove(PMA_FILE_NAME);
        std::filesystem::remove(FILED_CONFIG_FILE_NAME);
    }
    std::cout << "Test environment torn down." << std::endl;
}

// --- Operation Helpers (remain the same) ---
inline wutong::Task<void> apply_insert_op(BTreeTest& btree, RefModelTest& ref_model, idx_t_test k, val_t_test v) {
    storage_pair_test_t p = norb::make_pair(k, v);
    co_await btree.insert_async(k, v);
    ref_model.insert(p);
}

inline wutong::Task<void> apply_remove_op(BTreeTest& btree, RefModelTest& ref_model, idx_t_test k, val_t_test v) {
    storage_pair_test_t p = norb::make_pair(k, v);
    co_await btree.remove_async(k, v);
    ref_model.erase(p);
}

// --- Verification Helpers (remain the same) ---
inline wutong::Task<void> verify_range_query(BTreeTest& btree, const RefModelTest& ref_model,
                                      idx_t_test k_start, val_t_test v_start,
                                      idx_t_test k_end, val_t_test v_end,
                                      const std::string& query_context) {
    storage_pair_test_t range_s = norb::make_pair(k_start, BTreeTest::val_min);
    if (v_start != BTreeTest::val_min) range_s.second = v_start;

    storage_pair_test_t range_e = norb::make_pair(k_end, BTreeTest::val_max);
    if (v_end != BTreeTest::val_max) range_e.second = v_end;

    sjtu::vector<storage_pair_test_t> btree_res_sjtu = co_await btree.find_range_async(range_s, range_e);
    std::vector<storage_pair_test_t> btree_res = to_std_vector(btree_res_sjtu);

    RefModelTest expected_in_range_set;
    for (const auto& p_ref : ref_model) {
        if (p_ref >= range_s && p_ref < range_e) {
            expected_in_range_set.insert(p_ref);
        }
    }
    std::vector<storage_pair_test_t> ref_res = to_std_vector(expected_in_range_set);

    ASSERT_EQ_VEC(btree_res, ref_res, "Range Query: " + query_context);
}

inline wutong::Task<void> verify_all_query(BTreeTest& btree, const RefModelTest& ref_model,
                                    idx_t_test key, const std::string& query_context) {
    sjtu::vector<storage_pair_test_t> btree_res_sjtu = co_await btree.find_all_async(key);
    std::vector<storage_pair_test_t> btree_res = to_std_vector(btree_res_sjtu);

    RefModelTest expected_for_key_set;
    for (const auto& p_ref : ref_model) {
        if (p_ref.first == key) {
            expected_for_key_set.insert(p_ref);
        }
    }
    std::vector<storage_pair_test_t> ref_res = to_std_vector(expected_for_key_set);

    ASSERT_EQ_VEC(btree_res, ref_res, "Find_All Query for key " + std::to_string(key) + ": " + query_context);
}

inline wutong::Task<bool> is_bpt_empty_after_flush(BTreeTest& btree) {
    if (btree.tree_height_.val == 0) co_return true;
    storage_pair_test_t wide_range_start = {std::numeric_limits<idx_t_test>::min(), BTreeTest::val_min};
    storage_pair_test_t wide_range_end = {std::numeric_limits<idx_t_test>::max(), BTreeTest::val_max};
    sjtu::vector<storage_pair_test_t> res = co_await btree.find_range_async(wide_range_start, wide_range_end);
    co_return res.empty();
}