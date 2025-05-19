#include "disk_scheduler.h" // Should include tasks.h and other necessary headers for DiskScheduler
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <filesystem> // For std::filesystem::remove, temp_directory_path
#include <cassert>
#include <cstring>    // For memcpy, memcmp, strerror
#include <unistd.h>   // For sysconf, usleep
#include <stdlib.h>   // For posix_memalign, free
#include <atomic>     // For std::atomic
#include <iomanip>    // For std::fixed, std::setprecision

// Mocking shared.hpp for norb::PAGE_SIZE if not available
// Ensure this matches the actual PAGE_SIZE used by DiskScheduler if it comes from shared.hpp
namespace norb {
    const size_t PAGE_SIZE = 4096; // Common page size
}

// Global counter for active coroutines to ensure all complete
std::atomic<int> g_active_coroutines(0);

// Helper function to allocate aligned memory
void* allocate_aligned_buffer(size_t size, size_t alignment = norb::PAGE_SIZE) {
    void* buffer = nullptr;
    if (posix_memalign(&buffer, alignment, size) != 0) {
        perror("posix_memalign failed");
        throw std::runtime_error("Failed to allocate aligned buffer");
    }
    // Initialize buffer to prevent reading uninitialized data in some tests
    memset(buffer, 0, size);
    return buffer;
}

void free_aligned_buffer(void* buffer) {
    free(buffer);
}

// Helper to fill buffer with identifiable data
void fill_buffer_pattern(char* buffer, size_t size, int pattern_seed) {
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = static_cast<char>((pattern_seed + i) % 256);
    }
}

// Helper to verify buffer content
bool verify_buffer_pattern(const char* buffer, size_t size, int pattern_seed) {
    for (size_t i = 0; i < size; ++i) {
        if (buffer[i] != static_cast<char>((pattern_seed + i) % 256)) {
            std::cerr << "Data mismatch at index " << i << "! Expected "
                      << static_cast<int>(static_cast<char>((pattern_seed + i) % 256))
                      << ", got " << static_cast<int>(buffer[i]) << std::endl;
            return false;
        }
    }
    return true;
}

// Main loop to drive the scheduler and wait for tasks
void run_scheduler_till_completion(DiskScheduler& scheduler) {
    while (scheduler.get_unfinished_requests_count() > 0 || g_active_coroutines > 0) {
        scheduler.handle_completions();
        if (scheduler.get_unfinished_requests_count() == 0 && g_active_coroutines > 0) {
            usleep(100);
        }
    }
    scheduler.flush_requests(); // Final flush
    while (scheduler.get_unfinished_requests_count() > 0) { // Ensure all flushed requests are completed
        scheduler.handle_completions();
    }
}


// --- Test Coroutines ---

// Test 1: Basic Read/Write
wutong::Task<void> basic_read_write_coro(DiskScheduler& scheduler, int id) {
    g_active_coroutines++;
    char* write_buf = static_cast<char*>(allocate_aligned_buffer(norb::PAGE_SIZE));
    char* read_buf = static_cast<char*>(allocate_aligned_buffer(norb::PAGE_SIZE));

    fill_buffer_pattern(write_buf, norb::PAGE_SIZE, id);
    off_t offset = static_cast<off_t>(id) * norb::PAGE_SIZE;

    bool write_ok = co_await scheduler.async_write(write_buf, norb::PAGE_SIZE, offset);
    assert(write_ok && "Basic Write Failed");

    bool read_ok = co_await scheduler.async_read(read_buf, norb::PAGE_SIZE, offset);
    assert(read_ok && "Basic Read Failed");

    assert(verify_buffer_pattern(read_buf, norb::PAGE_SIZE, id) && "Basic Read/Write Data Verification Failed");

    free_aligned_buffer(write_buf);
    free_aligned_buffer(read_buf);
    g_active_coroutines--;
}

// Test 2 & 4: Mixed Operations & Concurrency
wutong::Task<void> mixed_op_coro(DiskScheduler& scheduler, int id, off_t offset, bool is_write,
                                 char* buffer, int pattern_seed, size_t op_size) {
    g_active_coroutines++;
    if (is_write) {
        fill_buffer_pattern(buffer, op_size, pattern_seed);
        bool write_ok = co_await scheduler.async_write(buffer, op_size, offset);
        assert(write_ok && "Mixed Op Write Failed");
    } else {
        bool read_ok = co_await scheduler.async_read(buffer, op_size, offset);
        assert(read_ok && "Mixed Op Read Failed");
        assert(verify_buffer_pattern(buffer, op_size, pattern_seed) && "Mixed Op Read Verification Failed");
    }
    g_active_coroutines--;
}

