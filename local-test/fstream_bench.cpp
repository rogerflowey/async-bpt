#include <iostream>
#include <vector>
#include <random>
#include <atomic>
#include <thread>     // For std::this_thread::sleep_for (though less critical here)
#include <filesystem> // For std::filesystem::remove
#include <algorithm>  // For std::remove_if (not used for tasks, but good include)
#include <cassert>    // For assert
#include <chrono>     // For timing
#include <iomanip>    // For std::fixed and std::setprecision
#include <cmath>      // For std::sqrt in dummy CPU work
#include <fstream>    // For std::fstream
#include <cstring>    // For memcpy

// --- Constants (mirrored from your shared.hpp or test setup) ---
// These would typically come from a shared header. For this standalone example:
namespace norb_sim { // Simulated namespace for constants
    using page_id_t = unsigned long;
    using page_size_t = unsigned long;
    constexpr page_size_t PAGE_SIZE = 4096; // Must match PageDataMixed padding if exact size is critical
    constexpr page_id_t SLOT_COUNT_SIM = 1024; // Smaller for faster fstream test runs
}

// --- PageData for Mixed Workload (same as before) ---
struct PageDataMixed {
    norb_sim::page_id_t id;
    char static_content[64];
    double dynamic_value;
    unsigned int write_count;
    char padding[norb_sim::PAGE_SIZE - (sizeof(norb_sim::page_id_t) + 64 + sizeof(double) + sizeof(unsigned int))]; // Ensure it fits PAGE_SIZE

    PageDataMixed(norb_sim::page_id_t _id = 0)
        : id(_id), dynamic_value(static_cast<double>(_id) + 0.5), write_count(0) {
        snprintf(static_content, sizeof(static_content), "PageDataMixed ID %lu", id);
        // Initialize padding to ensure consistent file content if needed, or leave it uninitialized
        // For this test, exact padding content isn't critical beyond size.
        // memset(padding, (char)(_id % 256), sizeof(padding)); 
    }
};
// Verify PageDataMixed fits within PAGE_SIZE
static_assert(sizeof(PageDataMixed) == norb_sim::PAGE_SIZE, "PageDataMixed size does not match PAGE_SIZE. Adjust padding.");


// --- Dummy CPU Work (same as before) ---
volatile double g_dummy_cpu_result_fstream = 0.0;
void do_cpu_work_fstream(double data, int iterations) {
    double result = data;
    for (int i = 0; i < iterations; ++i) {
        result = std::sqrt(std::abs(result) + i + 0.1) * std::log(std::abs(result) + 1.1 + i);
        if (!std::isfinite(result) || result == 0) result = static_cast<double>(i) + 0.456;
    }
    g_dummy_cpu_result_fstream = result;
}

// --- fstream Page Operations ---
bool read_page_fstream(std::fstream& file, norb_sim::page_id_t page_id, PageDataMixed& page_data) {
    if (!file.is_open() || !file.good()) return false;
    file.seekg(static_cast<std::streamoff>(page_id) * norb_sim::PAGE_SIZE);
    if (!file.good()) return false; // Check after seek
    file.read(reinterpret_cast<char*>(&page_data), sizeof(PageDataMixed));
    return file.gcount() == sizeof(PageDataMixed);
}

bool write_page_fstream(std::fstream& file, norb_sim::page_id_t page_id, const PageDataMixed& page_data) {
    if (!file.is_open() || !file.good()) return false;
    file.seekp(static_cast<std::streamoff>(page_id) * norb_sim::PAGE_SIZE);
    if (!file.good()) return false; // Check after seek
    file.write(reinterpret_cast<const char*>(&page_data), sizeof(PageDataMixed));
    file.flush(); // Ensure data is written to OS, potentially to disk
    return file.good();
}


