#include "persistent_memory_async.hpp" // Assuming this includes all necessary norb headers
#include "tasks.h"                   // For wutong::Task
#include "shared.hpp"                // For constants
#include "stlite/vector.hpp"         // For std::vector

#include <iostream>
#include <vector>
#include <random>
#include <atomic>
#include <thread>     // For std::this_thread::sleep_for
#include <filesystem> // For std::filesystem::remove
#include <algorithm>  // For std::remove_if
#include <cassert>    // For assert
#include <chrono>     // For timing
#include <iomanip>    // For std::fixed and std::setprecision
#include <cmath>      // For std::sqrt in dummy CPU work

// Define PMA_DEBUG and TASK_DEBUG in persistent_memory_async.h or tasks.h if verbose logging is needed
// #define PMA_DEBUG
// #define TASK_DEBUG

// Helper struct for page data
struct PageData {
    norb::page_id_t id;
    char content[128]; // Some payload
    double dummy_data_for_cpu_work; // Data for CPU work

    PageData(norb::page_id_t _id = 0) : id(_id), dummy_data_for_cpu_work(static_cast<double>(_id) + 0.5) {
        snprintf(content, sizeof(content), "PageData for ID %lu. Content verification pattern.", id);
    }
};
static_assert(sizeof(PageData) <= norb::PAGE_SIZE, "PageData too large for PAGE_SIZE");