// Test 3: Mixed Large-Scale Page Operations Coroutine
wutong::Task<void> mixed_large_page_op_coro(DiskScheduler& scheduler, int page_id, off_t page_offset, int num_rw_cycles,
                                            char* write_buf, char* read_buf, std::mt19937& local_rng) {
    g_active_coroutines++;
    // Using a distinct base for pattern seeds to avoid accidental clashes with other tests if file wasn't cleared.
    int current_pattern_seed = page_id * 1000 + 100000;

    //std::cout << "Page " << page_id << ": Initializing with pattern " << current_pattern_seed << " at offset " << page_offset << std::endl;
    fill_buffer_pattern(write_buf, norb::PAGE_SIZE, current_pattern_seed);
    bool ok = co_await scheduler.async_write(write_buf, norb::PAGE_SIZE, page_offset);
    assert(ok && "MixedLargePage: Initial write failed");

    std::uniform_int_distribution<> distrib(0, 1); // For choosing read (0) or write (1)

    for (int cycle = 0; cycle < num_rw_cycles; ++cycle) {
        bool do_write_action = (distrib(local_rng) == 1);

        if (do_write_action) {
            current_pattern_seed++; // Update the pattern for the new write
            fill_buffer_pattern(write_buf, norb::PAGE_SIZE, current_pattern_seed);
            //std::cout << "Page " << page_id << ", Cycle " << cycle << ": Writing pattern " << current_pattern_seed << " to offset " << page_offset << std::endl;
            ok = co_await scheduler.async_write(write_buf, norb::PAGE_SIZE, page_offset);
            assert(ok && "MixedLargePage: Write Failed");
        } else { // Do read action
            //std::cout << "Page " << page_id << ", Cycle " << cycle << ": Reading (expecting pattern " << current_pattern_seed << ") from offset " << page_offset << std::endl;
            ok = co_await scheduler.async_read(read_buf, norb::PAGE_SIZE, page_offset);
            assert(ok && "MixedLargePage: Read Failed");
            if (!verify_buffer_pattern(read_buf, norb::PAGE_SIZE, current_pattern_seed)) {
                std::cerr << "Verification FAILED for Page " << page_id << ", Cycle " << cycle
                          << ", Offset " << page_offset << ", Expected Pattern " << current_pattern_seed << std::endl;
                // For debugging, you might want to print the buffer contents here
                assert(false && "MixedLargePage: Read Verification Failed");
            }
        }
    }
    //std::cout << "Page " << page_id << ": Finished " << num_rw_cycles << " cycles. Final pattern " << current_pattern_seed << std::endl;
    g_active_coroutines--;
}


// Test 5: Performance Coroutine
wutong::Task<void> perf_op_coro(DiskScheduler& scheduler, void* buffer, off_t offset, size_t op_size, bool is_write) {
    g_active_coroutines++;
    bool op_ok;
    if (is_write) {
        op_ok = co_await scheduler.async_write(buffer, op_size, offset);
    } else {
        op_ok = co_await scheduler.async_read(buffer, op_size, offset);
    }
    assert(op_ok && "Perf Op Failed");
    g_active_coroutines--;
}


