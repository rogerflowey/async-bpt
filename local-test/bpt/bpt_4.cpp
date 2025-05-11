#include "b_plus_tree.hpp" // Assuming this is the correct path
#include <algorithm>
#include <cassert>
#include <iomanip> // For std::boolalpha
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

// --- Configuration ---
// Set to true to call norb_map.traverse() when an error is detected.
// Requires norb::BPlusTree to have a public void traverse() method.
#define NDEBUG
constexpr bool PRINT_ACTION = false;
constexpr bool DEBUG_TRAVERSE_ON_ERROR = true;
constexpr bool ALWAYS_TRAVERSE = false;
// --- End Configuration ---

int num_operations =
    100000; // Default number of operations, can be overridden by CLI
constexpr int random_seed = 600;
constexpr int debug_point = -1;

// Helper to convert norb::vector to std::vector for easier comparison
template <typename T> std::vector<T> to_std_vector(const norb::vector<T> &nv) {
  std::vector<T> sv;
  sv.reserve(nv.size());
  for (const auto &item : nv) {
    sv.push_back(item);
  }
  return sv;
}

// Helper to print a vector for debugging
template <typename T>
void print_vector_contents(std::ostream &os, const std::vector<T> &vec,
                           const std::string &name) {
  os << name << " (size " << vec.size() << "): [";
  for (size_t k = 0; k < vec.size(); ++k) {
    os << vec[k] << (k == vec.size() - 1 ? "" : ", ");
  }
  os << "]";
}

