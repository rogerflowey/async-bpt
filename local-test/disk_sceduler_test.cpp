#include "disk_scheduler.h" // Include the header file
#include <algorithm>        // For std::generate, std::fill, std::shuffle, std::iota
#include <cassert>          // For assertions
#include <chrono>           // For timing
#include <cstdio>           // For remove (alternative to unlink)
#include <cstdlib>          // For posix_memalign, free
#include <cstring>          // For memset, memcmp
#include <fstream>          // For fstream benchmark
#include <iostream>
#include <numeric>          // For std::iota
#include <random>           // For random shuffling
#include <string>
#include <unistd.h> // For getpid, unlink, usleep
#include <utils.hpp> // For norb::vector
#include <vector>
#include <thread> // For std::this_thread::sleep_for as an alternative to usleep
#include <utility> // For std::pair

// Test parameters
const int QUEUE_DEPTH = 128;
const int NUM_BUFFERS = 32;
const std::string TEST_FILENAME_BASE = "/home/rogerw/disksched_test_"; // Use WSL home dir
const unsigned int BATCH_SUBMIT_SIZE = 16;
// BUFFER_SIZE is defined in disk_scheduler.h
static_assert(BUFFER_SIZE > 0, "BUFFER_SIZE must be defined and positive");

// --- Simulation Parameter ---
// SET TO 0 FOR RAW SPEED COMPARISON
const unsigned int SIMULATED_WORK_US = 50;


// Helper function to free aligned buffers
void free_aligned_buffers(norb::vector<void*>& buffers) {
  for (void* buf : buffers) {
    if (buf) free(buf);
  }
  buffers.clear();
}

// Helper function to create aligned buffers (updated for norb::vector without resize)
bool create_aligned_buffers(norb::vector<void*>& buffers, size_t num_buffers, size_t size, size_t alignment) {
    buffers.clear();

    for (size_t i = 0; i < num_buffers; ++i) {
        void* buf = nullptr;
        if (posix_memalign(&buf, alignment, size) != 0) {
            std::cerr << "Failed to allocate aligned buffer " << i << std::endl;
            for (size_t j = 0; j < i; ++j) {
                 if (buffers[j]) free(buffers[j]);
            }
            buffers.clear();
            return false;
        }
        memset(buf, 0, size);
        buffers.push_back(buf);
    }
    if (buffers.size() != num_buffers) {
        std::cerr << "Internal error: Buffer vector size mismatch after allocation." << std::endl;
        free_aligned_buffers(buffers);
        return false;
    }
    return true;
}

// Coroutine for testing write operation
wutong::Task<bool> write_data_coro(DiskScheduler& scheduler,
                      norb::vector<void*>& external_buffers,
                      int buffer_id,
                      off_t offset,
                      size_t count, // Allow variable count
                      char pattern)
{
    assert(buffer_id >= 0 && static_cast<size_t>(buffer_id) < external_buffers.size());
    assert(external_buffers[buffer_id] != nullptr && "Buffer pointer is null!");
    assert(count <= BUFFER_SIZE);
    void* buffer = external_buffers[buffer_id];
    memset(buffer, pattern, count);
    bool success = co_await scheduler.async_write(buffer_id, count, offset);
    if (!success) {
         std::cerr << "Coroutine: Write failed for offset " << offset << "!" << std::endl;
    }
    co_return success;
}

// Coroutine for testing read operation and verifying data
wutong::Task<bool> read_and_verify_data_coro(DiskScheduler& scheduler,
                                norb::vector<void*>& external_buffers,
                                int buffer_id,
                                off_t offset,
                                size_t count, // Allow variable count
                                char expected_pattern)
{
    assert(buffer_id >= 0 && static_cast<size_t>(buffer_id) < external_buffers.size());
    assert(external_buffers[buffer_id] != nullptr && "Buffer pointer is null!");
    assert(count <= BUFFER_SIZE);
    void* buffer = external_buffers[buffer_id];
    memset(buffer, 0, BUFFER_SIZE); // Clear whole buffer for safety before read
    bool success = co_await scheduler.async_read(buffer_id, count, offset);
    if (!success) {
        std::cerr << "Coroutine: Read failed for offset " << offset << "!" << std::endl;
        co_return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (static_cast<char*>(buffer)[i] != expected_pattern) {
            std::cerr << "Coroutine: Data verification failed at offset " << offset << " index " << i
                      << "! Expected '" << expected_pattern << "' (ASCII: " << (int)expected_pattern
                      << "), got '" << static_cast<char*>(buffer)[i] << "' (ASCII: " << (int)static_cast<char*>(buffer)[i] << ")" << std::endl;
            co_return false;
        }
    }
    co_return true;
}