int main() {
    const std::string TEST_FILE_NAME = "test_disk_scheduler_file.dat";
    const int QUEUE_DEPTH = 1024;
    const unsigned int BATCH_SUBMIT_THRESHOLD = 32;

    std::filesystem::remove(TEST_FILE_NAME);

    std::cout << "=== DiskScheduler Test Suite ===" << std::endl;

    try {
        DiskScheduler scheduler(TEST_FILE_NAME, QUEUE_DEPTH, 0, BATCH_SUBMIT_THRESHOLD);
        std::vector<wutong::Task<void>> tasks;

        // --- Test 1: Basic Read/Write ---
        std::cout << "\n--- Test 1: Basic Read/Write ---" << std::endl;
        {
            tasks.clear();
            g_active_coroutines = 0;
            tasks.emplace_back(basic_read_write_coro(scheduler, 0));
            tasks.emplace_back(basic_read_write_coro(scheduler, 1));
            run_scheduler_till_completion(scheduler);
            std::cout << "Basic Read/Write Test PASSED." << std::endl;
        }

        // --- Test 2 & 4: Mixed Operations & Concurrency ---
        std::cout << "\n--- Test 2 & 4: Mixed Operations & Concurrency ---" << std::endl;
        {
            tasks.clear();
            g_active_coroutines = 0;
            const int num_mixed_ops_pairs = 25; // Will do 25 writes then 25 reads
            const size_t mixed_op_size = norb::PAGE_SIZE;
            std::vector<char*> mixed_write_buffers;
            std::vector<char*> mixed_read_buffers;

            for(int i=0; i<num_mixed_ops_pairs; ++i) {
                mixed_write_buffers.push_back(static_cast<char*>(allocate_aligned_buffer(mixed_op_size)));
                mixed_read_buffers.push_back(static_cast<char*>(allocate_aligned_buffer(mixed_op_size)));
            }

            std::vector<int> pattern_seeds(num_mixed_ops_pairs);
            for (int i = 0; i < num_mixed_ops_pairs; ++i) {
                off_t offset = static_cast<off_t>(i + 100) * mixed_op_size; // Use different offsets from Test 1
                pattern_seeds[i] = i + 50000; // Unique pattern seed
                tasks.emplace_back(mixed_op_coro(scheduler, i, offset, true, mixed_write_buffers[i], pattern_seeds[i], mixed_op_size));
            }
            run_scheduler_till_completion(scheduler);

            for (int i = 0; i < num_mixed_ops_pairs; ++i) {
                off_t offset = static_cast<off_t>(i + 100) * mixed_op_size;
                tasks.emplace_back(mixed_op_coro(scheduler, i + num_mixed_ops_pairs, offset, false, mixed_read_buffers[i], pattern_seeds[i], mixed_op_size));
            }
            run_scheduler_till_completion(scheduler);

            for(int i=0; i<num_mixed_ops_pairs; ++i) {
                free_aligned_buffer(mixed_write_buffers[i]);
                free_aligned_buffer(mixed_read_buffers[i]);
            }
            std::cout << "Mixed Operations & Concurrency Test PASSED." << std::endl;
        }

        // --- Test 3: Mixed Large-Scale Page Operations ---
        std::cout << "\n--- Test 3: Mixed Large-Scale Page Operations ---" << std::endl;
        {
            tasks.clear();
            g_active_coroutines = 0;
            const int NUM_CONCURRENT_PAGES = 16;
            const int NUM_RW_CYCLES_PER_PAGE = 10;
            const size_t op_size = norb::PAGE_SIZE;

            std::vector<char*> write_buffers_t3;
            std::vector<char*> read_buffers_t3;
            std::vector<std::mt19937> rngs_t3;

            for (int i = 0; i < NUM_CONCURRENT_PAGES; ++i) {
                write_buffers_t3.push_back(static_cast<char*>(allocate_aligned_buffer(op_size)));
                read_buffers_t3.push_back(static_cast<char*>(allocate_aligned_buffer(op_size)));
                rngs_t3.emplace_back(std::chrono::steady_clock::now().time_since_epoch().count() + i);
            }

            for (int i = 0; i < NUM_CONCURRENT_PAGES; ++i) {
                // Use offsets distinct from other tests to be safe, e.g., start after Test 2's area
                off_t page_offset = static_cast<off_t>(i + 200) * op_size;
                tasks.emplace_back(mixed_large_page_op_coro(scheduler, i, page_offset, NUM_RW_CYCLES_PER_PAGE,
                                                            write_buffers_t3[i], read_buffers_t3[i], rngs_t3[i]));
            }

            run_scheduler_till_completion(scheduler);

            for (int i = 0; i < NUM_CONCURRENT_PAGES; ++i) {
                free_aligned_buffer(write_buffers_t3[i]);
                free_aligned_buffer(read_buffers_t3[i]);
            }

            std::cout << "Mixed Large-Scale Page Operations Test PASSED." << std::endl;
        }


        // --- Test 5: Performance Test ---
        // (Assuming the file is now potentially larger due to previous tests, which is fine for perf test)
        std::cout << "\n--- Test 5: Performance Test ---" << std::endl;
        {
            tasks.clear();
            g_active_coroutines = 0;

            const size_t perf_total_data = norb::PAGE_SIZE * 1024 * 1; // 4MB total data
            const size_t perf_op_size = norb::PAGE_SIZE * 16; // 64KB operations
            const int num_perf_ops = perf_total_data / perf_op_size;

            std::vector<char*> perf_buffers;
            // Use a smaller buffer pool, e.g., QUEUE_DEPTH, as ops are larger
            int perf_buffer_pool_size = std::min(QUEUE_DEPTH, num_perf_ops > 0 ? num_perf_ops : 1);
            if (perf_buffer_pool_size == 0) perf_buffer_pool_size = 1;


            for(int i=0; i<perf_buffer_pool_size; ++i) {
                perf_buffers.push_back(static_cast<char*>(allocate_aligned_buffer(perf_op_size)));
                fill_buffer_pattern(perf_buffers.back(), perf_op_size, i + 200000);
            }

            // Sequential Write Performance
            auto start_time = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < num_perf_ops; ++i) {
                off_t offset = static_cast<off_t>(i) * perf_op_size; // Start perf test from offset 0 for clarity
                tasks.emplace_back(perf_op_coro(scheduler, perf_buffers[i % perf_buffer_pool_size], offset, perf_op_size, true));
            }
            run_scheduler_till_completion(scheduler);
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> duration = end_time - start_time;
            double throughput_write = (static_cast<double>(perf_total_data) / (1024 * 1024)) / duration.count();
            std::cout << "Sequential Write Throughput: " << std::fixed << std::setprecision(2)
                      << throughput_write << " MB/s (" << perf_total_data / (1024*1024) << " MB in " << duration.count() << "s)" << std::endl;
            tasks.clear();

            // Sequential Read Performance
            start_time = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < num_perf_ops; ++i) {
                off_t offset = static_cast<off_t>(i) * perf_op_size;
                tasks.emplace_back(perf_op_coro(scheduler, perf_buffers[i % perf_buffer_pool_size], offset, perf_op_size, false));
            }
            run_scheduler_till_completion(scheduler);
            end_time = std::chrono::high_resolution_clock::now();
            duration = end_time - start_time;
            double throughput_read = (static_cast<double>(perf_total_data) / (1024 * 1024)) / duration.count();
            std::cout << "Sequential Read Throughput: " << std::fixed << std::setprecision(2)
                      << throughput_read << " MB/s (" << perf_total_data / (1024*1024) << " MB in " << duration.count() << "s)" << std::endl;
            tasks.clear();

            // Random IOPS Performance (e.g., 4KB ops)
            const size_t iops_op_size = norb::PAGE_SIZE;
            const int num_iops_ops = 5000;
            const off_t max_rand_offset_pages = (perf_total_data / iops_op_size) -1;

            if (max_rand_offset_pages < 100) { // Need some space for random ops
                 std::cout << "Skipping IOPS test: file not large enough from previous perf test setup for meaningful random IO." << std::endl;
            } else {
                std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
                std::uniform_int_distribution<off_t> dist(0, max_rand_offset_pages);

                // Random Write IOPS
                start_time = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < num_iops_ops; ++i) {
                    off_t rand_page_idx = dist(rng);
                    off_t offset = rand_page_idx * iops_op_size;
                    // For IOPS, use smaller buffers from the pool, or re-purpose perf_buffers if sizes match
                    // Assuming perf_buffers[0] is large enough to hold iops_op_size
                    fill_buffer_pattern(static_cast<char*>(perf_buffers[i % perf_buffer_pool_size]), iops_op_size, i + 300000);
                    tasks.emplace_back(perf_op_coro(scheduler, perf_buffers[i % perf_buffer_pool_size], offset, iops_op_size, true));
                }
                run_scheduler_till_completion(scheduler);
                end_time = std::chrono::high_resolution_clock::now();
                duration = end_time - start_time;
                double iops_write = static_cast<double>(num_iops_ops) / duration.count();
                std::cout << "Random Write IOPS: " << std::fixed << std::setprecision(2)
                          << iops_write << " IOPS (" << num_iops_ops << " ops in " << duration.count() << "s)" << std::endl;
                tasks.clear();

                // Random Read IOPS
                start_time = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < num_iops_ops; ++i) {
                    off_t rand_page_idx = dist(rng);
                    off_t offset = rand_page_idx * iops_op_size;
                    tasks.emplace_back(perf_op_coro(scheduler, perf_buffers[i % perf_buffer_pool_size], offset, iops_op_size, false));
                }
                run_scheduler_till_completion(scheduler);
                end_time = std::chrono::high_resolution_clock::now();
                duration = end_time - start_time;
                double iops_read = static_cast<double>(num_iops_ops) / duration.count();
                std::cout << "Random Read IOPS: " << std::fixed << std::setprecision(2)
                          << iops_read << " IOPS (" << num_iops_ops << " ops in " << duration.count() << "s)" << std::endl;
                tasks.clear();
            }


            for(char* buf : perf_buffers) free_aligned_buffer(buf);
            std::cout << "Performance Test section finished." << std::endl;
        }


    } catch (const std::exception& e) {
        std::cerr << "Test suite failed with exception: " << e.what() << std::endl;
        std::filesystem::remove(TEST_FILE_NAME);
        return 1;
    }

    std::cout << "\nAll tests completed successfully." << std::endl;
    std::filesystem::remove(TEST_FILE_NAME);
    return 0;
}