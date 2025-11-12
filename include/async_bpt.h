#pragma once

#include "persistent_memory_async.hpp"
#include "stlite/filed_config.hpp"
#include "stlite/map.hpp"
#include "stlite/pair.hpp"
#include "stlite/vector.hpp"
#include "tasks.h"
#include "utils.hpp"

#include <cassert>
#include <limits>

#include <functional> // For std::function
#include <queue>      // For std::queue (if not already via other headers)
#include <shared.hpp>
#include <iomanip>    // For std::setw
#include <sstream>    // For std::stringstream in logging

//#define TEMPLATE_CHECK

#ifndef BPT_DEBUG
#define BPT_DEBUG false
#endif

#define BPT_LOG_DEBUG if(BPT_DEBUG) std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | "
#define BPT_LOG_WARN if(BPT_DEBUG) std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | WARN: "
#define BPT_LOG_CRITICAL std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | CRITICAL: "


namespace norb {

  enum class OpType { INSERT, DELETE };
#ifndef TEMPLATE_CHECK
  template <typename idx_t, typename val_t>
#else
  using idx_t = int;
  using val_t = int;
#endif
  class AsyncBPlusTree {
  public:
    using storage_pair_t = Pair<idx_t, val_t>;
    static constexpr val_t val_min = std::numeric_limits<val_t>::min();
    static constexpr val_t val_max = std::numeric_limits<val_t>::max();
#ifndef USE_SMALL_BATCH
    static constexpr size_t FLUSH_THRESHOLD = 10000;
#else
    static constexpr size_t FLUSH_THRESHOLD = 40;
#endif

  private:
    using MutableHandle = PersistentMemoryAsync::MutableHandle;
    template <typename val_t_inner>
    using TrackedConfig = FiledConfig::tracker_t_<val_t_inner>;
    PersistentMemoryAsync& pmem = PersistentMemoryAsync::get_instance();
    static constexpr size_t BATCH_SIZE = std::min(SLOT_MAX_SIZE/4,size_t(20));

    enum node_type { index, leaf };

    sjtu::map<storage_pair_t, OpType> write_map_;
    bool is_during_flush = false;
    wutong::CriteriaVariable<size_t> unfinished_count{0,[](const size_t& count){ return count == 0; }};




  public:
    struct IndexNode {
      static constexpr size_t aux_var_size = sizeof(size_t) * 2;
#ifndef USE_SMALL_BATCH
      static constexpr size_t node_capacity =
          (PAGE_SIZE - aux_var_size) /
              (sizeof(storage_pair_t) + sizeof(MutableHandle)) -
          1;
#else
      static constexpr size_t node_capacity = OVERWRITE_BLOCK_SIZE;
#endif
      static constexpr size_t merge_threshold = node_capacity * .25f;
      static constexpr size_t split_threshold = node_capacity * .75f;

      static_assert(PAGE_SIZE - aux_var_size >
                    (sizeof(idx_t) + sizeof(MutableHandle)));
      static_assert(node_capacity >= 4);

      size_t layer = 0;
      size_t size = 0;
      storage_pair_t data[node_capacity];
      MutableHandle children[node_capacity + 1];
    };

    struct LeafNode {
      static constexpr size_t aux_var_size =
          sizeof(size_t) + sizeof(MutableHandle);
#ifndef USE_SMALL_BATCH
      static constexpr size_t node_capacity =
          (PAGE_SIZE - aux_var_size) / sizeof(storage_pair_t);
#else
      static constexpr size_t node_capacity = OVERWRITE_BLOCK_SIZE;
#endif

      static constexpr size_t merge_threshold = node_capacity * .25f;
      static constexpr size_t split_threshold = node_capacity * .75f;
      static_assert(PAGE_SIZE - aux_var_size >
                    (sizeof(idx_t) + sizeof(MutableHandle)));
      static_assert(node_capacity >= 4);

      size_t size = 0;
      storage_pair_t data[node_capacity];
      MutableHandle sibling;
    };

    TrackedConfig<size_t> tree_height_ = FiledConfig::track<size_t>(0);
    TrackedConfig<MutableHandle> root_handle_ =
        FiledConfig::track<MutableHandle>();

    struct PinnedPath {
      sjtu::vector<
          Pair<PersistentMemoryAsync::ConstHandledReference<IndexNode>, size_t>>
          index_pos;
      std::optional<PersistentMemoryAsync::ConstHandledReference<LeafNode>>
          leaf_ref;
      sjtu::vector<MutableHandle> original_path_handles;

      bool is_valid() const { return leaf_ref.has_value(); }

      void clear() {
        index_pos.clear();
        leaf_ref.reset();
        original_path_handles.clear();
      }

      size_t height() const {
        return index_pos.size() + (leaf_ref.has_value() ? 1 : 0);
      }

      PinnedPath() = default;
    };

    static size_t find_idx_in_index(const IndexNode &node,
                                    const storage_pair_t &target_pair) {
      if (node.size == 0)
        return 0;
      size_t left = 0, right = node.size;
      while (left < right) {
        const size_t mid = (left + right) / 2;
        if (node.data[mid] > target_pair) {
          right = mid;
        } else if (node.data[mid] < target_pair) {
          left = mid + 1;
        } else {
          return mid + 1;
        }
      }
      return left;
    }

    static size_t lower_bound_in_leaf(const LeafNode &node,
                                      const storage_pair_t &target_pair) {
      size_t left = 0, right = node.size;
      while (left < right) {
        const size_t mid = (left + right) / 2;
        if (node.data[mid] >= target_pair)
          right = mid;
        else
          left = mid + 1;
      }
      return left;
    }

    wutong::Task<PinnedPath>
    find_path(const storage_pair_t &target_pair) const {
      BPT_LOG_DEBUG << "Entering. Target: (" << target_pair.first << "," << target_pair.second << ")" << std::endl;
      PinnedPath pinned_path;
      if (tree_height_.val == 0) {
        BPT_LOG_DEBUG << "Tree is empty (height 0)." << std::endl;
        co_return pinned_path;
      }

      MutableHandle current_handle = root_handle_.val;
      BPT_LOG_DEBUG << "Root handle: " << current_handle.page_id << std::endl;
      for (size_t current_level = 0;
           current_level < tree_height_.val - 1; ++current_level) {
        BPT_LOG_DEBUG << "Level " << current_level << ", current_handle: " << current_handle.page_id << std::endl;
        if (current_handle.is_nullptr()) {
          BPT_LOG_WARN << "Null handle encountered at level " << current_level << " in index traversal." << std::endl;
          pinned_path.clear();
          co_return pinned_path;
        }
        pinned_path.original_path_handles.push_back(current_handle);

        auto index_node_const_ref =
            co_await current_handle.const_ref<IndexNode>();
        BPT_LOG_DEBUG << "Fetched IndexNode " << current_handle.page_id << ": " << format_index_contents(*index_node_const_ref, current_handle.page_id) << std::endl;
        size_t child_idx_in_node =
            find_idx_in_index(*index_node_const_ref, target_pair);
        BPT_LOG_DEBUG << "Index node (page " << current_handle.page_id << ") size: " << index_node_const_ref->size
                  << ", child_idx_in_node for target: " << child_idx_in_node << std::endl;
        pinned_path.index_pos.push_back(norb::make_pair(
            std::move(index_node_const_ref), child_idx_in_node));
        current_handle =
            pinned_path.index_pos.back().first->children[child_idx_in_node];
           }

      BPT_LOG_DEBUG << "Reached leaf level, current_handle: " << current_handle.page_id << std::endl;
      if (current_handle.is_nullptr()) {
        BPT_LOG_WARN << "Null handle encountered for leaf node." << std::endl;
        pinned_path.clear();
        co_return pinned_path;
      }
      pinned_path.original_path_handles.push_back(current_handle);

      auto leaf_node_const_ref = co_await current_handle.const_ref<LeafNode>();
      BPT_LOG_DEBUG << "Fetched LeafNode " << current_handle.page_id << ": " << format_leaf_contents(*leaf_node_const_ref, current_handle.page_id) << std::endl;
      pinned_path.leaf_ref = std::move(leaf_node_const_ref);
      BPT_LOG_DEBUG << "Leaf node (page " << current_handle.page_id << ") obtained. Path valid: " << pinned_path.is_valid() << std::endl;

      co_return pinned_path;
    }

    wutong::Task<MutableHandle>
    find_leaf(const storage_pair_t &target_pair) const {
      BPT_LOG_DEBUG << "Entering. Target: (" << target_pair.first << "," << target_pair.second << ")" << std::endl;
      PinnedPath path = co_await find_path(target_pair);
      if (!path.is_valid()) {
        BPT_LOG_DEBUG << "Path not valid, returning null handle." << std::endl;
        co_return MutableHandle{};
      }
      MutableHandle leaf_handle = path.original_path_handles.back();
      BPT_LOG_DEBUG << "Path valid, returning leaf handle: " << leaf_handle.page_id << std::endl;
      co_return leaf_handle;
    }

    wutong::Task<sjtu::vector<storage_pair_t>>
    find_range_on_bpt_async(const storage_pair_t &range_start,
                            const storage_pair_t &range_end) const {
      BPT_LOG_DEBUG << "Entering. Range: [(" << range_start.first << "," << range_start.second << "), ("
                << range_end.first << "," << range_end.second << "))" << std::endl;
      sjtu::vector<storage_pair_t> bpt_pairs_vec;
      if (tree_height_.val == 0) {
        BPT_LOG_DEBUG << "Tree is empty (height 0)." << std::endl;
        co_return bpt_pairs_vec;
      }
      MutableHandle leaf_handle = co_await find_leaf(range_start);
      BPT_LOG_DEBUG << "Initial leaf handle for range start: " << leaf_handle.page_id << std::endl;
      while (!leaf_handle.is_nullptr()) {
        BPT_LOG_DEBUG << "Processing leaf node: " << leaf_handle.page_id << std::endl;
        auto leaf_node_obj = co_await leaf_handle.const_ref<LeafNode>();
        const LeafNode &leaf_node = *leaf_node_obj;
        BPT_LOG_DEBUG << "Current " << format_leaf_contents(leaf_node, leaf_handle.page_id) << std::endl;
        size_t pos_in_leaf = lower_bound_in_leaf(leaf_node, range_start);
        BPT_LOG_DEBUG << "Leaf node size: " << leaf_node.size << ", starting pos_in_leaf: " << pos_in_leaf << std::endl;
        for (; pos_in_leaf < leaf_node.size; ++pos_in_leaf) {
          const storage_pair_t &current_pair_in_leaf =
              leaf_node.data[pos_in_leaf];
          if (current_pair_in_leaf >= range_end) {
            BPT_LOG_DEBUG << "Current pair (" << current_pair_in_leaf.first << "," << current_pair_in_leaf.second
                      << ") >= range_end. Stopping scan in this leaf." << std::endl;
            leaf_handle.set_nullptr(); // To break outer loop
            break;
          }
          BPT_LOG_DEBUG << "Adding pair from BPT: (" << current_pair_in_leaf.first << "," << current_pair_in_leaf.second << ")" << std::endl;
          bpt_pairs_vec.push_back(current_pair_in_leaf);
        }
        if (leaf_handle.is_nullptr()) {
          BPT_LOG_DEBUG << "Exiting leaf scan due to range_end condition." << std::endl;
          break;
        }
        leaf_handle = leaf_node.sibling;
        BPT_LOG_DEBUG << "Moving to sibling leaf: " << leaf_handle.page_id << std::endl;
      }
      BPT_LOG_DEBUG << "Finished BPT range scan. Found " << bpt_pairs_vec.size() << " pairs." << std::endl;
      co_return bpt_pairs_vec;
    }

  public:
    AsyncBPlusTree() = default;
    ~AsyncBPlusTree() = default;

