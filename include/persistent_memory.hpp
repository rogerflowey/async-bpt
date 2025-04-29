#pragma once

#include "file_utils.hpp"
#include "shared.hpp"
#include "stlite/looped_queue.hpp"
#include "stlite/vector.hpp"
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace norb {
  /**
   * @class PersistentMemory
   * @brief Manages disk allocation and the memory pool.
   * @remark The FileManager must be a singleton.
   * @remark References MEMORY_SIZE, PAGE_SIZE, LRU_K_INDEX, PMEM_FILE_NAME from
   * shared.hpp
   */
  class PersistentMemory {
  public:
    static PersistentMemory &get_instance() {
      static PersistentMemory pmem{PMEM_FILE_NAME};
      return pmem;
    }

  private:
    // alias and constants
    using time_stamp_t = unsigned long;
    static constexpr auto time_stamp_inf_ =
        std::numeric_limits<time_stamp_t>::infinity();

    // The number of pages the memory can store.
    static constexpr slot_id_t SLOT_COUNT = MEMORY_SIZE / PAGE_SIZE;
    static_assert(LRU_K_INDEX <= SLOT_COUNT,
                  "LRU_K_INDEX is larger than PAGE_COUNT");

    class GarbageCollector;
    template <typename T> struct Handle;
    template <typename T> struct HandledReference;
    template <typename T> struct ConstHandledReference;

    // private params
    time_stamp_t time_stamp = 1;
    page_id_t current_pages_in_disk = 0;
    slot_id_t current_pages_in_buffer = 0;
    LoopedQueue<time_stamp_t, LRU_K_INDEX> history[SLOT_COUNT];
    page_id_t buffer_page_id[SLOT_COUNT]{};
    bool is_dirty[SLOT_COUNT]{};
    char buffer[SLOT_COUNT][PAGE_SIZE];

    std::fstream fconfig;
    std::fstream fmemory;

    // auxiliary functions

    // Returns the slot number for the page_id, -1 if not found
    slot_id_t find_page_id_in_buffer(const page_id_t &page_id) const {
      for (slot_id_t id = 0; id < current_pages_in_buffer; id++) {
        if (buffer_page_id[id] == page_id) {
          return id;
        }
      }
      return -1;
    }

    // Find the lru-k page from the buffer
    slot_id_t evict_lru_k() const {
      std::pair<time_stamp_t, slot_id_t> evict_lru_k = {time_stamp_inf_, -1};
      for (slot_id_t id = 0; id < current_pages_in_buffer; id++) {
        if (history[id].back() < evict_lru_k.first) {
          evict_lru_k = {history[id].back(), id};
        }
      }
      return evict_lru_k.second;
    }

    // Evict a page from the buffer pool
    void evict_page(const slot_id_t &slot_id) {
      if (is_dirty[slot_id]) {
        // write back to disk
        const page_id_t page_id = buffer_page_id[slot_id];
        fmemory.seekp(page_id * PAGE_SIZE, std::ios::beg);
        fmemory.write(buffer[slot_id], PAGE_SIZE);
      }
    }

    // Register a page to the buffer pool
    void register_page(const page_id_t &page_id, const slot_id_t &slot_id) {
      history[slot_id].insert(time_stamp++);
      buffer_page_id[slot_id] = page_id;
      // copy the disk info to the memory
      fmemory.seekg(page_id * PAGE_SIZE, std::ios::beg);
      filesystem::binary_read(fmemory, buffer[slot_id]);
    }

    /**
     * @class GarbageCollector
     * @brief A helper class to collect deallocated pages.
     */
    class GarbageCollector {
      vector<page_id_t> garbage;

    public:
      GarbageCollector() = default;
      ~GarbageCollector() = default;

      void dump(const page_id_t &page_id) { garbage.push_back(page_id); }

      [[nodiscard]] page_id_t recycle() {
        if (garbage.empty()) {
          return -1;
        }
        const auto back = garbage.back();
        garbage.pop_back();
        return back;
      }

      [[nodiscard]] bool available() const { return not garbage.empty(); }
    } garbage_collector{};

    /**
     * @struct Handle
     * @brief A persistent pointer to a stored data.
     * @details A FileManager::handle is a serializable object that acts like
     * a persistent pointer. It is not called 'iterator' because it is not
     * iterable.
     */
    template <typename T> struct Handle {
      page_id_t page_id = 0;

      explicit Handle(const page_id_t &page_id) : page_id(page_id) {}

      /**
       * @brief Retrieve a read-write reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      [[nodiscard]] HandledReference<T> ref() const {
        return HandledReference<T>(page_id);
      }

      /**
       * @brief Retrieve a read-only reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      [[nodiscard]] ConstHandledReference<T> const_ref() const {
        return ConstHandledReference<T>(page_id);
      }
    };

    /**
     * @struct MutableHandle
     * @brief A persistent pointer to a stored data without type at declaration.
     * @details A FileManager::handle is a serializable object that acts like
     * a persistent pointer. It is not called 'iterator' because it is not
     * iterable.
     */
    struct MutableHandle {
      page_id_t page_id = 0;

      explicit MutableHandle(const page_id_t &page_id) : page_id(page_id) {}

      /**
       * @brief Retrieve a read-write reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      template <typename T> [[nodiscard]] HandledReference<T> ref() const {
        return HandledReference<T>(page_id);
      }

      /**
       * @brief Retrieve a read-only reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      template <typename T>
      [[nodiscard]] HandledReference<T> const_ref() const {
        return ConstHandledReference<T>(page_id);
      }
    };

    /**
     * @struct HandledReference
     * @brief A reference to the data held by Handle.
     */
    template <typename T> struct HandledReference {
    private:
      page_id_t page_id = 0;
      slot_id_t slot_id = 0;

      void allocate_page_and_update_slot() {
        auto &pmem = get_instance();
        slot_id = pmem.find_page_id_in_buffer(page_id);
        if (slot_id == static_cast<page_id_t>(-1)) {
          if (pmem.current_pages_in_buffer < SLOT_COUNT) {
            // create a new page
            slot_id = pmem.current_pages_in_buffer++;
          } else {
            // use eviction to remove tree
            slot_id = pmem.evict_lru_k();
            pmem.evict_page(slot_id);
          }
        }
        pmem.register_page(page_id, slot_id);
        pmem.is_dirty[slot_id] = true;
      }

    public:
      explicit HandledReference(const page_id_t &page_id) : page_id(page_id) {
        allocate_page_and_update_slot();
      }

      ~HandledReference() = default;

      T *operator->() {
        if (get_instance().buffer_page_id[slot_id] != page_id) {
          allocate_page_and_update_slot();
        }
        return reinterpret_cast<T *>(get_instance().buffer[slot_id]);
      }

      T &operator*() {
        if (get_instance().buffer_page_id[slot_id] != page_id) {
          allocate_page_and_update_slot();
        }
        return reinterpret_cast<T *>(get_instance().buffer[slot_id]);
      }
    };

    /**
     * @struct ConstHandledReference
     * @brief A const reference to the data held by Handle.
     */
    template <typename T> struct ConstHandledReference {
    private:
      page_id_t page_id = 0;
      slot_id_t slot_id = 0;

      void allocate_page_and_update_slot() {
        auto &pmem = get_instance();
        slot_id = pmem.find_page_id_in_buffer(page_id);
        if (slot_id == static_cast<page_id_t>(-1)) {
          if (pmem.current_pages_in_buffer < SLOT_COUNT) {
            // create a new page
            slot_id = pmem.current_pages_in_buffer++;
          } else {
            // use eviction to remove tree
            slot_id = pmem.evict_lru_k();
            pmem.evict_page(slot_id);
          }
        }
        pmem.register_page(page_id, slot_id);
      }

    public:
      explicit ConstHandledReference(const page_id_t &page_id)
          : page_id(page_id) {
        allocate_page_and_update_slot();
      }

      ~ConstHandledReference() = default;

      const T *operator->() {
        if (get_instance().buffer_page_id[slot_id] != page_id) {
          allocate_page_and_update_slot();
        }
        return reinterpret_cast<T *>(get_instance().buffer[slot_id]);
      }

      const T &operator*() {
        if (get_instance().buffer_page_id[slot_id] != page_id) {
          allocate_page_and_update_slot();
        }
        return reinterpret_cast<const T *>(get_instance().buffer[slot_id]);
      }
    };

    explicit PersistentMemory(const std::string &path) {
      // create the file if it does not exist
      filesystem::fassert(path);
      filesystem::fassert(path + ".config");
      fconfig.open(path, std::ios::in | std::ios::out | std::ios::binary);
      // if the config file is not empty, read the contents
      if (!filesystem::is_empty(fconfig)) {
        // get the config contents
        fconfig.seekg(0, std::ios::beg);
        filesystem::binary_read(fconfig, current_pages_in_disk);
      }
      fmemory.open(path, std::ios::in | std::ios::out | std::ios::binary);
    }
    explicit PersistentMemory(const char *path)
        : PersistentMemory(std::string(path)) {}

    ~PersistentMemory() {
      // update all dirty pages
      for (slot_id_t slot = 0; slot < current_pages_in_buffer; slot++) {
        evict_page(slot);
      }
      // write the config to the fconfig
      fconfig.seekp(0, std::ios::beg);
      filesystem::binary_write(fconfig, current_pages_in_disk);
      // close the streams
      fconfig.close();
      fmemory.close();
    }

  public:
    /**
     * @brief Obtain a handle from the memory.
     * @tparam T The type of variable to store.
     * @return A handle to the variable stored in PersistentMemory.
     */
    template <typename T> [[nodiscard]] static Handle<T> create() {
      auto &pmem = get_instance();

      if (pmem.garbage_collector.available()) {
        // recycle from the garbage collector
        return Handle<T>(pmem.garbage_collector.recycle());
      } else {
        // create a new page and return the handle
        const auto page_id = ++pmem.current_pages_in_disk;
        std::filesystem::resize_file(PMEM_FILE_NAME, page_id * PAGE_SIZE);
        return Handle<T>(page_id);
      }
    }

    /**
     * @brief Obtain a handle from the memory.
     * @return A mutable handle to the variable stored in PersistentMemory.
     */
    [[nodiscard]] static MutableHandle create() {
      auto &pmem = get_instance();

      if (pmem.garbage_collector.available()) {
        // recycle from the garbage collector
        return MutableHandle(pmem.garbage_collector.recycle());
      } else {
        // create a new page and return the handle
        const auto page_id = ++pmem.current_pages_in_disk;
        std::filesystem::resize_file(PMEM_FILE_NAME, page_id * PAGE_SIZE);
        return MutableHandle(page_id);
      }
    }

    /**
     * @brief Remove a variable from a page, and deallocate the page if it is
     * empty.
     * @tparam T The type of variable to delete.
     * @param handle A handle to which the variable is to be removed.
     */
    template <typename T> static void remove(const Handle<T> &handle) {
      auto &persistent_memory = get_instance();
      persistent_memory.garbage_collector.dump(handle.page_id);
    }
  };
} // namespace norb