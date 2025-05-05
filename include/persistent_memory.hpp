#pragma once

#include "shared.hpp"
#include "stlite/looped_queue.hpp"
#include "stlite/vector.hpp"
#include "utils.hpp"
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
    PersistentMemory &operator=(const PersistentMemory &) = delete;
    PersistentMemory(const PersistentMemory &) = delete;

    // adhere to singleton principle
    static PersistentMemory &get_instance() {
      static PersistentMemory pmem{PMEM_FILE_NAME};
      return pmem;
    }

    template <typename T> struct Handle;
    struct MutableHandle;

  private:
    template <typename T> struct HandledReference;
    template <typename T> struct ConstHandledReference;

    // alias and constants
    using time_stamp_t = unsigned long;
    static constexpr auto time_stamp_inf_ =
        std::numeric_limits<time_stamp_t>::max();

    // The number of pages the memory can store.
    static constexpr slot_id_t SLOT_COUNT = MEMORY_SIZE / PAGE_SIZE;
    static_assert(LRU_K_INDEX <= SLOT_COUNT,
                  "LRU_K_INDEX is larger than PAGE_COUNT");

    class GarbageCollector;

    // private params
    time_stamp_t time_stamp = 1;
    page_id_t current_pages_in_disk = 0;
    slot_id_t current_pages_in_buffer = 0;
    LoopedQueue<time_stamp_t, LRU_K_INDEX> history[SLOT_COUNT];
    page_id_t buffer_page_id[SLOT_COUNT]{};
    bool is_dirty[SLOT_COUNT]{};
    short lock_count[SLOT_COUNT]{};
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
    slot_id_t get_lru_k() const {
      std::pair<time_stamp_t, slot_id_t> evict_lru_k = {time_stamp_inf_, -1};
      for (slot_id_t id = 0; id < current_pages_in_buffer; id++) {
        if (lock_count[id])
          continue;
        if (history[id].back() < evict_lru_k.first) {
          evict_lru_k = {history[id].back(), id};
        }
      }
      if (evict_lru_k.second == static_cast<slot_id_t>(-1))
        throw std::overflow_error("Memory Buffer overflowed!");
      return evict_lru_k.second;
    }

    // Evict a page from the buffer pool
    void evict_page(const slot_id_t &slot_id) {
      assert(fmemory.good());
      if (is_dirty[slot_id]) {
        // write back to disk
        const page_id_t page_id = buffer_page_id[slot_id];
        fmemory.seekp(page_id * PAGE_SIZE, std::ios::beg);
        fmemory.write(buffer[slot_id], PAGE_SIZE);
        assert(fmemory.good());
      }
    }

    // Register a page to the buffer pool
    void load_page_from_disk(const page_id_t &page_id,
                             const slot_id_t &slot_id) {
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

    //! [Changelog] Change the two Reference types to implement lifespan-lock
    //! modeling

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
        if (slot_id == static_cast<slot_id_t>(-1)) {
          if (pmem.current_pages_in_buffer < SLOT_COUNT) {
            // create a new page
            slot_id = pmem.current_pages_in_buffer++;
          } else {
            // use eviction to remove tree
            slot_id = pmem.get_lru_k();
            pmem.evict_page(slot_id);
          }
          pmem.load_page_from_disk(page_id, slot_id);
        }
        pmem.is_dirty[slot_id] = true;
        ++pmem.lock_count[slot_id];
      }

    public:
      explicit HandledReference(const page_id_t &page_id) : page_id(page_id) {
        allocate_page_and_update_slot();
      }

      ~HandledReference() { --get_instance().lock_count[slot_id]; }

      explicit HandledReference(const HandledReference<page_id_t> &) = delete;
      HandledReference<page_id_t> &operator=(const HandledReference<page_id_t> &) = delete;
      HandledReference(HandledReference &&) = delete;

      T *operator->() const {
        // if (get_instance().buffer_page_id[slot_id] != page_id) {
        //   allocate_page_and_update_slot();
        // }
        return reinterpret_cast<T *>(get_instance().buffer[slot_id]);
      }

      T &operator*() const {
        // if (get_instance().buffer_page_id[slot_id] != page_id) {
        //   allocate_page_and_update_slot();
        // }
        return reinterpret_cast<T &>(get_instance().buffer[slot_id]);
      }

      T *as_raw_ptr() const {
        // if (get_instance().buffer_page_id[slot_id] != page_id) {
        //   allocate_page_and_update_slot();
        // }
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
        if (slot_id == static_cast<slot_id_t>(-1)) {
          if (pmem.current_pages_in_buffer < SLOT_COUNT) {
            // create a new page
            slot_id = pmem.current_pages_in_buffer++;
          } else {
            // use eviction to remove tree
            slot_id = pmem.get_lru_k();
            pmem.evict_page(slot_id);
          }
          pmem.load_page_from_disk(page_id, slot_id);
        }
        ++pmem.lock_count[slot_id];
      }

    public:
      explicit ConstHandledReference(const page_id_t &page_id)
          : page_id(page_id) {
        allocate_page_and_update_slot();
      }

      ~ConstHandledReference() { --get_instance().lock_count[slot_id]; }

      explicit ConstHandledReference(const ConstHandledReference<page_id_t> &) = delete;
      ConstHandledReference<page_id_t> &operator = (const ConstHandledReference<page_id_t> &) = delete;
      ConstHandledReference(ConstHandledReference &&) = delete;

      const T *operator->() const {
        // if (get_instance().buffer_page_id[slot_id] != page_id) {
        //   allocate_page_and_update_slot();
        // }
        return reinterpret_cast<T *>(get_instance().buffer[slot_id]);
      }

      const T &operator*() const {
        // if (get_instance().buffer_page_id[slot_id] != page_id) {
        //   allocate_page_and_update_slot();
        // }
        return reinterpret_cast<const T &>(get_instance().buffer[slot_id]);
      }

      const T *as_raw_ptr() const {
        // if (get_instance().buffer_page_id[slot_id] != page_id) {
        //   allocate_page_and_update_slot();
        // }
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
      assert(fmemory.good());
    }
    explicit PersistentMemory(const char *path)
        : PersistentMemory(std::string(path)) {}

    ~PersistentMemory() {
      // update all dirty pages
      for (slot_id_t slot = 0; slot < current_pages_in_buffer; slot++) {
        // if locking fails, assert here to determine why
        // assert(lock_count[slot] == 0);
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
     * @struct Handle
     * @brief A persistent pointer to a stored data.
     * @details A FileManager::handle is a serializable object that acts like
     * a persistent pointer. It is not called 'iterator' because it is not
     * iterable.
     */
    template <typename T> struct Handle {
      page_id_t page_id;

      explicit Handle(const page_id_t &page_id = static_cast<page_id_t>(-1))
          : page_id(page_id) {}

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

      [[nodiscard]] bool is_nullptr() const {
        return page_id == static_cast<page_id_t>(-1);
      }

      void set_nullptr() { page_id = static_cast<page_id_t>(-1); }
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

      explicit MutableHandle(
          const page_id_t &page_id = static_cast<page_id_t>(-1))
          : page_id(page_id) {}

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
      [[nodiscard]] ConstHandledReference<T> const_ref() const {
        return ConstHandledReference<T>(page_id);
      }

      [[nodiscard]] bool is_nullptr() const {
        return page_id == static_cast<page_id_t>(-1);
      }

      void set_nullptr() { page_id = static_cast<page_id_t>(-1); }
    };

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
        const auto page_id = pmem.current_pages_in_disk++;
        std::filesystem::resize_file(PMEM_FILE_NAME, (page_id + 1) * PAGE_SIZE);
        return Handle<T>(page_id);
      }
    }

    /**
     * @brief Obtain a handle from memory and initialize it with placement new.
     * @tparam T The type of variable to store.
     * @param args The arguments to construct the handled instance by.
     * @return A handle to the variable stored in PersistentMemory.
     */
    template <typename T, typename... Args>
    [[nodiscard]] static Handle<T> create_and_init(Args &&...args) {
      const auto handle = create<T>();
      // initialize with list, calling placement new
      new (handle.ref().as_raw_ptr()) T{std::forward<Args>(args)...};
      return handle;
    }

    /**
     * @brief Obtain a mutable handle from the memory.
     * @return A mutable handle to the variable stored in PersistentMemory.
     */
    [[nodiscard]] static MutableHandle create_mutable() {
      auto &pmem = get_instance();

      if (pmem.garbage_collector.available()) {
        // recycle from the garbage collector
        return MutableHandle(pmem.garbage_collector.recycle());
      } else {
        // create a new page and return the handle
        const auto page_id = pmem.current_pages_in_disk++;
        std::filesystem::resize_file(PMEM_FILE_NAME, (page_id + 1) * PAGE_SIZE);
        return MutableHandle(page_id);
      }
    }

    /**
     * @brief Obtain a handle from memory and initialize it with placement new.
     * @tparam T The type of variable to store.
     * @param args The arguments to construct the handled instance by.
     * @return A handle to the variable stored in PersistentMemory.
     */
    template <typename T, typename... Args>
    [[nodiscard]] static MutableHandle create_mutable_and_init(Args &&...args) {
      const auto handle = create_mutable();
      // initialize with list, calling placement new
      new (handle.ref<T>().as_raw_ptr()) T{std::forward<Args>(args)...};
      return handle;
    }

    /**
     * @brief Fetch the handle to a page.
     * @tparam T The type of variable the page stores.
     * @param page_id The page-id for the handle.
     * @return An instance of Handle<T>: the handle to the specified page.
     */
    template <typename T>
    [[nodiscard]] static Handle<T> fetch_handle(const page_id_t &page_id) {
      return Handle<T>(page_id);
    }

    /**
     * @brief Fetch a mutable handle to a page.
     * @param page_id The page-id for the handle.
     * @return An instance of MutableHandle: the mutable handle to the specified
     * page.
     */
    [[nodiscard]] static MutableHandle
    fetch_mutable_handle(const page_id_t &page_id) {
      return MutableHandle(page_id);
    }

    /**
     * @brief Remove a variable from a page, and deallocate the page if it is
     * empty.
     * @tparam T The type of variable to delete.
     * @param handle A handle to which the variable is to be removed.
     */
    template <typename T> static void remove(const Handle<T> &handle) {
      if (handle.is_nullptr())
        return;
      auto &persistent_memory = get_instance();
      // call the destructor of T
      handle.ref().as_raw_ptr()->~T();
      persistent_memory.garbage_collector.dump(handle.page_id);
    }

    /**
     * @brief Remove a variable from a page, and deallocate the page if it is
     * empty.
     * @tparam T The type of variable to delete.
     * @param handle A mutable handle to which the variable is to be removed.
     */
    template <typename T> static void remove(const MutableHandle &handle) {
      if (handle.is_nullptr())
        return;
      auto &persistent_memory = get_instance();
      // call the destructor of T
      handle.ref<T>().as_raw_ptr()->~T();
      persistent_memory.garbage_collector.dump(handle.page_id);
    }

    /**
     * @brief Get the number of pages in the disk, without garbage collection.
     */
    static page_id_t get_page_count() {
      return get_instance().current_pages_in_disk;
    }
  };
} // namespace norb