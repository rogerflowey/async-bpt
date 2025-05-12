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
#include <liburing.h>   // Include liburing header
#include <sys/uio.h>    // For struct iovec
#include <fcntl.h>      // For open flags
#include <unistd.h>     // For close
#include <errno.h>      // For errno
#include <map>          // For tracking active requests by user_data
#include <vector>
#include <numeric>
#include <random>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cassert>
#include <cstring>      // For strerror, memset
#include <utils.hpp>    // For norb::vector (assuming it's needed for external_buffers type)


// Structure to hold info about a pending raw io_uring request
struct RawIOInfo {
    off_t offset;
    int buffer_id;
    char pattern; // Pattern written or expected
    bool is_write;
    size_t op_index; // Original index in the main loop (before shuffling)
};

// Function to run a benchmark using raw liburing calls
void run_raw_iouring_benchmark(const std::string& filename, size_t num_ops, size_t buffer_size, int queue_depth, const norb::vector<void*>& external_buffers) {
    std::cout << "\n--- Running Raw io_uring Benchmark (Random Access) ---" << std::endl;
    std::cout << "File: " << filename << ", Ops: " << num_ops << ", Buffer Size: " << buffer_size << ", QD: " << queue_depth << std::endl;

    const int num_buffers = external_buffers.size();
    if (num_buffers == 0) { /* ... error handling ... */ return; }

    // --- io_uring Setup ---
    struct io_uring ring;
    int ret = io_uring_queue_init(queue_depth, &ring, 0);
    if (ret < 0) { /* ... error handling ... */ return; }

    // --- File Setup ---
    int fd = open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) { /* ... error handling ... */ io_uring_queue_exit(&ring); return; }

    // Register File
    int files_fd[1] = {fd};
    ret = io_uring_register_files(&ring, files_fd, 1);
    if (ret < 0) { /* ... error handling ... */ close(fd); io_uring_queue_exit(&ring); return; }
    const int file_index = 0;

    // Register Buffers
    std::vector<struct iovec> iovs(num_buffers);
    for (int i = 0; i < num_buffers; ++i) {
        iovs[i].iov_base = external_buffers[i];
        iovs[i].iov_len = buffer_size;
    }
    ret = io_uring_register_buffers(&ring, iovs.data(), num_buffers);
    if (ret < 0) { /* ... error handling ... */ io_uring_unregister_files(&ring); close(fd); io_uring_queue_exit(&ring); return; }
    std::cout << "Raw bench: io_uring setup complete." << std::endl;

    // --- Generate and Shuffle Block Indices ---
    std::vector<size_t> block_indices(num_ops);
    std::iota(block_indices.begin(), block_indices.end(), 0);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(block_indices.begin(), block_indices.end(), g);
    std::cout << "Raw bench: Generated and shuffled " << num_ops << " block indices." << std::endl;

    // --- State Management ---
    std::vector<bool> buffer_is_busy(num_buffers, false);
    std::vector<RawIOInfo> io_info_pool(queue_depth);
    std::vector<bool> io_info_slot_busy(queue_depth, false);
    // No need for active_requests vector if we use the pool directly

    bool success = true;
    auto overall_start_time = std::chrono::high_resolution_clock::now();

    // --- Write Phase (Random Access) ---
    auto write_start_time = std::chrono::high_resolution_clock::now();
    size_t submitted_write_count = 0;
    size_t completed_write_count = 0;
    size_t active_io_count = 0;

    while (completed_write_count < num_ops) {
        // Declare variables needed in this loop iteration scope
        int submitted_now = 0;
        unsigned completions_reaped_this_iter = 0; // Renamed for clarity
        struct io_uring_cqe *cqe = nullptr;
        unsigned head;

        // 1. Submit new requests if possible
        while (active_io_count < (size_t)queue_depth && submitted_write_count < num_ops) {
            int free_buffer_id = -1; for (int k = 0; k < num_buffers; ++k) if (!buffer_is_busy[k]) { free_buffer_id = k; break; }
            if (free_buffer_id == -1) break;
            int free_info_slot = -1; for (int k = 0; k < queue_depth; ++k) if (!io_info_slot_busy[k]) { free_info_slot = k; break; }
            if (free_info_slot == -1) break;
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;

            size_t op_idx = submitted_write_count;
            size_t block_idx = block_indices[op_idx];
            RawIOInfo* current_io_info = &io_info_pool[free_info_slot];
            current_io_info->offset = (off_t)block_idx * buffer_size;
            current_io_info->buffer_id = free_buffer_id;
            current_io_info->pattern = 'R' + (block_idx % 26);
            current_io_info->is_write = true;
            current_io_info->op_index = op_idx;

            memset(external_buffers[free_buffer_id], current_io_info->pattern, buffer_size);
            io_uring_prep_write_fixed(sqe, file_index, external_buffers[free_buffer_id], buffer_size, current_io_info->offset, free_buffer_id);
            sqe->flags |= IOSQE_FIXED_FILE;
            io_uring_sqe_set_data(sqe, current_io_info);

            buffer_is_busy[free_buffer_id] = true;
            io_info_slot_busy[free_info_slot] = true;
            active_io_count++;
            submitted_write_count++;
        }

        // 2. Submit prepared requests (if any)
        if (io_uring_sq_ready(&ring) > 0) {
             ret = io_uring_submit(&ring);
             if (ret < 0) { /* ... handle submit error ... */ success = false; goto raw_write_failed; }
             submitted_now = ret;
        }

        // 3. Reap completions
        // Peek first, then wait if nothing submitted and nothing ready
        bool need_to_wait = (submitted_now == 0 && io_uring_cq_ready(&ring) == 0 && completed_write_count < num_ops);

        if (need_to_wait && active_io_count > 0) { // Only wait if ops are outstanding
             ret = io_uring_wait_cqe(&ring, &cqe); // cqe declared above
             if (ret < 0) { /* ... handle wait error ... */ success = false; goto raw_write_failed; }

             RawIOInfo* req_info = reinterpret_cast<RawIOInfo*>(io_uring_cqe_get_data(cqe));
             if (req_info) {
                 if (cqe->res < 0) { /* ... handle CQE error ... */ success = false; }
                 else if ((size_t)cqe->res != buffer_size) { /* ... handle wrong size ... */ success = false; }
                 buffer_is_busy[req_info->buffer_id] = false;
                 int slot_idx = req_info - io_info_pool.data();
                 if (slot_idx >= 0 && slot_idx < queue_depth) io_info_slot_busy[slot_idx] = false;
                 else { /* ... handle slot error ... */ }
                 active_io_count--;
                 completed_write_count++;
                 completions_reaped_this_iter++;
             } else { /* ... handle NULL user_data ... */ }
             io_uring_cqe_seen(&ring, cqe); // Mark this single CQE as seen

        } else if (!need_to_wait) { // Process batch if we didn't wait or if wait wasn't needed
             io_uring_for_each_cqe(&ring, head, cqe) { // cqe declared above
                 RawIOInfo* req_info = reinterpret_cast<RawIOInfo*>(io_uring_cqe_get_data(cqe));
                 if (!req_info) { /* ... handle NULL user_data ... */ continue; }
                 if (cqe->res < 0) { /* ... handle CQE error ... */ success = false; }
                 else if ((size_t)cqe->res != buffer_size) { /* ... handle wrong size ... */ success = false; }
                 buffer_is_busy[req_info->buffer_id] = false;
                 int slot_idx = req_info - io_info_pool.data();
                 if (slot_idx >= 0 && slot_idx < queue_depth) io_info_slot_busy[slot_idx] = false;
                 else { /* ... handle slot error ... */ }
                 active_io_count--;
                 completed_write_count++;
                 completions_reaped_this_iter++;
             }
             if (completions_reaped_this_iter > 0) {
                 io_uring_cq_advance(&ring, completions_reaped_this_iter);
             }
        } // End if/else for reaping

        if (!success && completed_write_count > 0) { /* ... break early on error ... */ break; }

    } // End write phase main loop