    // --- Public API (Asynchronous) ---
    wutong::Task<sjtu::vector<storage_pair_t>>
    find_range_async(const storage_pair_t &range_start,
                     const storage_pair_t &range_end) {
      BPT_LOG_DEBUG << "Entering. Range: [(" << range_start.first << "," << range_start.second << "), ("
                << range_end.first << "," << range_end.second << "))" << std::endl;
      (**unfinished_count)++;
      BPT_LOG_DEBUG << "unfinished_count incremented to " << unfinished_count.get_value() << std::endl;

      sjtu::vector<storage_pair_t> result_vec;

      sjtu::vector<Pair<storage_pair_t, OpType>> map_ops_in_range;
      auto map_it_start = write_map_.lower_bound(range_start);
      auto map_it_end = write_map_.lower_bound(range_end);
      for (auto it_map = map_it_start; it_map != map_it_end; ++it_map) {
        map_ops_in_range.push_back(
            norb::make_pair(it_map->first, it_map->second));
      }
      BPT_LOG_DEBUG << "Found " << map_ops_in_range.size() << " ops in write_map_ for the range." << std::endl;

      sjtu::vector<storage_pair_t> bpt_pairs_in_range =
          co_await find_range_on_bpt_async(range_start, range_end);
      BPT_LOG_DEBUG << "Found " << bpt_pairs_in_range.size() << " pairs in BPT for the range." << std::endl;

      // standard merge for ordered vectors;
      size_t map_idx = 0;
      size_t bpt_idx = 0;
      BPT_LOG_DEBUG << "Starting merge of write_map_ ops and BPT pairs." << std::endl;

      while (map_idx < map_ops_in_range.size() &&
             bpt_idx < bpt_pairs_in_range.size()) {
        const storage_pair_t &map_pair = map_ops_in_range[map_idx].first;
        OpType map_op = map_ops_in_range[map_idx].second;
        const storage_pair_t &bpt_pair = bpt_pairs_in_range[bpt_idx];

        if (map_pair < bpt_pair) {
          if (map_op == OpType::INSERT) {
            BPT_LOG_DEBUG << "Merge: Adding from map (INSERT): (" << map_pair.first << "," << map_pair.second << ")" << std::endl;
            result_vec.push_back(map_pair);
          } else {
            BPT_LOG_DEBUG << "Merge: Skipping from map (DELETE): (" << map_pair.first << "," << map_pair.second << ")" << std::endl;
          }
          map_idx++;
        } else if (bpt_pair < map_pair) {
          BPT_LOG_DEBUG << "Merge: Adding from BPT: (" << bpt_pair.first << "," << bpt_pair.second << ")" << std::endl;
          result_vec.push_back(bpt_pair);
          bpt_idx++;
        } else { // map_pair == bpt_pair
          if (map_op == OpType::INSERT) {
            BPT_LOG_DEBUG << "Merge: Adding from map (INSERT, overrides BPT): (" << map_pair.first << "," << map_pair.second << ")" << std::endl;
            result_vec.push_back(map_pair);
          } else {
            BPT_LOG_DEBUG << "Merge: Skipping (DELETE in map, matches BPT): (" << map_pair.first << "," << map_pair.second << ")" << std::endl;
          }
          map_idx++;
          bpt_idx++;
        }
      }
      while (map_idx < map_ops_in_range.size()) {
        if (map_ops_in_range[map_idx].second == OpType::INSERT) {
          BPT_LOG_DEBUG << "Merge (tail map): Adding from map (INSERT): (" << map_ops_in_range[map_idx].first.first << "," << map_ops_in_range[map_idx].first.second << ")" << std::endl;
          result_vec.push_back(map_ops_in_range[map_idx].first);
        } else {
          BPT_LOG_DEBUG << "Merge (tail map): Skipping from map (DELETE): (" << map_ops_in_range[map_idx].first.first << "," << map_ops_in_range[map_idx].first.second << ")" << std::endl;
        }
        map_idx++;
      }
      while (bpt_idx < bpt_pairs_in_range.size()) {
        BPT_LOG_DEBUG << "Merge (tail BPT): Adding from BPT: (" << bpt_pairs_in_range[bpt_idx].first << "," << bpt_pairs_in_range[bpt_idx].second << ")" << std::endl;
        result_vec.push_back(bpt_pairs_in_range[bpt_idx]);
        bpt_idx++;
      }
      (**unfinished_count)--;
      BPT_LOG_DEBUG << "unfinished_count decremented to " << unfinished_count.get_value() << ". Merge complete. Result size: " << result_vec.size() << std::endl;
      co_return result_vec;
    }

    wutong::Task<sjtu::vector<storage_pair_t>>
    find_all_async(const idx_t &hashed_key) {
      BPT_LOG_DEBUG << "Entering. Hashed key: " << hashed_key << std::endl;
      auto result = co_await find_range_async(norb::make_pair(hashed_key, val_min),
                                          norb::make_pair(hashed_key, val_max));
      BPT_LOG_DEBUG << "Exiting. Found " << result.size() << " pairs for hashed key " << hashed_key << std::endl;
      co_return result;
    }



    wutong::Task<void> insert_async(const idx_t &key, const val_t &val) {
      BPT_LOG_DEBUG << "Entering. Key: " << key << ", Val: " << val << ". write_map_ size: " << write_map_.size() << std::endl;

      storage_pair_t target_pair = norb::make_pair(key, val);
      write_map_[target_pair] = OpType::INSERT;
      BPT_LOG_DEBUG << "Inserted (" << key << "," << val << ") into write_map_. New size: " << write_map_.size() << std::endl;
      if(write_map_.size()>=FLUSH_THRESHOLD) {
        BPT_LOG_DEBUG << "Flush threshold (" << FLUSH_THRESHOLD << ") reached. Triggering flush." << std::endl;
        co_await flush();
      }
      co_return;
    }

    wutong::Task<void> remove_async(const idx_t &key, const val_t &val) {
      BPT_LOG_DEBUG << "Entering. Key: " << key << ", Val: " << val << ". write_map_ size: " << write_map_.size() << std::endl;
      storage_pair_t target_pair = norb::make_pair(key, val);
      write_map_[target_pair] = OpType::DELETE;
      BPT_LOG_DEBUG << "Marked (" << key << "," << val << ") as DELETE in write_map_. New size: " << write_map_.size() << std::endl;
      if(write_map_.size()>=FLUSH_THRESHOLD) {
        BPT_LOG_DEBUG << "Flush threshold (" << FLUSH_THRESHOLD << ") reached. Triggering flush." << std::endl;
        co_await flush();
      }
      co_return;
    }


    wutong::Task<void> shutdown() {
      BPT_LOG_DEBUG << "Entering shutdown sequence." << std::endl;
      co_await flush();
      BPT_LOG_DEBUG << "Flush completed. Calling pmem.flush_all()." << std::endl;
      co_await pmem.flush_all();
      BPT_LOG_DEBUG << "pmem.flush_all() completed. Shutdown finished." << std::endl;
      co_return;
    }



    wutong::Task<void> flush() {
    BPT_LOG_DEBUG << "Entering flush. write_map_ size: " << write_map_.size() << ", unfinished_count: " << unfinished_count.get_value() << std::endl;
    wutong::SmartTask<void> prefetch_task{};
#ifdef BPT_DISABLE_PREFETCH
    constexpr bool ENABLE_PREFETCH = false;
#else
    constexpr bool ENABLE_PREFETCH = true;
#endif
    is_during_flush = true;
    BPT_LOG_DEBUG << "is_during_flush set to true." << std::endl;
    co_await unfinished_count;
      assert(unfinished_count.get_value()==0);

    BPT_LOG_DEBUG << "All other operations finished." << std::endl;

    if (write_map_.empty()) {
        BPT_LOG_DEBUG << "write_map_ is empty. Nothing to flush." << std::endl;
        is_during_flush = false;
        BPT_LOG_DEBUG << "is_during_flush set to false." << std::endl;
        co_return;
    }

    if (root_handle_.val.is_nullptr()) {
        BPT_LOG_DEBUG << "Root handle is null. Initializing tree from write_map_." << std::endl;
        sjtu::vector<storage_pair_t> new_data;
        for (auto &op : write_map_) {
            if (op.second == OpType::INSERT) {
                new_data.push_back(op.first);
            }
        }
        BPT_LOG_DEBUG << "Collected " << new_data.size() << " items for initial tree construction." << std::endl;
        co_await initialize_from_vector_async(new_data);
        write_map_.clear();
        is_during_flush = false;
        BPT_LOG_DEBUG << "Tree initialized. write_map_ cleared. is_during_flush set to false." << std::endl;
        co_return;
    }

    auto it = write_map_.begin();
    PinnedPath path;
    bool path_needs_refresh = true;

    int ops_since_prefetch = 0;
    const int PREFETCH_OPS_LIMIT = 1000; // This is a local const

    page_id_t last_leaf_id_prefetch = -1;
    int distinct_leaves_prefetch = 0;
    const int PREFETCH_LEAF_LIMIT = 0.75*BATCH_SIZE; // This is a local const

    BPT_LOG_DEBUG << "Starting to process write_map_ operations." << std::endl;
    while (it != write_map_.end()) {
        storage_pair_t key = it->first;
        OpType op_type = it->second;
        BPT_LOG_DEBUG << "Processing op: " << (op_type == OpType::INSERT ? "INSERT" : "DELETE")
                  << " for key (" << key.first << "," << key.second << "). ops_since_prefetch: " << ops_since_prefetch
                  << ", distinct_leaves_prefetch: " << distinct_leaves_prefetch << std::endl;

        if (path_needs_refresh) {
            BPT_LOG_DEBUG << "Path needs refresh. Finding path for key (" << key.first << "," << key.second << ")." << std::endl;
            path.clear();
            path = co_await find_path(key); // find_path already logs fetched nodes
            path_needs_refresh = false;
            BPT_LOG_DEBUG << "Path found. Valid: " << path.is_valid() << ", Height: " << path.height() << std::endl;

            if (path.is_valid() && path.leaf_ref.has_value()) {
                MutableHandle leaf_handle = path.original_path_handles.back();
                if (leaf_handle.page_id != last_leaf_id_prefetch) {
                    distinct_leaves_prefetch++;
                    last_leaf_id_prefetch = leaf_handle.page_id;
                    BPT_LOG_DEBUG << "New distinct leaf for prefetch: " << leaf_handle.page_id << ". Count: " << distinct_leaves_prefetch << std::endl;
                }
            }
        }

        bool should_trigger_prefetch = (ops_since_prefetch >= PREFETCH_OPS_LIMIT) ||
                                       (distinct_leaves_prefetch >= PREFETCH_LEAF_LIMIT);

        if (ENABLE_PREFETCH && should_trigger_prefetch) {
          BPT_LOG_DEBUG << "Prefetch condition met. ops_since_prefetch: " << ops_since_prefetch
                    << ", distinct_leaves_prefetch: " << distinct_leaves_prefetch << std::endl;
          if (prefetch_task.is_valid()) {
            BPT_LOG_DEBUG << "Awaiting previous prefetch task." << std::endl;
            co_await prefetch_task;
            BPT_LOG_DEBUG << "Previous prefetch task completed." << std::endl;
          }
          if (path.is_valid() && !path.original_path_handles.empty()) {
            BPT_LOG_DEBUG << "Getting prefetch IDs." << std::endl;
            sjtu::vector<page_id_t> prefetch_ids = get_prefetch_id(path, it);
            if (!prefetch_ids.empty()) {
                BPT_LOG_DEBUG << "Starting new prefetch task for " << prefetch_ids.size() << " pages." << std::endl;
                prefetch_task = pmem.prefetch_batch(prefetch_ids);
            } else {
                BPT_LOG_DEBUG << "No prefetch IDs generated." << std::endl;
            }
          } else {
            BPT_LOG_DEBUG << "Path not valid or original_path_handles empty, skipping prefetch." << std::endl;
          }
          ops_since_prefetch = 0;
          distinct_leaves_prefetch = 0;
          BPT_LOG_DEBUG << "Prefetch counters reset." << std::endl;
        }

        if (!path.is_valid() && tree_height_.val > 0) {
            BPT_LOG_CRITICAL << "Find Path failed for key (" << key.first << "," << key.second << ") during flush." << std::endl;
        }

        BPT_LOG_DEBUG << "Handling operation for key (" << key.first << "," << key.second << ")." << std::endl;
        bool structure_changed = co_await handle_operation(key, op_type, path);
        BPT_LOG_DEBUG << "Operation handled. Structure changed: " << structure_changed << std::endl;

        ++it;
        if (structure_changed) {
            BPT_LOG_DEBUG << "Structure changed, path needs refresh for next op." << std::endl;
            path_needs_refresh = true;
        } else {
            bool in_current_leaf = false;
            if (it != write_map_.end() &&
                path.is_valid() && path.leaf_ref.has_value()) {
                const auto& leaf_ref_val = path.leaf_ref.value(); // This is a ConstHandledReference
                // To log its state, we can use it directly
                // const LeafNode& leaf = *leaf_ref_val; // Already dereferenced by format_leaf_contents
                storage_pair_t next_key = it->first;

                // We need the actual leaf content for the check
                // The PinnedPath leaf_ref is const. If we need to check its content against next_key, it's fine.
                // The check itself doesn't require a mutable ref.
                const LeafNode& current_leaf_node_content = *(path.leaf_ref.value());


                in_current_leaf = (current_leaf_node_content.size > 0 &&
                                          next_key >= current_leaf_node_content.data[0] &&
                                          next_key <= current_leaf_node_content.data[current_leaf_node_content.size - 1]);
                BPT_LOG_DEBUG << "Next key (" << next_key.first << "," << next_key.second
                          << ") in current leaf (page " << path.original_path_handles.back().page_id
                          << ", range [" << (current_leaf_node_content.size > 0 ? std::to_string(current_leaf_node_content.data[0].first) : "N/A")
                          << "," << (current_leaf_node_content.size > 0 ? std::to_string(current_leaf_node_content.data[current_leaf_node_content.size-1].first) : "N/A")
                          << "]): " << in_current_leaf << std::endl;
            }
            path_needs_refresh = !in_current_leaf;
            if (path_needs_refresh) {
                BPT_LOG_DEBUG << "Next key not in current leaf or end of map. Path needs refresh." << std::endl;
            }
        }
        ops_since_prefetch++;
    }
    BPT_LOG_DEBUG << "Finished processing all write_map_ operations." << std::endl;
    if (ENABLE_PREFETCH && prefetch_task.is_valid()) {
        BPT_LOG_DEBUG << "Awaiting final prefetch task." << std::endl;
        co_await prefetch_task;
        BPT_LOG_DEBUG << "Final prefetch task completed." << std::endl;
    }

    write_map_.clear();
    is_during_flush = false;
    BPT_LOG_DEBUG << "write_map_ cleared. is_during_flush set to false. Flush complete." << std::endl;
    co_return;
}

