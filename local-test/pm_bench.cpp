#include "persistent_memory.hpp" // Your synchronous PersistentMemory header
#include "shared.hpp"          // For constants like PMEM_FILE_NAME, PAGE_SIZE etc.
                               // Make sure MEMORY_SIZE and LRU_K_INDEX are defined appropriately in shared.hpp
                               // for this synchronous version.

#include <iostream>
#include <vector>
#include <random>
#include <atomic>
#include <thread>     // For std::this_thread::sleep_for (less critical, but can be used for pacing)
#include <filesystem> // For std::filesystem::remove
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <cstring> // For memset if used in PageDataMixed

// --- PageData for Mixed Workload (same as the async fstream test, but using norb types) ---
struct PageDataMixedSync {
    norb::page_id_t id;
    char static_content[64];
    double dynamic_value;
    unsigned int write_count;
    // Calculate padding to fill up to norb::PAGE_SIZE
    // This assumes PAGE_SIZE is known at compile time and is a constant in shared.hpp
    char padding[norb::PAGE_SIZE - (sizeof(norb::page_id_t) + 64 + sizeof(double) + sizeof(unsigned int))];

    PageDataMixedSync(norb::page_id_t _id = 0)
        : id(_id), dynamic_value(static_cast<double>(_id) + 0.5), write_count(0) {
        snprintf(static_content, sizeof(static_content), "PageDataMixedSync ID %lu", id);
        // Optionally initialize padding
        // memset(padding, (char)(_id % 256), sizeof(padding));
    }
};
static_assert(sizeof(PageDataMixedSync) == norb::PAGE_SIZE, "PageDataMixedSync size does not match norb::PAGE_SIZE. Adjust padding or PAGE_SIZE definition.");


// --- Dummy CPU Work (same as before) ---
volatile double g_dummy_cpu_result_sync_pm = 0.0;
void do_cpu_work_sync_pm(double data, int iterations) {
    double result = data;
    for (int i = 0; i < iterations; ++i) {
        result = std::sqrt(std::abs(result) + i + 0.1) * std::log(std::abs(result) + 1.1 + i);
        if (!std::isfinite(result) || result == 0) result = static_cast<double>(i) + 0.456;
    }
    g_dummy_cpu_result_sync_pm = result;
}

// --- Reader Task (Synchronous, using PersistentMemory) ---
void reader_task_sync_pm(
    const std::vector<norb::PersistentMemory::Handle<PageDataMixedSync>>& all_page_handles, // Pass handles
    const std::vector<size_t>& hot_set_indices, // Indices into all_page_handles
    const std::vector<size_t>& cold_set_indices,
    double hot_set_access_probability,
    int num_pages_to_read,
    int cpu_work_iterations,
    std::mt19937& generator,
    std::atomic<long long>& successful_reads_counter,
    std::atomic<long long>& hot_reads_counter) {

    std::uniform_real_distribution<> access_type_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> hot_idx_distrib(0, hot_set_indices.empty() ? 0 : hot_set_indices.size() - 1);
    std::uniform_int_distribution<size_t> cold_idx_distrib(0, cold_set_indices.empty() ? 0 : cold_set_indices.size() - 1);

    for (int i = 0; i < num_pages_to_read; ++i) {
        size_t handle_idx_to_access;
        bool accessed_hot = false;

        if (!hot_set_indices.empty() && (cold_set_indices.empty() || access_type_dist(generator) < hot_set_access_probability)) {
            handle_idx_to_access = hot_set_indices[hot_idx_distrib(generator)];
            accessed_hot = true;
        } else if (!cold_set_indices.empty()) {
            handle_idx_to_access = cold_set_indices[cold_idx_distrib(generator)];
        } else if (!hot_set_indices.empty()) {
            handle_idx_to_access = hot_set_indices[hot_idx_distrib(generator)];
            accessed_hot = true;
        }
        else { return; } // No pages to read

        const auto& page_handle = all_page_handles[handle_idx_to_access];
        
        { // Scope for ConstHandledReference to manage lock
            auto ref = page_handle.const_ref(); // This can trigger I/O and eviction
            
            assert(ref->id == page_handle.page_id); 
            char expected_static_content[64];
            snprintf(expected_static_content, sizeof(expected_static_content), "PageDataMixedSync ID %lu", page_handle.page_id);
            assert(strcmp(ref->static_content, expected_static_content) == 0);

            if (cpu_work_iterations > 0) {
                do_cpu_work_sync_pm(ref->dynamic_value, cpu_work_iterations);
            }
        } // ref goes out of scope, lock released

        successful_reads_counter++;
        if (accessed_hot) {
            hot_reads_counter++;
        }
    }
}