raw_write_failed:;
    auto write_end_time = std::chrono::high_resolution_clock::now();
    auto write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(write_end_time - write_start_time);
    if (!success) { /* ... error handling ... */ io_uring_unregister_buffers(&ring); io_uring_unregister_files(&ring); close(fd); io_uring_queue_exit(&ring); return; }
    std::cout << "Raw bench: Write Phase completed in " << write_duration.count() << " ms." << std::endl;
    assert(active_io_count == 0 && "Active IO count not zero after write phase");


    // --- Read and Verify Phase (Random Access) ---
    auto read_start_time = std::chrono::high_resolution_clock::now();
    size_t submitted_read_count = 0;
    size_t completed_read_count = 0;
    active_io_count = 0;
    std::fill(io_info_slot_busy.begin(), io_info_slot_busy.end(), false);
    // active_requests vector not needed

    while (completed_read_count < num_ops) {
        // Declare variables needed in this loop iteration scope
        int submitted_now = 0;
        unsigned completions_reaped_this_iter = 0; // Renamed for clarity
        struct io_uring_cqe *cqe = nullptr;
        unsigned head;

        // 1. Submit reads
         while (active_io_count < (size_t)queue_depth && submitted_read_count < num_ops) {
            int free_buffer_id = -1; for (int k = 0; k < num_buffers; ++k) if (!buffer_is_busy[k]) { free_buffer_id = k; break; }
            if (free_buffer_id == -1) break;
            int free_info_slot = -1; for (int k = 0; k < queue_depth; ++k) if (!io_info_slot_busy[k]) { free_info_slot = k; break; }
            if (free_info_slot == -1) break;
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;

            size_t op_idx = submitted_read_count;
            size_t block_idx = block_indices[op_idx];
            RawIOInfo* current_io_info = &io_info_pool[free_info_slot];
            current_io_info->offset = (off_t)block_idx * buffer_size;
            current_io_info->buffer_id = free_buffer_id;
            current_io_info->pattern = 'R' + (block_idx % 26);
            current_io_info->is_write = false;
            current_io_info->op_index = op_idx;

            memset(external_buffers[free_buffer_id], 0, buffer_size);
            io_uring_prep_read_fixed(sqe, file_index, external_buffers[free_buffer_id], buffer_size, current_io_info->offset, free_buffer_id);
            sqe->flags |= IOSQE_FIXED_FILE;
            io_uring_sqe_set_data(sqe, current_io_info);

            buffer_is_busy[free_buffer_id] = true;
            io_info_slot_busy[free_info_slot] = true;
            active_io_count++;
            submitted_read_count++;
        }

        // 2. Submit
        if (io_uring_sq_ready(&ring) > 0) {
             ret = io_uring_submit(&ring);
             if (ret < 0) { /* ... handle submit error ... */ success = false; goto raw_read_failed; }
             submitted_now = ret;
        }

        // 3. Reap & Verify
        bool need_to_wait = (submitted_now == 0 && io_uring_cq_ready(&ring) == 0 && completed_read_count < num_ops);

        if (need_to_wait && active_io_count > 0) { // Only wait if ops are outstanding
             ret = io_uring_wait_cqe(&ring, &cqe); // cqe declared above
             if (ret < 0) { /* ... handle wait error ... */ success = false; goto raw_read_failed; }

             RawIOInfo* req_info = reinterpret_cast<RawIOInfo*>(io_uring_cqe_get_data(cqe));
             if (req_info) {
                 if (cqe->res < 0) { /* ... handle CQE error ... */ success = false; }
                 else if ((size_t)cqe->res != buffer_size) { /* ... handle wrong size ... */ success = false; }
                 else {
                     // Verification
                     char* buf_ptr = static_cast<char*>(external_buffers[req_info->buffer_id]);
                     for (size_t j = 0; j < buffer_size; ++j) {
                         if (buf_ptr[j] != req_info->pattern) {
                             std::cerr << "Raw bench: Verify failed block_idx " << block_indices[req_info->op_index]
                                       << " offset " << req_info->offset << " index " << j
                                       << "! Expected '" << req_info->pattern << "', got '" << buf_ptr[j] << "'" << std::endl;
                             success = false;
                         }
                     }
                 }
                 buffer_is_busy[req_info->buffer_id] = false;
                 int slot_idx = req_info - io_info_pool.data();
                 if (slot_idx >= 0 && slot_idx < queue_depth) io_info_slot_busy[slot_idx] = false;
                 else { /* ... handle slot error ... */ }
                 active_io_count--;
                 completed_read_count++;
                 completions_reaped_this_iter++;
             } else { /* ... handle NULL user_data ... */ }
             io_uring_cqe_seen(&ring, cqe); // Mark this single CQE as seen

        } else if (!need_to_wait) { // Process batch if we didn't wait or if wait wasn't needed
             io_uring_for_each_cqe(&ring, head, cqe) { // cqe declared above
                 RawIOInfo* req_info = reinterpret_cast<RawIOInfo*>(io_uring_cqe_get_data(cqe));
                 if (!req_info) { /* ... handle NULL user_data ... */ continue; }
                 if (cqe->res < 0) { /* ... handle CQE error ... */ success = false; }
                 else if ((size_t)cqe->res != buffer_size) { /* ... handle wrong size ... */ success = false; }
                 else {
                     // Verification
                     char* buf_ptr = static_cast<char*>(external_buffers[req_info->buffer_id]);
                     for (size_t j = 0; j < buffer_size; ++j) {
                         if (buf_ptr[j] != req_info->pattern) {
                             std::cerr << "Raw bench: Verify failed block_idx " << block_indices[req_info->op_index]
                                       << " offset " << req_info->offset << " index " << j
                                       << "! Expected '" << req_info->pattern << "', got '" << buf_ptr[j] << "'" << std::endl;
                             success = false;
                         }
                     }
                 }
                 buffer_is_busy[req_info->buffer_id] = false;
                 int slot_idx = req_info - io_info_pool.data();
                 if (slot_idx >= 0 && slot_idx < queue_depth) io_info_slot_busy[slot_idx] = false;
                 else { /* ... handle slot error ... */ }
                 active_io_count--;
                 completed_read_count++;
                 completions_reaped_this_iter++;
             }
             if (completions_reaped_this_iter > 0) {
                 io_uring_cq_advance(&ring, completions_reaped_this_iter);
             }
        } // End if/else for reaping

        if (!success && completed_read_count > 0) { /* ... break early on error ... */ break; }

    } // End read phase main loop