    bool permit_operation() {
      return pmem.permit_IO() && !is_during_flush && write_map_.size()<FLUSH_THRESHOLD;
    }

    void poll() {
      pmem.poll();
    }

    sjtu::vector<page_id_t> get_prefetch_id(
        const PinnedPath& path,
        typename sjtu::map<storage_pair_t, OpType>::iterator map_it
    ) {
      BPT_LOG_DEBUG << "Entering. Current map_it key: "
                << (map_it != write_map_.end() ? "(" + std::to_string(map_it->first.first) + "," + std::to_string(map_it->first.second) + ")" : "end")
                << std::endl;
      sjtu::vector<page_id_t> prefetch_ids;

      if (tree_height_.val == 0) {
        BPT_LOG_DEBUG << "Tree height is 0, no pages to prefetch." << std::endl;
        return prefetch_ids;
      }
      if (tree_height_.val == 1) {
        BPT_LOG_DEBUG << "Tree height is 1 (only root leaf)." << std::endl;
        if (!root_handle_.val.is_nullptr() && map_it != write_map_.end()) {
          auto opt_root_leaf_ref = root_handle_.val.template try_const_ref<LeafNode>();
          if (!opt_root_leaf_ref) {
            BPT_LOG_DEBUG << "Root leaf (page " << root_handle_.val.page_id << ") not in cache, adding to prefetch." << std::endl;
            prefetch_ids.push_back(root_handle_.val.page_id);
          } else {
            BPT_LOG_DEBUG << "Root leaf (page " << root_handle_.val.page_id << ") already in cache." << std::endl;
          }
        }
        BPT_LOG_DEBUG << "Exiting. Prefetch IDs size: " << prefetch_ids.size() << std::endl;
        return prefetch_ids;
      }

      auto current_path_prefix = path.index_pos; // Create a mutable copy
      BPT_LOG_DEBUG << "Initial path_prefix size: " << current_path_prefix.size() << std::endl;


      while (prefetch_ids.size() < BATCH_SIZE) {
        if (map_it == write_map_.end()) {
            BPT_LOG_DEBUG << "Reached end of write_map_. Stopping prefetch candidate search." << std::endl;
            break;
        }

        storage_pair_t next_target_key = map_it->first;
        BPT_LOG_DEBUG << "Next target key for prefetch_step: (" << next_target_key.first << "," << next_target_key.second << ")" << std::endl;

        Pair<page_id_t, storage_pair_t> prefetch_info =
            next_prefetch_step(current_path_prefix, next_target_key);

        page_id_t page_to_load = prefetch_info.first;
        storage_pair_t next_separator_key = prefetch_info.second;
        BPT_LOG_DEBUG << "next_prefetch_step returned: page_to_load=" << page_to_load
                  << ", next_separator_key=(" << next_separator_key.first << "," << next_separator_key.second << ")" << std::endl;


        if (page_to_load != -1 && page_to_load != INVALID_PAGE_ID) {
           if (page_to_load < PersistentMemoryAsync::get_page_count()) {
             BPT_LOG_DEBUG << "Adding page " << page_to_load << " to prefetch list." << std::endl;
             prefetch_ids.push_back(page_to_load);
           } else {
             BPT_LOG_WARN << "Prefetch candidate page " << page_to_load
                          << " exceeds current page count " << PersistentMemoryAsync::get_page_count()
                          << ". Skipping." << std::endl;
           }
        }
        map_it = write_map_.lower_bound(next_separator_key);
        BPT_LOG_DEBUG << "Advanced map_it. New key: "
                  << (map_it != write_map_.end() ? "(" + std::to_string(map_it->first.first) + "," + std::to_string(map_it->first.second) + ")" : "end")
                  << ". Prefetch IDs size: " << prefetch_ids.size() << std::endl;
      }
      BPT_LOG_DEBUG << "Exiting. Prefetch IDs collected: " << prefetch_ids.size() << std::endl;
      return prefetch_ids;
    }

    Pair<page_id_t,storage_pair_t> next_prefetch_step(
        sjtu::vector<Pair<PersistentMemoryAsync::ConstHandledReference<IndexNode>, size_t>>& path_prefix,
        const storage_pair_t& target_key
    ) {
      BPT_LOG_DEBUG << "Entering. Target key: (" << target_key.first << "," << target_key.second
                << "). Initial path_prefix size: " << path_prefix.size() << std::endl;

      while (!path_prefix.empty()) {
        auto& current_level_info = path_prefix.back();
        const IndexNode& current_node = *(current_level_info.first);
        BPT_LOG_DEBUG << "Ascending: current_node (page " << current_level_info.first.get_handle().page_id
                  << ", size " << current_node.size << ", layer " << current_node.layer << ")" << std::endl;

        size_t child_idx = find_idx_in_index(current_node, target_key);
        BPT_LOG_DEBUG << "find_idx_in_index returned: " << child_idx << std::endl;

        if (child_idx <= current_node.size) {
             if (child_idx < current_node.size || path_prefix.size() == 1 || current_node.size == 0) {
                current_level_info.second = child_idx;
                BPT_LOG_DEBUG << "Found valid divergence/descent point at current level. child_idx updated to " << child_idx << std::endl;
                break;
             } else if (child_idx == current_node.size && (path_prefix.size() == 1 || current_node.size == 0)) {
                current_level_info.second = child_idx;
                BPT_LOG_DEBUG << "Found valid rightmost divergence/descent point for root/empty. child_idx updated to " << child_idx << std::endl;
                break;
             }
        }
        BPT_LOG_DEBUG << "Cannot use child_idx " << child_idx << " at this level (size " << current_node.size << ", path_prefix.size " << path_prefix.size() << "). Popping path_prefix." << std::endl;
        path_prefix.pop_back();
      }

      if (path_prefix.empty()) {
          storage_pair_t sp_max = {std::numeric_limits<idx_t>::max(),std::numeric_limits<val_t>::max()};
          BPT_LOG_WARN << "path_prefix became empty. Target key might be outside initial path's scope. Returning {-1, MAX_PAIR}." << std::endl;
          return {INVALID_PAGE_ID, sp_max};
      }

      BPT_LOG_DEBUG << "Descending from path_prefix.size() = " << path_prefix.size() << " (tree_height-1 = " << (tree_height_.val > 0 ? tree_height_.val -1 : 0) << ")" << std::endl;
      storage_pair_t upper_bound_for_next_key = {std::numeric_limits<idx_t>::max(),std::numeric_limits<val_t>::max()};

      while (path_prefix.size() < (tree_height_.val > 0 ? tree_height_.val - 1 : 0) ) {
        auto& current_level_ref_pair = path_prefix.back();
        PersistentMemoryAsync::ConstHandledReference<IndexNode> current_node_ref = current_level_ref_pair.first;
        size_t current_child_idx_in_node = current_level_ref_pair.second;

        BPT_LOG_DEBUG << "Descending loop: current_node (page " << current_node_ref.get_handle().page_id
                  << ", size " << current_node_ref->size << ", layer " << current_node_ref->layer
                  << "), child_idx " << current_child_idx_in_node << std::endl;
        BPT_LOG_DEBUG << "Current " << format_index_contents(*current_node_ref, current_node_ref.get_handle().page_id) << std::endl;


        if(current_child_idx_in_node < current_node_ref->size) {
          upper_bound_for_next_key = current_node_ref->data[current_child_idx_in_node];
          BPT_LOG_DEBUG << "Set upper_bound_for_next_key from data[" << current_child_idx_in_node << "]: ("
                    << upper_bound_for_next_key.first << "," << upper_bound_for_next_key.second << ")" << std::endl;
        } else {
          BPT_LOG_DEBUG << "current_child_idx " << current_child_idx_in_node << " is rightmost child or node empty. upper_bound_for_next_key remains: ("
                    << upper_bound_for_next_key.first << "," << upper_bound_for_next_key.second << ")" << std::endl;
        }

        MutableHandle child_handle = current_node_ref->children[current_child_idx_in_node];
        BPT_LOG_DEBUG << "Child handle to descend to: " << child_handle.page_id << std::endl;
        auto opt_child_node_ref = child_handle.template try_const_ref<IndexNode>();

        if (!opt_child_node_ref) {
            BPT_LOG_DEBUG << "Child index node (page " << child_handle.page_id << ") not in cache. Returning its page_id for prefetch." << std::endl;
            return {child_handle.page_id, upper_bound_for_next_key};
        }
        BPT_LOG_DEBUG << "Child index node (page " << child_handle.page_id << ") is in cache." << std::endl;

        PersistentMemoryAsync::ConstHandledReference<IndexNode> next_node_ref = opt_child_node_ref.value();
        size_t new_child_idx_in_next_node = find_idx_in_index(*next_node_ref, target_key);
        path_prefix.emplace_back(std::move(next_node_ref), new_child_idx_in_next_node);
        BPT_LOG_DEBUG << "Descended. New path_prefix size: " << path_prefix.size() << ". New child_idx for next level: " << new_child_idx_in_next_node << std::endl;
      }

      auto& final_index_level_ref_pair = path_prefix.back();
      PersistentMemoryAsync::ConstHandledReference<IndexNode> final_index_node_ref = final_index_level_ref_pair.first;
      size_t child_idx_in_final_index = final_index_level_ref_pair.second;

      BPT_LOG_DEBUG << "Reached parent of leaf level. Final index_node (page " << final_index_node_ref.get_handle().page_id
                << ", size " << final_index_node_ref->size << "), child_idx " << child_idx_in_final_index << std::endl;
      BPT_LOG_DEBUG << "Final index " << format_index_contents(*final_index_node_ref, final_index_node_ref.get_handle().page_id) << std::endl;


      if(child_idx_in_final_index > final_index_node_ref->size) {
        BPT_LOG_WARN << "Child index " << child_idx_in_final_index << " exceeds node size " << final_index_node_ref->size
                     << " for index node " << final_index_node_ref.get_handle().page_id
                     << ". Skipping prefetch for this path." << std::endl;
        return {INVALID_PAGE_ID, upper_bound_for_next_key};
      }

      if(child_idx_in_final_index < final_index_node_ref->size) {
        upper_bound_for_next_key = final_index_node_ref->data[child_idx_in_final_index];
        BPT_LOG_DEBUG << "Set upper_bound_for_next_key from final index node data[" << child_idx_in_final_index << "]: ("
                  << upper_bound_for_next_key.first << "," << upper_bound_for_next_key.second << ")" << std::endl;
      } else {
        BPT_LOG_DEBUG << "Final current_child_idx " << child_idx_in_final_index << " is rightmost. upper_bound_for_next_key remains: ("
                  << upper_bound_for_next_key.first << "," << upper_bound_for_next_key.second << ")" << std::endl;
      }

      MutableHandle leaf_handle = final_index_node_ref->children[child_idx_in_final_index];
      BPT_LOG_DEBUG << "Target leaf handle: " << leaf_handle.page_id << std::endl;
      auto opt_leaf_ref = leaf_handle.template try_const_ref<LeafNode>();
      if(!opt_leaf_ref) {
        BPT_LOG_DEBUG << "Leaf node (page " << leaf_handle.page_id << ") not in cache. Returning its page_id for prefetch." << std::endl;
        return {leaf_handle.page_id, upper_bound_for_next_key};
      }

      BPT_LOG_DEBUG << "Leaf node (page " << leaf_handle.page_id << ") is in cache: " << format_leaf_contents(*opt_leaf_ref.value(), leaf_handle.page_id) << ". No prefetch needed for this path. Returning INVALID_PAGE_ID." << std::endl;
      return {INVALID_PAGE_ID, upper_bound_for_next_key};
    }