// Helper to run tasks and wait for completion (Used for Phase 1 and 3 verification)
// (Code remains the same as previous correct version)
void run_tasks_to_completion(DiskScheduler& scheduler, std::vector<wutong::Task<bool>>& tasks, const std::string& stage_name) {
    if (tasks.empty()) {
        std::cout << "No tasks to run for stage: " << stage_name << std::endl;
        return;
    }
    std::cout << "Starting " << tasks.size() << " tasks for stage: " << stage_name << std::endl;
    scheduler.flush_requests();
    std::cout << "Entering completion loop for stage: " << stage_name << std::endl;
    size_t completed_count = 0;
    size_t total_tasks = tasks.size();
    auto start_time = std::chrono::high_resolution_clock::now();
    while(completed_count < total_tasks) {
        size_t done_before_handling = 0;
        for(const auto& task : tasks) if (!task.handle || task.handle.done()) done_before_handling++;
        scheduler.handle_completions();
        size_t current_done = 0;
        for(const auto& task : tasks) if (!task.handle || task.handle.done()) current_done++;
        completed_count = current_done;
        if (completed_count == done_before_handling && completed_count < total_tasks) {
            usleep(500); // Yield if no progress
        }
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
        if (elapsed.count() > 300) { // Increased timeout slightly
             std::cerr << "Timeout waiting for tasks in stage: " << stage_name << std::endl;
             size_t not_done_count = 0;
             for(size_t i = 0; i < tasks.size(); ++i) if (tasks[i].handle && !tasks[i].handle.done()) not_done_count++;
             std::cerr << "Total pending tasks: " << not_done_count << " out of " << total_tasks << std::endl;
             throw std::runtime_error("Task completion timeout in stage: " + stage_name);
        }
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "All " << total_tasks << " tasks for stage '" << stage_name << "' completed in " << duration.count() << " ms." << std::endl;
    std::cout << "Verifying results for stage: " << stage_name << "..." << std::endl;
    for(size_t i = 0; i < tasks.size(); ++i) {
        assert(tasks[i].handle && tasks[i].handle.done() && "Task handle is invalid or not done before resume");
        bool result = tasks[i].await_resume();
        assert(result && "A task reported failure");
    }
    std::cout << "Results verified for stage: " << stage_name << std::endl;
}


// Function to run a simple fstream benchmark WITH RANDOM ACCESS
void run_fstream_benchmark(const std::string& filename, size_t num_ops, size_t buffer_size) {
    std::cout << "\n--- Running fstream Benchmark (Random Access) ---" << std::endl;
    std::cout << "File: " << filename << ", Ops: " << num_ops << ", Buffer Size: " << buffer_size << std::endl;

    // Allocate buffer
    void* buffer = nullptr;
    if (posix_memalign(&buffer, buffer_size, buffer_size) != 0) {
        std::cerr << "fstream bench: Failed to allocate aligned buffer." << std::endl;
        return;
    }

    // --- Generate and Shuffle Block Indices ---
    std::vector<size_t> block_indices(num_ops);
    std::iota(block_indices.begin(), block_indices.end(), 0); // 0, 1, 2...
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(block_indices.begin(), block_indices.end(), g);
    std::cout << "fstream bench: Generated and shuffled " << num_ops << " block indices." << std::endl;
    // ---

    bool success = true;
    auto overall_start_time = std::chrono::high_resolution_clock::now();

    // --- Write Phase (Random Access) ---
    auto write_start_time = std::chrono::high_resolution_clock::now();
    {
        std::ofstream outfile(filename, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!outfile) { /* ... error handling ... */ free(buffer); return; }

        for (size_t i = 0; i < num_ops; ++i) {
            size_t block_idx = block_indices[i]; // Use shuffled index
            char pattern = 'S' + (block_idx % 26); // Pattern based on block index
            off_t offset = (off_t)block_idx * buffer_size; // Offset based on block index
            memset(buffer, pattern, buffer_size);

            outfile.seekp(offset);
            if (!outfile) { /* ... error handling ... */ success = false; break; }
            outfile.write(static_cast<char*>(buffer), buffer_size);
            if (!outfile) { /* ... error handling ... */ success = false; break; }
        }
    }
    auto write_end_time = std::chrono::high_resolution_clock::now();
    auto write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(write_end_time - write_start_time);
    if (!success) { /* ... error handling ... */ free(buffer); remove(filename.c_str()); return; }
    std::cout << "fstream Write Phase completed in " << write_duration.count() << " ms." << std::endl;


    // --- Read and Verify Phase (Random Access) ---
    auto read_start_time = std::chrono::high_resolution_clock::now();
    {
        std::ifstream infile(filename, std::ios::binary | std::ios::in);
        if (!infile) { /* ... error handling ... */ free(buffer); remove(filename.c_str()); return; }

        // Use the *same* shuffled block_indices for reading
        for (size_t i = 0; i < num_ops; ++i) {
            size_t block_idx = block_indices[i]; // Use shuffled index
            char expected_pattern = 'S' + (block_idx % 26); // Expected pattern for this block
            off_t offset = (off_t)block_idx * buffer_size; // Offset for this block

            infile.seekg(offset);
            if (!infile) { /* ... error handling ... */ success = false; break; }
            infile.read(static_cast<char*>(buffer), buffer_size);
            if (!infile || infile.gcount() != static_cast<std::streamsize>(buffer_size)) { /* ... error handling ... */ success = false; break; }

            // Verification
            for (size_t j = 0; j < buffer_size; ++j) {
                if (static_cast<char*>(buffer)[j] != expected_pattern) {
                    std::cerr << "fstream bench: Data verification failed at block_idx " << block_idx
                              << " (offset " << offset << ") index " << j
                              << "! Expected '" << expected_pattern << "', got '" << static_cast<char*>(buffer)[j] << "'" << std::endl;
                    success = false;
                    goto fstream_read_verify_failed; // Quick exit
                }
            }
        }
fstream_read_verify_failed:; // Label for goto
    }
    auto read_end_time = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);
    if (!success) { std::cerr << "fstream bench: Read/Verify phase failed." << std::endl; }
    else { std::cout << "fstream Read/Verify Phase completed in " << read_duration.count() << " ms." << std::endl; }

    // --- Calculate Stats ---
    // (Stats calculation code remains the same)
    auto overall_end_time = std::chrono::high_resolution_clock::now();
    auto overall_duration = std::chrono::duration_cast<std::chrono::milliseconds>(overall_end_time - overall_start_time);
    double total_data_gb = 2.0 * num_ops * buffer_size / (1024.0 * 1024.0 * 1024.0);
    double duration_sec = overall_duration.count() / 1000.0;
    double throughput_mibps = (duration_sec > 0) ? (total_data_gb * 1024.0) / duration_sec : 0;
    double iops = (duration_sec > 0) ? (2.0 * num_ops) / duration_sec : 0;
    std::cout << "--- fstream Benchmark Complete ---" << std::endl;
    std::cout << "    Result: " << (success ? "Passed" : "FAILED") << std::endl;
    std::cout << "    Total Duration (W+R): " << overall_duration.count() << " ms" << std::endl;
    std::cout << "    Total Ops (W+R): " << 2 * num_ops << std::endl;
    std::cout << "    Total Data (W+R): " << total_data_gb << " GiB" << std::endl;
    std::cout << "    Approx Throughput: " << throughput_mibps << " MiB/s" << std::endl;
    std::cout << "    Approx IOPS: " << iops << std::endl;
    std::cout << "--------------------------------" << std::endl;

    // Cleanup
    free(buffer);
    remove(filename.c_str());
}
#include <fcntl.h>  // For open() and O_DIRECT, O_RDWR, etc.
#include <unistd.h> // For pread(), pwrite(), close()
#include <cerrno>   // For errno
#include <system_error> // For std::system_error

