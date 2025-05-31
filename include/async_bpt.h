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

//#define TEMPLATE_CHECK

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
    static constexpr size_t FLUSH_THRESHOLD = 10;
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
    size_t unfinished_count = 0;

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
      PinnedPath pinned_path;
      if (tree_height_.val == 0) {
        co_return pinned_path;
      }

      MutableHandle current_handle = root_handle_.val;
      for (size_t current_level = 0;
           current_level < tree_height_.val - 1; ++current_level) {
        if (current_handle.is_nullptr()) {
          pinned_path.clear();
          co_return pinned_path;
        }
        pinned_path.original_path_handles.push_back(current_handle);

        auto index_node_const_ref =
            co_await current_handle.const_ref<IndexNode>();
        size_t child_idx_in_node =
            find_idx_in_index(*index_node_const_ref, target_pair);
        pinned_path.index_pos.push_back(norb::make_pair(
            std::move(index_node_const_ref), child_idx_in_node));
        current_handle =
            pinned_path.index_pos.back().first->children[child_idx_in_node];
           }

      if (current_handle.is_nullptr()) {
        pinned_path.clear();
        co_return pinned_path;
      }
      pinned_path.original_path_handles.push_back(current_handle);

      auto leaf_node_const_ref = co_await current_handle.const_ref<LeafNode>();
      pinned_path.leaf_ref = std::move(leaf_node_const_ref);

      co_return pinned_path;
    }

    wutong::Task<MutableHandle>
    find_leaf(const storage_pair_t &target_pair) const {
      PinnedPath path = co_await find_path(target_pair);
      if (!path.is_valid()) {
        co_return MutableHandle{};
      }
      co_return path.original_path_handles.back();
    }

    wutong::Task<sjtu::vector<storage_pair_t>>
    find_range_on_bpt_async(const storage_pair_t &range_start,
                            const storage_pair_t &range_end) const {
      sjtu::vector<storage_pair_t> bpt_pairs_vec;
      if (tree_height_.val == 0) {
        co_return bpt_pairs_vec;
      }
      MutableHandle leaf_handle = co_await find_leaf(range_start);
      while (!leaf_handle.is_nullptr()) {
        auto leaf_node_obj = co_await leaf_handle.const_ref<LeafNode>();
        const LeafNode &leaf_node = *leaf_node_obj;
        size_t pos_in_leaf = lower_bound_in_leaf(leaf_node, range_start);
        for (; pos_in_leaf < leaf_node.size; ++pos_in_leaf) {
          const storage_pair_t &current_pair_in_leaf =
              leaf_node.data[pos_in_leaf];
          if (current_pair_in_leaf >= range_end) {
            leaf_handle.set_nullptr();
            break;
          }
          bpt_pairs_vec.push_back(current_pair_in_leaf);
        }
        if (leaf_handle.is_nullptr())
          break;
        leaf_handle = leaf_node.sibling;
      }
      co_return bpt_pairs_vec;
    }

  public:
    AsyncBPlusTree() = default;
    ~AsyncBPlusTree() = default;

    // --- Public API (Asynchronous) ---
    wutong::Task<sjtu::vector<storage_pair_t>>
    find_range_async(const storage_pair_t &range_start,
                     const storage_pair_t &range_end) {
      unfinished_count++;

      sjtu::vector<storage_pair_t> result_vec;

      sjtu::vector<Pair<storage_pair_t, OpType>> map_ops_in_range;
      auto map_it_start = write_map_.lower_bound(range_start);
      auto map_it_end = write_map_.lower_bound(range_end);
      for (auto it_map = map_it_start; it_map != map_it_end; ++it_map) {
        map_ops_in_range.push_back(
            norb::make_pair(it_map->first, it_map->second));
      }

      sjtu::vector<storage_pair_t> bpt_pairs_in_range =
          co_await find_range_on_bpt_async(range_start, range_end);

      // standard merge for ordered vectors;
      size_t map_idx = 0;
      size_t bpt_idx = 0;

      while (map_idx < map_ops_in_range.size() &&
             bpt_idx < bpt_pairs_in_range.size()) {
        const storage_pair_t &map_pair = map_ops_in_range[map_idx].first;
        OpType map_op = map_ops_in_range[map_idx].second;
        const storage_pair_t &bpt_pair = bpt_pairs_in_range[bpt_idx];

        if (map_pair < bpt_pair) {
          if (map_op == OpType::INSERT) {
            result_vec.push_back(map_pair);
          }
          map_idx++;
        } else if (bpt_pair < map_pair) {
          result_vec.push_back(bpt_pair);
          bpt_idx++;
        } else {
          if (map_op == OpType::INSERT) {
            result_vec.push_back(map_pair);
          }
          map_idx++;
          bpt_idx++;
        }
      }
      while (map_idx < map_ops_in_range.size()) {
        if (map_ops_in_range[map_idx].second == OpType::INSERT) {
          result_vec.push_back(map_ops_in_range[map_idx].first);
        }
        map_idx++;
      }
      while (bpt_idx < bpt_pairs_in_range.size()) {
        result_vec.push_back(bpt_pairs_in_range[bpt_idx]);
        bpt_idx++;
      }
      unfinished_count--;
      co_return result_vec;
    }

    wutong::Task<sjtu::vector<storage_pair_t>>
    find_all_async(const idx_t &hashed_key) {
      co_return co_await find_range_async(norb::make_pair(hashed_key, val_min),
                                          norb::make_pair(hashed_key, val_max));
    }

    wutong::Task<void> insert_async(const idx_t &key, const val_t &val) {
      if(write_map_.size()>=FLUSH_THRESHOLD) {
        co_await flush();
      }
      storage_pair_t target_pair = norb::make_pair(key, val);
      write_map_[target_pair] = OpType::INSERT;
      co_return;
    }

    wutong::Task<void> remove_async(const idx_t &key, const val_t &val) {
      if(write_map_.size()>=FLUSH_THRESHOLD) {
        co_await flush();
      }
      storage_pair_t target_pair = norb::make_pair(key, val);
      write_map_[target_pair] = OpType::DELETE;
      co_return;
    }


    wutong::Task<void> shutdown() {
      co_await flush();
      co_await pmem.flush_all();
      co_return;
    }
    // this manages all actual modification of the disk b+tree.
    // For simplicity(and because we are single threaded), I design it as
    // blocking other operations.
    wutong::Task<void> flush() {
    wutong::SmartTask<void> prefetch_task{};
    is_during_flush = true;
    while (unfinished_count) {
        pmem.poll();
    }

    if (write_map_.empty()) {
        is_during_flush = false;
        co_return;
    }

    if (root_handle_.val.is_nullptr()) {
        sjtu::vector<storage_pair_t> new_data;
        for (auto &op : write_map_) {
            if (op.second == OpType::INSERT) {
                new_data.push_back(op.first);
            }
        }
        co_await initialize_from_vector_async(new_data);
        write_map_.clear();
        is_during_flush = false;
        co_return;
    }

    auto it = write_map_.begin();
    PinnedPath path;
    bool path_needs_refresh = true;

    int ops_since_prefetch = 0;
    const int PREFETCH_OPS_LIMIT = 1000;

    page_id_t last_leaf_id_prefetch = -1;
    int distinct_leaves_prefetch = 0;
    const int PREFETCH_LEAF_LIMIT = 0.75*BATCH_SIZE;

    while (it != write_map_.end()) {
        storage_pair_t key = it->first;
        OpType op_type = it->second;

        if (path_needs_refresh) {
            path.clear();
            path = co_await find_path(key);
            path_needs_refresh = false;

            if (path.is_valid() && path.leaf_ref.has_value()) {
                MutableHandle leaf_handle = path.original_path_handles.back();
                if (leaf_handle.page_id != last_leaf_id_prefetch) {
                    distinct_leaves_prefetch++;
                    last_leaf_id_prefetch = leaf_handle.page_id;
                }
            }
        }

        bool should_trigger_prefetch = (ops_since_prefetch >= PREFETCH_OPS_LIMIT) ||
                                       (distinct_leaves_prefetch >= PREFETCH_LEAF_LIMIT);

        if (should_trigger_prefetch) {
          if (prefetch_task.is_valid()) {
            co_await prefetch_task;
          }
          if (path.is_valid() && !path.original_path_handles.empty()) {
            sjtu::vector<page_id_t> prefetch_ids = get_prefetch_id(path, it);
            if (!prefetch_ids.empty()) {
                prefetch_task = pmem.prefetch_batch(prefetch_ids);
            }
          }
          ops_since_prefetch = 0;
          distinct_leaves_prefetch = 0;
        }

        if (!path.is_valid() && tree_height_.val > 0) {
            std::cerr << "[CRITICAL]Find Path failed" << std::endl;
        }

        bool structure_changed = co_await handle_operation(key, op_type, path);

        ++it;
        if (structure_changed) {
            path_needs_refresh = true;
        } else {
            bool in_current_leaf = false;
            if (it != write_map_.end() &&
                path.is_valid() && path.leaf_ref.has_value()) {
                const auto& leaf = path.leaf_ref.value();
                storage_pair_t next_key = it->first;

                in_current_leaf = (leaf->size > 0 &&
                                          next_key >= leaf->data[0] &&
                                          next_key <= leaf->data[leaf->size - 1]);
            }
            path_needs_refresh = !in_current_leaf;
        }
        ops_since_prefetch++;
    }

    write_map_.clear();
    is_during_flush = false;
    co_return;
}

    bool permit_operation() {
      return pmem.permit_IO() && !is_during_flush;
    }

    // IMPORTANT: should only exist in the main event loop.call in a coroutine
    // is STRICTLY forbidden(Ask me if you need)
    void poll() {
      pmem.poll();
      // other actions for check flush
    }

    sjtu::vector<page_id_t> get_prefetch_id(
        const PinnedPath& path,
        typename sjtu::map<storage_pair_t, OpType>::iterator map_it
    ) {
      sjtu::vector<page_id_t> prefetch_ids;

      if (tree_height_.val == 0) {
        return prefetch_ids;
      }
      if (tree_height_.val == 1) {
        if (!root_handle_.val.is_nullptr() && map_it != write_map_.end()) {
          auto opt_root_leaf_ref = root_handle_.val.template try_const_ref<LeafNode>();
          if (!opt_root_leaf_ref) {
            prefetch_ids.push_back(root_handle_.val.page_id);
          }
        }
        return prefetch_ids;
      }

      auto current_path = path.index_pos;

      while (prefetch_ids.size() < BATCH_SIZE) {
        if (map_it == write_map_.end()) {
            break;
        }

        storage_pair_t next_target_key = map_it->first;

        Pair<page_id_t, storage_pair_t> prefetch_info =
            next_prefetch_step(current_path, next_target_key);

        page_id_t page_to_load = prefetch_info.first;
        storage_pair_t next_separator_key = prefetch_info.second;

        if (page_to_load != -1) {
           prefetch_ids.push_back(page_to_load);
        }
        map_it = write_map_.lower_bound(next_separator_key);
      }
      return prefetch_ids;
    }

    Pair<page_id_t,storage_pair_t> next_prefetch_step(
        sjtu::vector<Pair<PersistentMemoryAsync::ConstHandledReference<IndexNode>, size_t>>& path_prefix,
        const storage_pair_t& target_key
    ) {
      //going up to find a valid range
      while (!path_prefix.empty()) {
        auto& current_level_info = path_prefix.back();
        const IndexNode& current_node = *(current_level_info.first);

        size_t child_idx = find_idx_in_index(current_node, target_key);

        if (child_idx <= current_node.size) {
             if (child_idx != current_node.size || path_prefix.size() == 1 || current_node.size == 0) {
                current_level_info.second = child_idx;
                break;
             }
        }
        path_prefix.pop_back();
      }

      //if (path_prefix.empty()) {
      //    return {-1, sp_max};
      //}

      //descending to the deepest node in cache
      storage_pair_t upper = {std::numeric_limits<idx_t>::max(),std::numeric_limits<val_t>::max()};
      auto& current = path_prefix.back();
      PersistentMemoryAsync::ConstHandledReference<IndexNode> cur_node = (current.first);
      size_t cur_idx = current.second;
      while (path_prefix.size() < tree_height_.val - 1) {
        if(cur_idx<cur_node->size) {
          upper = cur_node->data[cur_idx];
        }
        MutableHandle child_handle = cur_node->children[cur_idx];
        auto opt_child = child_handle.template try_const_ref<IndexNode>();

        if (!opt_child) {
            return {child_handle.page_id, upper};
        }
        cur_node = opt_child.value();
        cur_idx = find_idx_in_index(*cur_node,target_key);
        path_prefix.emplace_back(std::move(opt_child.value()), cur_idx);
      }
      if(cur_idx<cur_node->size) {
        upper = cur_node->data[cur_idx];
      }

      auto leaf_handle = path_prefix.back().first->children[path_prefix.back().second];
      auto opt_leaf = leaf_handle.template try_const_ref<LeafNode>();
      if(!opt_leaf) {
        return {leaf_handle.page_id,upper};
      }
      return {INVALID_PAGE_ID,upper};
    }

    wutong::Task<bool> handle_operation(storage_pair_t value, OpType op, PinnedPath &path) { // path is PinnedPath&
        bool structural_or_key_change_occurred = false;

        if (tree_height_.val == 0) {
            if (op == OpType::INSERT) {
                MutableHandle new_leaf_handle = co_await pmem.create_mutable_and_init<LeafNode>();
                auto leaf_ref_init = co_await new_leaf_handle.ref<LeafNode>(true); // Renamed to avoid conflict
                leaf_ref_init->data[0] = value;
                leaf_ref_init->size = 1;
                // sibling is already nullptr
                root_handle_.val = new_leaf_handle;
                tree_height_.val = 1;
                co_return true;
            } else {
                co_return false;
            }
        }

        if (!path.is_valid() || path.original_path_handles.empty() || !path.leaf_ref.has_value()) { // CHANGED: check leaf_ref
            if (op == OpType::DELETE) co_return false;
            // This implies an issue with find_path or an unexpected state.
            assert(false && "Invalid path provided to handle_operation for non-empty tree");
            co_return false;
        }

        MutableHandle leaf_node_handle = path.original_path_handles.back();

        size_t within_leaf_node_pos; // Will be calculated now

        if (op == OpType::INSERT) {
            auto leaf_node_href = co_await leaf_node_handle.ref<LeafNode>();
            within_leaf_node_pos = lower_bound_in_leaf(*leaf_node_href, value);

            if (within_leaf_node_pos < leaf_node_href->size && leaf_node_href->data[within_leaf_node_pos] == value) {
                 co_return false;
            }


            array::insert_at(leaf_node_href->data, leaf_node_href->size, within_leaf_node_pos, value);
            leaf_node_href->size++;

            bool needs_propagation_upwards = false;
            if (leaf_node_href->size > LeafNode::split_threshold) {
                structural_or_key_change_occurred = true;
                if (path.height() == 1) {
                    leaf_node_href.Drop();
                    co_await handle_root_overflow_async(node_type::leaf);
                } else {
                    Pair<MutableHandle, size_t> parent_frame = {
                        path.original_path_handles[path.original_path_handles.size() - 2], // Parent handle
                        path.index_pos.back().second
                    };
                    leaf_node_href.Drop();
                    needs_propagation_upwards = co_await handle_leaf_overflow_async(parent_frame);
                }
            } else {
                 leaf_node_href.Drop();
            }

            int current_level_idx_in_path_handles = path.original_path_handles.size() - 2; // Start with parent
            int current_level_idx_in_index_pos = path.index_pos.size() - 1; // Corresponds to parent

            while (current_level_idx_in_path_handles >= 0 && needs_propagation_upwards) {
                structural_or_key_change_occurred = true;
                if (current_level_idx_in_path_handles == 0) { // Current node is root
                     co_await handle_root_overflow_async(node_type::index);
                     needs_propagation_upwards = false;
                } else {
                    Pair<MutableHandle, size_t> grandparent_frame = {
                        path.original_path_handles[current_level_idx_in_path_handles - 1],
                        path.index_pos[current_level_idx_in_index_pos -1].second
                    };
                    needs_propagation_upwards = co_await handle_index_overflow_async(grandparent_frame);
                }
                current_level_idx_in_path_handles--;
                current_level_idx_in_index_pos--;
            }
            co_return structural_or_key_change_occurred;

        } else { // OpType::DELETE
            auto leaf_node_href = co_await leaf_node_handle.ref<LeafNode>();
            within_leaf_node_pos = lower_bound_in_leaf(*leaf_node_href, value); // CHANGED: Calculate here

            if (within_leaf_node_pos >= leaf_node_href->size || leaf_node_href->data[within_leaf_node_pos] != value) {
                leaf_node_href.Drop();
                co_return false;
            }

            array::remove_at(leaf_node_href->data, leaf_node_href->size, within_leaf_node_pos);
            leaf_node_href->size--;

            bool needs_propagation_upwards = false;
            if (path.height() == 1) { // Root is a leaf
                if (leaf_node_href->size == 0) {
                    leaf_node_href.Drop();
                    co_await handle_root_underflow_async(node_type::leaf);
                    structural_or_key_change_occurred = true;
                } else {
                    leaf_node_href.Drop();
                }
            } else if (leaf_node_href->size < LeafNode::merge_threshold) {
                structural_or_key_change_occurred = true;
                Pair<MutableHandle, size_t> parent_frame = {
                    path.original_path_handles[path.original_path_handles.size() - 2],
                    path.index_pos.back().second
                };
                leaf_node_href.Drop();
                needs_propagation_upwards = co_await handle_leaf_underflow_async(parent_frame);
            } else {
                leaf_node_href.Drop();
            }

            int current_level_idx_in_path_handles = path.original_path_handles.size() - 2;
            int current_level_idx_in_index_pos = path.index_pos.size() - 1;

            while (current_level_idx_in_path_handles >= 0 && needs_propagation_upwards) {
                structural_or_key_change_occurred = true;
                if (current_level_idx_in_path_handles == 0) {
                    co_await handle_root_underflow_async(node_type::index);
                    needs_propagation_upwards = false;
                } else {
                     Pair<MutableHandle, size_t> grandparent_frame = {
                        path.original_path_handles[current_level_idx_in_path_handles - 1],
                        path.index_pos[current_level_idx_in_index_pos -1].second
                    };
                    needs_propagation_upwards = co_await handle_index_underflow_async(grandparent_frame);
                }
                current_level_idx_in_path_handles--;
                current_level_idx_in_index_pos--;
            }
            co_return structural_or_key_change_occurred;
        }
    }
  private:
    // Overflow handlers remain the same, they return Task<bool> (parent_overflowed)
    // handle_leaf_overflow_async, handle_index_overflow_async, handle_root_overflow_async

    // --- Underflow Handlers with Aggressive Borrowing ---

    // Returns: true if parent underflowed, false otherwise.
    // This function itself signifies a key/structural change.
    wutong::Task<bool> handle_leaf_underflow_async(Pair<MutableHandle, size_t> frame) {
        auto parent_node_href = co_await frame.first.ref<IndexNode>();
        const size_t child_idx_in_parent = frame.second;
        MutableHandle current_node_handle = parent_node_href->children[child_idx_in_parent];

        // Try Aggressive Borrow from Left Sibling
        if (child_idx_in_parent > 0) {
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            auto left_sibling_href = co_await left_sibling_handle.ref<LeafNode>();
            auto current_node_href_check_size = co_await current_node_handle.const_ref<LeafNode>(); // Just to check current size safely

            // Condition for redistribution: if left can give elements and total can be balanced
            // A simple condition: if left has more than minimum by a decent margin
            size_t combined_size_for_borrow = left_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (left_sibling_href->size > LeafNode::merge_threshold && combined_size_for_borrow >= 2 * LeafNode::merge_threshold +1 ) { // Ensure left can give at least one and combined is enough for two valid nodes
                auto current_node_href = co_await current_node_handle.ref<LeafNode>(); // Get mutable ref now

                size_t num_to_move_from_left = (left_sibling_href->size - LeafNode::merge_threshold) / 2; // Example: move half of excess from left
                if (left_sibling_href->size > current_node_href->size) { // More sophisticated: try to balance them
                    num_to_move_from_left = (left_sibling_href->size - current_node_href->size) / 2;
                }
                if (num_to_move_from_left == 0 && left_sibling_href->size > LeafNode::merge_threshold) num_to_move_from_left = 1; // Ensure at least one if possible


                if (num_to_move_from_left > 0 && left_sibling_href->size >= LeafNode::merge_threshold + num_to_move_from_left) {
                    // Make space in current_node_href at the beginning
                    for (size_t i = current_node_href->size + num_to_move_from_left - 1; i >= num_to_move_from_left; --i) {
                        current_node_href->data[i] = current_node_href->data[i - num_to_move_from_left];
                    }
                    // Move elements from end of left_sibling_href
                    for (size_t i = 0; i < num_to_move_from_left; ++i) {
                        current_node_href->data[i] = left_sibling_href->data[left_sibling_href->size - num_to_move_from_left + i];
                    }
                    current_node_href->size += num_to_move_from_left;
                    left_sibling_href->size -= num_to_move_from_left;

                    parent_node_href->data[child_idx_in_parent - 1] = current_node_href->data[0]; // Update separator
                    // All refs drop. Borrow successful.
                    co_return false; // Parent structure (child count) didn't change, but key did.
                                     // The main handle_operation will return true because this func was called.
                }
                // current_node_href drops if not returned
            }
            // left_sibling_href drops
        }

        // Try Aggressive Borrow from Right Sibling
        if (child_idx_in_parent < parent_node_href->size) {
            MutableHandle right_sibling_handle = parent_node_href->children[child_idx_in_parent + 1];
            auto right_sibling_href = co_await right_sibling_handle.ref<LeafNode>();
            auto current_node_href_check_size = co_await current_node_handle.const_ref<LeafNode>();
            size_t combined_size_for_borrow = right_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (right_sibling_href->size > LeafNode::merge_threshold && combined_size_for_borrow >= 2 * LeafNode::merge_threshold +1) {
                auto current_node_href = co_await current_node_handle.ref<LeafNode>();
                size_t num_to_move_from_right = (right_sibling_href->size - LeafNode::merge_threshold) / 2;
                 if (right_sibling_href->size > current_node_href->size) {
                    num_to_move_from_right = (right_sibling_href->size - current_node_href->size) / 2;
                }
                if (num_to_move_from_right == 0 && right_sibling_href->size > LeafNode::merge_threshold) num_to_move_from_right = 1;


                if (num_to_move_from_right > 0 && right_sibling_href->size >= LeafNode::merge_threshold + num_to_move_from_right) {
                    // Move elements from beginning of right_sibling_href to end of current_node_href
                    for (size_t i = 0; i < num_to_move_from_right; ++i) {
                        current_node_href->data[current_node_href->size + i] = right_sibling_href->data[i];
                    }
                    current_node_href->size += num_to_move_from_right;
                    // Shift data in right_sibling_href
                    for(size_t i=0; i < right_sibling_href->size - num_to_move_from_right; ++i) {
                        right_sibling_href->data[i] = right_sibling_href->data[i + num_to_move_from_right];
                    }
                    right_sibling_href->size -= num_to_move_from_right;

                    parent_node_href->data[child_idx_in_parent] = right_sibling_href->data[0]; // Update separator
                    co_return false;
                }
            }
        }

        // Borrow failed or not aggressive enough, proceed to Merge
        // Merge logic remains the same: if it happens, parent loses a key/child.
        if (child_idx_in_parent < parent_node_href->size) {
            co_await merge_leaf_with_right_async(parent_node_href, child_idx_in_parent, current_node_handle);
        } else {
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            co_await merge_leaf_with_right_async(parent_node_href, child_idx_in_parent - 1, left_sibling_handle);
        }

        co_return parent_node_href->size < IndexNode::merge_threshold;
    }

    // Returns: true if parent underflowed, false otherwise.
    wutong::Task<bool> handle_index_underflow_async(Pair<MutableHandle, size_t> frame) {
        auto parent_node_href = co_await frame.first.ref<IndexNode>();
        const size_t child_idx_in_parent = frame.second;
        MutableHandle current_node_handle = parent_node_href->children[child_idx_in_parent];

        // Try Aggressive Borrow from Left Sibling (Index Node)
        if (child_idx_in_parent > 0) {
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            auto left_sibling_href = co_await left_sibling_handle.ref<IndexNode>();
            auto current_node_href_check_size = co_await current_node_handle.const_ref<IndexNode>();
            size_t combined_keys_for_borrow = left_sibling_href->size + current_node_href_check_size->size; // Num keys
            current_node_href_check_size.Drop();

            // Condition: left has > min keys, and combined they can support two valid nodes + the key from parent
            if (left_sibling_href->size > IndexNode::merge_threshold && combined_keys_for_borrow + 1 >= 2 * IndexNode::merge_threshold +1) {
                auto current_node_href = co_await current_node_handle.ref<IndexNode>();

                // Simplified: move 1 key and 1 child via parent (standard B-tree borrow)
                // True aggressive redistribution for index nodes is more complex, involving multiple keys/children.
                // For now, stick to standard single element borrow for index nodes, as it's already complex.
                // If we wanted to move more, we'd shift multiple keys from left_sibling->data and
                // multiple children from left_sibling->children, plus the parent separator.

                // Standard borrow 1 element:
                array::insert_at(current_node_href->children, current_node_href->size + 1, 0, left_sibling_href->children[left_sibling_href->size]);
                array::insert_at(current_node_href->data, current_node_href->size, 0, parent_node_href->data[child_idx_in_parent - 1]);
                current_node_href->size++;

                parent_node_href->data[child_idx_in_parent - 1] = left_sibling_href->data[left_sibling_href->size - 1];
                left_sibling_href->size--; // Child already moved, just decrement key count
                co_return false;
            }
        }

        // Try Aggressive Borrow from Right Sibling (Index Node) - Sticking to standard 1 element borrow
        if (child_idx_in_parent < parent_node_href->size) {
            MutableHandle right_sibling_handle = parent_node_href->children[child_idx_in_parent + 1];
            auto right_sibling_href = co_await right_sibling_handle.ref<IndexNode>();
             auto current_node_href_check_size = co_await current_node_handle.const_ref<IndexNode>();
            size_t combined_keys_for_borrow = right_sibling_href->size + current_node_href_check_size->size;
            current_node_href_check_size.Drop();

            if (right_sibling_href->size > IndexNode::merge_threshold && combined_keys_for_borrow + 1 >= 2 * IndexNode::merge_threshold +1) {
                auto current_node_href = co_await current_node_handle.ref<IndexNode>();
                current_node_href->data[current_node_href->size] = parent_node_href->data[child_idx_in_parent];
                current_node_href->children[current_node_href->size + 1] = right_sibling_href->children[0];
                current_node_href->size++;

                parent_node_href->data[child_idx_in_parent] = right_sibling_href->data[0];
                array::remove_at(right_sibling_href->data, right_sibling_href->size, 0);
                array::remove_at(right_sibling_href->children, right_sibling_href->size + 1, 0);
                right_sibling_href->size--;
                co_return false;
            }
        }

        // Borrow failed, Merge
        if (child_idx_in_parent < parent_node_href->size) {
            co_await merge_index_with_right_async(parent_node_href, child_idx_in_parent, current_node_handle);
        } else {
            MutableHandle left_sibling_handle = parent_node_href->children[child_idx_in_parent - 1];
            co_await merge_index_with_right_async(parent_node_href, child_idx_in_parent - 1, left_sibling_handle);
        }

        co_return parent_node_href->size < IndexNode::merge_threshold;
    }

    // merge_leaf_with_right_async, merge_index_with_right_async,
    // handle_root_overflow_async, handle_root_underflow_async remain as previously defined.
    // ...
    // Definition for handle_leaf_overflow_async
    wutong::Task<bool> handle_leaf_overflow_async(Pair<MutableHandle, size_t> frame) {
        auto parent_node_href = co_await frame.first.ref<IndexNode>();
        const size_t child_idx_in_parent = frame.second;
        MutableHandle old_node_handle = parent_node_href->children[child_idx_in_parent];
        auto old_node_href = co_await old_node_handle.ref<LeafNode>();
        MutableHandle new_node_handle = co_await pmem.create_mutable_and_init<LeafNode>();
        auto new_node_href = co_await new_node_handle.ref<LeafNode>(true);

        size_t total_size = old_node_href->size;
        size_t size_for_old_node = total_size / 2;
        size_t size_for_new_node = total_size - size_for_old_node;
        array::migrate(new_node_href->data, old_node_href->data+size_for_old_node, size_for_new_node);
        new_node_href->size = size_for_new_node;
        old_node_href->size = size_for_old_node;
        new_node_href->sibling = old_node_href->sibling;
        old_node_href->sibling = new_node_handle;

        array::insert_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent, new_node_href->data[0]);
        array::insert_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1, new_node_handle);
        parent_node_href->size++;

        bool parent_overflowed = parent_node_href->size > IndexNode::split_threshold;
        co_return parent_overflowed;
    }

    wutong::Task<bool> handle_index_overflow_async(Pair<MutableHandle, size_t> frame) {
        auto parent_node_href = co_await frame.first.ref<IndexNode>();
        const size_t child_idx_in_parent = frame.second;
        MutableHandle old_node_handle = parent_node_href->children[child_idx_in_parent];
        auto old_node_href = co_await old_node_handle.ref<IndexNode>();
        MutableHandle new_node_handle = co_await pmem.create_mutable_and_init<IndexNode>();
        auto new_node_href = co_await new_node_handle.ref<IndexNode>(true);
        new_node_href->layer = old_node_href->layer;

        size_t total_keys = old_node_href->size;
        size_t keys_for_old_node = (total_keys - 1) / 2;
        storage_pair_t key_to_promote = old_node_href->data[keys_for_old_node];
        size_t keys_for_new_node = total_keys - 1 - keys_for_old_node;
        array::migrate(new_node_href->data, old_node_href->data+ keys_for_old_node + 1, keys_for_new_node);
        array::migrate(new_node_href->children, old_node_href->children+ keys_for_old_node + 1, keys_for_new_node + 1);
        new_node_href->size = keys_for_new_node;
        old_node_href->size = keys_for_old_node;

        array::insert_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent, key_to_promote);
        array::insert_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1, new_node_handle);
        parent_node_href->size++;

        bool parent_overflowed = parent_node_href->size > IndexNode::split_threshold;
        co_return parent_overflowed;
    }

    wutong::Task<void> merge_leaf_with_right_async(
        PersistentMemoryAsync::HandledReference<IndexNode>& parent_node_href,
        size_t child_idx_in_parent,
        MutableHandle left_node_of_merge_pair_handle
    ) {
        auto old_node_href = co_await left_node_of_merge_pair_handle.ref<LeafNode>();
        MutableHandle right_node_handle = parent_node_href->children[child_idx_in_parent + 1];
        auto right_node_href = co_await right_node_handle.ref<LeafNode>();

        array::migrate(old_node_href->data+old_node_href->size, right_node_href->data, right_node_href->size);
        old_node_href->size += right_node_href->size;
        old_node_href->sibling = right_node_href->sibling;

        right_node_href.Drop();
        co_await pmem.remove(right_node_handle);

        array::remove_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent);
        array::remove_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1);
        parent_node_href->size--;
    }

    wutong::Task<void> merge_index_with_right_async(
        PersistentMemoryAsync::HandledReference<IndexNode>& parent_node_href,
        size_t child_idx_in_parent,
        MutableHandle left_node_of_merge_pair_handle
    ) {
        auto old_node_href = co_await left_node_of_merge_pair_handle.ref<IndexNode>();
        MutableHandle right_node_handle = parent_node_href->children[child_idx_in_parent + 1];
        auto right_node_href = co_await right_node_handle.ref<IndexNode>();

        old_node_href->data[old_node_href->size] = parent_node_href->data[child_idx_in_parent];
        old_node_href->size++;

        array::migrate(old_node_href->data+old_node_href->size, right_node_href->data, right_node_href->size);
        array::migrate(old_node_href->children+old_node_href->size, right_node_href->children, right_node_href->size + 1);
        old_node_href->size += right_node_href->size;

        right_node_href.Drop();
        co_await pmem.remove(right_node_handle);

        array::remove_at(parent_node_href->data, parent_node_href->size, child_idx_in_parent);
        array::remove_at(parent_node_href->children, parent_node_href->size + 1, child_idx_in_parent + 1);
        parent_node_href->size--;
    }

    wutong::Task<void> handle_root_overflow_async(node_type root_node_is) {
        MutableHandle old_root_handle = root_handle_.val;
        MutableHandle new_root_handle = co_await pmem.create_mutable_and_init<IndexNode>();
        auto new_root_href = co_await new_root_handle.ref<IndexNode>(true);

        new_root_href->layer = 0;
        new_root_href->children[0] = old_root_handle;

        root_handle_.val = new_root_handle;
        tree_height_.val++;

        if (root_node_is == node_type::leaf) {
            auto old_leaf_root_href = co_await old_root_handle.ref<LeafNode>();
            MutableHandle new_leaf_sibling = co_await pmem.create_mutable_and_init<LeafNode>();
            auto new_leaf_sibling_href = co_await new_leaf_sibling.ref<LeafNode>(true);

            size_t total_size = old_leaf_root_href->size;
            size_t size_old = total_size / 2;
            size_t size_new = total_size - size_old;
            array::migrate(new_leaf_sibling_href->data, old_leaf_root_href->data+size_old, size_new);
            new_leaf_sibling_href->size = size_new;
            old_leaf_root_href->size = size_old;
            new_leaf_sibling_href->sibling = old_leaf_root_href->sibling;
            old_leaf_root_href->sibling = new_leaf_sibling;

            new_root_href->data[0] = new_leaf_sibling_href->data[0];
            new_root_href->children[1] = new_leaf_sibling;
            new_root_href->size = 1;
        } else {
            auto old_index_root_href = co_await old_root_handle.ref<IndexNode>();
            old_index_root_href->layer = 1;
            MutableHandle new_index_sibling = co_await pmem.create_mutable_and_init<IndexNode>();
            auto new_index_sibling_href = co_await new_index_sibling.ref<IndexNode>(true);
            new_index_sibling_href->layer = 1;

            size_t total_keys = old_index_root_href->size;
            size_t keys_old = (total_keys - 1) / 2;
            storage_pair_t promoted_key = old_index_root_href->data[keys_old];
            size_t keys_new = total_keys - 1 - keys_old;

            array::migrate(new_index_sibling_href->data, old_index_root_href->data+keys_old + 1, keys_new);
            array::migrate(new_index_sibling_href->children, old_index_root_href->children+keys_old + 1, keys_new + 1);
            new_index_sibling_href->size = keys_new;
            old_index_root_href->size = keys_old;

            new_root_href->data[0] = promoted_key;
            new_root_href->children[1] = new_index_sibling;
            new_root_href->size = 1;
        }
        co_return;
    }

    wutong::Task<void> handle_root_underflow_async(node_type root_node_is) {
        if (root_node_is == node_type::index) {
            auto root_ref = co_await root_handle_.val.ref<IndexNode>();
            if (root_ref->size == 0) {
                MutableHandle old_root_handle = root_handle_.val;
                root_handle_.val = root_ref->children[0];
                tree_height_.val--;
                root_ref.Drop();
                co_await pmem.remove(old_root_handle);
                if (tree_height_.val > 1 && !root_handle_.val.is_nullptr()) {
                     auto new_root_idx_ref = co_await root_handle_.val.ref<IndexNode>();
                     new_root_idx_ref->layer = 0;
                }
            } else {
                root_ref.Drop();
            }
        } else {
            auto root_ref = co_await root_handle_.val.ref<LeafNode>();
            if (root_ref->size == 0) {
                 MutableHandle old_root_handle = root_handle_.val;
                 root_handle_.val.set_nullptr();
                 tree_height_.val = 0;
                 root_ref.Drop();
                 co_await pmem.remove(old_root_handle);
            } else {
                root_ref.Drop();
            }
        }
        co_return;
    }




    //-----------Test used functions-------
    // they are mostly generated by AI and doesn't guarantee correctness
    //
    //-------------------------------------
  public:
    wutong::Task<void> traverse_async(const bool &do_check = false) const {
      std::cout << "--- Traversing ON-DISK B+ Tree (" << this << ") ---"
                << std::endl;
      // std::cout << "[Info] Logical Size: (No longer maintained)" <<
      // std::endl;
      std::cout << "[Info] On-Disk Height: " << tree_height_.val
                << std::endl;

      if (root_handle_.val.is_nullptr()) {
        std::cout << "[Tree] On-disk structure is Empty." << std::endl;
        std::cout << "--- End Traversal ---" << std::endl << std::endl;
        co_return;
      }
      // ... (rest of traverse_async is the same)
      std::cout << "[Info] On-Disk Root Page ID: "
                << root_handle_.val.page_id << std::endl;
      std::queue<std::pair<MutableHandle, size_t>> q;
      q.push({root_handle_.val, 0});
      size_t current_level_print = 0;
      std::cout << "Level " << current_level_print << ":" << std::endl;
      while (!q.empty()) {
        auto [curr_handle, node_level] = q.front();
        q.pop();
        if (node_level > current_level_print) {
          current_level_print = node_level;
          std::cout << "\nLevel " << current_level_print << ":" << std::endl;
        }
        bool is_leaf = (node_level == tree_height_.val - 1);
        std::cout << "  Node (Page ID: " << curr_handle.page_id << ") ";
        if (is_leaf) {
          auto leaf_obj = co_await curr_handle.template const_ref<LeafNode>();
          const LeafNode &leaf_node = *leaf_obj;
          std::cout << "[Leaf] Size: " << leaf_node.size << " | Sibling: "
                    << (leaf_node.sibling.is_nullptr()
                            ? "NULL"
                            : std::to_string(leaf_node.sibling.page_id))
                    << " | Data: [";
          for (size_t i = 0; i < leaf_node.size; ++i) {
            std::cout << "(" << leaf_node.data[i].first << ","
                      << leaf_node.data[i].second << ")"
                      << (i == leaf_node.size - 1 ? "" : ", ");
          }
          std::cout << "]" << std::endl;
        } else {
          auto index_obj = co_await curr_handle.template const_ref<IndexNode>();
          const IndexNode &index_node = *index_obj;
          std::cout << "[Index] NumKeys: " << index_node.size
                    << " (NumChildren: " << index_node.size + 1 << ")"
                    << " (Layer: " << index_node.layer << ") | Structure: \n";
          for (size_t i = 0; i <= index_node.size; ++i) {
            std::cout << "    ChildPtr[" << i << "]: ";
            if (index_node.children[i].is_nullptr()) {
              std::cout << "NULL";
            } else {
              std::cout << index_node.children[i].page_id;
              q.push({index_node.children[i], node_level + 1});
            }
            if (i < index_node.size) {
              std::cout << " -- Key[" << i << "]: (" << index_node.data[i].first
                        << "," << index_node.data[i].second << ") -- ";
            }
            std::cout << "\n";
          }
        }
      }
      std::cout << "--- End Traversal ---" << std::endl << std::endl;
      co_return;
    }

    wutong::Task<void> initialize_from_vector_async(
        const sjtu::vector<storage_pair_t> &sorted_data) {
      // Clear the write_map_ as we are reinitializing the on-disk structure.
      write_map_.clear();

      // Reset tree metadata.
      // Note: This doesn't deallocate old pages from PMA if the tree was
      // previously populated. For a temporary test function, this is usually
      // acceptable.
      root_handle_.val.set_nullptr();
      tree_height_.val = 0;

      if (sorted_data.empty()) {
        co_return; // Tree remains empty
      }

      // --- 1. Create Leaf Nodes ---
      sjtu::vector<MutableHandle>
          current_level_handles; // Handles of nodes at the current level being
                                 // processed
      size_t data_idx = 0;       // Index for iterating through sorted_data
      MutableHandle prev_leaf_handle; // To link siblings

      while (data_idx < sorted_data.size()) {
        MutableHandle current_leaf_handle =
            PersistentMemoryAsync::create_mutable();

        // Scope for HandledReference to ensure it's released after
        // initialization
        {
          auto leaf_ref = co_await current_leaf_handle.template ref<LeafNode>(
              true /*is_initializing*/);
          leaf_ref->size = 0;
          // Fill data for the leaf node
          for (size_t i = 0;
               i < LeafNode::node_capacity && data_idx < sorted_data.size();
               ++i) {
            leaf_ref->data[leaf_ref->size++] = sorted_data[data_idx++];
          }
          leaf_ref->sibling.set_nullptr(); // Initialize sibling; will be linked
                                           // correctly by the previous node
        } // leaf_ref goes out of scope, page is unlocked and marked dirty

        if (!prev_leaf_handle.is_nullptr()) {
          // Link the previous leaf's sibling pointer to the current leaf
          auto prev_leaf_editor_ref =
              co_await prev_leaf_handle.template ref<LeafNode>(
                  false /*not initializing*/);
          prev_leaf_editor_ref->sibling = current_leaf_handle;
        } // prev_leaf_editor_ref goes out of scope

        current_level_handles.push_back(current_leaf_handle);
        prev_leaf_handle = current_leaf_handle;
      }

      // Leaves are created. current_level_handles contains all leaf node
      // handles. current_nodes_layer_from_leaves: 0 for leaves, 1 for their
      // parents, etc.
      size_t current_nodes_layer_from_leaves = 0;
      tree_height_.val = 1; // Initial height is 1 (leaf level)

      // --- Helper lambda for recursively getting the first key of a subtree
      // --- This is needed to populate keys in parent IndexNodes.
      // node_level_from_leaves: 0 if node_h points to a leaf, 1 if it's a
      // direct parent of leaves, etc.
      std::function<wutong::Task<storage_pair_t>(MutableHandle, size_t)>
          get_first_key_of_subtree;
      get_first_key_of_subtree =
          [&](MutableHandle node_h,
              size_t node_level_from_leaves) -> wutong::Task<storage_pair_t> {
        if (node_h.is_nullptr()) {
          throw std::logic_error("Encountered null handle when trying to get "
                                 "first key during B+ tree initialization.");
        }
        if (node_level_from_leaves == 0) { // Node is a LeafNode
          auto leaf_const_ref = co_await node_h.template const_ref<LeafNode>();
          if (leaf_const_ref->size == 0) {
            throw std::logic_error("Leaf node is empty during B+ tree "
                                   "initialization; cannot get first key.");
          }
          co_return leaf_const_ref->data[0];
        } else { // Node is an IndexNode
          auto index_const_ref =
              co_await node_h.template const_ref<IndexNode>();
          // An index node must have at least one child to be part of a valid
          // path.
          if (index_const_ref->children[0].is_nullptr()) {
            throw std::logic_error("Index node's first child is null during B+ "
                                   "tree initialization.");
          }
          // The first key of an index node's subtree is found by descending its
          // leftmost path.
          co_return co_await get_first_key_of_subtree(
              index_const_ref->children[0], node_level_from_leaves - 1);
        }
      };

      // --- 2. Build Index Levels (Bottom-Up) ---
      while (current_level_handles.size() > 1) {
        sjtu::vector<MutableHandle>
            next_level_handles; // Handles for the parent level being
                                // constructed
        size_t child_idx_in_current_level =
            0; // Index for iterating through current_level_handles

        while (child_idx_in_current_level < current_level_handles.size()) {
          MutableHandle new_index_handle =
              PersistentMemoryAsync::create_mutable();
          size_t num_children_for_this_node = 0;

          // Scope for HandledReference
          {
            auto index_ref = co_await new_index_handle.template ref<IndexNode>(
                true /*is_initializing*/);
            // index_ref->layer will be set in a final pass after total height
            // is known.
            index_ref->size = 0; // Number of keys (will be num_children - 1)

            // Assign children to this new index node
            for (size_t i = 0;
                 i <= IndexNode::node_capacity &&
                 child_idx_in_current_level < current_level_handles.size();
                 ++i) {
              index_ref->children[i] =
                  current_level_handles[child_idx_in_current_level++];
              num_children_for_this_node++;
            }
            index_ref->size = num_children_for_this_node - 1;

            // Populate keys for this index node.
            // Key data[k] is the first key of the subtree rooted at
            // children[k+1].
            for (size_t k = 0; k < index_ref->size; ++k) {
              index_ref->data[k] = co_await get_first_key_of_subtree(
                  index_ref->children[k + 1],
                  current_nodes_layer_from_leaves // This is the
                                                  // layer_from_leaves of the
                                                  // children nodes
              );
            }
          } // index_ref goes out of scope

          next_level_handles.push_back(new_index_handle);
        }
        current_level_handles = std::move(next_level_handles);
        current_nodes_layer_from_leaves++;
        tree_height_.val++;
      }

      // --- 3. Set Root Handle ---
      // The single handle remaining in current_level_handles is the root of the
      // tree.
      if (!current_level_handles.empty()) {
        root_handle_.val = current_level_handles[0];
      } else {
        // This case should only be reached if sorted_data was empty (handled at
        // the start). If somehow reached otherwise, it implies an error or an
        // empty tree.
        root_handle_.val.set_nullptr();
        tree_height_.val = 0;
        co_return;
      }

      // --- 4. Final Pass: Set IndexNode::layer (distance from root) ---
      // This is mainly for debugging/traversal consistency if IndexNode::layer
      // is used. Root is layer 0, its children layer 1, ..., leaves are at
      // layer H-1.
      if (tree_height_.val > 1) { // Only if there are index nodes
        std::queue<std::pair<MutableHandle, size_t>>
            q; // {handle, layer_from_root}
        q.push({root_handle_.val, 0});

        while (!q.empty()) {
          auto [curr_h, current_node_actual_layer_from_root] = q.front();
          q.pop();

          // Leaves are at layer tree_height_on_disk_.val - 1.
          // Only IndexNodes (not leaves) have the 'layer' field to set.
          if (current_node_actual_layer_from_root <
              tree_height_.val - 1) {
            // Scope for HandledReference
            {
              auto index_node_ref = co_await curr_h.template ref<IndexNode>(
                  false /*not initializing*/);
              index_node_ref->layer = current_node_actual_layer_from_root;

              // Enqueue children for the next level
              for (size_t i = 0; i <= index_node_ref->size; ++i) {
                if (!index_node_ref->children[i].is_nullptr()) {
                  q.push({index_node_ref->children[i],
                          current_node_actual_layer_from_root + 1});
                }
              }
            } // index_node_ref goes out of scope
          }
        }
      }
      co_return;
    }
  };

} // namespace norb