    wutong::Task<bool> handle_operation(storage_pair_t value, OpType op, PinnedPath &path) {
        BPT_LOG_DEBUG << "Entering. Op: " << (op == OpType::INSERT ? "INSERT" : "DELETE")
                  << ", Value: (" << value.first << "," << value.second << ")"
                  << ", Path valid: " << path.is_valid() << ", Path height: " << path.height() << std::endl;
        bool structural_or_key_change_occurred = false;

        if (tree_height_.val == 0) {
            BPT_LOG_DEBUG << "Tree is empty (height 0)." << std::endl;
            if (op == OpType::INSERT) {
                BPT_LOG_DEBUG << "Performing INSERT on empty tree." << std::endl;
                MutableHandle new_leaf_handle = co_await pmem.create_mutable_and_init<LeafNode>();
                auto leaf_ref_init = co_await new_leaf_handle.ref<LeafNode>(true);
                BPT_LOG_DEBUG << "Before INSERT on new Leaf " << new_leaf_handle.page_id << ": " << format_leaf_contents(*leaf_ref_init, new_leaf_handle.page_id) << std::endl;
                leaf_ref_init->data[0] = value;
                leaf_ref_init->size = 1;
                BPT_LOG_DEBUG << "After INSERT on new Leaf " << new_leaf_handle.page_id << ": " << format_leaf_contents(*leaf_ref_init, new_leaf_handle.page_id) << std::endl;
                root_handle_.val = new_leaf_handle;
                tree_height_.val = 1;
                BPT_LOG_DEBUG << "New root (leaf " << new_leaf_handle.page_id << ") created. Tree height now 1." << std::endl;
                co_return true;
            } else {
                BPT_LOG_DEBUG << "Performing DELETE on empty tree. No change." << std::endl;
                co_return false;
            }
        }

        if (!path.is_valid() || path.original_path_handles.empty() || !path.leaf_ref.has_value()) {
            BPT_LOG_WARN << "Invalid path provided. Path valid: " << path.is_valid()
                      << ", original_path_handles empty: " << path.original_path_handles.empty()
                      << ", leaf_ref has_value: " << path.leaf_ref.has_value() << std::endl;
            if (op == OpType::DELETE) {
                BPT_LOG_DEBUG << "DELETE op with invalid path. No change." << std::endl;
                co_return false;
            }
            BPT_LOG_CRITICAL << "Invalid path provided to handle_operation for non-empty tree with INSERT op." << std::endl;
            assert(false && "Invalid path provided to handle_operation for non-empty tree");
            co_return false;
        }

        MutableHandle leaf_node_handle = path.original_path_handles.back();
        BPT_LOG_DEBUG << "Operating on leaf node: " << leaf_node_handle.page_id << std::endl;

        size_t within_leaf_node_pos;

        if (op == OpType::INSERT) {
            BPT_LOG_DEBUG << "Handling INSERT." << std::endl;
            auto leaf_node_href = co_await leaf_node_handle.ref<LeafNode>();
            BPT_LOG_DEBUG << "Before INSERT on Leaf " << leaf_node_handle.page_id << ": " << format_leaf_contents(*leaf_node_href, leaf_node_handle.page_id) << std::endl;
            within_leaf_node_pos = lower_bound_in_leaf(*leaf_node_href, value);
            BPT_LOG_DEBUG << "Leaf (page " << leaf_node_handle.page_id << ") size: " << leaf_node_href->size
                      << ". Lower bound for insert: " << within_leaf_node_pos << std::endl;

            if (within_leaf_node_pos < leaf_node_href->size && leaf_node_href->data[within_leaf_node_pos] == value) {
                 BPT_LOG_DEBUG << "Value already exists. No change." << std::endl;
                 BPT_LOG_DEBUG << "State of Leaf " << leaf_node_handle.page_id << " (no change): " << format_leaf_contents(*leaf_node_href, leaf_node_handle.page_id) << std::endl;
                 leaf_node_href.Drop();
                 co_return false;
            }

            array::insert_at(leaf_node_href->data, leaf_node_href->size, within_leaf_node_pos, value);
            leaf_node_href->size++;
            BPT_LOG_DEBUG << "After INSERT on Leaf " << leaf_node_handle.page_id << ": " << format_leaf_contents(*leaf_node_href, leaf_node_handle.page_id) << std::endl;

            bool needs_propagation_upwards = false;
            if (leaf_node_href->size > LeafNode::split_threshold) {
                BPT_LOG_DEBUG << "Leaf overflow. Size " << leaf_node_href->size << " > threshold " << LeafNode::split_threshold << std::endl;
                structural_or_key_change_occurred = true;
                if (path.height() == 1) {
                    BPT_LOG_DEBUG << "Handling root (leaf) overflow." << std::endl;
                    leaf_node_href.Drop(); // Must drop before calling another function that might take a ref
                    co_await handle_root_overflow_async(node_type::leaf);
                } else {
                    Pair<MutableHandle, size_t> parent_frame = {
                        path.original_path_handles[path.original_path_handles.size() - 2],
                        path.index_pos.back().second
                    };
                    BPT_LOG_DEBUG << "Handling leaf overflow. Parent page: " << parent_frame.first.page_id
                              << ", child_idx_in_parent: " << parent_frame.second << std::endl;
                    leaf_node_href.Drop(); // Must drop
                    needs_propagation_upwards = co_await handle_leaf_overflow_async(parent_frame);
                }
            } else {
                 BPT_LOG_DEBUG << "Leaf size " << leaf_node_href->size << " within limits. No overflow." << std::endl;
                 leaf_node_href.Drop(); // Drop if not dropped already
            }

            int current_level_idx_in_path_handles = path.original_path_handles.size() - 2;
            int current_level_idx_in_index_pos = path.index_pos.size() - 1;

            while (current_level_idx_in_path_handles >= 0 && needs_propagation_upwards) {
                BPT_LOG_DEBUG << "Propagation needed upwards. Current level in path_handles: " << current_level_idx_in_path_handles << std::endl;
                structural_or_key_change_occurred = true;
                if (current_level_idx_in_path_handles == 0) {
                     BPT_LOG_DEBUG << "Handling root (index) overflow due to propagation." << std::endl;
                     co_await handle_root_overflow_async(node_type::index);
                     needs_propagation_upwards = false;
                } else {
                    Pair<MutableHandle, size_t> grandparent_frame = {
                        path.original_path_handles[current_level_idx_in_path_handles - 1],
                        path.index_pos[current_level_idx_in_index_pos -1].second
                    };
                    BPT_LOG_DEBUG << "Handling index overflow. Grandparent page: " << grandparent_frame.first.page_id
                              << ", child_idx_in_grandparent: " << grandparent_frame.second << std::endl;
                    needs_propagation_upwards = co_await handle_index_overflow_async(grandparent_frame);
                }
                current_level_idx_in_path_handles--;
                current_level_idx_in_index_pos--;
            }
            BPT_LOG_DEBUG << "Exiting INSERT. structural_or_key_change_occurred: " << structural_or_key_change_occurred << std::endl;
            co_return structural_or_key_change_occurred;

        } else { // OpType::DELETE
            BPT_LOG_DEBUG << "Handling DELETE." << std::endl;
            auto leaf_node_href = co_await leaf_node_handle.ref<LeafNode>();
            BPT_LOG_DEBUG << "Before DELETE on Leaf " << leaf_node_handle.page_id << ": " << format_leaf_contents(*leaf_node_href, leaf_node_handle.page_id) << std::endl;
            within_leaf_node_pos = lower_bound_in_leaf(*leaf_node_href, value);
            BPT_LOG_DEBUG << "Leaf (page " << leaf_node_handle.page_id << ") size: " << leaf_node_href->size
                      << ". Lower bound for delete: " << within_leaf_node_pos << std::endl;

            if (within_leaf_node_pos >= leaf_node_href->size || leaf_node_href->data[within_leaf_node_pos] != value) {
                BPT_LOG_DEBUG << "Value not found for deletion. No change." << std::endl;
                BPT_LOG_DEBUG << "State of Leaf " << leaf_node_handle.page_id << " (no change): " << format_leaf_contents(*leaf_node_href, leaf_node_handle.page_id) << std::endl;
                leaf_node_href.Drop();
                co_return false;
            }

            array::remove_at(leaf_node_href->data, leaf_node_href->size, within_leaf_node_pos);
            leaf_node_href->size--;
            BPT_LOG_DEBUG << "After DELETE on Leaf " << leaf_node_handle.page_id << ": " << format_leaf_contents(*leaf_node_href, leaf_node_handle.page_id) << std::endl;

            bool needs_propagation_upwards = false;
            if (path.height() == 1) {
                if (leaf_node_href->size == 0) {
                    BPT_LOG_DEBUG << "Root (leaf) underflow (empty)." << std::endl;
                    structural_or_key_change_occurred = true;
                    leaf_node_href.Drop(); // Must drop
                    co_await handle_root_underflow_async(node_type::leaf);
                } else {
                    BPT_LOG_DEBUG << "Root (leaf) size " << leaf_node_href->size << " after delete. No underflow action." << std::endl;
                    leaf_node_href.Drop(); // Must drop
                }
            } else if (leaf_node_href->size < LeafNode::merge_threshold) {
                BPT_LOG_DEBUG << "Leaf underflow. Size " << leaf_node_href->size << " < threshold " << LeafNode::merge_threshold << std::endl;
                structural_or_key_change_occurred = true;
                Pair<MutableHandle, size_t> parent_frame = {
                    path.original_path_handles[path.original_path_handles.size() - 2],
                    path.index_pos.back().second
                };
                BPT_LOG_DEBUG << "Handling leaf underflow. Parent page: " << parent_frame.first.page_id
                          << ", child_idx_in_parent: " << parent_frame.second << std::endl;
                leaf_node_href.Drop(); // Must drop
                needs_propagation_upwards = co_await handle_leaf_underflow_async(parent_frame);
            } else {
                BPT_LOG_DEBUG << "Leaf size " << leaf_node_href->size << " within limits. No underflow." << std::endl;
                leaf_node_href.Drop(); // Must drop
            }

            int current_level_idx_in_path_handles = path.original_path_handles.size() - 2;
            int current_level_idx_in_index_pos = path.index_pos.size() - 1;

            while (current_level_idx_in_path_handles >= 0 && needs_propagation_upwards) {
                BPT_LOG_DEBUG << "Propagation needed upwards (underflow). Current level in path_handles: " << current_level_idx_in_path_handles << std::endl;
                structural_or_key_change_occurred = true;
                if (current_level_idx_in_path_handles == 0) {
                    BPT_LOG_DEBUG << "Handling root (index) underflow due to propagation." << std::endl;
                    co_await handle_root_underflow_async(node_type::index);
                    needs_propagation_upwards = false;
                } else {
                     Pair<MutableHandle, size_t> grandparent_frame = {
                        path.original_path_handles[current_level_idx_in_path_handles - 1],
                        path.index_pos[current_level_idx_in_index_pos -1].second
                    };
                    BPT_LOG_DEBUG << "Handling index underflow. Grandparent page: " << grandparent_frame.first.page_id
                              << ", child_idx_in_grandparent: " << grandparent_frame.second << std::endl;
                    needs_propagation_upwards = co_await handle_index_underflow_async(grandparent_frame);
                }
                current_level_idx_in_path_handles--;
                current_level_idx_in_index_pos--;
            }
            BPT_LOG_DEBUG << "Exiting DELETE. structural_or_key_change_occurred: " << structural_or_key_change_occurred << std::endl;
            co_return structural_or_key_change_occurred;
        }
    }
  private:

    wutong::Task<bool> handle_leaf_underflow_async(Pair<MutableHandle, size_t> frame) {
        BPT_LOG_DEBUG << "Entering. Parent page: " << frame.first.page_id << ", child_idx_in_parent: " << frame.second << std::endl;
        auto parent_node_href = co_await frame.first.ref<IndexNode>(); // Mutable ref to parent
        BPT_LOG_DEBUG << "Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
        const size_t child_idx_in_parent = frame.second;
        MutableHandle current_node_handle = parent_node_href->children[child_idx_in_parent];
        BPT_LOG_DEBUG << "Current underflowing leaf node: " << current_node_handle.page_id << std::endl;

        // Log current node state before any operation
        auto temp_current_const_ref = co_await current_node_handle.const_ref<LeafNode>();
        BPT_LOG_DEBUG << "Initial state of underflowing " << format_leaf_contents(*temp_current_const_ref, current_node_handle.page_id) << std::endl;
        temp_current_const_ref.Drop();


        if (child_idx_in_parent > 0) {
            BPT_LOG_DEBUG << "Attempting borrow from left sibling." << std::endl;
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            auto left_sibling_href = co_await left_sibling_handle.ref<LeafNode>(); // Mutable ref to left sibling
            auto current_node_href_check_size = co_await current_node_handle.const_ref<LeafNode>(); // Const ref for size check

            BPT_LOG_DEBUG << "Before borrow from left: Left Sibling " << format_leaf_contents(*left_sibling_href, left_sibling_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from left: Current Node " << format_leaf_contents(*current_node_href_check_size, current_node_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from left: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;


            size_t combined_size_for_borrow = left_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (left_sibling_href->size > LeafNode::merge_threshold && combined_size_for_borrow >= 2 * LeafNode::merge_threshold +1 ) {
                BPT_LOG_DEBUG << "Condition for borrow from left met." << std::endl;
                auto current_node_href = co_await current_node_handle.ref<LeafNode>(); // Mutable ref to current

                size_t num_to_move_from_left = (left_sibling_href->size - LeafNode::merge_threshold) / 2;
                if (left_sibling_href->size > current_node_href->size) {
                    num_to_move_from_left = (left_sibling_href->size - current_node_href->size) / 2;
                }
                if (num_to_move_from_left == 0 && left_sibling_href->size > LeafNode::merge_threshold) num_to_move_from_left = 1;
                BPT_LOG_DEBUG << "Calculated num_to_move_from_left: " << num_to_move_from_left << std::endl;

                if (num_to_move_from_left > 0 && left_sibling_href->size >= LeafNode::merge_threshold + num_to_move_from_left) {
                    BPT_LOG_DEBUG << "Performing borrow from left." << std::endl;
                    for (size_t i = current_node_href->size + num_to_move_from_left - 1; i >= num_to_move_from_left; --i) {
                        current_node_href->data[i] = current_node_href->data[i - num_to_move_from_left];
                    }
                    for (size_t i = 0; i < num_to_move_from_left; ++i) {
                        current_node_href->data[i] = left_sibling_href->data[left_sibling_href->size - num_to_move_from_left + i];
                    }
                    current_node_href->size += num_to_move_from_left;
                    left_sibling_href->size -= num_to_move_from_left;
                    parent_node_href->data[child_idx_in_parent - 1] = current_node_href->data[0];

                    BPT_LOG_DEBUG << "After borrow from left: Current Node " << format_leaf_contents(*current_node_href, current_node_handle.page_id) << std::endl;
                    BPT_LOG_DEBUG << "After borrow from left: Left Sibling " << format_leaf_contents(*left_sibling_href, left_sibling_handle.page_id) << std::endl;
                    BPT_LOG_DEBUG << "After borrow from left: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
                    // All refs (parent_node_href, left_sibling_href, current_node_href) will be dropped on exit or if co_return.
                    co_return false;
                } else {
                    BPT_LOG_DEBUG << "num_to_move_from_left is 0 or left sibling cannot give enough. Borrow from left failed." << std::endl;
                }
                // current_node_href drops if not returned
            } else {
                 BPT_LOG_DEBUG << "Condition for borrow from left NOT met (left_sibling_href->size=" << left_sibling_href->size
                           << ", merge_threshold=" << LeafNode::merge_threshold
                           << ", combined_size_for_borrow=" << combined_size_for_borrow << ")" << std::endl;
            }
            // left_sibling_href drops
        }

        if (child_idx_in_parent < parent_node_href->size) {
            BPT_LOG_DEBUG << "Attempting borrow from right sibling." << std::endl;
            MutableHandle right_sibling_handle = parent_node_href->children[child_idx_in_parent + 1];
            auto right_sibling_href = co_await right_sibling_handle.ref<LeafNode>(); // Mutable ref to right sibling
            auto current_node_href_check_size = co_await current_node_handle.const_ref<LeafNode>(); // Const ref for size check

            BPT_LOG_DEBUG << "Before borrow from right: Right Sibling " << format_leaf_contents(*right_sibling_href, right_sibling_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from right: Current Node " << format_leaf_contents(*current_node_href_check_size, current_node_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from right: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;

            size_t combined_size_for_borrow = right_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (right_sibling_href->size > LeafNode::merge_threshold && combined_size_for_borrow >= 2 * LeafNode::merge_threshold +1) {
                BPT_LOG_DEBUG << "Condition for borrow from right met." << std::endl;
                auto current_node_href = co_await current_node_handle.ref<LeafNode>(); // Mutable ref to current
                size_t num_to_move_from_right = (right_sibling_href->size - LeafNode::merge_threshold) / 2;
                 if (right_sibling_href->size > current_node_href->size) {
                    num_to_move_from_right = (right_sibling_href->size - current_node_href->size) / 2;
                }
                if (num_to_move_from_right == 0 && right_sibling_href->size > LeafNode::merge_threshold) num_to_move_from_right = 1;
                BPT_LOG_DEBUG << "Calculated num_to_move_from_right: " << num_to_move_from_right << std::endl;

                if (num_to_move_from_right > 0 && right_sibling_href->size >= LeafNode::merge_threshold + num_to_move_from_right) {
                    BPT_LOG_DEBUG << "Performing borrow from right." << std::endl;
                    for (size_t i = 0; i < num_to_move_from_right; ++i) {
                        current_node_href->data[current_node_href->size + i] = right_sibling_href->data[i];
                    }
                    current_node_href->size += num_to_move_from_right;
                    for(size_t i=0; i < right_sibling_href->size - num_to_move_from_right; ++i) {
                        right_sibling_href->data[i] = right_sibling_href->data[i + num_to_move_from_right];
                    }
                    right_sibling_href->size -= num_to_move_from_right;
                    parent_node_href->data[child_idx_in_parent] = right_sibling_href->data[0];

                    BPT_LOG_DEBUG << "After borrow from right: Current Node " << format_leaf_contents(*current_node_href, current_node_handle.page_id) << std::endl;
                    BPT_LOG_DEBUG << "After borrow from right: Right Sibling " << format_leaf_contents(*right_sibling_href, right_sibling_handle.page_id) << std::endl;
                    BPT_LOG_DEBUG << "After borrow from right: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
                    co_return false;
                } else {
                    BPT_LOG_DEBUG << "num_to_move_from_right is 0 or right sibling cannot give enough. Borrow from right failed." << std::endl;
                }
                // current_node_href drops
            } else {
                BPT_LOG_DEBUG << "Condition for borrow from right NOT met (right_sibling_href->size=" << right_sibling_href->size
                           << ", merge_threshold=" << LeafNode::merge_threshold
                           << ", combined_size_for_borrow=" << combined_size_for_borrow << ")" << std::endl;
            }
            // right_sibling_href drops
        }

        BPT_LOG_DEBUG << "Borrow failed or not possible. Proceeding to Merge." << std::endl;
        // parent_node_href is still live here.
        if (child_idx_in_parent < parent_node_href->size) {
            BPT_LOG_DEBUG << "Merging current node (page " << current_node_handle.page_id
                      << ") with its right sibling (page " << parent_node_href->children[child_idx_in_parent + 1].page_id << ")." << std::endl;
            co_await merge_leaf_with_right_async(parent_node_href, child_idx_in_parent, current_node_handle);
        } else { // Merge with left sibling
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            BPT_LOG_DEBUG << "Merging left sibling (page " << left_sibling_handle.page_id
                      << ") with current node (page " << current_node_handle.page_id << " - which is right part of merge)." << std::endl;
            // In this case, current_node_handle is the right node of the merge pair.
            // merge_leaf_with_right_async takes (parent, left_child_idx, left_child_handle)
            co_await merge_leaf_with_right_async(parent_node_href, child_idx_in_parent - 1, left_sibling_handle);
        }
        BPT_LOG_DEBUG << "After Merge operation (in handle_leaf_underflow_async): Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
        bool parent_underflowed = parent_node_href->size < IndexNode::merge_threshold;
        BPT_LOG_DEBUG << "Exiting. Parent underflowed: " << parent_underflowed << std::endl;
        co_return parent_underflowed;
    }

    wutong::Task<bool> handle_index_underflow_async(Pair<MutableHandle, size_t> frame) {
        BPT_LOG_DEBUG << "Entering. Parent page: " << frame.first.page_id << ", child_idx_in_parent: " << frame.second << std::endl;
        auto parent_node_href = co_await frame.first.ref<IndexNode>(); // Mutable ref to parent
        BPT_LOG_DEBUG << "Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
        const size_t child_idx_in_parent = frame.second;
        MutableHandle current_node_handle = parent_node_href->children[child_idx_in_parent];
        BPT_LOG_DEBUG << "Current underflowing index node: " << current_node_handle.page_id << std::endl;

        auto temp_current_const_ref = co_await current_node_handle.const_ref<IndexNode>();
        BPT_LOG_DEBUG << "Initial state of underflowing " << format_index_contents(*temp_current_const_ref, current_node_handle.page_id) << std::endl;
        temp_current_const_ref.Drop();

        if (child_idx_in_parent > 0) {
            BPT_LOG_DEBUG << "Attempting borrow from left index sibling." << std::endl;
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            auto left_sibling_href = co_await left_sibling_handle.ref<IndexNode>(); // Mutable ref to left sibling
            auto current_node_href_check_size = co_await current_node_handle.const_ref<IndexNode>(); // Const ref for size check

            BPT_LOG_DEBUG << "Before borrow from left: Left Sibling " << format_index_contents(*left_sibling_href, left_sibling_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from left: Current Node " << format_index_contents(*current_node_href_check_size, current_node_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from left: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;

            size_t combined_keys_for_borrow = left_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (left_sibling_href->size > IndexNode::merge_threshold && combined_keys_for_borrow + 1 >= 2 * IndexNode::merge_threshold +1) {
                 BPT_LOG_DEBUG << "Condition for borrow from left index met. Performing standard 1-element borrow." << std::endl;
                auto current_node_href = co_await current_node_handle.ref<IndexNode>(); // Mutable ref to current
                array::insert_at(current_node_href->children, current_node_href->size + 1, 0, left_sibling_href->children[left_sibling_href->size]);
                array::insert_at(current_node_href->data, current_node_href->size, 0, parent_node_href->data[child_idx_in_parent - 1]);
                current_node_href->size++;
                parent_node_href->data[child_idx_in_parent - 1] = left_sibling_href->data[left_sibling_href->size - 1];
                left_sibling_href->size--;

                BPT_LOG_DEBUG << "After borrow from left: Current Node " << format_index_contents(*current_node_href, current_node_handle.page_id) << std::endl;
                BPT_LOG_DEBUG << "After borrow from left: Left Sibling " << format_index_contents(*left_sibling_href, left_sibling_handle.page_id) << std::endl;
                BPT_LOG_DEBUG << "After borrow from left: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
                co_return false;
            } else {
                BPT_LOG_DEBUG << "Condition for borrow from left index NOT met (left_sibling_href->size=" << left_sibling_href->size
                           << ", merge_threshold=" << IndexNode::merge_threshold
                           << ", combined_keys_for_borrow=" << combined_keys_for_borrow << ")" << std::endl;
            }
            // left_sibling_href drops
        }

        if (child_idx_in_parent < parent_node_href->size) {
            BPT_LOG_DEBUG << "Attempting borrow from right index sibling." << std::endl;
            MutableHandle right_sibling_handle = parent_node_href->children[child_idx_in_parent + 1];
            auto right_sibling_href = co_await right_sibling_handle.ref<IndexNode>(); // Mutable ref to right sibling
             auto current_node_href_check_size = co_await current_node_handle.const_ref<IndexNode>(); // Const ref for size check

            BPT_LOG_DEBUG << "Before borrow from right: Right Sibling " << format_index_contents(*right_sibling_href, right_sibling_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from right: Current Node " << format_index_contents(*current_node_href_check_size, current_node_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "Before borrow from right: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;

            size_t combined_keys_for_borrow = right_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (right_sibling_href->size > IndexNode::merge_threshold && combined_keys_for_borrow + 1 >= 2 * IndexNode::merge_threshold +1) {
                BPT_LOG_DEBUG << "Condition for borrow from right index met. Performing standard 1-element borrow." << std::endl;
                auto current_node_href = co_await current_node_handle.ref<IndexNode>(); // Mutable ref to current
                current_node_href->data[current_node_href->size] = parent_node_href->data[child_idx_in_parent];
                current_node_href->children[current_node_href->size + 1] = right_sibling_href->children[0];
                current_node_href->size++;
                parent_node_href->data[child_idx_in_parent] = right_sibling_href->data[0];
                array::remove_at(right_sibling_href->data, right_sibling_href->size, 0);
                array::remove_at(right_sibling_href->children, right_sibling_href->size + 1, 0);
                right_sibling_href->size--;

                BPT_LOG_DEBUG << "After borrow from right: Current Node " << format_index_contents(*current_node_href, current_node_handle.page_id) << std::endl;
                BPT_LOG_DEBUG << "After borrow from right: Right Sibling " << format_index_contents(*right_sibling_href, right_sibling_handle.page_id) << std::endl;
                BPT_LOG_DEBUG << "After borrow from right: Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
                co_return false;
            } else {
                 BPT_LOG_DEBUG << "Condition for borrow from right index NOT met (right_sibling_href->size=" << right_sibling_href->size
                           << ", merge_threshold=" << IndexNode::merge_threshold
                           << ", combined_keys_for_borrow=" << combined_keys_for_borrow << ")" << std::endl;
            }
            // right_sibling_href drops
        }

        BPT_LOG_DEBUG << "Index borrow failed or not possible. Proceeding to Merge." << std::endl;
        // parent_node_href is still live
        if (child_idx_in_parent < parent_node_href->size) {
            BPT_LOG_DEBUG << "Merging current index node (page " << current_node_handle.page_id
                      << ") with its right sibling (page " << parent_node_href->children[child_idx_in_parent + 1].page_id << ")." << std::endl;
            co_await merge_index_with_right_async(parent_node_href, child_idx_in_parent, current_node_handle);
        } else { // Merge with left
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            BPT_LOG_DEBUG << "Merging left index sibling (page " << left_sibling_handle.page_id
                      << ") with current index node (page " << current_node_handle.page_id << " - which is right part of merge)." << std::endl;
            co_await merge_index_with_right_async(parent_node_href, child_idx_in_parent - 1, left_sibling_handle);
        }
        BPT_LOG_DEBUG << "After Merge operation (in handle_index_underflow_async): Parent " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
        bool parent_underflowed = parent_node_href->size < IndexNode::merge_threshold;
        BPT_LOG_DEBUG << "Exiting. Parent underflowed: " << parent_underflowed << std::endl;
        co_return parent_underflowed;
    }

    wutong::Task<bool> handle_leaf_overflow_async(Pair<MutableHandle, size_t> frame) {
        BPT_LOG_DEBUG << "Entering. Parent page: " << frame.first.page_id << ", child_idx_in_parent: " << frame.second << std::endl;
        auto parent_node_href = co_await frame.first.ref<IndexNode>(); // Mutable ref
        BPT_LOG_DEBUG << "Parent (before child split affecting it): " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
        const size_t child_idx_in_parent = frame.second;
        MutableHandle old_node_handle = parent_node_href->children[child_idx_in_parent];
        BPT_LOG_DEBUG << "Old (overflowing) leaf node: " << old_node_handle.page_id << std::endl;
        auto old_node_href = co_await old_node_handle.ref<LeafNode>(); // Mutable ref
        BPT_LOG_DEBUG << "Before split: Old Leaf " << format_leaf_contents(*old_node_href, old_node_handle.page_id) << std::endl;

        MutableHandle new_node_handle = co_await pmem.create_mutable_and_init<LeafNode>();
        auto new_node_href = co_await new_node_handle.ref<LeafNode>(true); // Mutable ref
        BPT_LOG_DEBUG << "New sibling leaf node created: " << new_node_handle.page_id << ". Initial state: " << format_leaf_contents(*new_node_href, new_node_handle.page_id) << std::endl;


        size_t total_size = old_node_href->size;
        size_t size_for_old_node = total_size / 2;
        size_t size_for_new_node = total_size - size_for_old_node;
        BPT_LOG_DEBUG << "Splitting leaf. Total size: " << total_size << ". Old node new size: " << size_for_old_node
                  << ". New node size: " << size_for_new_node << std::endl;
        array::migrate(new_node_href->data, old_node_href->data+size_for_old_node, size_for_new_node);
        new_node_href->size = size_for_new_node;
        old_node_href->size = size_for_old_node;
        new_node_href->sibling = old_node_href->sibling;
        old_node_href->sibling = new_node_handle;

        BPT_LOG_DEBUG << "After split: Old Leaf " << format_leaf_contents(*old_node_href, old_node_handle.page_id) << std::endl;
        BPT_LOG_DEBUG << "After split: New Leaf " << format_leaf_contents(*new_node_href, new_node_handle.page_id) << std::endl;

        BPT_LOG_DEBUG << "Inserting new key (" << new_node_href->data[0].first << "," << new_node_href->data[0].second
                  << ") and child (page " << new_node_handle.page_id << ") into parent (page " << frame.first.page_id << ")." << std::endl;
        array::insert_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent, new_node_href->data[0]);
        array::insert_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1, new_node_handle);
        parent_node_href->size++;
        BPT_LOG_DEBUG << "Parent node new size: " << parent_node_href->size << std::endl;
        BPT_LOG_DEBUG << "Parent (after child split affecting it): " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;

        bool parent_overflowed = parent_node_href->size > IndexNode::split_threshold;
        BPT_LOG_DEBUG << "Exiting. Parent overflowed: " << parent_overflowed << std::endl;
        co_return parent_overflowed;
    }

    wutong::Task<bool> handle_index_overflow_async(Pair<MutableHandle, size_t> frame) {
        BPT_LOG_DEBUG << "Entering. Parent page: " << frame.first.page_id << ", child_idx_in_parent: " << frame.second << std::endl;
        auto parent_node_href = co_await frame.first.ref<IndexNode>(); // Mutable ref
        BPT_LOG_DEBUG << "Parent (before child split affecting it): " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;
        const size_t child_idx_in_parent = frame.second;
        MutableHandle old_node_handle = parent_node_href->children[child_idx_in_parent];
        BPT_LOG_DEBUG << "Old (overflowing) index node: " << old_node_handle.page_id << std::endl;
        auto old_node_href = co_await old_node_handle.ref<IndexNode>(); // Mutable ref
        BPT_LOG_DEBUG << "Before split: Old Index " << format_index_contents(*old_node_href, old_node_handle.page_id) << std::endl;

        MutableHandle new_node_handle = co_await pmem.create_mutable_and_init<IndexNode>();
        auto new_node_href = co_await new_node_handle.ref<IndexNode>(true); // Mutable ref
        new_node_href->layer = old_node_href->layer;
        BPT_LOG_DEBUG << "New sibling index node created: " << new_node_handle.page_id << ". Layer: " << new_node_href->layer
                  << ". Initial state: " << format_index_contents(*new_node_href, new_node_handle.page_id) << std::endl;


        size_t total_keys = old_node_href->size;
        size_t keys_for_old_node = (total_keys - 1) / 2;
        storage_pair_t key_to_promote = old_node_href->data[keys_for_old_node];
        size_t keys_for_new_node = total_keys - 1 - keys_for_old_node;
        BPT_LOG_DEBUG << "Splitting index. Total keys: " << total_keys << ". Key to promote: (" << key_to_promote.first << "," << key_to_promote.second
                  << "). Old node new keys: " << keys_for_old_node << ". New node keys: " << keys_for_new_node << std::endl;
        array::migrate(new_node_href->data, old_node_href->data+ keys_for_old_node + 1, keys_for_new_node);
        array::migrate(new_node_href->children, old_node_href->children+ keys_for_old_node + 1, keys_for_new_node + 1);
        new_node_href->size = keys_for_new_node;
        old_node_href->size = keys_for_old_node;

        BPT_LOG_DEBUG << "After split: Old Index " << format_index_contents(*old_node_href, old_node_handle.page_id) << std::endl;
        BPT_LOG_DEBUG << "After split: New Index " << format_index_contents(*new_node_href, new_node_handle.page_id) << std::endl;

        BPT_LOG_DEBUG << "Inserting promoted key and child (page " << new_node_handle.page_id << ") into parent (page " << frame.first.page_id << ")." << std::endl;
        array::insert_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent, key_to_promote);
        array::insert_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1, new_node_handle);
        parent_node_href->size++;
        BPT_LOG_DEBUG << "Parent node new size: " << parent_node_href->size << std::endl;
        BPT_LOG_DEBUG << "Parent (after child split affecting it): " << format_index_contents(*parent_node_href, frame.first.page_id) << std::endl;

        bool parent_overflowed = parent_node_href->size > IndexNode::split_threshold;
        BPT_LOG_DEBUG << "Exiting. Parent overflowed: " << parent_overflowed << std::endl;
        co_return parent_overflowed;
    }