// --- Reader Task (Synchronous) ---
void reader_task_fstream(
    std::fstream& file,
    const std::vector<norb_sim::page_id_t>& hot_set_page_ids,
    const std::vector<norb_sim::page_id_t>& cold_set_page_ids,
    double hot_set_access_probability,
    int num_pages_to_read,
    int cpu_work_iterations,
    std::mt19937& generator,
    std::atomic<long long>& successful_reads_counter,
    std::atomic<long long>& hot_reads_counter) {

    std::uniform_real_distribution<> access_type_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> hot_idx_distrib(0, hot_set_page_ids.empty() ? 0 : hot_set_page_ids.size() - 1);
    std::uniform_int_distribution<size_t> cold_idx_distrib(0, cold_set_page_ids.empty() ? 0 : cold_set_page_ids.size() - 1);

    PageDataMixed current_page_data;

    for (int i = 0; i < num_pages_to_read; ++i) {
        norb_sim::page_id_t page_id_to_read;
        bool accessed_hot = false;

        if (!hot_set_page_ids.empty() && (cold_set_page_ids.empty() || access_type_dist(generator) < hot_set_access_probability)) {
            page_id_to_read = hot_set_page_ids[hot_idx_distrib(generator)];
            accessed_hot = true;
        } else if (!cold_set_page_ids.empty()) {
            page_id_to_read = cold_set_page_ids[cold_idx_distrib(generator)];
        } else if (!hot_set_page_ids.empty()) {
             page_id_to_read = hot_set_page_ids[hot_idx_distrib(generator)];
             accessed_hot = true;
        } else { return; }

        if (!read_page_fstream(file, page_id_to_read, current_page_data)) {
            std::cerr << "ERROR: Failed to read page " << page_id_to_read << " in reader_task_fstream." << std::endl;
            continue; // Skip this page
        }
        
        assert(current_page_data.id == page_id_to_read); 
        char expected_static_content[64];
        snprintf(expected_static_content, sizeof(expected_static_content), "PageDataMixed ID %lu", page_id_to_read);
        assert(strcmp(current_page_data.static_content, expected_static_content) == 0);

        if (cpu_work_iterations > 0) {
            do_cpu_work_fstream(current_page_data.dynamic_value, cpu_work_iterations);
        }

        successful_reads_counter++;
        if (accessed_hot) {
            hot_reads_counter++;
        }
    }
}

// --- Writer Task (Synchronous) ---
void writer_task_fstream(
    std::fstream& file,
    const std::vector<norb_sim::page_id_t>& hot_set_page_ids,
    const std::vector<norb_sim::page_id_t>& cold_set_page_ids,
    double hot_set_access_probability,
    int num_pages_to_write,
    int cpu_work_iterations,
    std::mt19937& generator,
    std::atomic<long long>& successful_writes_counter,
    std::atomic<long long>& hot_writes_counter) {

    std::uniform_real_distribution<> access_type_dist(0.0, 1.0);
    std::uniform_int_distribution<size_t> hot_idx_distrib(0, hot_set_page_ids.empty() ? 0 : hot_set_page_ids.size() - 1);
    std::uniform_int_distribution<size_t> cold_idx_distrib(0, cold_set_page_ids.empty() ? 0 : cold_set_page_ids.size() - 1);
    
    PageDataMixed current_page_data;

    for (int i = 0; i < num_pages_to_write; ++i) {
        norb_sim::page_id_t page_id_to_write;
        bool accessed_hot = false;

        if (!hot_set_page_ids.empty() && (cold_set_page_ids.empty() || access_type_dist(generator) < hot_set_access_probability)) {
            page_id_to_write = hot_set_page_ids[hot_idx_distrib(generator)];
            accessed_hot = true;
        } else if (!cold_set_page_ids.empty()) {
            page_id_to_write = cold_set_page_ids[cold_idx_distrib(generator)];
        } else if (!hot_set_page_ids.empty()) {
             page_id_to_write = hot_set_page_ids[hot_idx_distrib(generator)];
             accessed_hot = true;
        } else { return; }

        // Read before write to simulate modifying existing data
        if (!read_page_fstream(file, page_id_to_write, current_page_data)) {
            std::cerr << "ERROR: Failed to read page " << page_id_to_write << " before writing in writer_task_fstream." << std::endl;
            continue; 
        }
        
        assert(current_page_data.id == page_id_to_write);

        current_page_data.dynamic_value += 1.0;
        current_page_data.write_count++;

        if (!write_page_fstream(file, page_id_to_write, current_page_data)) {
            std::cerr << "ERROR: Failed to write page " << page_id_to_write << " in writer_task_fstream." << std::endl;
            continue;
        }

        if (cpu_work_iterations > 0) {
            do_cpu_work_fstream(current_page_data.dynamic_value, cpu_work_iterations);
        }

        successful_writes_counter++;
        if (accessed_hot) {
            hot_writes_counter++;
        }
    }
}