int main(int argc, char *argv[]) {
  norb::chore::remove_associated();
  if (argc > 1) {
    try {
      num_operations = std::stoi(argv[1]);
      if (num_operations <= 0) {
        std::cerr << "Number of operations must be positive. Using default: "
                  << num_operations << std::endl;
        // Keep default or exit, for now, let's allow default if parsing fails
        // for positive check
        num_operations = 10000; // Reset to a valid default
      }
    } catch (const std::invalid_argument &ia) {
      std::cerr << "Invalid argument for number of operations: " << argv[1]
                << ". Using default: " << num_operations << std::endl;
    } catch (const std::out_of_range &oor) {
      std::cerr << "Number of operations out of range: " << argv[1]
                << ". Using default: " << num_operations << std::endl;
    }
  }

  std::cout << "Starting BPlusTree test with " << num_operations
            << " operations." << std::endl;
  if (DEBUG_TRAVERSE_ON_ERROR) {
    std::cout << "DEBUG_TRAVERSE_ON_ERROR is enabled. norb_map.traverse() will "
                 "be called on errors."
              << std::endl;
  }

  norb::BPlusTree<char, int> norb_map;
  std::multimap<char, int> map;
  std::set<std::pair<char, int>> current_used;

  std::mt19937 gen(random_seed);

  std::uniform_int_distribution<> op_dist(
      0, 2); // 0: insert, 1: remove, 2: find_all
  std::uniform_int_distribution<char> key_dist(
      'A', 'z'); // Smaller key range for more collisions
  std::uniform_int_distribution<int> val_dist(0, 1000000); // Smaller value range

  std::vector<std::pair<char, int>> existing_pairs; // For targeted removal

  for (int i = 0; i < num_operations; ++i) {
    if (PRINT_ACTION) {
      // Progress indicator
      if (num_operations >= 100) {
        if (i % (num_operations / 100) == 0) {
          std::cout << "\rProgress: " << (100 * i / num_operations) << "%\n"
                    << std::flush;
        }
      } else if (i % 10 == 0) { // Print more frequently for small N
        std::cout << "\rProgress: " << (100 * i / num_operations) << "%\n"
                  << std::flush;
      }
    }

    int operation_type = op_dist(gen);
    char current_key = key_dist(gen);
    int current_value = val_dist(gen);

    if (operation_type == 0) // insert
    {
      while (std::find(existing_pairs.begin(), existing_pairs.end(),
                       std::make_pair(current_key, current_value)) !=
             existing_pairs.end()) {
        current_key = key_dist(gen);
        current_value = val_dist(gen);
      }
    }

    if (i == debug_point) {
      std::cerr << "Debug point: " << i << std::endl;
    }

    if (operation_type == 0) { // INSERT
      if (PRINT_ACTION) {
        std::cout << "[" << i << "] Operation: INSERT (" << current_key << ", "
                  << current_value << ")" << std::endl;
      }
      norb_map.insert(current_key, current_value);
      map.insert({current_key, current_value});
      existing_pairs.push_back({current_key, current_value});

    } else if (operation_type == 1) { // REMOVE
      char key_to_remove;
      int value_to_remove;

      // 50% chance to try to remove an actually existing item (if any exist)
      // This makes successful removals more frequent in the test.
      bool try_remove_existing = !existing_pairs.empty() && (gen() % 2 == 0);

      if (try_remove_existing) {
        std::uniform_int_distribution<> existing_idx_dist(
            0, existing_pairs.size() - 1);
        int idx_in_existing = existing_idx_dist(gen);
        key_to_remove = existing_pairs[idx_in_existing].first;
        value_to_remove = existing_pairs[idx_in_existing].second;
        if (PRINT_ACTION) {
          std::cout << "[" << i << "] Operation: REMOVE existing ("
                    << key_to_remove << ", " << value_to_remove << ")"
                    << std::endl;
        }
      } else {
        key_to_remove =
            current_key; // Try to remove a random (possibly non-existent) pair
        value_to_remove = current_value;
        if (PRINT_ACTION) {
          std::cout << "[" << i << "] Operation: REMOVE random ("
                    << key_to_remove << ", " << value_to_remove << ")"
                    << std::endl;
        }
      }

      bool norb_removed = norb_map.remove(key_to_remove, value_to_remove);

      bool std_removed = false;
      auto range = map.equal_range(key_to_remove);
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second == value_to_remove) {
          map.erase(it); // Erase one specific instance
          std_removed = true;
          break;
        }
      }

      if (norb_removed != std_removed) {
        std::cerr << "\n\nERROR at operation " << i << ": REMOVE mismatch!"
                  << std::endl;
        std::cerr << "  Attempted to remove: Key='" << key_to_remove
                  << "', Value=" << value_to_remove << std::endl;
        std::cerr << "  norb_map.remove() returned: " << std::boolalpha
                  << norb_removed << std::endl;
        std::cerr << "  std::multimap removal expected: " << std::boolalpha
                  << std_removed << std::endl;
        if constexpr (DEBUG_TRAVERSE_ON_ERROR) {
          std::cout << "  --- norb_map state (traversal): ---" << std::endl;
          norb_map.traverse(); // Call the traverse method
        }
        assert(norb_removed == std_removed); // Trigger abort
      }

      if (norb_removed) { // If successfully removed from both, update our
                          // tracking list
        // Remove one instance of the pair from existing_pairs
        auto it_existing =
            std::find(existing_pairs.begin(), existing_pairs.end(),
                      std::make_pair(key_to_remove, value_to_remove));
        if (it_existing != existing_pairs.end()) {
          existing_pairs.erase(it_existing);
        }
      }

    } else { // FIND_ALL (operation_type == 2)
      if (PRINT_ACTION) {
        std::cout << "[" << i << "] Operation: FIND_ALL ('" << current_key
                  << "')" << std::endl;
      }
      norb::vector<int> norb_results_norb_vec = norb_map.find_all(current_key);
      std::vector<int> norb_results_std_vec =
          to_std_vector(norb_results_norb_vec);

      std::vector<int> map_results_std_vec;
      auto range = map.equal_range(current_key);
      for (auto it = range.first; it != range.second; ++it) {
        map_results_std_vec.push_back(it->second);
      }

      std::sort(norb_results_std_vec.begin(), norb_results_std_vec.end());
      std::sort(map_results_std_vec.begin(), map_results_std_vec.end());

      if (norb_results_std_vec != map_results_std_vec) {
        std::cerr << "\n\nERROR at operation " << i << ": FIND_ALL mismatch!"
                  << std::endl;
        std::cerr << "  Key searched: '" << current_key << "'" << std::endl;
        print_vector_contents(std::cerr, norb_results_std_vec,
                              "  norb_map results (sorted)");
        std::cerr << std::endl;
        print_vector_contents(std::cerr, map_results_std_vec,
                              "  std::multimap results (sorted)");
        std::cerr << std::endl;
        if constexpr (DEBUG_TRAVERSE_ON_ERROR) {
          std::cout << "  --- norb_map state (traversal): ---" << std::endl;
          // norb_map.traverse(); // Call the traverse method
        }
        assert(norb_results_std_vec == map_results_std_vec); // Trigger abort
      }
    }

    if (ALWAYS_TRAVERSE)
      norb_map.traverse(true);
  }

  std::cout << "\rProgress: 100%" << std::endl;
  std::cout << "All " << num_operations << " operations passed successfully!"
            << std::endl;

  return 0;
}