raw_read_failed:;
    auto read_end_time = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);
    if (!success) { std::cerr << "Raw bench: Read/Verify phase failed." << std::endl; }
    else { std::cout << "Raw bench: Read/Verify Phase completed in " << read_duration.count() << " ms." << std::endl; }
    assert(active_io_count == 0 && "Active IO count not zero after read phase");

    // --- Calculate Stats ---
    auto overall_end_time = std::chrono::high_resolution_clock::now();
    auto overall_duration = std::chrono::duration_cast<std::chrono::milliseconds>(overall_end_time - overall_start_time);
    double total_data_gb = 2.0 * num_ops * buffer_size / (1024.0 * 1024.0 * 1024.0);
    double duration_sec = overall_duration.count() / 1000.0;
    double throughput_mibps = (duration_sec > 0) ? (total_data_gb * 1024.0) / duration_sec : 0;
    double iops = (duration_sec > 0) ? (2.0 * num_ops) / duration_sec : 0;

    std::cout << "--- Raw io_uring Benchmark Complete ---" << std::endl;
    std::cout << "    Result: " << (success ? "Passed" : "FAILED") << std::endl;
    std::cout << "    Total Duration (W+R): " << overall_duration.count() << " ms" << std::endl;
    std::cout << "    Total Ops (W+R): " << 2 * num_ops << std::endl;
    std::cout << "    Total Data (W+R): " << total_data_gb << " GiB" << std::endl;
    std::cout << "    Approx Throughput: " << throughput_mibps << " MiB/s" << std::endl;
    std::cout << "    Approx IOPS: " << iops << std::endl;
    std::cout << "-------------------------------------" << std::endl;

    // --- Cleanup ---
    io_uring_unregister_buffers(&ring);
    io_uring_unregister_files(&ring);
    close(fd);
    io_uring_queue_exit(&ring);
    remove(filename.c_str()); // Remove benchmark file
}
int main() {
    // Use a base filename in the WSL home directory
    const std::string base_filename = "/home/rogerw/benchmark_test_";
    std::cout << "Using base filename: " << base_filename << std::endl;
    std::cout << "Buffer Size: " << BUFFER_SIZE
              << ", Num Buffers: " << NUM_BUFFERS
              << ", Queue Depth: " << QUEUE_DEPTH
              << ", Batch Submit Size: " << BATCH_SUBMIT_SIZE // Relevant for DiskScheduler test if run
              << ", Simulated Work Between Submissions: " << SIMULATED_WORK_US << " us" // Relevant for DiskScheduler test if run
              << std::endl;

    norb::vector<void*> buffers; // Buffers needed for raw io_uring test too
    DiskScheduler* scheduler = nullptr; // Keep variable, but might not initialize

    // Define op count for benchmarks (can be adjusted)
    const int num_benchmark_ops = QUEUE_DEPTH * 1000;

    try {
        std::cout << "Allocating " << NUM_BUFFERS << " aligned buffers (Size: " << BUFFER_SIZE << ", Alignment: " << BUFFER_SIZE <<")..." << std::endl;
        if (!create_aligned_buffers(buffers, NUM_BUFFERS, BUFFER_SIZE, BUFFER_SIZE)) {
             throw std::runtime_error("Buffer allocation failed.");
        }
        std::cout << "Buffers allocated." << std::endl;

        // --- Original DiskScheduler Tests (Skipped/Commented Out) ---
        /*
        std::cout << "Initializing DiskScheduler..." << std::endl;
        int open_flags = O_RDWR | O_CREAT | O_TRUNC;
        #ifdef __linux__
            open_flags |= O_DIRECT;
            std::cout << "Using O_DIRECT flag." << std::endl;
        #else
            std::cout << "O_DIRECT flag not available or not used on this OS." << std::endl;
        #endif
        std::string test_filename = base_filename + "diskscheduler_" + std::to_string(getpid()) + ".dat";
        scheduler = new DiskScheduler(test_filename, QUEUE_DEPTH, open_flags, BATCH_SUBMIT_SIZE);
        std::cout << "DiskScheduler initialized." << std::endl;

        std::cout << "Registering buffers..." << std::endl;
        scheduler->register_buffers(buffers);
        std::cout << "Buffers registered." << std::endl;

        // --- Test Phase 1 --- (Skipped)
        // --- Test Phase 2 --- (Skipped)
        // --- Test Phase 3 --- (Skipped)
        // --- Test Phase 4 --- (Skipped)

        std::cout << "\nAll io_uring scheduler tests passed!" << std::endl;
        */
        // --- End of Skipped DiskScheduler Tests ---


        // --- Run fstream Benchmark (Random Access) ---
        std::string fstream_filename = base_filename + "fstream_" + std::to_string(getpid()) + ".dat";
        run_fstream_benchmark(fstream_filename, num_benchmark_ops, BUFFER_SIZE);

        // --- Run Raw io_uring Benchmark (Random Access) ---
        std::string raw_iouring_filename = base_filename + "raw_iouring_" + std::to_string(getpid()) + ".dat";
        // Pass the already allocated buffers to the raw benchmark
        run_raw_iouring_benchmark(raw_iouring_filename, num_benchmark_ops, BUFFER_SIZE, QUEUE_DEPTH, buffers);

        // --- Optional: Add POSIX Direct I/O Benchmark Call Here ---
        // std::string posix_direct_filename = base_filename + "posix_direct_" + std::to_string(getpid()) + ".dat";
        // run_posix_direct_benchmark(posix_direct_filename, num_benchmark_ops, BUFFER_SIZE, buffers); // Assuming buffers are needed


    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        if (scheduler) delete scheduler; // Delete if it was created
        free_aligned_buffers(buffers);
        // Benchmark files are removed internally or attempt removal here if needed
        return 1;
    } catch (...) {
        std::cerr << "Caught unknown exception!" << std::endl;
         if (scheduler) delete scheduler; // Delete if it was created
        free_aligned_buffers(buffers);
        return 1;
    }

    // --- Cleanup ---
    std::cout << "Cleaning up..." << std::endl;
    if (scheduler) delete scheduler; // Delete scheduler only if it was initialized
    scheduler = nullptr;
    free_aligned_buffers(buffers); // Free buffers used by raw benchmark
    // Benchmark files are removed within their respective functions
    std::cout << "Cleanup complete." << std::endl;

    return 0;
}