#include "b_plus_tree.hpp" // The synchronous B+Tree to test

// These are typically defined in a project-wide config or compiler flags.
// Defining them here for the test to compile b_plus_tree.h.
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
// #define USE_SMALL_BATCH // If you want to test with smaller node capacities
#ifndef OVERWRITE_BLOCK_SIZE
#define OVERWRITE_BLOCK_SIZE 4 // Used if USE_SMALL_BATCH is defined
#endif

// stlite includes are part of b_plus_tree.h
// persistent_memory.hpp is part of b_plus_tree.h

// Standard library includes for the test
#include <algorithm>  // For std::sort, std::unique
#include <chrono>     // For timing
#include <filesystem> // For file cleanup
#include <functional> // For std::hash for a simple hash
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

// --- Placeholder for Hashing (if not provided by b_plus_tree.hpp's includes)
// ---


// Define PMA/FiledConfig file names for cleanup.
const std::string SYNC_PMA_FILE_NAME_PERF = "perf_sync_pma.dat";
const std::string SYNC_FILED_CONFIG_FILE_NAME_PERF = "perf_sync_config.dat";


// --- Test Environment Setup/Teardown ---
void setup_performance_test_environment() {
    std::cout << "Setting up performance test environment..." << std::endl;
    std::filesystem::remove(SYNC_PMA_FILE_NAME_PERF);
    std::filesystem::remove(SYNC_FILED_CONFIG_FILE_NAME_PERF);

    // Ensure PersistentMemory uses the correct file for this test if it's configurable
    // For now, assume it defaults or the file deletion is sufficient.
    norb::PersistentMemory::get_instance();
    std::cout << "Performance test environment set up." << std::endl;
}

void teardown_performance_test_environment() {
    std::cout << "Tearing down performance test environment..." << std::endl;
    std::filesystem::remove(SYNC_PMA_FILE_NAME_PERF);
    std::filesystem::remove(SYNC_FILED_CONFIG_FILE_NAME_PERF);
    std::cout << "Performance test environment torn down." << std::endl;
}


// --- Performance Test for Synchronous BPlusTree (main.cpp style) ---
void run_main_style_performance_test() {
    std::cout << "\n--- Running Main-Style Performance Test for Synchronous BPlusTree ---" << std::endl;
    setup_performance_test_environment();

    norb::BPlusTree<norb::hash::hashed_t_, int> tree;

    const long long NUM_OPERATIONS = 70000; // Total operations (inserts, deletes, finds)
    const int MAX_DISTINCT_INDICES = 15000; // Number of distinct string indices to generate
    const int FIND_ALL_RATIO = 3;      // 1 in (FIND_ALL_RATIO + 1) operations will be a find_all

    std::vector<std::string> string_indices;
    string_indices.reserve(MAX_DISTINCT_INDICES);
    std::mt19937 gen(std::chrono::system_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<> char_dist('a', 'z');
    std::uniform_int_distribution<> len_dist(5, 15);

    std::cout << "Generating " << MAX_DISTINCT_INDICES << " distinct string indices for hashing..." << std::endl;
    std::set<std::string> unique_indices_set;
    while(unique_indices_set.size() < MAX_DISTINCT_INDICES) {
        int len = len_dist(gen);
        std::string s = "";
        for(int i=0; i<len; ++i) {
            s += (char)char_dist(gen);
        }
        unique_indices_set.insert(s);
    }
    for(const auto& s : unique_indices_set) {
        string_indices.push_back(s);
    }
    std::cout << "Generated " << string_indices.size() << " actual distinct string indices." << std::endl;


    std::uniform_int_distribution<> index_picker_dist(0, string_indices.size() - 1);
    std::uniform_int_distribution<> value_dist(0, 100000);
    std::uniform_int_distribution<> op_type_dist(0, FIND_ALL_RATIO); // 0:insert, 1:delete, 2..N:find

    long long insert_count = 0;
    long long delete_count = 0;
    long long find_count = 0;

    std::cout << "Starting " << NUM_OPERATIONS << " operations..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    auto last_print_time = start_time;

    for (long long i = 0; i < NUM_OPERATIONS; ++i) {
        std::string mode_str;
        std::string index_str = string_indices[index_picker_dist(gen)];
        int value = value_dist(gen); // Generate value even if not used by find

        const auto hashed_index = norb::hash::fnv1a_hash(index_str);
        int op_type = op_type_dist(gen);

        if (op_type == 0) { // Insert
            mode_str = "insert";
            tree.insert(hashed_index, value);
            insert_count++;
        } else if (op_type == 1) { // Delete
            mode_str = "delete";
            tree.remove(hashed_index, value); // Value is used for specific pair deletion
            delete_count++;
        } else { // Find
            mode_str = "find";
            bool found_any = false;
            // The lambda for find_all_do captures found_any.
            // For performance, we might not want to print, but the original main.cpp does.
            // To minimize IO impact during timing, we can skip the actual printing inside the loop.
            tree.find_all_do(hashed_index, [&found_any](const int &x) {
                // In a pure performance test, this lambda would be empty or just increment a counter.
                // To match main.cpp structure, it sets a flag.
                found_any = true;
                // (void)x; // Suppress unused variable warning if not printing
            });
            // The printing of "null" or results happens outside the timed B-Tree call.
            find_count++;
        }

        if ((i + 1) % (NUM_OPERATIONS / 20) == 0) { // Print progress ~20 times
            auto current_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed_segment = current_time - last_print_time;
            std::cout << "  Performed " << (i + 1) << "/" << NUM_OPERATIONS
                      << " ops. (Last segment: " << elapsed_segment.count() << "s)"
                      << " Tree size: " << tree.size() << std::endl;
            last_print_time = current_time;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_duration = end_time - start_time;

    std::cout << "\n--- Performance Summary ---" << std::endl;
    std::cout << "Total operations performed: " << NUM_OPERATIONS << std::endl;
    std::cout << "  Inserts: " << insert_count << std::endl;
    std::cout << "  Deletes: " << delete_count << std::endl;
    std::cout << "  Finds:   " << find_count << std::endl;
    std::cout << "Total time taken: " << total_duration.count() << " seconds." << std::endl;
    if (total_duration.count() > 0) {
        std::cout << "Operations per second (OPS): " << NUM_OPERATIONS / total_duration.count() << std::endl;
    }
    std::cout << "Final B+Tree size: " << tree.size() << std::endl;
    std::cout << "Final B+Tree height: " << tree.tree_height.val << std::endl;


    teardown_performance_test_environment();
    std::cout << "--- Main-Style Performance Test FINISHED ---" << std::endl;
}


int main() {
    // Disable synchronization with C stdio for potentially faster C++ iostreams.
    // Not strictly necessary for this test as most IO is for progress/results,
    // but good practice if there were heavy std::cin/cout usage within timed sections.
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);
    // std::cout.tie(nullptr); // Be careful with this if mixing cerr/cout and needing strict order

    try {
        run_main_style_performance_test();
    } catch (const std::exception& e) {
        std::cerr << "\nPERFORMANCE TEST FAILED WITH EXCEPTION: " << e.what() << std::endl;
        teardown_performance_test_environment();
        return 1;
    } catch (...) {
        std::cerr << "\nPERFORMANCE TEST FAILED WITH UNKNOWN EXCEPTION" << std::endl;
        teardown_performance_test_environment();
        return 1;
    }

    std::cout << "\n*********************************" << std::endl;
    std::cout << "*** PERFORMANCE TEST COMPLETED ***" << std::endl;
    std::cout << "*********************************" << std::endl;

    return 0;
}