// --- Writer Task (Synchronous, using PersistentMemory) ---
void writer_task_sync_pm(
    const std::vector<norb::PersistentMemory::Handle<PageDataMixedSync>>& all_page_handles,
    const std::vector<size_t>& hot_set_indices,
    const std::vector<size_t>& cold_set_indices,
    double hot_set_access_probability,
    int num_pages_to_write,
    int cpu_work_iterations,
    std::mt19937& generator,
    std::atomic<long long>& successful_writes_counter,
    std::atomic<long long>& hot_writes_counter) {

    std::uniform_real_distribution<> access_type_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> hot_idx_distrib(0, hot_set_indices.empty() ? 0 : hot_set_indices.size() - 1);
    std::uniform_int_distribution<size_t> cold_idx_distrib(0, cold_set_indices.empty() ? 0 : cold_set_indices.size() - 1);
    
    for (int i = 0; i < num_pages_to_write; ++i) {
        size_t handle_idx_to_access;
        bool accessed_hot = false;

        if (!hot_set_indices.empty() && (cold_set_indices.empty() || access_type_dist(generator) < hot_set_access_probability)) {
            handle_idx_to_access = hot_set_indices[hot_idx_distrib(generator)];
            accessed_hot = true;
        } else if (!cold_set_indices.empty()) {
            handle_idx_to_access = cold_set_indices[cold_idx_distrib(generator)];
        } else if (!hot_set_indices.empty()) {
            handle_idx_to_access = hot_set_indices[hot_idx_distrib(generator)];
            accessed_hot = true;
        }
        else { return; }

        const auto& page_handle = all_page_handles[handle_idx_to_access];

        { // Scope for HandledReference
            auto ref = page_handle.ref(); // This can trigger I/O, eviction, and marks page dirty
            
            assert(ref->id == page_handle.page_id);

            ref->dynamic_value += 1.0;
            ref->write_count++;

            if (cpu_work_iterations > 0) {
                do_cpu_work_sync_pm(ref->dynamic_value, cpu_work_iterations);
            }
        } // ref goes out of scope, lock released, page remains dirty in buffer

        successful_writes_counter++;
        if (accessed_hot) {
            hot_writes_counter++;
        }
    }
}


