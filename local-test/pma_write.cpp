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

// --- PageData for Mixed Workload ---
struct PageDataMixed {
    norb::page_id_t id;          // Static
    char static_content[64];     // Static part of content, ensures some fixed data
    double dynamic_value;        // Value modified by writers
    unsigned int write_count;    // Incremented by writers
    char padding[64];            // Ensure page has some more bulk

    PageDataMixed(norb::page_id_t _id = 0)
        : id(_id), dynamic_value(static_cast<double>(_id) + 0.5), write_count(0) {
        snprintf(static_content, sizeof(static_content), "PageDataMixed ID %lu", id);
        memset(padding, (char)(_id % 256), sizeof(padding)); // Some dummy padding
    }
};
static_assert(sizeof(PageDataMixed) <= norb::PAGE_SIZE, "PageDataMixed too large for PAGE_SIZE");


// --- Helper Functions (Copied from previous test) ---
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

volatile double g_dummy_cpu_result_mixed = 0.0;
void do_cpu_work_mixed(double data, int iterations) {
    double result = data;
    for (int i = 0; i < iterations; ++i) {
        result = std::sqrt(std::abs(result) + i + 0.1) * std::log(std::abs(result) + 1.1 + i);
        if (!std::isfinite(result) || result == 0) result = static_cast<double>(i) + 0.456;
    }
    g_dummy_cpu_result_mixed = result;
}

// --- Reader Coroutine ---
wutong::Task<void> reader_coroutine_mixed(
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
        } else if (!hot_set_page_ids.empty()) { // Fallback if cold set is empty but hot is not
             page_id_to_read = hot_set_page_ids[hot_idx_distrib(generator)];
             accessed_hot = true;
        }
        else { co_return; } // No pages to read

        norb::PersistentMemoryAsync::Handle<PageDataMixed> handle =
            norb::PersistentMemoryAsync::fetch_handle<PageDataMixed>(page_id_to_read);

        norb::PersistentMemoryAsync::ConstHandledReference<PageDataMixed> ref = co_await handle.const_ref();
        
        assert(ref->id == page_id_to_read); 
        char expected_static_content[64];
        snprintf(expected_static_content, sizeof(expected_static_content), "PageDataMixed ID %lu", page_id_to_read);
        assert(strcmp(ref->static_content, expected_static_content) == 0);
        // We don't assert dynamic_value or write_count as they are modified by writers.

        if (cpu_work_iterations > 0) {
            do_cpu_work_mixed(ref->dynamic_value, cpu_work_iterations);
        }

        successful_reads_counter++;
        if (accessed_hot) {
            hot_reads_counter++;
        }
    }
    co_return;
}

// --- Writer Coroutine ---
wutong::Task<void> writer_coroutine_mixed(
    norb::PersistentMemoryAsync& pmem,
    const std::vector<norb::page_id_t>& hot_set_page_ids,
    const std::vector<norb::page_id_t>& cold_set_page_ids,
    double hot_set_access_probability, // Writers can also have a hot/cold preference
    int num_pages_to_write,
    int cpu_work_iterations,
    std::mt19937& generator,
    std::atomic<long long>& successful_writes_counter,
    std::atomic<long long>& hot_writes_counter) {

    std::uniform_real_distribution<> access_type_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> hot_idx_distrib(0, hot_set_page_ids.empty() ? 0 : hot_set_page_ids.size() - 1);
    std::uniform_int_distribution<size_t> cold_idx_distrib(0, cold_set_page_ids.empty() ? 0 : cold_set_page_ids.size() - 1);

    for (int i = 0; i < num_pages_to_write; ++i) {
        norb::page_id_t page_id_to_write;
        bool accessed_hot = false;

        if (!hot_set_page_ids.empty() && (cold_set_page_ids.empty() || access_type_dist(generator) < hot_set_access_probability)) {
            page_id_to_write = hot_set_page_ids[hot_idx_distrib(generator)];
            accessed_hot = true;
        } else if (!cold_set_page_ids.empty()) {
            page_id_to_write = cold_set_page_ids[cold_idx_distrib(generator)];
        } else if (!hot_set_page_ids.empty()) {
             page_id_to_write = hot_set_page_ids[hot_idx_distrib(generator)];
             accessed_hot = true;
        }
        else { co_return; }

        norb::PersistentMemoryAsync::Handle<PageDataMixed> handle =
            norb::PersistentMemoryAsync::fetch_handle<PageDataMixed>(page_id_to_write);

        // Get mutable reference
        norb::PersistentMemoryAsync::HandledReference<PageDataMixed> ref = co_await handle.ref();
        
        assert(ref->id == page_id_to_write); // Verify ID before modification

        // Modify page data
        ref->dynamic_value += 1.0;
        ref->write_count++;
        // static_content remains unchanged

        if (cpu_work_iterations > 0) {
            do_cpu_work_mixed(ref->dynamic_value, cpu_work_iterations);
        }
        // ref goes out of scope, marking page as dirty

        successful_writes_counter++;
        if (accessed_hot) {
            hot_writes_counter++;
        }
    }
    co_return;
}