// --- Main Test Function (fstream version) ---
bool run_mixed_read_write_test_fstream() {
    const std::string FSTREAM_TEST_FILE = "pma_test_fstream.db";
    std::cout << "--- Starting Mixed Read/Write Test (std::fstream) ---" << std::endl;
    
    std::filesystem::remove(FSTREAM_TEST_FILE);

    std::fstream file(FSTREAM_TEST_FILE, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "FATAL: Could not open test file: " << FSTREAM_TEST_FILE << std::endl;
        return false;
    }

    // Test Parameters (can be adjusted)
    const int NUM_CYCLES = 3;
    const norb_sim::page_id_t TOTAL_PAGES_TO_CREATE = norb_sim::SLOT_COUNT_SIM * 3; // e.g., 256*3 = 768
    
    const int NUM_READER_TASKS_PER_PHASE = norb_sim::SLOT_COUNT_SIM * 2; // e.g., 128
    const int PAGES_PER_READER_TASK_MIN = 3;
    const int PAGES_PER_READER_TASK_MAX = 7;
    const int READER_CPU_WORK_ITERATIONS = 50;

    const int NUM_WRITER_TASKS_PER_PHASE = norb_sim::SLOT_COUNT_SIM / 2; // e.g., 64
    const int PAGES_PER_WRITER_TASK_MIN = 2;
    const int PAGES_PER_WRITER_TASK_MAX = 5;
    const int WRITER_CPU_WORK_ITERATIONS = 70;
    
    const double HOT_SET_PERCENTAGE = 0.25; 
    const double READER_HOT_SET_ACCESS_PROB = 0.75;
    const double WRITER_HOT_SET_ACCESS_PROB = 0.60;

    std::cout << "Test Configuration (fstream):" << std::endl;
    // (Print config similar to the async version)
    std::cout << "  Cycles: " << NUM_CYCLES << ", Total Pages: " << TOTAL_PAGES_TO_CREATE << std::endl;
    std::cout << "  Readers/Phase: " << NUM_READER_TASKS_PER_PHASE << ", Writes/Phase: " << NUM_WRITER_TASKS_PER_PHASE << std::endl;


    std::vector<norb_sim::page_id_t> all_managed_page_ids;
    std::vector<norb_sim::page_id_t> hot_set_page_ids;
    std::vector<norb_sim::page_id_t> cold_set_page_ids;
    norb_sim::page_id_t num_hot_pages = static_cast<norb_sim::page_id_t>(TOTAL_PAGES_TO_CREATE * HOT_SET_PERCENTAGE);
    if (num_hot_pages == 0 && TOTAL_PAGES_TO_CREATE > 0 && HOT_SET_PERCENTAGE > 0) num_hot_pages = 1;

    // 1. Populate data
    std::cout << "\nPopulating " << TOTAL_PAGES_TO_CREATE << " pages (fstream)..." << std::endl;
    auto init_start_time = std::chrono::high_resolution_clock::now();
    for (norb_sim::page_id_t i = 0; i < TOTAL_PAGES_TO_CREATE; ++i) {
        PageDataMixed page(i);
        if (!write_page_fstream(file, i, page)) {
            std::cerr << "FATAL: Failed to initialize page " << i << std::endl;
            file.close();
            return false;
        }
        all_managed_page_ids.push_back(i);
        if (i < num_hot_pages) hot_set_page_ids.push_back(i);
        else cold_set_page_ids.push_back(i);
    }
    auto init_end_time = std::chrono::high_resolution_clock::now();
    std::cout << "Data population complete in " << std::chrono::duration<double>(init_end_time - init_start_time).count() << "s." << std::endl;

    // Overall Test Statistics
    std::atomic<long long> grand_total_reads(0), grand_total_hot_reads(0);
    std::atomic<long long> grand_total_writes(0), grand_total_hot_writes(0);
    // No "completed coroutines" count needed as tasks are synchronous
    double total_read_phase_duration = 0.0;
    double total_write_phase_duration = 0.0;

    std::random_device rd;
    std::mt19937 main_generator(rd());
    auto overall_test_start_time = std::chrono::high_resolution_clock::now();

    // 2. Phased Execution
    for (int cycle = 0; cycle < NUM_CYCLES; ++cycle) {
        std::cout << "\n--- Cycle " << cycle + 1 << "/" << NUM_CYCLES << " (fstream) ---" << std::endl;

        // --- Read Phase ---
        std::cout << "  Starting Read Phase (fstream)..." << std::endl;
        auto read_phase_start_time = std::chrono::high_resolution_clock::now();
        std::uniform_int_distribution<> pages_per_reader_dist(PAGES_PER_READER_TASK_MIN, PAGES_PER_READER_TASK_MAX);
        for(int i=0; i < NUM_READER_TASKS_PER_PHASE; ++i) {
            int num_pages = pages_per_reader_dist(main_generator);
            reader_task_fstream(file, hot_set_page_ids, cold_set_page_ids, READER_HOT_SET_ACCESS_PROB,
                                num_pages, READER_CPU_WORK_ITERATIONS, main_generator,
                                grand_total_reads, grand_total_hot_reads);
        }
        auto read_phase_end_time = std::chrono::high_resolution_clock::now();
        total_read_phase_duration += std::chrono::duration<double>(read_phase_end_time - read_phase_start_time).count();
        std::cout << "  Read Phase completed." << std::endl;

        // --- Write Phase ---
        std::cout << "  Starting Write Phase (fstream)..." << std::endl;
        auto write_phase_start_time = std::chrono::high_resolution_clock::now();
        std::uniform_int_distribution<> pages_per_writer_dist(PAGES_PER_WRITER_TASK_MIN, PAGES_PER_WRITER_TASK_MAX);
        for(int i=0; i < NUM_WRITER_TASKS_PER_PHASE; ++i) {
            int num_pages = pages_per_writer_dist(main_generator);
            writer_task_fstream(file, hot_set_page_ids, cold_set_page_ids, WRITER_HOT_SET_ACCESS_PROB,
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
    std::cout << "\n--- Performance Summary (std::fstream) ---" << std::endl;
    std::cout << "  Overall Test Duration: " << std::fixed << std::setprecision(3) << overall_duration << "s" << std::endl;
    // (Print other metrics similar to the async version, adapting "CPS" to "Tasks Per Second")
    std::cout << "  Total Read Phase Duration: " << total_read_phase_duration << "s" << std::endl;
    std::cout << "  Total Write Phase Duration: " << total_write_phase_duration << "s" << std::endl;

    std::cout << "  Grand Total Reads: " << grand_total_reads.load() << " (Hot: " << grand_total_hot_reads.load() << ")" << std::endl;
    std::cout << "  Avg RPS (during read phases): " << (total_read_phase_duration > 0 ? grand_total_reads.load() / total_read_phase_duration : 0) << std::endl;
    
    std::cout << "  Grand Total Writes: " << grand_total_writes.load() << " (Hot: " << grand_total_hot_writes.load() << ")" << std::endl;
    std::cout << "  Avg WPS (during write phases): " << (total_write_phase_duration > 0 ? grand_total_writes.load() / total_write_phase_duration : 0) << std::endl;
    
    // 4. Verification (basic)
    bool success = true; // Add more detailed checks if needed
    std::cout << "\nSampling page write counts (first 5 pages, fstream):" << std::endl;
    PageDataMixed check_page;
    for(int i=0; i < 5 && i < all_managed_page_ids.size(); ++i) {
        if (read_page_fstream(file, all_managed_page_ids[i], check_page)) {
            std::cout << "  Page " << check_page.id << ": write_count = " << check_page.write_count 
                      << ", dynamic_value = " << check_page.dynamic_value << std::endl;
        }
    }

    // 5. Cleanup
    file.close();
    std::filesystem::remove(FSTREAM_TEST_FILE);
    std::cout << "Test file (fstream) cleaned up." << std::endl;

    return success;
}

int main() {
    if (run_mixed_read_write_test_fstream()) {
        std::cout << "\n--- Mixed Read/Write Test (std::fstream) PASSED ---" << std::endl;
        return 0;
    } else {
        std::cerr << "\n--- Mixed Read/Write Test (std::fstream) FAILED ---" << std::endl;
        return 1;
    }
}