// ... (other includes from your main.cpp)

// Function to run a POSIX I/O benchmark with O_DIRECT (Random Access)
void run_posix_direct_io_benchmark(const std::string& filename, size_t num_ops, size_t buffer_size) {
    std::cout << "\n--- Running POSIX O_DIRECT Benchmark (Random Access) ---" << std::endl;
    std::cout << "File: " << filename << ", Ops: " << num_ops << ", Buffer Size: " << buffer_size << std::endl;
    std::cout << "IMPORTANT: O_DIRECT requires buffer, offset, and count to be aligned to filesystem block size." << std::endl;
    std::cout << "           Ensure BUFFER_SIZE (" << buffer_size << ") is a multiple of this (e.g., 512 or 4096)." << std::endl;


    // Allocate aligned buffer (O_DIRECT requires this)
    void* buffer = nullptr;
    // For O_DIRECT, alignment should typically be to the logical block size of the device (e.g., 512 or 4096)
    // Using buffer_size as alignment is fine if buffer_size is a multiple of the block size.
    size_t alignment = buffer_size; // Or a known block size like 4096
    if (posix_memalign(&buffer, alignment, buffer_size) != 0) {
        std::cerr << "posix_direct bench: Failed to allocate aligned buffer." << std::endl;
        return;
    }

    // --- Generate and Shuffle Block Indices ---
    std::vector<size_t> block_indices(num_ops);
    std::iota(block_indices.begin(), block_indices.end(), 0);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(block_indices.begin(), block_indices.end(), g);
    std::cout << "posix_direct bench: Generated and shuffled " << num_ops << " block indices." << std::endl;

    bool success = true;
    auto overall_start_time = std::chrono::high_resolution_clock::now();
    int fd = -1;

    // --- Write Phase (Random Access with O_DIRECT) ---
    auto write_start_time = std::chrono::high_resolution_clock::now();
    {
        // Open file for writing with O_DIRECT
        // O_TRUNC will clear the file if it exists
        // O_CREAT will create it if it doesn't exist
        fd = open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
        if (fd == -1) {
            perror("posix_direct bench: open for write failed");
            free(buffer);
            return;
        }

        for (size_t i = 0; i < num_ops; ++i) {
            size_t block_idx = block_indices[i];
            char pattern = 'S' + (block_idx % 26);
            off_t offset = (off_t)block_idx * buffer_size;
            memset(buffer, pattern, buffer_size);

            ssize_t bytes_written = pwrite(fd, buffer, buffer_size, offset);
            if (bytes_written == -1) {
                perror(("posix_direct bench: pwrite failed at offset " + std::to_string(offset)).c_str());
                success = false;
                break;
            }
            if (static_cast<size_t>(bytes_written) != buffer_size) {
                std::cerr << "posix_direct bench: pwrite short write. Wrote " << bytes_written << " expected " << buffer_size << std::endl;
                success = false;
                break;
            }
        }
        // O_DIRECT often requires fsync to ensure data and metadata are on disk,
        // though closing the file descriptor should also achieve this.
        // For benchmarking, the close is usually sufficient.
        // if (success && fsync(fd) == -1) {
        //     perror("posix_direct bench: fsync after writes failed");
        //     success = false;
        // }
        if (close(fd) == -1) {
             perror("posix_direct bench: close after write failed");
             // success might already be false, but this is an additional error
        }
        fd = -1; // Reset fd
    }
    auto write_end_time = std::chrono::high_resolution_clock::now();
    auto write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(write_end_time - write_start_time);
    if (!success) {
        std::cerr << "posix_direct bench: Write phase failed." << std::endl;
        free(buffer);
        remove(filename.c_str());
        return;
    }
    std::cout << "posix_direct Write Phase completed in " << write_duration.count() << " ms." << std::endl;

    // --- Read and Verify Phase (Random Access with O_DIRECT) ---
    auto read_start_time = std::chrono::high_resolution_clock::now();
    {
        fd = open(filename.c_str(), O_RDONLY | O_DIRECT);
        if (fd == -1) {
            perror("posix_direct bench: open for read failed");
            free(buffer);
            remove(filename.c_str());
            return;
        }

        for (size_t i = 0; i < num_ops; ++i) {
            size_t block_idx = block_indices[i];
            char expected_pattern = 'S' + (block_idx % 26);
            off_t offset = (off_t)block_idx * buffer_size;

            // Clear buffer before read for stricter verification (optional but good practice)
            // memset(buffer, 0, buffer_size);

            ssize_t bytes_read = pread(fd, buffer, buffer_size, offset);
            if (bytes_read == -1) {
                perror(("posix_direct bench: pread failed at offset " + std::to_string(offset)).c_str());
                success = false;
                break;
            }
            if (static_cast<size_t>(bytes_read) != buffer_size) {
                std::cerr << "posix_direct bench: pread short read. Read " << bytes_read << " expected " << buffer_size << std::endl;
                success = false;
                break;
            }

            for (size_t j = 0; j < buffer_size; ++j) {
                if (static_cast<char*>(buffer)[j] != expected_pattern) {
                    std::cerr << "posix_direct bench: Data verification failed at block_idx " << block_idx
                              << " (offset " << offset << ") index " << j
                              << "! Expected '" << expected_pattern << "', got '" << static_cast<char*>(buffer)[j] << "'" << std::endl;
                    success = false;
                    goto posix_read_verify_failed;
                }
            }
        }
posix_read_verify_failed:;
        if (close(fd) == -1) {
            perror("posix_direct bench: close after read failed");
        }
        fd = -1;
    }
    auto read_end_time = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);
    if (!success) { std::cerr << "posix_direct bench: Read/Verify phase failed." << std::endl; }
    else { std::cout << "posix_direct Read/Verify Phase completed in " << read_duration.count() << " ms." << std::endl; }

    // --- Calculate Stats ---
    auto overall_end_time = std::chrono::high_resolution_clock::now();
    auto overall_duration = std::chrono::duration_cast<std::chrono::milliseconds>(overall_end_time - overall_start_time);
    double total_data_gb = 2.0 * num_ops * buffer_size / (1024.0 * 1024.0 * 1024.0);
    double duration_sec = overall_duration.count() / 1000.0;
    double throughput_mibps = (duration_sec > 0) ? (total_data_gb * 1024.0) / duration_sec : 0;
    double iops = (duration_sec > 0) ? (2.0 * num_ops) / duration_sec : 0;

    std::cout << "--- POSIX O_DIRECT Benchmark Complete ---" << std::endl;
    std::cout << "    Result: " << (success ? "Passed" : "FAILED") << std::endl;
    std::cout << "    Total Duration (W+R): " << overall_duration.count() << " ms" << std::endl;
    std::cout << "    Total Ops (W+R): " << 2 * num_ops << std::endl;
    std::cout << "    Total Data (W+R): " << total_data_gb << " GiB" << std::endl;
    std::cout << "    Approx Throughput: " << throughput_mibps << " MiB/s" << std::endl;
    std::cout << "    Approx IOPS: " << iops << std::endl;
    std::cout << "---------------------------------------" << std::endl;

    free(buffer);
    remove(filename.c_str());
}