// Simplified sync_wait for test setup and teardown
template<typename T_Ret>
T_Ret test_sync_wait(wutong::Task<T_Ret> task, norb::PersistentMemoryAsync& pmem_instance) {
    while (task.handle && !task.handle.done()) {
        pmem_instance.poll();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    if (!task.handle) {
        throw std::runtime_error("sync_wait on a null task handle");
    }
    return task.await_resume();
}

void test_sync_wait(wutong::Task<void> task, norb::PersistentMemoryAsync& pmem_instance) {
    while (task.handle && !task.handle.done()) {
        pmem_instance.poll();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
     if (!task.handle) {
        throw std::runtime_error("sync_wait on a null task handle (void)");
    }
    task.await_resume();
}

// Dummy CPU-intensive work
volatile double g_dummy_cpu_result = 0.0; // Use volatile to prevent optimization
void do_cpu_work(double data, int iterations) {
    double result = data;
    for (int i = 0; i < iterations; ++i) {
        result = std::sqrt(std::abs(result) + i) * std::log(std::abs(result) + 1.0 + i);
        // Ensure result doesn't become NaN or Inf too quickly, or stay zero
        if (!std::isfinite(result) || result == 0) result = static_cast<double>(i) + 0.123;
    }
    g_dummy_cpu_result = result; // Assign to volatile to ensure work is done
}


// Coroutine for reading pages with hot/cold access pattern and CPU work
wutong::Task<void> reader_coroutine(
    norb::PersistentMemoryAsync& pmem,
    const std::vector<norb::page_id_t>& hot_set_page_ids,
    const std::vector<norb::page_id_t>& cold_set_page_ids,
    double hot_set_access_probability,
    int num_pages_to_read,
    int cpu_work_iterations,
    std::mt19937& generator,
    std::atomic<long long>& successful_reads_counter,
    std::atomic<long long>& hot_reads_counter) {

    std::uniform_real_distribution<> access_type_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> hot_idx_distrib(0, hot_set_page_ids.empty() ? 0 : hot_set_page_ids.size() - 1);
    std::uniform_int_distribution<size_t> cold_idx_distrib(0, cold_set_page_ids.empty() ? 0 : cold_set_page_ids.size() - 1);

    for (int i = 0; i < num_pages_to_read; ++i) {
        norb::page_id_t page_id_to_read;
        bool accessed_hot = false;

        if (!hot_set_page_ids.empty() && (cold_set_page_ids.empty() || access_type_dist(generator) < hot_set_access_probability)) {
            page_id_to_read = hot_set_page_ids[hot_idx_distrib(generator)];
            accessed_hot = true;
        } else if (!cold_set_page_ids.empty()) {
            page_id_to_read = cold_set_page_ids[cold_idx_distrib(generator)];
        } else {
            // Should not happen if at least one set is non-empty
            if (!hot_set_page_ids.empty()) page_id_to_read = hot_set_page_ids[0];
            else if (!cold_set_page_ids.empty()) page_id_to_read = cold_set_page_ids[0];
            else co_return; // No pages to read
        }

        norb::PersistentMemoryAsync::Handle<PageData> handle =
            norb::PersistentMemoryAsync::fetch_handle<PageData>(page_id_to_read);

        norb::PersistentMemoryAsync::ConstHandledReference<PageData> ref = co_await handle.const_ref();
        
        assert(ref->id == page_id_to_read); 
        char expected_content[128];
        snprintf(expected_content, sizeof(expected_content), "PageData for ID %lu. Content verification pattern.", page_id_to_read);
        assert(strcmp(ref->content, expected_content) == 0);

        // Perform CPU work
        if (cpu_work_iterations > 0) {
            do_cpu_work(ref->dummy_data_for_cpu_work, cpu_work_iterations);
        }

        successful_reads_counter++;
        if (accessed_hot) {
            hot_reads_counter++;
        }
    }
    co_return;
}

bool run_concurrent_read_test() {
    // 0. Cleanup previous test files (if any)
    std::cout << "Cleaning up old test files..." << std::endl;
    std::filesystem::remove(norb::PMEM_FILE_NAME);
    std::filesystem::remove(std::string(norb::PMEM_FILE_NAME) + ".config");
    std::cout << "Cleanup done." << std::endl;

    norb::PersistentMemoryAsync& pmem = norb::PersistentMemoryAsync::get_instance();

    // Test Parameters
    const norb::page_id_t TOTAL_PAGES_TO_CREATE = norb::PersistentMemoryAsync::SLOT_COUNT * 5; // e.g., 5120 if SLOT_COUNT = 1024
    const int NUM_READER_COROUTINES = norb::PersistentMemoryAsync::SLOT_COUNT * 10; // e.g., 2048
    const int PAGES_PER_CORO_MIN = 5;
    const int PAGES_PER_CORO_MAX = 10;
    
    const double HOT_SET_PERCENTAGE = 0.20; // 20% of pages are "hot"
    const double HOT_SET_ACCESS_PROBABILITY = 0.80; // 80% of accesses go to the hot set
    const int CPU_WORK_ITERATIONS = 100; // Number of iterations for dummy CPU work per page read

    std::cout << "Test Configuration:" << std::endl;
    std::cout << "  Total Pages in DB: " << TOTAL_PAGES_TO_CREATE << std::endl;
    std::cout << "  Buffer Pool Slots (SLOT_COUNT): " << norb::PersistentMemoryAsync::SLOT_COUNT << std::endl;
    std::cout << "  Number of Reader Coroutines: " << NUM_READER_COROUTINES << std::endl;
    std::cout << "  Pages Read per Coroutine: " << PAGES_PER_CORO_MIN << "-" << PAGES_PER_CORO_MAX << std::endl;
    std::cout << "  LRU_K_INDEX: " << norb::LRU_K_INDEX << std::endl;
    std::cout << "  Hot Set Percentage: " << HOT_SET_PERCENTAGE * 100 << "%" << std::endl;
    std::cout << "  Hot Set Access Probability: " << HOT_SET_ACCESS_PROBABILITY * 100 << "%" << std::endl;
    std::cout << "  CPU Work Iterations per Page: " << CPU_WORK_ITERATIONS << std::endl;


    std::vector<norb::page_id_t> all_managed_page_ids;
    all_managed_page_ids.reserve(TOTAL_PAGES_TO_CREATE);
    std::vector<norb::page_id_t> hot_set_page_ids;
    std::vector<norb::page_id_t> cold_set_page_ids;

    norb::page_id_t num_hot_pages = static_cast<norb::page_id_t>(TOTAL_PAGES_TO_CREATE * HOT_SET_PERCENTAGE);
    if (num_hot_pages == 0 && TOTAL_PAGES_TO_CREATE > 0 && HOT_SET_PERCENTAGE > 0) num_hot_pages = 1; // Ensure at least one hot page if configured

    // 1. Populate data
    std::cout << "Populating persistent memory with " << TOTAL_PAGES_TO_CREATE << " pages..." << std::endl;
    auto init_start_time = std::chrono::high_resolution_clock::now();
    for (norb::page_id_t i = 0; i < TOTAL_PAGES_TO_CREATE; ++i) {
        auto handle = test_sync_wait(norb::PersistentMemoryAsync::create_and_init<PageData>(i), pmem);
        all_managed_page_ids.push_back(handle.page_id);
        if (i < num_hot_pages) {
            hot_set_page_ids.push_back(handle.page_id);
        } else {
            cold_set_page_ids.push_back(handle.page_id);
        }
        if ((i + 1) % (TOTAL_PAGES_TO_CREATE / 20 == 0 ? 1 : TOTAL_PAGES_TO_CREATE / 20) == 0 || (i+1) == TOTAL_PAGES_TO_CREATE) {
            std::cout << "  Initialized " << (i + 1) << "/" << TOTAL_PAGES_TO_CREATE << " pages." << std::endl;
        }
    }
    test_sync_wait(pmem.flush_all(), pmem);
    auto init_end_time = std::chrono::high_resolution_clock::now();
    double init_duration_s = std::chrono::duration<double>(init_end_time - init_start_time).count();
    std::cout << "Data population complete in " << std::fixed << std::setprecision(3) << init_duration_s << " seconds. "
              << "Total pages in PMA: " << pmem.get_page_count() << std::endl;
    std::cout << "  Hot set size: " << hot_set_page_ids.size() << " pages." << std::endl;
    std::cout << "  Cold set size: " << cold_set_page_ids.size() << " pages." << std::endl;


    if (pmem.get_page_count() < TOTAL_PAGES_TO_CREATE) {
        std::cerr << "FAILURE: PMA page count (" << pmem.get_page_count() 
                  << ") less than expected (" << TOTAL_PAGES_TO_CREATE << ")" << std::endl;
        return false;
    }


    // 2. Launch and manage reader coroutines
    std::atomic<long long> total_successful_reads(0);
    std::atomic<long long> total_hot_reads(0);
    std::atomic<int> completed_coroutines_count(0); 
    int launched_coroutines_count = 0;

    std::vector<wutong::Task<void>> active_reader_tasks;
    active_reader_tasks.reserve(NUM_READER_COROUTINES);

    std::random_device rd;
    std::mt19937 main_generator(rd()); 
    std::uniform_int_distribution<> pages_to_read_dist(PAGES_PER_CORO_MIN, PAGES_PER_CORO_MAX);

    std::cout << "Starting main event loop to launch and poll reader coroutines..." << std::endl;
    auto event_loop_start_time = std::chrono::high_resolution_clock::now();

    while (launched_coroutines_count < NUM_READER_COROUTINES || !active_reader_tasks.empty()) {
        pmem.poll(); 

        if (launched_coroutines_count < NUM_READER_COROUTINES && pmem.permit_IO()) {
            int num_pages = pages_to_read_dist(main_generator);
            active_reader_tasks.push_back(
                reader_coroutine(pmem, hot_set_page_ids, cold_set_page_ids, 
                                 HOT_SET_ACCESS_PROBABILITY, num_pages, CPU_WORK_ITERATIONS,
                                 main_generator, total_successful_reads, total_hot_reads)
            );
            launched_coroutines_count++;
            if (launched_coroutines_count % (NUM_READER_COROUTINES / 20 == 0 ? 1 : NUM_READER_COROUTINES / 20) == 0 || launched_coroutines_count == NUM_READER_COROUTINES) {
                 std::cout << "[Event Loop] Launched " << launched_coroutines_count << "/" << NUM_READER_COROUTINES << " coroutines." << std::endl;
            }
        }

        active_reader_tasks.erase(
            std::remove_if(active_reader_tasks.begin(), active_reader_tasks.end(),
                [&](wutong::Task<void>& task_item) {
                    if (task_item.handle && task_item.handle.done()) {
                        try {
                            task_item.await_resume(); 
                            completed_coroutines_count++; 
                        } catch (const std::exception& e) {
                            std::cerr << "[Event Loop] Exception from a reader_coroutine: " << e.what() << std::endl;
                        }
                        return true; 
                    }
                    return false; 
                }),
            active_reader_tasks.end()
        );

        if (!(launched_coroutines_count < NUM_READER_COROUTINES && pmem.permit_IO())) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    pmem.poll();
    auto event_loop_end_time = std::chrono::high_resolution_clock::now();
    std::cout << "Main event loop finished. All coroutines launched and processed." << std::endl;

    // Performance Metrics
    double event_loop_duration_s = std::chrono::duration<double>(event_loop_end_time - event_loop_start_time).count();
    double page_reads_per_second = (event_loop_duration_s > 0) ? (total_successful_reads.load() / event_loop_duration_s) : 0;
    double coroutines_per_second = (event_loop_duration_s > 0) ? (completed_coroutines_count.load() / event_loop_duration_s) : 0;
    double actual_hot_access_ratio = (total_successful_reads.load() > 0) ? (static_cast<double>(total_hot_reads.load()) / total_successful_reads.load()) : 0.0;


    std::cout << "\nPerformance Evaluation:" << std::endl;
    std::cout << "  Event Loop Duration: " << std::fixed << std::setprecision(3) << event_loop_duration_s << " seconds" << std::endl;
    std::cout << "  Total Successful Page Reads: " << total_successful_reads.load() << std::endl;
    std::cout << "  Page Reads Per Second (RPS): " << std::fixed << std::setprecision(2) << page_reads_per_second << std::endl;
    std::cout << "  Completed Coroutines: " << completed_coroutines_count.load() << std::endl;
    std::cout << "  Coroutines Per Second (CPS): " << std::fixed << std::setprecision(2) << coroutines_per_second << std::endl;
    std::cout << "  Total Hot Set Reads: " << total_hot_reads.load() << std::endl;
    std::cout << "  Actual Hot Set Access Ratio: " << std::fixed << std::setprecision(3) << actual_hot_access_ratio 
              << " (Expected ~" << HOT_SET_ACCESS_PROBABILITY << ")" << std::endl;


    // 3. Verification
    std::cout << "\nVerification phase..." << std::endl;
    std::cout << "  Total successful page reads: " << total_successful_reads.load() << std::endl;
    std::cout << "  Completed (successfully) reader coroutines: " << completed_coroutines_count.load() << std::endl;
    std::cout << "  Launched reader coroutines: " << launched_coroutines_count << std::endl;

    bool success = true;
    long long expected_min_total_reads = (long long)NUM_READER_COROUTINES * PAGES_PER_CORO_MIN;
    long long expected_max_total_reads = (long long)NUM_READER_COROUTINES * PAGES_PER_CORO_MAX;

    if (completed_coroutines_count.load() != NUM_READER_COROUTINES) {
        std::cerr << "FAILURE: Completed coroutines (" << completed_coroutines_count.load() 
                  << ") does not match expected (" << NUM_READER_COROUTINES << ")" << std::endl;
        success = false;
    }
    if (launched_coroutines_count != NUM_READER_COROUTINES) {
        std::cerr << "FAILURE: Launched coroutines (" << launched_coroutines_count 
                  << ") does not match expected (" << NUM_READER_COROUTINES << ")" << std::endl;
        success = false;
    }
    if (total_successful_reads.load() < expected_min_total_reads || total_successful_reads.load() > expected_max_total_reads) {
        std::cerr << "FAILURE: Total successful reads (" << total_successful_reads.load() 
                  << ") is outside the expected range [" << expected_min_total_reads 
                  << ", " << expected_max_total_reads << "]" << std::endl;
        success = false;
    }
    // Verify hot set access ratio is reasonably close to expected
    // Allow some leeway due to randomness, e.g., +/- 5% of the expected probability range
    double expected_hot_ratio_lower_bound = HOT_SET_ACCESS_PROBABILITY - 0.05;
    double expected_hot_ratio_upper_bound = HOT_SET_ACCESS_PROBABILITY + 0.05;
    if (total_successful_reads.load() > 0 && (actual_hot_access_ratio < expected_hot_ratio_lower_bound || actual_hot_access_ratio > expected_hot_ratio_upper_bound)) {
         std::cerr << "WARNING: Actual hot set access ratio (" << actual_hot_access_ratio 
                   << ") is outside the expected range [" << expected_hot_ratio_lower_bound 
                   << ", " << expected_hot_ratio_upper_bound << "]. This might be due to randomness or an issue." << std::endl;
        // Not marking as failure for now, as it's probabilistic, but good to note.
    }


    std::cout << "  Expected total reads between " << expected_min_total_reads << " and " << expected_max_total_reads << ". Actual: " << total_successful_reads.load() << std::endl;


    // 4. Cleanup
    std::cout << "\nFlushing PMA and cleaning up test files..." << std::endl;
    test_sync_wait(pmem.flush_all(), pmem);

    std::filesystem::remove(norb::PMEM_FILE_NAME);
    std::filesystem::remove(std::string(norb::PMEM_FILE_NAME) + ".config");
    std::cout << "Test files cleaned up." << std::endl;

    return success;
}


int main() {
    if (run_concurrent_read_test()) {
        std::cout << "\nTest PASSED." << std::endl;
        return 0;
    } else {
        std::cerr << "\nTest FAILED." << std::endl;
        return 1;
    }
}