    wutong::Task<void> merge_leaf_with_right_async(
        PersistentMemoryAsync::HandledReference<IndexNode>& parent_node_href, // Is mutable ref
        size_t child_idx_in_parent, // This is index of left node of merge pair in parent
        MutableHandle left_node_of_merge_pair_handle
    ) {
        BPT_LOG_DEBUG << "Entering. Parent page: " << parent_node_href.get_handle().page_id
                  << ", child_idx_in_parent (of left node): " << child_idx_in_parent
                  << ", Left leaf node (absorber): " << left_node_of_merge_pair_handle.page_id << std::endl;

        auto old_node_href = co_await left_node_of_merge_pair_handle.ref<LeafNode>(); // Mutable ref to left (absorber)
        MutableHandle right_node_handle = parent_node_href->children[child_idx_in_parent + 1];
        BPT_LOG_DEBUG << "Right leaf node (absorbed): " << right_node_handle.page_id << std::endl;
        auto right_node_href = co_await right_node_handle.ref<LeafNode>(); // Mutable ref to right (absorbed)

        BPT_LOG_DEBUG << "Before merge: Parent " << format_index_contents(*parent_node_href, parent_node_href.get_handle().page_id) << std::endl;
        BPT_LOG_DEBUG << "Before merge: Left (Absorber) " << format_leaf_contents(*old_node_href, left_node_of_merge_pair_handle.page_id) << std::endl;
        BPT_LOG_DEBUG << "Before merge: Right (Absorbed) " << format_leaf_contents(*right_node_href, right_node_handle.page_id) << std::endl;

        BPT_LOG_DEBUG << "Merging " << right_node_href->size << " elements from right (page " << right_node_handle.page_id
                  << ") into left (page " << left_node_of_merge_pair_handle.page_id << ", current size " << old_node_href->size << ")." << std::endl;
        array::migrate(old_node_href->data+old_node_href->size, right_node_href->data, right_node_href->size);
        old_node_href->size += right_node_href->size;
        old_node_href->sibling = right_node_href->sibling;
        BPT_LOG_DEBUG << "After migration to Left (Absorber) " << format_leaf_contents(*old_node_href, left_node_of_merge_pair_handle.page_id) << std::endl;

        right_node_href.Drop(); // Drop ref before removing page
        co_await pmem.remove(right_node_handle);
        BPT_LOG_DEBUG << "Right node (page " << right_node_handle.page_id << ") removed." << std::endl;

        BPT_LOG_DEBUG << "Removing key and child pointer from parent." << std::endl;
        array::remove_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent);
        array::remove_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1);
        parent_node_href->size--;
        BPT_LOG_DEBUG << "After merge: Parent " << format_index_contents(*parent_node_href, parent_node_href.get_handle().page_id) << std::endl;
        BPT_LOG_DEBUG << "Exiting." << std::endl;
    }

    wutong::Task<void> merge_index_with_right_async(
        PersistentMemoryAsync::HandledReference<IndexNode>& parent_node_href, // Is mutable ref
        size_t child_idx_in_parent, // Index of left node of merge pair
        MutableHandle left_node_of_merge_pair_handle
    ) {
        BPT_LOG_DEBUG << "Entering. Parent page: " << parent_node_href.get_handle().page_id
                  << ", child_idx_in_parent (of left node): " << child_idx_in_parent
                  << ", Left index node (absorber): " << left_node_of_merge_pair_handle.page_id << std::endl;
        auto old_node_href = co_await left_node_of_merge_pair_handle.ref<IndexNode>(); // Mutable ref to left (absorber)
        MutableHandle right_node_handle = parent_node_href->children[child_idx_in_parent + 1];
        BPT_LOG_DEBUG << "Right index node (absorbed): " << right_node_handle.page_id << std::endl;
        auto right_node_href = co_await right_node_handle.ref<IndexNode>(); // Mutable ref to right (absorbed)

        BPT_LOG_DEBUG << "Before merge: Parent " << format_index_contents(*parent_node_href, parent_node_href.get_handle().page_id) << std::endl;
        BPT_LOG_DEBUG << "Before merge: Left (Absorber) " << format_index_contents(*old_node_href, left_node_of_merge_pair_handle.page_id) << std::endl;
        BPT_LOG_DEBUG << "Before merge: Right (Absorbed) " << format_index_contents(*right_node_href, right_node_handle.page_id) << std::endl;

        BPT_LOG_DEBUG << "Pulling down separator key (" << parent_node_href->data[child_idx_in_parent].first << "," << parent_node_href->data[child_idx_in_parent].second
                  << ") from parent into left node (page " << left_node_of_merge_pair_handle.page_id
                  << ", current size " << old_node_href->size << ")." << std::endl;
        old_node_href->data[old_node_href->size] = parent_node_href->data[child_idx_in_parent];
        old_node_href->size++;

        BPT_LOG_DEBUG << "Merging " << right_node_href->size << " keys and " << right_node_href->size + 1
                  << " children from right (page " << right_node_handle.page_id << ") into left." << std::endl;
        array::migrate(old_node_href->data+old_node_href->size, right_node_href->data, right_node_href->size);
        array::migrate(old_node_href->children+old_node_href->size, right_node_href->children, right_node_href->size + 1); // Note: old_node_href->size was already incremented for the pulled-down key
        old_node_href->size += right_node_href->size;
        BPT_LOG_DEBUG << "After migration to Left (Absorber) " << format_index_contents(*old_node_href, left_node_of_merge_pair_handle.page_id) << std::endl;

        right_node_href.Drop(); // Drop ref before removing page
        co_await pmem.remove(right_node_handle);
        BPT_LOG_DEBUG << "Right node (page " << right_node_handle.page_id << ") removed." << std::endl;

        BPT_LOG_DEBUG << "Removing key and child pointer from parent." << std::endl;
        array::remove_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent);
        array::remove_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1);
        parent_node_href->size--;
        BPT_LOG_DEBUG << "After merge: Parent " << format_index_contents(*parent_node_href, parent_node_href.get_handle().page_id) << std::endl;
        BPT_LOG_DEBUG << "Exiting." << std::endl;
    }

    wutong::Task<void> handle_root_overflow_async(node_type root_node_is) {
        BPT_LOG_DEBUG << "Entering. Root is currently a " << (root_node_is == node_type::leaf ? "LEAF" : "INDEX") << " node." << std::endl;
        MutableHandle old_root_handle = root_handle_.val;
        BPT_LOG_DEBUG << "Old root handle: " << old_root_handle.page_id << std::endl;

        // Log state of old root before it's split
        if (root_node_is == node_type::leaf) {
            auto temp_old_root_ref = co_await old_root_handle.const_ref<LeafNode>();
            BPT_LOG_DEBUG << "Before root split: Old Root " << format_leaf_contents(*temp_old_root_ref, old_root_handle.page_id) << std::endl;
        } else {
            auto temp_old_root_ref = co_await old_root_handle.const_ref<IndexNode>();
            BPT_LOG_DEBUG << "Before root split: Old Root " << format_index_contents(*temp_old_root_ref, old_root_handle.page_id) << std::endl;
        }

        MutableHandle new_root_handle = co_await pmem.create_mutable_and_init<IndexNode>();
        auto new_root_href = co_await new_root_handle.ref<IndexNode>(true); // Mutable ref
        BPT_LOG_DEBUG << "New root (index) node created: " << new_root_handle.page_id << ". Initial state: " << format_index_contents(*new_root_href, new_root_handle.page_id) << std::endl;

        new_root_href->layer = 0;
        new_root_href->children[0] = old_root_handle;

        root_handle_.val = new_root_handle;
        tree_height_.val++;
        BPT_LOG_DEBUG << "Tree height incremented to " << tree_height_.val << ". New root set." << std::endl;

        if (root_node_is == node_type::leaf) {
            BPT_LOG_DEBUG << "Old root was a LEAF. Splitting it." << std::endl;
            auto old_leaf_root_href = co_await old_root_handle.ref<LeafNode>(); // Mutable ref
            MutableHandle new_leaf_sibling = co_await pmem.create_mutable_and_init<LeafNode>();
            auto new_leaf_sibling_href = co_await new_leaf_sibling.ref<LeafNode>(true); // Mutable ref
            BPT_LOG_DEBUG << "New leaf sibling created: " << new_leaf_sibling.page_id << ". Initial state: " << format_leaf_contents(*new_leaf_sibling_href, new_leaf_sibling.page_id) << std::endl;

            size_t total_size = old_leaf_root_href->size;
            size_t size_old = total_size / 2;
            size_t size_new = total_size - size_old;
            array::migrate(new_leaf_sibling_href->data, old_leaf_root_href->data+size_old, size_new);
            new_leaf_sibling_href->size = size_new;
            old_leaf_root_href->size = size_old;
            new_leaf_sibling_href->sibling = old_leaf_root_href->sibling;
            old_leaf_root_href->sibling = new_leaf_sibling;
            BPT_LOG_DEBUG << "Old root (leaf) split. Old size: " << old_leaf_root_href->size
                      << ", New sibling size: " << new_leaf_sibling_href->size << std::endl;
            BPT_LOG_DEBUG << "After split: Old Root (now child) " << format_leaf_contents(*old_leaf_root_href, old_root_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "After split: New Sibling (now child) " << format_leaf_contents(*new_leaf_sibling_href, new_leaf_sibling.page_id) << std::endl;


            new_root_href->data[0] = new_leaf_sibling_href->data[0];
            new_root_href->children[1] = new_leaf_sibling;
            new_root_href->size = 1;
            BPT_LOG_DEBUG << "New root populated. Key: (" << new_root_href->data[0].first << "," << new_root_href->data[0].second
                      << "), Children: " << new_root_href->children[0].page_id << ", " << new_root_href->children[1].page_id << std::endl;
            BPT_LOG_DEBUG << "Final state of New Root: " << format_index_contents(*new_root_href, new_root_handle.page_id) << std::endl;
        } else { // Old root was Index
            BPT_LOG_DEBUG << "Old root was an INDEX. Splitting it." << std::endl;
            auto old_index_root_href = co_await old_root_handle.ref<IndexNode>(); // Mutable ref
            old_index_root_href->layer = 1; // Old root becomes child, so layer increases
            BPT_LOG_DEBUG << "Old root (index, page " << old_root_handle.page_id << ") layer set to 1." << std::endl;
            MutableHandle new_index_sibling = co_await pmem.create_mutable_and_init<IndexNode>();
            auto new_index_sibling_href = co_await new_index_sibling.ref<IndexNode>(true); // Mutable ref
            new_index_sibling_href->layer = 1; // New sibling is also at layer 1
            BPT_LOG_DEBUG << "New index sibling created: " << new_index_sibling.page_id << ". Layer: 1. Initial state: " << format_index_contents(*new_index_sibling_href, new_index_sibling.page_id) << std::endl;

            size_t total_keys = old_index_root_href->size;
            size_t keys_old = (total_keys - 1) / 2;
            storage_pair_t promoted_key = old_index_root_href->data[keys_old];
            size_t keys_new = total_keys - 1 - keys_old;

            array::migrate(new_index_sibling_href->data, old_index_root_href->data+keys_old + 1, keys_new);
            array::migrate(new_index_sibling_href->children, old_index_root_href->children+keys_old + 1, keys_new + 1);
            new_index_sibling_href->size = keys_new;
            old_index_root_href->size = keys_old;
            BPT_LOG_DEBUG << "Old root (index) split. Promoted key: (" << promoted_key.first << "," << promoted_key.second
                      << "). Old size: " << old_index_root_href->size << ", New sibling size: " << new_index_sibling_href->size << std::endl;
            BPT_LOG_DEBUG << "After split: Old Root (now child) " << format_index_contents(*old_index_root_href, old_root_handle.page_id) << std::endl;
            BPT_LOG_DEBUG << "After split: New Sibling (now child) " << format_index_contents(*new_index_sibling_href, new_index_sibling.page_id) << std::endl;

            new_root_href->data[0] = promoted_key;
            new_root_href->children[1] = new_index_sibling;
            new_root_href->size = 1;
            BPT_LOG_DEBUG << "New root populated. Key: (" << new_root_href->data[0].first << "," << new_root_href->data[0].second
                      << "), Children: " << new_root_href->children[0].page_id << ", " << new_root_href->children[1].page_id << std::endl;
            BPT_LOG_DEBUG << "Final state of New Root: " << format_index_contents(*new_root_href, new_root_handle.page_id) << std::endl;
        }
        BPT_LOG_DEBUG << "Exiting." << std::endl;
        co_return;
    }

    wutong::Task<void> handle_root_underflow_async(node_type root_node_is) {
        BPT_LOG_DEBUG << "Entering. Root is currently a " << (root_node_is == node_type::leaf ? "LEAF" : "INDEX") << " node."
                  << " Current root: " << root_handle_.val.page_id << std::endl;
        if (root_node_is == node_type::index) {
            auto root_ref = co_await root_handle_.val.ref<IndexNode>(); // Mutable ref
            BPT_LOG_DEBUG << "Before root (index) underflow check: " << format_index_contents(*root_ref, root_handle_.val.page_id) << std::endl;
            if (root_ref->size == 0) { // Root index node has only one child left
                BPT_LOG_DEBUG << "Root (index) is empty (size 0). Promoting its only child." << std::endl;
                MutableHandle old_root_handle = root_handle_.val;
                root_handle_.val = root_ref->children[0]; // This child becomes the new root
                tree_height_.val--;
                BPT_LOG_DEBUG << "New root is page " << root_handle_.val.page_id << ". Tree height decremented to " << tree_height_.val << std::endl;
                root_ref.Drop(); // Drop ref to old root before removing
                co_await pmem.remove(old_root_handle);
                BPT_LOG_DEBUG << "Old root (page " << old_root_handle.page_id << ") removed." << std::endl;

                if (tree_height_.val > 1 && !root_handle_.val.is_nullptr()) { // New root is an index node
                     BPT_LOG_DEBUG << "New root is an index node. Setting its layer to 0." << std::endl;
                     auto new_root_idx_ref = co_await root_handle_.val.ref<IndexNode>(); // Mutable ref
                     BPT_LOG_DEBUG << "Before layer update: New Root " << format_index_contents(*new_root_idx_ref, root_handle_.val.page_id) << std::endl;
                     new_root_idx_ref->layer = 0;
                     BPT_LOG_DEBUG << "After layer update: New Root " << format_index_contents(*new_root_idx_ref, root_handle_.val.page_id) << std::endl;
                } else if (tree_height_.val == 1 && !root_handle_.val.is_nullptr()) { // New root is a leaf
                    BPT_LOG_DEBUG << "New root is a leaf node (tree height 1)." << std::endl;
                    auto new_root_leaf_ref = co_await root_handle_.val.const_ref<LeafNode>();
                    BPT_LOG_DEBUG << "New Root " << format_leaf_contents(*new_root_leaf_ref, root_handle_.val.page_id) << std::endl;
                } else if (root_handle_.val.is_nullptr()) {
                    BPT_LOG_DEBUG << "Tree became empty after root underflow." << std::endl;
                }
            } else {
                BPT_LOG_DEBUG << "Root (index) size " << root_ref->size << " > 0. No structural change to root itself." << std::endl;
                BPT_LOG_DEBUG << "Final state (no change): " << format_index_contents(*root_ref, root_handle_.val.page_id) << std::endl;
                root_ref.Drop();
            }
        } else { // Root is Leaf
            auto root_ref = co_await root_handle_.val.ref<LeafNode>(); // Mutable ref
            BPT_LOG_DEBUG << "Before root (leaf) underflow check: " << format_leaf_contents(*root_ref, root_handle_.val.page_id) << std::endl;
            if (root_ref->size == 0) { // Root leaf node is empty
                 BPT_LOG_DEBUG << "Root (leaf) is empty. Tree becomes empty." << std::endl;
                 MutableHandle old_root_handle = root_handle_.val;
                 root_handle_.val.set_nullptr();
                 tree_height_.val = 0;
                 root_ref.Drop(); // Drop ref to old root before removing
                 co_await pmem.remove(old_root_handle);
                 BPT_LOG_DEBUG << "Old root (page " << old_root_handle.page_id << ") removed. Tree is now empty." << std::endl;
            } else {
                BPT_LOG_DEBUG << "Root (leaf) size " << root_ref->size << " > 0. No structural change to root." << std::endl;
                BPT_LOG_DEBUG << "Final state (no change): " << format_leaf_contents(*root_ref, root_handle_.val.page_id) << std::endl;
                root_ref.Drop();
            }
        }
        BPT_LOG_DEBUG << "Exiting." << std::endl;
        co_return;
    }

  public:
    wutong::Task<void> traverse_async(const bool &do_check = false) const {
      BPT_LOG_DEBUG << "--- Traversing ON-DISK B+ Tree (" << this << ") ---" << std::endl;
      BPT_LOG_DEBUG << "[Info] On-Disk Height: " << tree_height_.val << std::endl;

      if (root_handle_.val.is_nullptr()) {
        BPT_LOG_DEBUG << "[Tree] On-disk structure is Empty." << std::endl;
        BPT_LOG_DEBUG << "--- End Traversal ---" << std::endl << std::endl;
        co_return;
      }
      BPT_LOG_DEBUG << "[Info] On-Disk Root Page ID: " << root_handle_.val.page_id << std::endl;
      std::queue<std::pair<MutableHandle, size_t>> q;
      q.push({root_handle_.val, 0});
      size_t current_level_print = 0;
      if (!q.empty()) {
          // Use std::cerr directly for traverse_async as it's a utility
          std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | Level " << current_level_print << ":" << std::endl;
      }

      while (!q.empty()) {
        auto [curr_handle, node_level] = q.front();
        q.pop();
        if (node_level > current_level_print) {
          current_level_print = node_level;
          std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | \nLevel " << current_level_print << ":" << std::endl;
        }
        bool is_leaf = (node_level == tree_height_.val - 1);

        std::stringstream ss_node_info;
        if (is_leaf) {
          auto leaf_obj = co_await curr_handle.template const_ref<LeafNode>();
          ss_node_info << "  Node (Page ID: " << curr_handle.page_id << ") " << format_leaf_contents(*leaf_obj, curr_handle.page_id);
        } else {
          auto index_obj = co_await curr_handle.template const_ref<IndexNode>();
          ss_node_info << "  Node (Page ID: " << curr_handle.page_id << ") " << format_index_contents(*index_obj, curr_handle.page_id);
          // Enqueue children
          for (size_t i = 0; i <= index_obj->size; ++i) {
            if (!index_obj->children[i].is_nullptr()) {
              q.push({index_obj->children[i], node_level + 1});
            }
          }
        }
        std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | " << ss_node_info.str() << std::endl;
      }
      std::cerr << std::left << std::setw(LOG_PREFIX_WIDTH) << __func__ << " | --- End Traversal ---" << std::endl << std::endl;
      co_return;
    }

    wutong::Task<void> initialize_from_vector_async(
        const sjtu::vector<storage_pair_t> &sorted_data) {
      BPT_LOG_DEBUG << "Entering. Initializing with " << sorted_data.size() << " elements." << std::endl;
      write_map_.clear();
      BPT_LOG_DEBUG << "write_map_ cleared." << std::endl;

      root_handle_.val.set_nullptr();
      tree_height_.val = 0;
      BPT_LOG_DEBUG << "Tree metadata reset (root=nullptr, height=0)." << std::endl;

      if (sorted_data.empty()) {
        BPT_LOG_DEBUG << "Input data is empty. Tree remains empty. Exiting." << std::endl;
        co_return;
      }

      BPT_LOG_DEBUG << "Phase 1: Creating Leaf Nodes." << std::endl;
      sjtu::vector<MutableHandle> current_level_handles;
      size_t data_idx = 0;
      MutableHandle prev_leaf_handle;

      while (data_idx < sorted_data.size()) {
        MutableHandle current_leaf_handle = PersistentMemoryAsync::create_mutable();
        // BPT_LOG_DEBUG << "Created new leaf handle: " << current_leaf_handle.page_id << std::endl; // Redundant if logged after init
        {
          auto leaf_ref = co_await current_leaf_handle.template ref<LeafNode>(true);
          leaf_ref->size = 0;
          // size_t start_data_idx_for_leaf = data_idx; // Not used
          for (size_t i = 0;
               i < LeafNode::split_threshold && data_idx < sorted_data.size();
               ++i) {
            leaf_ref->data[leaf_ref->size++] = sorted_data[data_idx++];
          }
          leaf_ref->sibling.set_nullptr();
          BPT_LOG_DEBUG << "Initialized " << format_leaf_contents(*leaf_ref, current_leaf_handle.page_id) << std::endl;
        }

        if (!prev_leaf_handle.is_nullptr()) {
          auto prev_leaf_editor_ref = co_await prev_leaf_handle.template ref<LeafNode>(false);
          prev_leaf_editor_ref->sibling = current_leaf_handle;
          BPT_LOG_DEBUG << "Linked prev leaf " << prev_leaf_handle.page_id << " sibling to current leaf " << current_leaf_handle.page_id << ". Prev leaf now: " << format_leaf_contents(*prev_leaf_editor_ref, prev_leaf_handle.page_id) << std::endl;
        }

        current_level_handles.push_back(current_leaf_handle);
        prev_leaf_handle = current_leaf_handle;
      }
      BPT_LOG_DEBUG << "Leaf node creation complete. " << current_level_handles.size() << " leaf nodes created." << std::endl;

      size_t current_nodes_layer_from_leaves = 0;
      tree_height_.val = 1;
      BPT_LOG_DEBUG << "Initial tree height: 1 (leaf level)." << std::endl;

      std::function<wutong::Task<storage_pair_t>(MutableHandle, size_t)> get_first_key_of_subtree;
      get_first_key_of_subtree =
          [&](MutableHandle node_h, size_t node_level_from_leaves) -> wutong::Task<storage_pair_t> {
        if (node_h.is_nullptr()) {
          BPT_LOG_CRITICAL << "Null handle in get_first_key_of_subtree." << std::endl;
          throw std::logic_error("Encountered null handle when trying to get first key during B+ tree initialization.");
        }
        if (node_level_from_leaves == 0) {
          auto leaf_const_ref = co_await node_h.template const_ref<LeafNode>();
          if (leaf_const_ref->size == 0) {
            BPT_LOG_CRITICAL << "Leaf node " << node_h.page_id << " is empty in get_first_key_of_subtree." << std::endl;
            throw std::logic_error("Leaf node is empty during B+ tree initialization; cannot get first key.");
          }
          co_return leaf_const_ref->data[0];
        } else {
          auto index_const_ref = co_await node_h.template const_ref<IndexNode>();
          if (index_const_ref->children[0].is_nullptr()) {
            BPT_LOG_CRITICAL << "Index node " << node_h.page_id << " first child is null in get_first_key_of_subtree." << std::endl;
            throw std::logic_error("Index node's first child is null during B+ tree initialization.");
          }
          co_return co_await get_first_key_of_subtree(index_const_ref->children[0], node_level_from_leaves - 1);
        }
      };

      BPT_LOG_DEBUG << "Phase 2: Building Index Levels." << std::endl;
      while (current_level_handles.size() > 1) {
        BPT_LOG_DEBUG << "Building parent level for " << current_level_handles.size() << " nodes at current_nodes_layer_from_leaves " << current_nodes_layer_from_leaves << std::endl;
        sjtu::vector<MutableHandle> next_level_handles;
        size_t child_idx_in_current_level = 0;

        while (child_idx_in_current_level < current_level_handles.size()) {
          MutableHandle new_index_handle = PersistentMemoryAsync::create_mutable();
          size_t num_children_for_this_node = 0;
          {
            auto index_ref = co_await new_index_handle.template ref<IndexNode>(true);
            index_ref->size = 0;
            // size_t start_child_idx = child_idx_in_current_level; // Not used
            for (size_t i = 0;
                 i <= IndexNode::split_threshold && child_idx_in_current_level < current_level_handles.size();
                 ++i) {
              index_ref->children[i] = current_level_handles[child_idx_in_current_level++];
              num_children_for_this_node++;
            }
            index_ref->size = num_children_for_this_node - 1;
            
            for (size_t k = 0; k < index_ref->size; ++k) {
              index_ref->data[k] = co_await get_first_key_of_subtree(index_ref->children[k + 1], current_nodes_layer_from_leaves);
            }
            BPT_LOG_DEBUG << "Initialized " << format_index_contents(*index_ref, new_index_handle.page_id) << std::endl;
          }
          next_level_handles.push_back(new_index_handle);
        }
        current_level_handles = std::move(next_level_handles);
        current_nodes_layer_from_leaves++;
        tree_height_.val++;
        BPT_LOG_DEBUG << "Parent level created with " << current_level_handles.size() << " nodes. Tree height now " << tree_height_.val << std::endl;
      }

      BPT_LOG_DEBUG << "Phase 3: Setting Root Handle." << std::endl;
      if (!current_level_handles.empty()) {
        root_handle_.val = current_level_handles[0];
        BPT_LOG_DEBUG << "Root handle set to page " << root_handle_.val.page_id << std::endl;
      } else {
        root_handle_.val.set_nullptr();
        tree_height_.val = 0;
        BPT_LOG_WARN << "current_level_handles is empty after index construction. Setting tree to empty." << std::endl;
        co_return;
      }

      BPT_LOG_DEBUG << "Phase 4: Setting IndexNode layers." << std::endl;
      if (tree_height_.val > 1) {
        std::queue<std::pair<MutableHandle, size_t>> q_layer_set;
        q_layer_set.push({root_handle_.val, 0});
        BPT_LOG_DEBUG << "Starting layer setting from root " << root_handle_.val.page_id << " (layer 0)." << std::endl;

        while (!q_layer_set.empty()) {
          auto [curr_h, current_node_actual_layer_from_root] = q_layer_set.front();
          q_layer_set.pop();

          if (current_node_actual_layer_from_root < tree_height_.val - 1) {
            {
              auto index_node_ref = co_await curr_h.template ref<IndexNode>(false);
              // BPT_LOG_DEBUG << "Before layer set: " << format_index_contents(*index_node_ref, curr_h.page_id) << std::endl; // Can be too verbose
              index_node_ref->layer = current_node_actual_layer_from_root;
              BPT_LOG_DEBUG << "Set layer for " << format_index_contents(*index_node_ref, curr_h.page_id) << std::endl;
              for (size_t i = 0; i <= index_node_ref->size; ++i) {
                if (!index_node_ref->children[i].is_nullptr()) {
                  q_layer_set.push({index_node_ref->children[i], current_node_actual_layer_from_root + 1});
                }
              }
            }
          }
        }
        BPT_LOG_DEBUG << "IndexNode layer setting complete." << std::endl;
      } else {
        BPT_LOG_DEBUG << "Tree height is 1 (only root leaf). No index node layers to set." << std::endl;
      }
      BPT_LOG_DEBUG << "Exiting. Initialization complete." << std::endl;
      co_return;
    }

    // Helper for logging LeafNode state
    static std::string format_leaf_contents(const LeafNode& node, page_id_t pid) {
      std::stringstream ss;
      ss << "LeafNode (Page " << pid << ") State: Size=" << node.size
         << ", Sibling=" << (node.sibling.is_nullptr() ? "NULL" : std::to_string(node.sibling.page_id))
         << ", Data=[";
      for (size_t i = 0; i < node.size; ++i) {
        ss << "(" << node.data[i].first << "," << node.data[i].second << ")"
           << (i == node.size - 1 ? "" : ", ");
      }
      ss << "]";
      return ss.str();
    }

    // Helper for logging IndexNode state
    static std::string format_index_contents(const IndexNode& node, page_id_t pid) {
      std::stringstream ss;
      ss << "IndexNode (Page " << pid << ") State: Size=" << node.size
         << ", Layer=" << node.layer << ", Keys=[";
      for (size_t i = 0; i < node.size; ++i) {
        ss << "(" << node.data[i].first << "," << node.data[i].second << ")"
           << (i == node.size - 1 ? "" : ", ");
      }
      ss << "], ChildrenPIDs=[";
      for (size_t i = 0; i <= node.size; ++i) {
        ss << (node.children[i].is_nullptr() ? "NULL" : std::to_string(node.children[i].page_id))
           << (i == node.size ? "" : ", ");
      }
      ss << "]";
      return ss.str();
    }
  };



} // namespace norb