// --- Main Test Function ---
bool run_mixed_read_write_test() {
    std::cout << "--- Starting Mixed Read/Write Test ---" << std::endl;
    // 0. Cleanup
    std::filesystem::remove(norb::PMEM_FILE_NAME);
    std::filesystem::remove(std::string(norb::PMEM_FILE_NAME) + ".config");

    norb::PersistentMemoryAsync& pmem = norb::PersistentMemoryAsync::get_instance();

    // Test Parameters
    const int NUM_CYCLES = 3; // Number of Read-Write cycles
    const norb::page_id_t TOTAL_PAGES_TO_CREATE = norb::PersistentMemoryAsync::SLOT_COUNT * 3; // Smaller set for more contention
    
    const int NUM_READER_COROUTINES_PER_PHASE = norb::PersistentMemoryAsync::SLOT_COUNT*2;
    const int PAGES_PER_READER_CORO_MIN = 3;
    const int PAGES_PER_READER_CORO_MAX = 7;
    const int READER_CPU_WORK_ITERATIONS = 50;

    const int NUM_WRITER_COROUTINES_PER_PHASE = norb::PersistentMemoryAsync::SLOT_COUNT / 2;
    const int PAGES_PER_WRITER_CORO_MIN = 2;
    const int PAGES_PER_WRITER_CORO_MAX = 5;
    const int WRITER_CPU_WORK_ITERATIONS = 70;
    
    const double HOT_SET_PERCENTAGE = 0.25; 
    const double READER_HOT_SET_ACCESS_PROB = 0.75;
    const double WRITER_HOT_SET_ACCESS_PROB = 0.60; // Writers might have a slightly different hot set affinity

    std::cout << "Test Configuration:" << std::endl;
    std::cout << "  Cycles: " << NUM_CYCLES << std::endl;
    std::cout << "  Total Pages in DB: " << TOTAL_PAGES_TO_CREATE << std::endl;
    std::cout << "  Buffer Slots: " << norb::PersistentMemoryAsync::SLOT_COUNT << ", LRU_K: " << norb::LRU_K_INDEX << std::endl;
    std::cout << "  Readers/Phase: " << NUM_READER_COROUTINES_PER_PHASE << ", Pages/Reader: " << PAGES_PER_READER_CORO_MIN << "-" << PAGES_PER_READER_CORO_MAX << ", CPU Work: " << READER_CPU_WORK_ITERATIONS << std::endl;
    std::cout << "  Writers/Phase: " << NUM_WRITER_COROUTINES_PER_PHASE << ", Pages/Writer: " << PAGES_PER_WRITER_CORO_MIN << "-" << PAGES_PER_WRITER_CORO_MAX << ", CPU Work: " << WRITER_CPU_WORK_ITERATIONS << std::endl;
    std::cout << "  Hot Set: " << HOT_SET_PERCENTAGE * 100 << "%, Reader Hot Access: " << READER_HOT_SET_ACCESS_PROB * 100 << "%, Writer Hot Access: " << WRITER_HOT_SET_ACCESS_PROB * 100 << "%" << std::endl;

    std::vector<norb::page_id_t> all_managed_page_ids;
    std::vector<norb::page_id_t> hot_set_page_ids;
    std::vector<norb::page_id_t> cold_set_page_ids;
    norb::page_id_t num_hot_pages = static_cast<norb::page_id_t>(TOTAL_PAGES_TO_CREATE * HOT_SET_PERCENTAGE);
    if (num_hot_pages == 0 && TOTAL_PAGES_TO_CREATE > 0 && HOT_SET_PERCENTAGE > 0) num_hot_pages = 1;


    // 1. Populate data
    std::cout << "\nPopulating " << TOTAL_PAGES_TO_CREATE << " pages..." << std::endl;
    auto init_start_time = std::chrono::high_resolution_clock::now();
    for (norb::page_id_t i = 0; i < TOTAL_PAGES_TO_CREATE; ++i) {
        auto handle = test_sync_wait(norb::PersistentMemoryAsync::create_and_init<PageDataMixed>(i), pmem);
        all_managed_page_ids.push_back(handle.page_id);
        if (i < num_hot_pages) hot_set_page_ids.push_back(handle.page_id);
        else cold_set_page_ids.push_back(handle.page_id);
    }
    test_sync_wait(pmem.flush_all(), pmem);
    auto init_end_time = std::chrono::high_resolution_clock::now();
    std::cout << "Data population complete in " << std::chrono::duration<double>(init_end_time - init_start_time).count() << "s." << std::endl;
    std::cout << "  Hot set: " << hot_set_page_ids.size() << " pages, Cold set: " << cold_set_page_ids.size() << " pages." << std::endl;

    // Overall Test Statistics
    std::atomic<long long> grand_total_reads(0), grand_total_hot_reads(0);
    std::atomic<long long> grand_total_writes(0), grand_total_hot_writes(0);
    std::atomic<int> grand_total_reader_coros_completed(0);
    std::atomic<int> grand_total_writer_coros_completed(0);
    double total_read_phase_duration = 0.0;
    double total_write_phase_duration = 0.0;

    std::random_device rd;
    std::mt19937 main_generator(rd());

    auto overall_test_start_time = std::chrono::high_resolution_clock::now();

    // 2. Phased Execution
    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        std::cout << "\n--- Cycle " << cycle + 1 << "/" << NUM_CYCLES << " ---" << std::endl;

        // --- Read Phase ---
        std::cout << "  Starting Read Phase..." << std::endl;
        auto read_phase_start_time = std::chrono::high_resolution_clock::now();
        std::vector<wutong::Task<void>> active_read_tasks;
        active_read_tasks.reserve(NUM_READER_COROUTINES_PER_PHASE);
        int launched_readers = 0;
        int completed_readers_in_phase = 0;
        std::uniform_int_distribution<> pages_per_reader_dist(PAGES_PER_READER_CORO_MIN, PAGES_PER_READER_CORO_MAX);

        while(launched_readers < NUM_READER_COROUTINES_PER_PHASE || !active_read_tasks.empty()) {
            pmem.poll();
            if (launched_readers < NUM_READER_COROUTINES_PER_PHASE && pmem.permit_IO()) {
                int num_pages = pages_per_reader_dist(main_generator);
                active_read_tasks.push_back(reader_coroutine_mixed(pmem, hot_set_page_ids, cold_set_page_ids,
                                                                  READER_HOT_SET_ACCESS_PROB, num_pages, READER_CPU_WORK_ITERATIONS,
                                                                  main_generator, grand_total_reads, grand_total_hot_reads));
                launched_readers++;
            }
            active_read_tasks.erase(std::remove_if(active_read_tasks.begin(), active_read_tasks.end(),
                [&](auto& task){ if(task.handle && task.handle.done()){ try{task.await_resume(); completed_readers_in_phase++;} catch(const std::exception& e){std::cerr<<"Ex in Read: "<<e.what()<<std::endl;} return true;} return false;}), active_read_tasks.end());
            if (!(launched_readers < NUM_READER_COROUTINES_PER_PHASE && pmem.permit_IO())) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        auto read_phase_end_time = std::chrono::high_resolution_clock::now();
        double read_duration = std::chrono::duration<double>(read_phase_end_time - read_phase_start_time).count();
        total_read_phase_duration += read_duration;
        grand_total_reader_coros_completed += completed_readers_in_phase;
        std::cout << "  Read Phase completed in " << read_duration << "s. Readers completed: " << completed_readers_in_phase << std::endl;
        assert(completed_readers_in_phase == NUM_READER_COROUTINES_PER_PHASE);

        // --- Write Phase ---
        std::cout << "  Starting Write Phase..." << std::endl;
        auto write_phase_start_time = std::chrono::high_resolution_clock::now();
        std::vector<wutong::Task<void>> active_write_tasks;
        active_write_tasks.reserve(NUM_WRITER_COROUTINES_PER_PHASE);
        int launched_writers = 0;
        int completed_writers_in_phase = 0;
        std::uniform_int_distribution<> pages_per_writer_dist(PAGES_PER_WRITER_CORO_MIN, PAGES_PER_WRITER_CORO_MAX);

        while(launched_writers < NUM_WRITER_COROUTINES_PER_PHASE || !active_write_tasks.empty()) {
            pmem.poll();
            if (launched_writers < NUM_WRITER_COROUTINES_PER_PHASE && pmem.permit_IO()) {
                int num_pages = pages_per_writer_dist(main_generator);
                active_write_tasks.push_back(writer_coroutine_mixed(pmem, hot_set_page_ids, cold_set_page_ids,
                                                                   WRITER_HOT_SET_ACCESS_PROB, num_pages, WRITER_CPU_WORK_ITERATIONS,
                                                                   main_generator, grand_total_writes, grand_total_hot_writes));
                launched_writers++;
            }
            active_write_tasks.erase(std::remove_if(active_write_tasks.begin(), active_write_tasks.end(),
                [&](auto& task){ if(task.handle && task.handle.done()){ try{task.await_resume(); completed_writers_in_phase++;} catch(const std::exception& e){std::cerr<<"Ex in Write: "<<e.what()<<std::endl;} return true;} return false;}), active_write_tasks.end());
            if (!(launched_writers < NUM_WRITER_COROUTINES_PER_PHASE && pmem.permit_IO())) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        auto write_phase_end_time = std::chrono::high_resolution_clock::now();
        double write_duration = std::chrono::duration<double>(write_phase_end_time - write_phase_start_time).count();
        total_write_phase_duration += write_duration;
        grand_total_writer_coros_completed += completed_writers_in_phase;
        std::cout << "  Write Phase completed in " << write_duration << "s. Writers completed: " << completed_writers_in_phase << std::endl;
        assert(completed_writers_in_phase == NUM_WRITER_COROUTINES_PER_PHASE);
    }
    auto overall_test_end_time = std::chrono::high_resolution_clock::now();
    double overall_duration = std::chrono::duration<double>(overall_test_end_time - overall_test_start_time).count();

    // 3. Performance Summary
    std::cout << "\n--- Performance Summary (Mixed Workload) ---" << std::endl;
    std::cout << "  Overall Test Duration: " << std::fixed << std::setprecision(3) << overall_duration << "s" << std::endl;
    std::cout << "  Total Read Phase Duration: " << total_read_phase_duration << "s" << std::endl;
    std::cout << "  Total Write Phase Duration: " << total_write_phase_duration << "s" << std::endl;

    std::cout << "  Grand Total Reads: " << grand_total_reads.load() << " (Hot: " << grand_total_hot_reads.load() << ")" << std::endl;
    std::cout << "  Avg RPS (during read phases): " << (total_read_phase_duration > 0 ? grand_total_reads.load() / total_read_phase_duration : 0) << std::endl;
    std::cout << "  Avg Reader CPS (during read phases): " << (total_read_phase_duration > 0 ? grand_total_reader_coros_completed.load() / total_read_phase_duration : 0) << std::endl;
    
    std::cout << "  Grand Total Writes: " << grand_total_writes.load() << " (Hot: " << grand_total_hot_writes.load() << ")" << std::endl;
    std::cout << "  Avg WPS (during write phases): " << (total_write_phase_duration > 0 ? grand_total_writes.load() / total_write_phase_duration : 0) << std::endl;
    std::cout << "  Avg Writer CPS (during write phases): " << (total_write_phase_duration > 0 ? grand_total_writer_coros_completed.load() / total_write_phase_duration : 0) << std::endl;

    double actual_reader_hot_ratio = grand_total_reads.load() > 0 ? (double)grand_total_hot_reads.load() / grand_total_reads.load() : 0;
    double actual_writer_hot_ratio = grand_total_writes.load() > 0 ? (double)grand_total_hot_writes.load() / grand_total_writes.load() : 0;
    std::cout << "  Actual Reader Hot Access Ratio: " << actual_reader_hot_ratio << " (Expected ~" << READER_HOT_SET_ACCESS_PROB << ")" << std::endl;
    std::cout << "  Actual Writer Hot Access Ratio: " << actual_writer_hot_ratio << " (Expected ~" << WRITER_HOT_SET_ACCESS_PROB << ")" << std::endl;

    // 4. Verification
    bool success = true;
    if (grand_total_reader_coros_completed.load() != NUM_CYCLES * NUM_READER_COROUTINES_PER_PHASE) {
        std::cerr << "FAILURE: Reader coroutine completion count mismatch." << std::endl; success = false;
    }
    if (grand_total_writer_coros_completed.load() != NUM_CYCLES * NUM_WRITER_COROUTINES_PER_PHASE) {
        std::cerr << "FAILURE: Writer coroutine completion count mismatch." << std::endl; success = false;
    }
    // Basic check on total reads/writes
    long long min_reads = (long long)NUM_CYCLES * NUM_READER_COROUTINES_PER_PHASE * PAGES_PER_READER_CORO_MIN;
    long long max_reads = (long long)NUM_CYCLES * NUM_READER_COROUTINES_PER_PHASE * PAGES_PER_READER_CORO_MAX;
    if (grand_total_reads.load() < min_reads || grand_total_reads.load() > max_reads) {
        std::cerr << "FAILURE: Total reads out of expected range." << std::endl; success = false;
    }
    long long min_writes = (long long)NUM_CYCLES * NUM_WRITER_COROUTINES_PER_PHASE * PAGES_PER_WRITER_CORO_MIN;
    long long max_writes = (long long)NUM_CYCLES * NUM_WRITER_COROUTINES_PER_PHASE * PAGES_PER_WRITER_CORO_MAX;
     if (grand_total_writes.load() < min_writes || grand_total_writes.load() > max_writes) {
        std::cerr << "FAILURE: Total writes out of expected range." << std::endl; success = false;
    }

    // Optional: Sample some pages to check write_count (basic check)
    std::cout << "\nSampling page write counts (first 5 pages):" << std::endl;
    for(int i=0; i < 5 && i < all_managed_page_ids.size(); ++i) {
        norb::page_id_t pid_to_check = all_managed_page_ids[i];
        auto check_handle = norb::PersistentMemoryAsync::fetch_handle<PageDataMixed>(pid_to_check);
        auto ref = test_sync_wait(check_handle.const_ref(), pmem);
        std::cout << "  Page " << ref->id << ": write_count = " << ref->write_count << ", dynamic_value = " << ref->dynamic_value << std::endl;
    }


    // 5. Cleanup
    std::cout << "\nFlushing PMA and cleaning up test files..." << std::endl;
    test_sync_wait(pmem.flush_all(), pmem);
    std::filesystem::remove(norb::PMEM_FILE_NAME);
    std::filesystem::remove(std::string(norb::PMEM_FILE_NAME) + ".config");
    std::cout << "Test files cleaned up." << std::endl;

    return success;
}

int main() {
    if (run_mixed_read_write_test()) {
        std::cout << "\n--- Mixed Read/Write Test PASSED ---" << std::endl;
        return 0;
    } else {
        std::cerr << "\n--- Mixed Read/Write Test FAILED ---" << std::endl;
        return 1;
    }
}