// In your main function, replace the call to run_fstream_benchmark:
// run_fstream_benchmark(fstream_filename, num_fstream_ops, BUFFER_SIZE);
// with:
// run_posix_direct_io_benchmark(fstream_filename, num_fstream_ops, BUFFER_SIZE);

int main() {
    std::string test_filename = TEST_FILENAME_BASE + std::to_string(getpid()) + ".dat";
    std::cout << "Using test file: " << test_filename << std::endl;
    std::cout << "Buffer Size: " << BUFFER_SIZE
              << ", Num Buffers: " << NUM_BUFFERS
              << ", Queue Depth: " << QUEUE_DEPTH
              << ", Batch Submit Size: " << BATCH_SUBMIT_SIZE
              << ", Simulated Work Between Submissions: " << SIMULATED_WORK_US << " us" // Note if this is 0
              << std::endl;

    norb::vector<void*> buffers;
    DiskScheduler* scheduler = nullptr;

    off_t current_offset = 0;
    const off_t phase_offset_increment = (off_t)(QUEUE_DEPTH * 25) * BUFFER_SIZE;

    // --- Discussion Point: Keeping the Disk Busy ---
    // (Note remains relevant, especially if SIMULATED_WORK_US > 0)
    std::cout << "\n--- Note on Disk Saturation ---" << std::endl;
    // ... (rest of note) ...
    std::cout << "--- End Note ---" << std::endl;


    try {
        std::cout << "Allocating " << NUM_BUFFERS << " aligned buffers (Size: " << BUFFER_SIZE << ", Alignment: " << BUFFER_SIZE <<")..." << std::endl;
        if (!create_aligned_buffers(buffers, NUM_BUFFERS, BUFFER_SIZE, BUFFER_SIZE)) {
             throw std::runtime_error("Buffer allocation failed.");
        }
        std::cout << "Buffers allocated." << std::endl;

        std::cout << "Initializing DiskScheduler..." << std::endl;

        scheduler = new DiskScheduler(test_filename, QUEUE_DEPTH, 0, BATCH_SUBMIT_SIZE);
        std::cout << "DiskScheduler initialized." << std::endl;

        std::cout << "Registering buffers..." << std::endl;
        scheduler->register_buffers(buffers);
        std::cout << "Buffers registered." << std::endl;

        // --- Test Phase 1: Basic Batched Writes & Reads ---
        // (Sequential, SIMULATED_WORK_US applies if > 0)
        {
            std::cout << "\n--- Test Phase 1: Basic Batched Writes & Reads ---" << std::endl;
            std::vector<wutong::Task<bool>> tasks;
            const int num_ops_phase1 = BATCH_SUBMIT_SIZE * 2;
            off_t phase1_offset_start = current_offset;
            current_offset += phase_offset_increment;
            std::cout << "Writing " << num_ops_phase1 << " blocks..." << std::endl;
            for(int i = 0; i < num_ops_phase1; ++i) {
                char pattern = 'A' + (i % 26);
                off_t offset = phase1_offset_start + (off_t)i * BUFFER_SIZE;
                int buffer_id = i % NUM_BUFFERS;
                tasks.push_back(write_data_coro(*scheduler, buffers, buffer_id, offset, BUFFER_SIZE, pattern));
                tasks.back().start();
                if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US); // Conditional sleep
            }
            run_tasks_to_completion(*scheduler, tasks, "Phase 1 Writes");
            tasks.clear();
            std::cout << "Reading and Verifying " << num_ops_phase1 << " blocks..." << std::endl;
            for(int i = 0; i < num_ops_phase1; ++i) {
                char expected_pattern = 'A' + (i % 26);
                off_t offset = phase1_offset_start + (off_t)i * BUFFER_SIZE;
                int buffer_id = (i + num_ops_phase1 / 2) % NUM_BUFFERS;
                tasks.push_back(read_and_verify_data_coro(*scheduler, buffers, buffer_id, offset, BUFFER_SIZE, expected_pattern));
                tasks.back().start();
                if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US); // Conditional sleep
            }
            run_tasks_to_completion(*scheduler, tasks, "Phase 1 Reads");
            std::cout << "--- Test Phase 1 Complete ---" << std::endl;
        }

        // --- Test Phase 2: Larger Data Test (Caller Buffer Management, Sequential) ---
        // (Sequential, SIMULATED_WORK_US applies if > 0)
        {
            std::cout << "\n--- Test Phase 2: Larger Data Test (Caller Buffer Management, Sequential) ---" << std::endl;
            const size_t large_data_num_blocks = NUM_BUFFERS * 3;
            off_t phase2_offset_start = current_offset;
            current_offset += phase_offset_increment;
            const size_t large_data_size = large_data_num_blocks * BUFFER_SIZE;
            std::cout << "Preparing to write " << large_data_num_blocks << " blocks (" << large_data_size << " bytes) at offset " << phase2_offset_start << "..." << std::endl;
            std::vector<bool> buffer_is_busy(NUM_BUFFERS, false);
            std::vector<std::pair<wutong::Task<bool>, int>> active_write_tasks;
            active_write_tasks.reserve(NUM_BUFFERS);
            size_t submitted_write_count = 0;
            size_t completed_write_count = 0;
            const size_t total_write_ops = large_data_num_blocks;
            auto stage_start_time = std::chrono::high_resolution_clock::now();
            while (completed_write_count < total_write_ops) {
                while (submitted_write_count < total_write_ops) {
                    int free_buffer_id = -1;
                    for (int k = 0; k < NUM_BUFFERS; ++k) if (!buffer_is_busy[k]) { free_buffer_id = k; break; }
                    if (free_buffer_id == -1) break;
                    buffer_is_busy[free_buffer_id] = true;
                    size_t i = submitted_write_count; // Use sequential index 'i'
                    char pattern = 'L' + (i % 10);
                    off_t offset = phase2_offset_start + (off_t)i * BUFFER_SIZE;
                    active_write_tasks.emplace_back(write_data_coro(*scheduler, buffers, free_buffer_id, offset, BUFFER_SIZE, pattern), free_buffer_id);
                    active_write_tasks.back().first.start();
                    if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US); // Conditional sleep
                    submitted_write_count++;
                }
                scheduler->flush_requests();
                size_t completions_handled_this_iteration = 0;
                scheduler->handle_completions();
                auto it = active_write_tasks.begin();
                while (it != active_write_tasks.end()) {
                    if (it->first.handle && it->first.handle.done()) {
                        bool result = it->first.await_resume(); assert(result && "Phase 2 write task failed");
                        buffer_is_busy[it->second] = false;
                        it = active_write_tasks.erase(it); completed_write_count++; completions_handled_this_iteration++;
                    } else if (!it->first.handle) { buffer_is_busy[it->second] = false; it = active_write_tasks.erase(it); }
                    else { ++it; }
                }
                if (completions_handled_this_iteration == 0) {
                    bool all_buffers_were_busy = (active_write_tasks.size() == NUM_BUFFERS); bool all_submitted = (submitted_write_count == total_write_ops);
                    if ((all_buffers_were_busy && !all_submitted) || (all_submitted && !active_write_tasks.empty())) { usleep(500); }
                }
                auto now = std::chrono::high_resolution_clock::now(); auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stage_start_time);
                if (elapsed.count() > 180) { throw std::runtime_error("Timeout in Phase 2 Writes loop"); }
            }
            auto stage_end_time = std::chrono::high_resolution_clock::now(); auto stage_duration = std::chrono::duration_cast<std::chrono::milliseconds>(stage_end_time - stage_start_time);
            std::cout << "Phase 2 Caller-Managed Writes completed in " << stage_duration.count() << " ms." << std::endl;
            assert(completed_write_count == total_write_ops && active_write_tasks.empty());
            // --- Read Phase (Sequential) ---
            std::cout << "Preparing to read and verify " << large_data_num_blocks << " blocks..." << std::endl;
            std::fill(buffer_is_busy.begin(), buffer_is_busy.end(), false);
            std::vector<std::pair<wutong::Task<bool>, int>> active_read_tasks; active_read_tasks.reserve(NUM_BUFFERS);
            size_t submitted_read_count = 0; size_t completed_read_count = 0; const size_t total_read_ops = large_data_num_blocks;
            stage_start_time = std::chrono::high_resolution_clock::now();
             while (completed_read_count < total_read_ops) {
                while (submitted_read_count < total_read_ops) {
                    int free_buffer_id = -1; for (int k = 0; k < NUM_BUFFERS; ++k) if (!buffer_is_busy[k]) { free_buffer_id = k; break; }
                    if (free_buffer_id == -1) break;
                    buffer_is_busy[free_buffer_id] = true;
                    size_t i = submitted_read_count; // Use sequential index 'i'
                    char expected_pattern = 'L' + (i % 10);
                    off_t offset = phase2_offset_start + (off_t)i * BUFFER_SIZE;
                    active_read_tasks.emplace_back(read_and_verify_data_coro(*scheduler, buffers, free_buffer_id, offset, BUFFER_SIZE, expected_pattern), free_buffer_id);
                    active_read_tasks.back().first.start();
                    if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US); // Conditional sleep
                    submitted_read_count++;
                }
                scheduler->flush_requests();
                size_t completions_handled_this_iteration = 0; scheduler->handle_completions();
                auto it = active_read_tasks.begin();
                while (it != active_read_tasks.end()) {
                    if (it->first.handle && it->first.handle.done()) {
                        bool result = it->first.await_resume(); assert(result && "Phase 2 read task failed");
                        buffer_is_busy[it->second] = false; it = active_read_tasks.erase(it); completed_read_count++; completions_handled_this_iteration++;
                    } else if (!it->first.handle) { buffer_is_busy[it->second] = false; it = active_read_tasks.erase(it); }
                    else { ++it; }
                }
                if (completions_handled_this_iteration == 0) {
                    bool all_buffers_were_busy = (active_read_tasks.size() == NUM_BUFFERS); bool all_submitted = (submitted_read_count == total_read_ops);
                    if ((all_buffers_were_busy && !all_submitted) || (all_submitted && !active_read_tasks.empty())) { usleep(500); }
                }
                auto now = std::chrono::high_resolution_clock::now(); auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stage_start_time);
                if (elapsed.count() > 180) { throw std::runtime_error("Timeout in Phase 2 Reads loop"); }
            }
            stage_end_time = std::chrono::high_resolution_clock::now(); stage_duration = std::chrono::duration_cast<std::chrono::milliseconds>(stage_end_time - stage_start_time);
            std::cout << "Phase 2 Caller-Managed Reads completed in " << stage_duration.count() << " ms." << std::endl;
            assert(completed_read_count == total_read_ops && active_read_tasks.empty());
            std::cout << "--- Test Phase 2 Complete ---" << std::endl;
        }


        // --- Test Phase 3: Asynchronicity Check ---
        // (Sequential, SIMULATED_WORK_US applies if > 0)
        {
            std::cout << "\n--- Test Phase 3: Asynchronicity Check ---" << std::endl;
            std::vector<wutong::Task<bool>> io_tasks;
            const int num_async_ops = BATCH_SUBMIT_SIZE * 4;
            off_t phase3_offset_start = current_offset;
            current_offset += phase_offset_increment;
            std::cout << "Submitting " << num_async_ops << " async write operations..." << std::endl;
            for (int i = 0; i < num_async_ops; ++i) {
                char pattern = 'X'; off_t offset = phase3_offset_start + (off_t)i * BUFFER_SIZE; int buffer_id = i % NUM_BUFFERS;
                io_tasks.push_back(write_data_coro(*scheduler, buffers, buffer_id, offset, BUFFER_SIZE, pattern));
                io_tasks.back().start();
                if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US); // Conditional sleep
            }
            scheduler->flush_requests();
            std::cout << "Async I/O submitted. Performing other work while I/O is in flight:" << std::endl;
            auto start_time = std::chrono::high_resolution_clock::now(); volatile int non_io_work_counter = 0; const int work_iterations = 5;
            for(int i = 0; i < work_iterations; ++i) {
                std::cout << "  Doing non-I/O work unit " << (i + 1) << "/" << work_iterations << "..." << std::endl;
                auto work_unit_start = std::chrono::high_resolution_clock::now();
                for (int j = 0; j < 20000000; ++j) non_io_work_counter += (j % 3) -1;
                auto work_unit_end = std::chrono::high_resolution_clock::now(); auto work_unit_duration = std::chrono::duration_cast<std::chrono::milliseconds>(work_unit_end - work_unit_start);
                std::cout << "  Non-I/O work unit " << (i+1) << " done. (Counter: " << non_io_work_counter << ", Duration: " << work_unit_duration.count() << " ms)" << std::endl;
                std::cout << "  Checking for I/O completions..." << std::endl;
                size_t done_count_before = 0; for(const auto& task : io_tasks) if (!task.handle || task.handle.done()) done_count_before++;
                scheduler->handle_completions();
                size_t done_count_after = 0; bool all_io_done_this_check = true;
                for(const auto& task : io_tasks) { if (task.handle && !task.handle.done()) all_io_done_this_check = false; else done_count_after++; }
                std::cout << "  Handled completions. Progress: " << done_count_after << "/" << io_tasks.size() << " (Previously " << done_count_before << ")" << std::endl;
                if (all_io_done_this_check) { std::cout << "  All async I/O operations completed during non-I/O work." << std::endl; break; }
                else { std::cout << "  Some async I/O still pending." << std::endl; }
            }
            auto end_time = std::chrono::high_resolution_clock::now(); auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            std::cout << "Non-I/O work phase took " << duration.count() << " ms." << std::endl;
            std::cout << "Ensuring all async I/O tasks are fully completed..." << std::endl;
            run_tasks_to_completion(*scheduler, io_tasks, "Phase 3 Async Writes Completion");
            io_tasks.clear();
            std::cout << "Verifying async writes..." << std::endl;
            for (int i = 0; i < num_async_ops; ++i) {
                 off_t offset = phase3_offset_start + (off_t)i * BUFFER_SIZE; int buffer_id = (i + num_async_ops / 2) % NUM_BUFFERS;
                 io_tasks.push_back(read_and_verify_data_coro(*scheduler, buffers, buffer_id, offset, BUFFER_SIZE, 'X'));
                io_tasks.back().start();
                if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US); // Conditional sleep
            }
            run_tasks_to_completion(*scheduler, io_tasks, "Phase 3 Async Writes Verification");
            std::cout << "--- Test Phase 3 Complete ---" << std::endl;
        }

        // --- Test Phase 4: High Concurrency Pressure Test (Caller Buffer Management, Random Access) ---
        {
            std::cout << "\n--- Test Phase 4: High Concurrency Pressure Test (Caller Buffer Management, Random Access) ---" << std::endl;
            auto pressure_start_time = std::chrono::high_resolution_clock::now();

            const int num_pressure_ops = QUEUE_DEPTH * 1000; // Increased ops count
            off_t phase4_offset_start = current_offset;

            // --- Generate and Shuffle Block Indices ---
            std::vector<size_t> block_indices(num_pressure_ops);
            std::iota(block_indices.begin(), block_indices.end(), 0); // 0, 1, 2...
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(block_indices.begin(), block_indices.end(), g);
            std::cout << "Phase 4: Generated and shuffled " << num_pressure_ops << " block indices." << std::endl;
            // ---

            // --- Caller-Managed Write Phase (Random Access) ---
            std::cout << "Preparing " << num_pressure_ops << " concurrent write operations starting at offset " << phase4_offset_start << " (Random Access)..." << std::endl;
            std::vector<bool> buffer_is_busy(NUM_BUFFERS, false);
            std::vector<std::pair<wutong::Task<bool>, int>> active_write_tasks; active_write_tasks.reserve(NUM_BUFFERS);
            size_t submitted_write_count = 0; size_t completed_write_count = 0; const size_t total_write_ops = num_pressure_ops;
            auto stage_start_time = std::chrono::high_resolution_clock::now();
            while (completed_write_count < total_write_ops) {
                while (submitted_write_count < total_write_ops) {
                    int free_buffer_id = -1; for (int k = 0; k < NUM_BUFFERS; ++k) if (!buffer_is_busy[k]) { free_buffer_id = k; break; }
                    if (free_buffer_id == -1) break;
                    buffer_is_busy[free_buffer_id] = true;
                    size_t i = submitted_write_count;
                    size_t block_idx = block_indices[i]; // Use shuffled index
                    char pattern = 'P' + (block_idx % 26); // Pattern based on block index
                    off_t offset = phase4_offset_start + (off_t)block_idx * BUFFER_SIZE; // Offset based on block index
                    active_write_tasks.emplace_back(write_data_coro(*scheduler, buffers, free_buffer_id, offset, BUFFER_SIZE, pattern), free_buffer_id);
                    active_write_tasks.back().first.start();
                    // NO usleep here if SIMULATED_WORK_US is 0
                    if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US);
                    submitted_write_count++;
                }
                scheduler->flush_requests();
                size_t completions_handled_this_iteration = 0; scheduler->handle_completions();
                auto it = active_write_tasks.begin();
                while (it != active_write_tasks.end()) {
                    if (it->first.handle && it->first.handle.done()) {
                        bool result = it->first.await_resume(); assert(result && "Phase 4 write task failed");
                        buffer_is_busy[it->second] = false; it = active_write_tasks.erase(it); completed_write_count++; completions_handled_this_iteration++;
                    } else if (!it->first.handle) { buffer_is_busy[it->second] = false; it = active_write_tasks.erase(it); }
                    else { ++it; }
                }
                if (completions_handled_this_iteration == 0) {
                    bool all_buffers_were_busy = (active_write_tasks.size() == NUM_BUFFERS); bool all_submitted = (submitted_write_count == total_write_ops);
                    if ((all_buffers_were_busy && !all_submitted) || (all_submitted && !active_write_tasks.empty())) { usleep(500); }
                }
                auto now = std::chrono::high_resolution_clock::now(); auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stage_start_time);
                if (elapsed.count() > 600) { // Increased timeout for more ops
                     std::cerr << "Timeout waiting for Phase 4 writes!" << std::endl;
                     std::cerr << "Completed: " << completed_write_count << "/" << total_write_ops << std::endl;
                     std::cerr << "Active: " << active_write_tasks.size() << std::endl;
                     throw std::runtime_error("Timeout in Phase 4 Writes loop");
                }
            }
            auto stage_end_time = std::chrono::high_resolution_clock::now(); auto stage_duration = std::chrono::duration_cast<std::chrono::milliseconds>(stage_end_time - stage_start_time);
            std::cout << "Phase 4 Caller-Managed Writes (Random) completed in " << stage_duration.count() << " ms." << std::endl;
            assert(completed_write_count == total_write_ops && active_write_tasks.empty());

            // --- Caller-Managed Read Phase (Random Access) ---
            std::cout << "Preparing " << num_pressure_ops << " concurrent read and verification operations (Random Access)..." << std::endl;
            std::fill(buffer_is_busy.begin(), buffer_is_busy.end(), false);
            std::vector<std::pair<wutong::Task<bool>, int>> active_read_tasks; active_read_tasks.reserve(NUM_BUFFERS);
            size_t submitted_read_count = 0; size_t completed_read_count = 0; const size_t total_read_ops = num_pressure_ops;
            stage_start_time = std::chrono::high_resolution_clock::now();
             while (completed_read_count < total_read_ops) {
                while (submitted_read_count < total_read_ops) {
                    int free_buffer_id = -1; for (int k = 0; k < NUM_BUFFERS; ++k) if (!buffer_is_busy[k]) { free_buffer_id = k; break; }
                    if (free_buffer_id == -1) break;
                    buffer_is_busy[free_buffer_id] = true;
                    size_t i = submitted_read_count;
                    size_t block_idx = block_indices[i]; // Use same shuffled index
                    char expected_pattern = 'P' + (block_idx % 26); // Pattern based on block index
                    off_t offset = phase4_offset_start + (off_t)block_idx * BUFFER_SIZE; // Offset based on block index
                    active_read_tasks.emplace_back(read_and_verify_data_coro(*scheduler, buffers, free_buffer_id, offset, BUFFER_SIZE, expected_pattern), free_buffer_id);
                    active_read_tasks.back().first.start();
                    // NO usleep here if SIMULATED_WORK_US is 0
                    if constexpr (SIMULATED_WORK_US > 0) usleep(SIMULATED_WORK_US);
                    submitted_read_count++;
                }
                scheduler->flush_requests();
                size_t completions_handled_this_iteration = 0; scheduler->handle_completions();
                auto it = active_read_tasks.begin();
                while (it != active_read_tasks.end()) {
                    if (it->first.handle && it->first.handle.done()) {
                        bool result = it->first.await_resume(); assert(result && "Phase 4 read task failed");
                        buffer_is_busy[it->second] = false; it = active_read_tasks.erase(it); completed_read_count++; completions_handled_this_iteration++;
                    } else if (!it->first.handle) { buffer_is_busy[it->second] = false; it = active_read_tasks.erase(it); }
                    else { ++it; }
                }
                if (completions_handled_this_iteration == 0) {
                    bool all_buffers_were_busy = (active_read_tasks.size() == NUM_BUFFERS); bool all_submitted = (submitted_read_count == total_read_ops);
                    if ((all_buffers_were_busy && !all_submitted) || (all_submitted && !active_read_tasks.empty())) { usleep(500); }
                }
                auto now = std::chrono::high_resolution_clock::now(); auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stage_start_time);
                 if (elapsed.count() > 600) { // Increased timeout for more ops
                     std::cerr << "Timeout waiting for Phase 4 reads!" << std::endl;
                     std::cerr << "Completed: " << completed_read_count << "/" << total_read_ops << std::endl;
                     std::cerr << "Active: " << active_read_tasks.size() << std::endl;
                     throw std::runtime_error("Timeout in Phase 4 Reads loop");
                }
            }
            stage_end_time = std::chrono::high_resolution_clock::now(); stage_duration = std::chrono::duration_cast<std::chrono::milliseconds>(stage_end_time - stage_start_time);
            std::cout << "Phase 4 Caller-Managed Reads (Random) completed in " << stage_duration.count() << " ms." << std::endl;
            assert(completed_read_count == total_read_ops && active_read_tasks.empty());

            // --- Calculate and Print Phase 4 Stats ---
            auto pressure_end_time = std::chrono::high_resolution_clock::now();
            auto pressure_duration = std::chrono::duration_cast<std::chrono::milliseconds>(pressure_end_time - pressure_start_time);
            double total_data_gb = 2.0 * num_pressure_ops * BUFFER_SIZE / (1024.0 * 1024.0 * 1024.0);
            double duration_sec = pressure_duration.count() / 1000.0;
            double throughput_mibps = (duration_sec > 0) ? (total_data_gb * 1024.0) / duration_sec : 0; // MiB/s
            double iops = (duration_sec > 0) ? (2.0 * num_pressure_ops) / duration_sec : 0;
            std::cout << "--- Test Phase 4 Complete ---" << std::endl;
            std::cout << "    Access Pattern: Random" << std::endl; // Note access pattern
            std::cout << "    Total Duration (W+R): " << pressure_duration.count() << " ms" << std::endl;
            std::cout << "    Total Ops (W+R): " << 2 * num_pressure_ops << std::endl;
            std::cout << "    Total Data (W+R): " << total_data_gb << " GiB" << std::endl;
            std::cout << "    Approx Throughput: " << throughput_mibps << " MiB/s" << std::endl;
             std::cout << "    Approx IOPS: " << iops << std::endl;
            std::cout << "---------------------------" << std::endl;
        } // End of Phase 4 block


        std::cout << "\nAll tests passed!" << std::endl;

        


    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        if (scheduler) delete scheduler;
        free_aligned_buffers(buffers);
        if (remove(test_filename.c_str()) != 0) {
             perror(("Error deleting test file: " + test_filename).c_str());
        }
        return 1;
    } catch (...) {
        std::cerr << "Caught unknown exception!" << std::endl;
         if (scheduler) delete scheduler;
        free_aligned_buffers(buffers);
         if (remove(test_filename.c_str()) != 0) {
             perror(("Error deleting test file: " + test_filename).c_str());
        }
        return 1;
    }


    std::cout << "Cleaning up..." << std::endl;
    delete scheduler;
    scheduler = nullptr;
    free_aligned_buffers(buffers);
    // remove(test_filename.c_str()); // Already removed by fstream bench, or handled in catch
    std::cout << "Cleanup complete." << std::endl;

    return 0;
}