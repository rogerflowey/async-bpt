#include "b_plus_tree.hpp" // Assuming this is the correct path
#include <algorithm>       // For std::find, std::max
#include <chrono>          // For timing
#include <iostream>
#include <random>
#include <set>    // For std::set
#include <string> // For std::string, std::stoi
#include <vector> // For std::vector

// --- Configuration ---
// Set to true for verbose logging of each action.
constexpr bool PRINT_DETAILED_ACTIONS = false;
// Set to true to call norb_map.traverse(true) after each operation.
// This will significantly impact performance and is for debugging tree
// structure.
constexpr bool ALWAYS_TRAVERSE_AFTER_OP = false;
// Default number of operations
int num_operations = 1000000;
// Random seed for execution
constexpr int random_seed = 600;
// --- End Configuration ---

// Global volatile variable to ensure computations (and thus B+ tree calls)
// are not optimized away by the compiler.
volatile long long g_sink = 0;

int main(int argc, char *argv[]) {
  // Specific to norb environment, likely for cleaning up persistent storage.
  norb::chore::remove_associated();

  if (argc > 1) {
    try {
      num_operations = std::stoi(argv[1]);
      if (num_operations <= 0) {
        std::cerr
            << "Number of operations must be positive. Using default: 100000"
            << std::endl;
        num_operations = 100000; // Reset to a valid default
      }
    } catch (const std::invalid_argument &ia) {
      std::cerr << "Invalid argument for number of operations: " << argv[1]
                << ". Using default: 100000" << std::endl;
      num_operations = 100000;
    } catch (const std::out_of_range &oor) {
      std::cerr << "Number of operations out of range: " << argv[1]
                << ". Using default: 100000" << std::endl;
      num_operations = 100000;
    }
  }

  std::cout << "Starting BPlusTree pressure test with " << num_operations
            << " operations." << std::endl;
  std::cout << "Key type: int, Value type: int" << std::endl;
  if (ALWAYS_TRAVERSE_AFTER_OP) {
    std::cout << "WARNING: ALWAYS_TRAVERSE_AFTER_OP is enabled. "
                 "norb_map.traverse(true) will be called after each operation, "
                 "impacting performance."
              << std::endl;
  }

  norb::BPlusTree<int, int> norb_map;

  // Tracking inserted <key, value> pairs:
  // - To satisfy the constraint: "if x: <key, val> is in the BPT, you may not
  //   insert another pair the same as x into it."
  // - To efficiently pick existing pairs for removal.
  std::set<std::pair<int, int>>
      inserted_pairs_set; // For O(log N) duplicate check during insertion
  std::vector<std::pair<int, int>>
      inserted_pairs_vec; // For O(1) random element selection (by index) for
                          // removal
  inserted_pairs_vec.reserve(num_operations / 2); // Pre-allocate some space

  std::mt19937 gen(random_seed);

  std::uniform_int_distribution<> op_dist(
      0, 2); // 0: insert, 1: remove, 2: find_all
  std::uniform_int_distribution<int> key_dist(
      0, 10000000); // Large key range for int keys
  std::uniform_int_distribution<int> val_dist(0, 1000000); // Value range

  auto start_time = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < num_operations; ++i) {
    // Progress indicator (prints roughly every 10% of operations)
    if (num_operations >= 10 && i > 0 && (i % (num_operations / 10) == 0)) {
      std::cout << "\rProgress: " << (100 * i / num_operations) << "%"
                << std::flush;
    }

    int operation_type = op_dist(gen);
    int current_key;
    int current_value;

    if (operation_type == 0) { // INSERT
      // Ensure the <key, value> pair is not already in our tracked set
      do {
        current_key = key_dist(gen);
        current_value = val_dist(gen);
      } while (inserted_pairs_set.count({current_key, current_value}));

      if (PRINT_DETAILED_ACTIONS) {
        std::cout << "[" << i << "] Operation: INSERT (" << current_key << ", "
                  << current_value << ")" << std::endl;
      }
      norb_map.insert(current_key, current_value);
      inserted_pairs_set.insert({current_key, current_value});
      inserted_pairs_vec.push_back({current_key, current_value});

    } else if (operation_type == 1) { // REMOVE
      if (inserted_pairs_vec.empty()) {
        if (PRINT_DETAILED_ACTIONS) {
          std::cout << "[" << i
                    << "] Operation: REMOVE (skipped, no items to remove)"
                    << std::endl;
        }
        continue; // Skip REMOVE if nothing has been inserted yet
      }

      int key_to_remove;
      int value_to_remove;
      int idx_in_vec =
          -1; // Index in inserted_pairs_vec if removing a known item

      // 50% chance to try to remove an actually existing item
      bool try_remove_existing = (gen() % 2 == 0);

      if (try_remove_existing) {
        std::uniform_int_distribution<> existing_idx_dist(
            0, inserted_pairs_vec.size() - 1);
        idx_in_vec = existing_idx_dist(gen);
        key_to_remove = inserted_pairs_vec[idx_in_vec].first;
        value_to_remove = inserted_pairs_vec[idx_in_vec].second;
        if (PRINT_DETAILED_ACTIONS) {
          std::cout << "[" << i << "] Operation: REMOVE existing ("
                    << key_to_remove << ", " << value_to_remove << ")"
                    << std::endl;
        }
      } else {
        // Try to remove a random (possibly non-existent) pair
        key_to_remove = key_dist(gen);
        value_to_remove = val_dist(gen);
        if (PRINT_DETAILED_ACTIONS) {
          std::cout << "[" << i << "] Operation: REMOVE random ("
                    << key_to_remove << ", " << value_to_remove << ")"
                    << std::endl;
        }
      }

      bool removed = norb_map.remove(key_to_remove, value_to_remove);
      g_sink += (removed ? 1 : 0); // Use the result to prevent optimization

      if (removed) {
        // If B+ Tree confirmed removal, update our tracking structures.
        // The pair should have been in inserted_pairs_set.
        auto set_iter =
            inserted_pairs_set.find({key_to_remove, value_to_remove});
        if (set_iter != inserted_pairs_set.end()) {
          inserted_pairs_set.erase(set_iter);

          // Remove from vector efficiently.
          // If idx_in_vec is valid, we used that specific element.
          // Otherwise, we need to find the element in the vector.
          if (idx_in_vec != -1 &&
              inserted_pairs_vec[idx_in_vec].first == key_to_remove &&
              inserted_pairs_vec[idx_in_vec].second == value_to_remove) {
            // Swap with last element and pop (if it's not already the last)
            inserted_pairs_vec[idx_in_vec] = inserted_pairs_vec.back();
            inserted_pairs_vec.pop_back();
          } else {
            // Removed a random pair that happened to exist, or vector was
            // reordered. Find it in the vector (this part can be
            // O(N_vector_size)).
            auto vec_iter =
                std::find(inserted_pairs_vec.begin(), inserted_pairs_vec.end(),
                          std::make_pair(key_to_remove, value_to_remove));
            if (vec_iter != inserted_pairs_vec.end()) {
              // Swap with last element and pop
              *vec_iter = inserted_pairs_vec.back();
              inserted_pairs_vec.pop_back();
            }
          }
        }
        // If set_iter was not found but 'removed' is true, it implies BPT
        // removed a pair we weren't tracking or our tracking is imperfect.
        // For a pressure test, we assume BPT behaves as expected.
      }

    } else { // FIND_ALL (operation_type == 2)
      current_key = key_dist(gen);
      if (PRINT_DETAILED_ACTIONS) {
        std::cout << "[" << i << "] Operation: FIND_ALL (" << current_key << ")"
                  << std::endl;
      }
      norb::vector<int> results = norb_map.find_all(current_key);

      // Use the results to prevent optimization
      g_sink += results.size();
      // Example: could also sum values if desired, e.g.
      // for(int val : results) { g_sink += val; }
    }

    if (ALWAYS_TRAVERSE_AFTER_OP) {
      // Assuming norb::BPlusTree has a public void traverse(bool verbose)
      // method or void traverse() method. The original test used
      // `traverse(true)` for this flag.
      norb_map.traverse(true);
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_seconds = end_time - start_time;

  std::cout << "\rProgress: 100%" << std::endl;
  std::cout << "Finished " << num_operations << " operations." << std::endl;
  std::cout << "Total time: " << elapsed_seconds.count() << " seconds."
            << std::endl;
  if (elapsed_seconds.count() > 0) {
    std::cout << "Operations per second: "
              << num_operations / elapsed_seconds.count() << std::endl;
  } else {
    std::cout << "Operations per second: N/A (duration too short)" << std::endl;
  }
  std::cout << "Final sink value (to prevent over-optimization): " << g_sink
            << std::endl;
  if (!inserted_pairs_set.empty() || !inserted_pairs_vec.empty()) {
    std::cout << "Tracked pairs at end: " << inserted_pairs_set.size()
              << " (set) / " << inserted_pairs_vec.size() << " (vector)"
              << std::endl;
  }

  return 0;
}