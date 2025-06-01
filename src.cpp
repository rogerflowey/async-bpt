#include "async_bpt.h"
#include "tasks.h"
#include "smart_task.h"

#include <iostream>
#include <string>
#include <queue>

#include "b_plus_tree.hpp"
const auto hash_method = norb::hash::fnv1a_hash;
using norb::hash::hashed_t_;


// Type Aliases
using BPT = norb::AsyncBPlusTree<hashed_t_, int>;
using FindResultType = sjtu::vector<BPT::storage_pair_t>;
using FindTaskInQueueType = wutong::Task<FindResultType>; // For find operations

// --- Individual Operation Coroutines ---

wutong::SmartTask<void> do_insert_smart_async(BPT& tree, hashed_t_ hashed_index, int value) {
    co_await tree.insert_async(hashed_index, value);
    // co_return; // Implicit for void coroutines
}

wutong::SmartTask<void> do_delete_smart_async(BPT& tree, hashed_t_ hashed_index, int value) {
    co_await tree.remove_async(hashed_index, value);
    // co_return;
}

wutong::Task<FindResultType> do_find_async(BPT& tree, hashed_t_ hashed_index) {
    FindResultType result = co_await tree.find_all_async(hashed_index);
    co_return result;
}

// --- Helper for Printing Find Results (Synchronous) ---
void print_find_results(const FindResultType& results) {
    if (results.empty()) {
        std::cout << "null";
    } else {
        bool first = true;
        for (const auto& pair : results) {
            if (!first) {
                std::cout << ' ';
            }
            std::cout << pair.second; // Assuming pair.second is the int value
            first = false;
        }
    }
    std::cout << '\n';
}


int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::cout.tie(nullptr);

    BPT tree;

    int n_total_operations;
    std::cin >> n_total_operations;

    int operations_issued_count = 0;
    int finds_issued_count = 0;
    int finds_outputted_count = 0;

    std::queue<FindTaskInQueueType> pending_find_tasks;
    // No vector for active_modification_tasks needed due to SmartTask<void>

    // Main Polling and Dispatch Loop
    while (operations_issued_count < n_total_operations || finds_outputted_count < finds_issued_count) {
        // --- Part 1: Try to Issue a New Operation from Input ---
        if (operations_issued_count < n_total_operations && tree.permit_operation()) {
            std::string mode, index_str;
            int value;

            if (!(std::cin >> mode >> index_str)) {
                n_total_operations = operations_issued_count; // Adjust n to what was actually issued
                continue; // Skip to polling existing tasks
            }

            const auto hashed_index = hash_method(index_str);

            if (mode == "insert") {
                std::cin >> value;
                [[maybe_unused]] wutong::SmartTask<void> insert_stask =
                    do_insert_smart_async(tree, hashed_index, value);
            } else if (mode == "delete") {
                std::cin >> value;
                [[maybe_unused]] wutong::SmartTask<void> delete_stask =
                    do_delete_smart_async(tree, hashed_index, value);
            } else { // "find"
                pending_find_tasks.push(do_find_async(tree, hashed_index));
                finds_issued_count++;
            }
            operations_issued_count++;
        }

        // --- Part 2: Process Ready Find Tasks from the Front of the Queue ---
        if (!pending_find_tasks.empty()) {
            FindTaskInQueueType& front_find_task = pending_find_tasks.front();

            // Check if the task's handle is valid and the coroutine is done
            if (front_find_task.handle && front_find_task.handle.done()) {
                FindResultType results = front_find_task.handle.promise().get_result();
                print_find_results(results);
                pending_find_tasks.pop(); // Destroys the wutong::Task object and its coroutine handle
                finds_outputted_count++;
            }
        }

        // --- Part 3: Drive all asynchronous tree operations ---
        tree.poll();

        // --- Loop Exit Condition ---
        // If all operations from input have been issued AND all issued finds have been outputted
        if (operations_issued_count == n_total_operations && finds_outputted_count == finds_issued_count) {
            break;
        }
    }

    wutong::Task<void> shutdown_task = tree.shutdown();
    while (shutdown_task.handle && !shutdown_task.handle.done()) {
        tree.poll();
    }
    if (shutdown_task.handle && shutdown_task.handle.promise().exception_ptr_) {
        try {
            shutdown_task.handle.promise().get_result(); // Rethrow
        } catch (const std::exception& e) {
            std::cerr << "Error during B+ tree shutdown: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "Unknown error during B+ tree shutdown." << '\n';
        }
    }

    return 0;
}