// --- Main Test Function (Synchronous PersistentMemory version) ---
bool run_mixed_read_write_test_sync_pm() {
    std::cout << "--- Starting Mixed Read/Write Test (Synchronous PersistentMemory) ---" << std::endl;
    
    // Ensure PMEM_FILE_NAME is defined in shared.hpp
    const std::string SYNC_PM_TEST_FILE = norb::PMEM_FILE_NAME; 
    const std::string SYNC_PM_CONFIG_FILE = norb::PMEM_FILE_NAME + ".config";

    // 0. Cleanup: Must be careful with singletons.
    // If PersistentMemory::get_instance() has already been called, its destructor will run at program exit.
    // For a clean test run, we delete files *before* the first get_instance() if possible,
    // or accept that the singleton might persist across test function calls if not handled carefully.
    // A robust approach would be to have a reset method in the singleton or manage its lifetime explicitly.
    // For this example, we assume this test function is the primary user or runs in a fresh process.
    std::filesystem::remove(SYNC_PM_TEST_FILE);
    std::filesystem::remove(SYNC_PM_CONFIG_FILE);

    // Get the singleton instance. This will create/open files.
    norb::PersistentMemory& pmem = norb::PersistentMemory::get_instance();


    // Test Parameters
    // Note: MEMORY_SIZE in shared.hpp defines SLOT_COUNT for PersistentMemory
    // For a fair comparison, ensure MEMORY_SIZE / PAGE_SIZE is similar to SLOT_COUNT_SIM in fstream test
    const int NUM_CYCLES = 3;
    // TOTAL_PAGES_TO_CREATE should be significantly larger than pmem.SLOT_COUNT to test eviction
    const norb::page_id_t TOTAL_PAGES_TO_CREATE = (norb::MEMORY_SIZE / norb::PAGE_SIZE) * 3; 
    
    const int NUM_READER_TASKS_PER_PHASE = (norb::MEMORY_SIZE / norb::PAGE_SIZE) * 2;
    const int PAGES_PER_READER_TASK_MIN = 3;
    const int PAGES_PER_READER_TASK_MAX = 7;
    const int READER_CPU_WORK_ITERATIONS = 50;

    const int NUM_WRITER_TASKS_PER_PHASE = (norb::MEMORY_SIZE / norb::PAGE_SIZE) / 4;
    const int PAGES_PER_WRITER_TASK_MIN = 2;
    const int PAGES_PER_WRITER_TASK_MAX = 5;
    const int WRITER_CPU_WORK_ITERATIONS = 70;
    
    const double HOT_SET_PERCENTAGE = 0.25; 
    const double READER_HOT_SET_ACCESS_PROB = 0.75;
    const double WRITER_HOT_SET_ACCESS_PROB = 0.60;

    std::cout << "Test Configuration (Sync PM):" << std::endl;
    std::cout << "  Cycles: " << NUM_CYCLES << ", Total Pages: " << TOTAL_PAGES_TO_CREATE << std::endl;
    std::cout << "  PM Slot Count (from MEMORY_SIZE): " << (norb::MEMORY_SIZE / norb::PAGE_SIZE) 
              << ", LRU_K: " << norb::LRU_K_INDEX << std::endl;
    std::cout << "  Readers/Phase: " << NUM_READER_TASKS_PER_PHASE << ", Writes/Phase: " << NUM_WRITER_TASKS_PER_PHASE << std::endl;


    std::vector<norb::PersistentMemory::Handle<PageDataMixedSync>> all_page_handles;
    std::vector<size_t> hot_set_indices; // Store indices into all_page_handles
    std::vector<size_t> cold_set_indices;
    norb::page_id_t num_hot_pages = static_cast<norb::page_id_t>(TOTAL_PAGES_TO_CREATE * HOT_SET_PERCENTAGE);
     if (num_hot_pages == 0 && TOTAL_PAGES_TO_CREATE > 0 && HOT_SET_PERCENTAGE > 0) num_hot_pages = 1;

    all_page_handles.reserve(TOTAL_PAGES_TO_CREATE);

    // 1. Populate data
    std::cout << "\nPopulating " << TOTAL_PAGES_TO_CREATE << " pages (Sync PM)..." << std::endl;
    auto init_start_time = std::chrono::high_resolution_clock::now();
    for (norb::page_id_t i = 0; i < TOTAL_PAGES_TO_CREATE; ++i) {
        // Use create_and_init. The page_id is managed by PersistentMemory.
        auto handle = norb::PersistentMemory::create_and_init<PageDataMixedSync>(i); // Pass 'i' as the ID for PageDataMixedSync
        all_page_handles.push_back(handle);
        // We assume page_ids are somewhat sequential from create for hot/cold split.
        // If create() recycles, this assumption might be weak without knowing recycled IDs.
        // For a fresh run, it's usually fine.
        if (all_page_handles.size() -1 < num_hot_pages) { // Use size as a proxy for sequential creation order
             hot_set_indices.push_back(all_page_handles.size() - 1);
        } else {
             cold_set_indices.push_back(all_page_handles.size() - 1);
        }
    }
    // Destructor of PersistentMemory will flush. Or add an explicit flush_all if available.
    auto init_end_time = std::chrono::high_resolution_clock::now();
    std::cout << "Data population complete in " << std::chrono::duration<double>(init_end_time - init_start_time).count() << "s." << std::endl;
    std::cout << "  Hot set size (indices): " << hot_set_indices.size() << ", Cold set size (indices): " << cold_set_indices.size() << std::endl;


    // Overall Test Statistics
    std::atomic<long long> grand_total_reads(0), grand_total_hot_reads(0);
    std::atomic<long long> grand_total_writes(0), grand_total_hot_writes(0);
    double total_read_phase_duration = 0.0;
    double total_write_phase_duration = 0.0;

    std::random_device rd;
    std::mt19937 main_generator(rd());
    auto overall_test_start_time = std::chrono::high_resolution_clock::now();

    // 2. Phased Execution
    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        std::cout << "\n--- Cycle " << cycle + 1 << "/" << NUM_CYCLES << " (Sync PM) ---" << std::endl;

        // --- Read Phase ---
        std::cout << "  Starting Read Phase (Sync PM)..." << std::endl;
        auto read_phase_start_time = std::chrono::high_resolution_clock::now();
        std::uniform_int_distribution<> pages_per_reader_dist(PAGES_PER_READER_TASK_MIN, PAGES_PER_READER_TASK_MAX);
        for(int i=0; i < NUM_READER_TASKS_PER_PHASE; ++i) {
            int num_pages = pages_per_reader_dist(main_generator);
            reader_task_sync_pm(all_page_handles, hot_set_indices, cold_set_indices, READER_HOT_SET_ACCESS_PROB,
                                num_pages, READER_CPU_WORK_ITERATIONS, main_generator,
                                grand_total_reads, grand_total_hot_reads);
        }
        auto read_phase_end_time = std::chrono::high_resolution_clock::now();
        total_read_phase_duration += std::chrono::duration<double>(read_phase_end_time - read_phase_start_time).count();
        std::cout << "  Read Phase completed." << std::endl;

        // --- Write Phase ---
        std::cout << "  Starting Write Phase (Sync PM)..." << std::endl;
        auto write_phase_start_time = std::chrono::high_resolution_clock::now();
        std::uniform_int_distribution<> pages_per_writer_dist(PAGES_PER_WRITER_TASK_MIN, PAGES_PER_WRITER_TASK_MAX);
        for(int i=0; i < NUM_WRITER_TASKS_PER_PHASE; ++i) {
            int num_pages = pages_per_writer_dist(main_generator);
            writer_task_sync_pm(all_page_handles, hot_set_indices, cold_set_indices, WRITER_HOT_SET_ACCESS_PROB,
                                num_pages, WRITER_CPU_WORK_ITERATIONS, main_generator,
                                grand_total_writes, grand_total_hot_writes);
        }
        auto write_phase_end_time = std::chrono::high_resolution_clock::now();
        total_write_phase_duration += std::chrono::duration<double>(write_phase_end_time - write_phase_start_time).count();
        std::cout << "  Write Phase completed." << std::endl;
    }
    auto overall_test_end_time = std::chrono::high_resolution_clock::now();
    double overall_duration = std::chrono::duration<double>(overall_test_end_time - overall_test_start_time).count();

    // 3. Performance Summary
    std::cout << "\n--- Performance Summary (Sync PM) ---" << std::endl;
    std::cout << "  Overall Test Duration: " << std::fixed << std::setprecision(3) << overall_duration << "s" << std::endl;
    // (Print other metrics similar to the async version)
    std::cout << "  Total Read Phase Duration: " << total_read_phase_duration << "s" << std::endl;
    std::cout << "  Total Write Phase Duration: " << total_write_phase_duration << "s" << std::endl;

    std::cout << "  Grand Total Reads: " << grand_total_reads.load() << " (Hot: " << grand_total_hot_reads.load() << ")" << std::endl;
    std::cout << "  Avg RPS (during read phases): " << (total_read_phase_duration > 0 ? grand_total_reads.load() / total_read_phase_duration : 0) << std::endl;
    
    std::cout << "  Grand Total Writes: " << grand_total_writes.load() << " (Hot: " << grand_total_hot_writes.load() << ")" << std::endl;
    std::cout << "  Avg WPS (during write phases): " << (total_write_phase_duration > 0 ? grand_total_writes.load() / total_write_phase_duration : 0) << std::endl;

    // 4. Verification (basic)
    bool success = true; // Add more detailed checks if needed
    std::cout << "\nSampling page write counts (first 5 created pages, Sync PM):" << std::endl;
    for(int i=0; i < 5 && i < all_page_handles.size(); ++i) {
        const auto& handle_to_check = all_page_handles[i];
        if (!handle_to_check.is_nullptr()) {
            auto ref = handle_to_check.const_ref();
            std::cout << "  Page " << ref->id << " (handle page_id " << handle_to_check.page_id << "): write_count = " << ref->write_count 
                      << ", dynamic_value = " << ref->dynamic_value << std::endl;
        }
    }

    // 5. Cleanup
    // The PersistentMemory singleton's destructor will handle flushing and closing files
    // when the program exits. If you need to explicitly clean up files *before* exit
    // and after the test, you'd need a way to destroy/reset the singleton or do it manually.
    // For this test, we rely on the program exit for cleanup of the singleton's resources.
    // The files themselves will remain unless explicitly deleted after the program finishes,
    // or if the test is run again (initial cleanup step).
    std::cout << "\nTest (Sync PM) finished. PM destructor will handle final flush." << std::endl;


    return success;
}

int main() {
    // Ensure shared.hpp defines MEMORY_SIZE, PAGE_SIZE, LRU_K_INDEX, PMEM_FILE_NAME
    // Example values for shared.hpp if not present:
    /*
    namespace norb {
        using page_id_t = unsigned long;
        using slot_id_t = unsigned long;
        using page_size_t = unsigned long;
        using mem_size_t = unsigned long;

        constexpr mem_size_t MEMORY_SIZE = 4096 * 256; // e.g., 256 slots
        constexpr page_size_t PAGE_SIZE = 4096;
        constexpr page_id_t LRU_K_INDEX = 3;
        const std::string PMEM_FILE_NAME = "pm_sync_test.db";
    }
    */

    if (run_mixed_read_write_test_sync_pm()) {
        std::cout << "\n--- Mixed Read/Write Test (Synchronous PersistentMemory) PASSED ---" << std::endl;
        return 0;
    } else {
        std::cerr << "\n--- Mixed Read/Write Test (Synchronous PersistentMemory) FAILED ---" << std::endl;
        return 1;
    }
}