#pragma once

#include "disk_scheduler.h"
#include "shared.hpp"
#include "stlite/looped_queue.hpp"
#include "stlite/map.hpp"
#include "stlite/pair.hpp"
#include "stlite/sized_queue.h"
#include "stlite/vector.hpp"
#include "tasks.h"
#include "utils.hpp"
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include <iostream>
#include <map>


namespace norb {
  /**
   * @class PersistentMemoryAsync
   * @brief Manages disk allocation and the memory pool.
   * @remark The FileManager must be a singleton.
   * @remark References MEMORY_SIZE, PAGE_SIZE, LRU_K_INDEX, PMEM_FILE_NAME from
   * shared.hpp
   */
  class PersistentMemoryAsync {
  public:
    PersistentMemoryAsync &operator=(const PersistentMemoryAsync &) = delete;
    PersistentMemoryAsync(const PersistentMemoryAsync &) = delete;


    static PersistentMemoryAsync &get_instance() {
      static PersistentMemoryAsync pmem{PMEM_FILE_NAME};
      return pmem;
    }

    template <typename T> struct Handle;
    struct MutableHandle;


    template <typename T> struct HandledReference;
    template <typename T> struct ConstHandledReference;
    // alias and constants
    using time_stamp_t = unsigned long;
    static constexpr auto time_stamp_inf_ =
        std::numeric_limits<time_stamp_t>::max();

    // The number of pages the memory can store.
    static constexpr slot_id_t SLOT_COUNT = SLOT_MAX_SIZE;
    static_assert(LRU_K_INDEX <= SLOT_COUNT,
                  "LRU_K_INDEX is larger than PAGE_COUNT");

    class GarbageCollector;

    // private params
    time_stamp_t time_stamp = 1;
    page_id_t current_pages_in_disk = 0;
    slot_id_t current_pages_in_buffer = 0;
    page_id_t max_page_size_ = 16;

    struct SlotHeader {
      LoopedQueue<time_stamp_t, LRU_K_INDEX> history{};
      page_id_t buffer_page_id{};
      slot_id_t slot_id{};
      bool is_dirty{};
      short lock_count{};

      sjtu::map<Pair<time_stamp_t, time_stamp_t>, slot_id_t>::iterator it;

      Pair<time_stamp_t, time_stamp_t> get_timestamp() const {
        if (history.empty()) {
          return {0, 0};
        }
        if (history.size() >= LRU_K_INDEX) {
          return {history.back(), 0};
        }
        return {0, history.back()};
      }

      void Reset() {
#ifdef PMA_DEBUG
        std::cout<<"Clearing slot:"<<slot_id<<std::endl;
#endif
        history.clear();
        buffer_page_id = -1;
        is_dirty = false;
        lock_count = 0;
        if (it != sjtu::map<Pair<time_stamp_t, time_stamp_t>,
                            slot_id_t>::iterator{}) {
          std::cerr << "WARNING:iterator not cleared when reset" << std::endl;
        }
        it = {};
      }
    };

    SlotHeader slots[SLOT_COUNT];
    alignas(4096) char buffer[SLOT_COUNT][PAGE_SIZE] = {};

    sjtu::map<page_id_t, slot_id_t> page_table{};
    sjtu::map<Pair<time_stamp_t, time_stamp_t>, slot_id_t> evict_order{};
    sjtu::map<page_id_t, wutong::SharedTask<slot_id_t>> ongoing;

    std::fstream fconfig;
    DiskScheduler fmemory;

    struct pma_config {
      page_id_t current_pages_in_disk;
      page_id_t max_page_size_;
    };

    // auxiliary functions

    // Returns the slot number for the page_id, -1 if not found
    slot_id_t find_page_id_in_buffer(const page_id_t &page_id) const {
      auto result = page_table.find(page_id);
      if (result == page_table.cend()) {
        return -1;
      }
      return result->second;
    }

    // Find and pop the lru-k page from the buffer
    slot_id_t get_lru_k() {
      if (evict_order.empty()){
        std::cerr<<"Memory Buffer overflowed!"<<std::endl;
        throw std::overflow_error("Memory Buffer overflowed!");
      }
      slot_id_t slot_id = evict_order.cbegin()->second;
      evict_order.erase(evict_order.begin());
#ifdef PMA_DEBUG
      std::cout<<"Slot erased from map:"<<slot_id<<std::endl;
#endif

      slots[slot_id].it = {};
      if (slots[slot_id].buffer_page_id != static_cast<page_id_t>(-1)) {
        page_table.erase(slots[slot_id].buffer_page_id);
      }
      return slot_id;
    }

    //Should check this before creating a coroutine
    bool permit_IO() {
      if(evict_order.size()<=SLOT_COUNT/2) {
        return false;
      }
      return true;
    }

    // Evict a page from the buffer pool
    wutong::Task<void> evict_page(const slot_id_t &slot_id) {
      auto &slot = slots[slot_id];
      if (slot.is_dirty) {
        // write back to disk
        const page_id_t page_id = slot.buffer_page_id;
#ifdef PMA_DEBUG
        std::cout<<"Sending write request in evict_page,slot id:"<<slot_id<<" page_id:"<<page_id<<std::endl;
#endif
        bool success = co_await fmemory.async_write(buffer[slot_id], PAGE_SIZE,
                                                    page_id * PAGE_SIZE);
#ifdef PMA_DEBUG
        std::cout<<"Finish write request in evict_page,slot id:"<<slot_id<<" page_id:"<<page_id<<std::endl;
#endif
        assert(success);
        slot.is_dirty = false;
      }
    }

    wutong::Task<slot_id_t> acquire_page_in_slot(const page_id_t &page_id,
                                                 bool is_new = false) {
      slot_id_t found_slot_id = find_page_id_in_buffer(page_id);
      if (found_slot_id != static_cast<slot_id_t>(-1)) {
        co_return found_slot_id;
      }

      bool is_lead = false;
      wutong::SharedTask<slot_id_t> task;
      auto it = ongoing.find(page_id);
      if (it != ongoing.end()) {
        // if there are ongoing requests on the same page and is processing
        task = it->second;
      } else {
        is_lead = true;
        task = evict_and_load(page_id, is_new);
        it = ongoing.insert({page_id, task}).first;
      }
      slot_id_t slot_id = co_await task;
      if (is_lead) {
        ongoing.erase(it);
        slots[slot_id].lock_count--;
      }
      co_return slot_id;

      //wutong::SharedTask<slot_id_t> shared_op_task = evict_and_load(page_id, is_new);
      //slot_id_t slot_id = co_await shared_op_task;
      //co_return slot_id;
    }

    wutong::SharedTask<slot_id_t> evict_and_load(page_id_t page_id,
                                                 bool is_new = false) {
      slot_id_t slot_id = get_lru_k();
      auto &slot = slots[slot_id];
#ifdef PMA_DEBUG
      std::cout<<"Evict slot:"<<slot_id<<" which stores:"<<slot.buffer_page_id<<" for page:"<<page_id<<std::endl;
#endif
      co_await evict_page(slot_id);
      if (!is_new) {
#ifdef PMA_DEBUG
        memset(buffer[slot_id], 'X', PAGE_SIZE);
#endif
#ifdef PMA_DEBUG
        std::cout<<"Sending read request in evict_and_load,slot id:"<<slot_id<<" page_id:"<<page_id<<std::endl;
#endif
        bool success = co_await fmemory.async_read(buffer[slot_id], PAGE_SIZE,
                                                   page_id * PAGE_SIZE);
#ifdef PMA_DEBUG
        std::cout<<"Finish read request in evict_and_load,slot id:"<<slot_id<<" page_id:"<<page_id<<std::endl;
#endif
        assert(success);
      }
      // metadata
      slot.Reset();
      slot.buffer_page_id = page_id;
      page_table.insert({page_id, slot_id});

      slot.lock_count++;

      co_return slot_id;
    }

    void update_history(const slot_id_t slot_id) {
      auto &slot = slots[slot_id];
#ifdef PMA_DEBUG
      std::cout<<"Updating slot:"<<slot_id<<std::endl;
#endif
      slot.history.insert(time_stamp);
      ++time_stamp;
    }

    void lock_slot(const slot_id_t slot_id) {
      auto &slot = slots[slot_id];
#ifdef PMA_DEBUG
      std::cout<<"Locking slot:"<<slot_id<<"pin:"<<slot.lock_count<<std::endl;
#endif

      if (++slot.lock_count == 1 && !slot.history.empty()) {
        if(slot.it!=decltype(slot.it){}) {
          evict_order.erase(slot.it);
          slot.it = {};
        }else {
          //std::cerr<<"WARNING: locking slot with empty iterator, slot id:"<<slot_id<<std::endl;
        }
      };
    }

    void unlock_slot(const slot_id_t slot_id) {
      auto &slot = slots[slot_id];
#ifdef PMA_DEBUG
      std::cout<<"Unlocking slot:"<<slot_id<<"pin:"<<slot.lock_count<<std::endl;
#endif
      if (--slot.lock_count == 0) {
        assert(slot.it == decltype(slot.it){});
        slot.it = evict_order.insert({slot.get_timestamp(), slot_id}).first;
#ifdef PMA_DEBUG
        std::cout<<"Releasing slot:"<<slot_id<<std::endl;
#endif
      };
    }

    void remove_page_from_buffer(page_id_t page_id) {
      if (page_id == static_cast<page_id_t>(-1))
        return;

      slot_id_t slot_id = find_page_id_in_buffer(page_id);

      if (slot_id != static_cast<slot_id_t>(-1)) {
        SlotHeader &slot = slots[slot_id];
        lock_slot(slot_id);
        slot.is_dirty = false;
        page_table.erase(page_id);

        slot.buffer_page_id = static_cast<page_id_t>(-1);
        slot.history.clear();
        unlock_slot(slot_id);
      }
    }

    void double_space() {
      max_page_size_ *= 2;
      std::filesystem::resize_file(PMEM_FILE_NAME, max_page_size_ * PAGE_SIZE);
    }

    /**
     * @class GarbageCollector
     * @brief A helper class to collect deallocated pages.
     */
    class GarbageCollector {
      sjtu::vector<page_id_t> garbage;

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
     * @struct HandledReference
     * @brief A reference to the data held by Handle.
     */
    template <typename T> struct HandledReference {
    private:
      PersistentMemoryAsync *pmem_ptr_;
      slot_id_t slot_id_;
      page_id_t page_id_;

      friend struct Handle<T>;
      friend struct MutableHandle;

      // Private constructor, to be called by Handle's async methods
      HandledReference(PersistentMemoryAsync &pmem, slot_id_t sid,
                       page_id_t pid)
          : pmem_ptr_(&pmem), slot_id_(sid), page_id_(pid) {
        assert(pmem_ptr_ != nullptr && "PMA pointer is null in HandledReference constructor");
        assert(slot_id_ != static_cast<slot_id_t>(-1) && "Invalid slot_id in HandledReference constructor");
        assert(page_id_ != static_cast<page_id_t>(-1) && "Invalid page_id in HandledReference constructor");
        // CRITICAL ASSERTION: The slot's metadata must match the page_id this reference is for.
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_ && "PMA: Slot metadata page_id mismatch with handle's page_id at mutable ref creation");

        pmem_ptr_->lock_slot(slot_id_);
        pmem_ptr_->update_history(slot_id_);
        pmem_ptr_->slots[slot_id_].is_dirty = true;
      }

    public:
      // Rule of 5/0: Make non-copyable, but movable for returning from tasks.
      HandledReference(const HandledReference &) = delete;
      HandledReference &operator=(const HandledReference &) = delete;

      HandledReference(HandledReference &&other) noexcept
          : pmem_ptr_(other.pmem_ptr_), slot_id_(other.slot_id_),
            page_id_(other.page_id_) {
        other.pmem_ptr_ = nullptr;
        other.slot_id_ = -1;
        other.page_id_ = -1;
      }

      HandledReference &operator=(HandledReference &&other) noexcept {
        if (this != &other) {
          Drop();
          pmem_ptr_ = other.pmem_ptr_;
          slot_id_ = other.slot_id_;
          page_id_ = other.page_id_;
          // Invalidate moved-from object
          other.pmem_ptr_ = nullptr;
          other.slot_id_ = -1;
          other.page_id_ = -1;
        }
        return *this;
      }

      ~HandledReference() { Drop(); }

      T *operator->() const {
        assert(pmem_ptr_ && slot_id_ != static_cast<slot_id_t>(-1));
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_);
        return reinterpret_cast<T *>(pmem_ptr_->buffer[slot_id_]);
      }

      T &operator*() const {
        assert(pmem_ptr_ && slot_id_ != static_cast<slot_id_t>(-1));
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_);
        return *reinterpret_cast<T *>(pmem_ptr_->buffer[slot_id_]);
      }

      T *as_raw_ptr() const {
        assert(pmem_ptr_ && slot_id_ != static_cast<slot_id_t>(-1));
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_);
        return reinterpret_cast<T *>(pmem_ptr_->buffer[slot_id_]);
      }

      void Drop() {
        if (!pmem_ptr_ || slot_id_ == -1) {
          return;
        }
        pmem_ptr_->unlock_slot(slot_id_);
        pmem_ptr_ = nullptr;
        slot_id_ = -1;
        page_id_ = -1;
      }
    };

    /**
     * @struct ConstHandledReference
     * @brief A const reference to the data held by Handle.
     */
    template <typename T> struct ConstHandledReference {
    private:
      PersistentMemoryAsync *pmem_ptr_; // Pointer to the manager instance
      slot_id_t slot_id_;
      page_id_t page_id_; // Store for assertions/debugging

      friend struct PersistentMemoryAsync::Handle<T>;
      friend struct PersistentMemoryAsync::MutableHandle;

      ConstHandledReference(PersistentMemoryAsync &pmem, slot_id_t sid,
                            page_id_t pid)
          : pmem_ptr_(&pmem), slot_id_(sid), page_id_(pid) {
        assert(pmem_ptr_ != nullptr && "PMA pointer is null in ConstHandledReference constructor");
        assert(slot_id_ != static_cast<slot_id_t>(-1) && "Invalid slot_id in ConstHandledReference constructor");
        assert(page_id_ != static_cast<page_id_t>(-1) && "Invalid page_id in ConstHandledReference constructor");
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_ && "PMA: Slot metadata page_id mismatch with handle's page_id at const_ref creation");

        pmem_ptr_->lock_slot(slot_id_);
        pmem_ptr_->update_history(slot_id_);
      }

    public:
      // Rule of 5/0: Make non-copyable, but movable for returning from tasks.
      ConstHandledReference(const ConstHandledReference &) = delete;
      ConstHandledReference &operator=(const ConstHandledReference &) = delete;

      ConstHandledReference(ConstHandledReference &&other) noexcept
          : pmem_ptr_(other.pmem_ptr_), slot_id_(other.slot_id_),
            page_id_(other.page_id_) {
        other.pmem_ptr_ = nullptr;
        other.slot_id_ = -1;
        other.page_id_ = -1;
      }

      ConstHandledReference &operator=(ConstHandledReference &&other) noexcept {
        if (this != &other) {
          Drop();
          // Pilfer other's resources
          pmem_ptr_ = other.pmem_ptr_;
          slot_id_ = other.slot_id_;
          page_id_ = other.page_id_;
          // Invalidate moved-from object
          other.pmem_ptr_ = nullptr;
          other.slot_id_ = -1;
          other.page_id_ = -1;
        }
        return *this;
      }

      ~ConstHandledReference() { Drop(); }

      const T *operator->() const {
        assert(pmem_ptr_ && slot_id_ != static_cast<slot_id_t>(-1));
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_);
        return reinterpret_cast<T *>(pmem_ptr_->buffer[slot_id_]);
      }

      const T &operator*() const {
        assert(pmem_ptr_ && slot_id_ != static_cast<slot_id_t>(-1));
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_);
        return *reinterpret_cast<T *>(pmem_ptr_->buffer[slot_id_]);
      }

      const T *as_raw_ptr() const {
        assert(pmem_ptr_ && slot_id_ != static_cast<slot_id_t>(-1));
        assert(pmem_ptr_->slots[slot_id_].buffer_page_id == page_id_);
        return reinterpret_cast<T *>(pmem_ptr_->buffer[slot_id_]);
      }

      void Drop() {
        if (!pmem_ptr_ || slot_id_ == -1) {
          return;
        }
        pmem_ptr_->unlock_slot(slot_id_);
        pmem_ptr_ = nullptr;
        slot_id_ = -1;
        page_id_ = -1;
      }
    };

    explicit PersistentMemoryAsync(const std::string &path)
        : fmemory(path, SLOT_COUNT) {
      // create the file if it does not exist
      filesystem::fassert(path);
      filesystem::fassert(path + ".config");
      fconfig.open(path + ".config",
                   std::ios::in | std::ios::out | std::ios::binary);
      // if the config file is not empty, read the contents
      if (!filesystem::is_empty(fconfig)) {
        // get the config contents
        fconfig.seekg(0, std::ios::beg);
        pma_config temp;
        filesystem::binary_read(fconfig, temp);
        current_pages_in_disk = temp.current_pages_in_disk;
        max_page_size_ = temp.max_page_size_;
      } else {
        current_pages_in_disk = 0;
        max_page_size_ = 16;
        std::filesystem::resize_file(PMEM_FILE_NAME, (16 * PAGE_SIZE));
      }
      // file open finished
      for (slot_id_t i = 0; i < SLOT_COUNT; ++i) {
        slots[i].slot_id = i;
        slots[i].Reset();
        slots[i].it = evict_order.insert({{0, (time_stamp_t)i}, i})
                          .first;
      }
      time_stamp=SLOT_COUNT;
      sjtu::vector<void *> buffers = {};
      for (auto & i : buffer) {
        buffers.push_back(&i);
      }
      //fmemory.register_buffers(buffers);
    }
    explicit PersistentMemoryAsync(const char *path)
        : PersistentMemoryAsync(std::string(path)) {}

    wutong::Task<void> flush_all() {
      // This task will collect all IOAwaitables for the writes.
      sjtu::vector<IOAwaitable> write_tasks;

      for (auto & slot : slots) {
        if (slot.buffer_page_id != static_cast<page_id_t>(-1) &&
            slot.is_dirty) {
          write_tasks.push_back(
              fmemory.async_write(buffer[slot.slot_id], PAGE_SIZE,
                                  slot.buffer_page_id * PAGE_SIZE));
          slot.is_dirty = false;
        }
      }
      if (!write_tasks.empty()) {
        for (auto &io: write_tasks) {
          bool success = co_await io;
          assert(success);
        }
      }

      fconfig.seekp(0, std::ios::beg);
      pma_config temp{current_pages_in_disk, max_page_size_};
      filesystem::binary_write(fconfig, temp);
      co_return;
    }


    //should not be called inside a coroutine. BEST PRACTICE: call only in the main event loop
    void poll() {
      fmemory.handle_completions();
    }

    // should always manually call flush_all before destruction, it doesn't
    // automatically flush for some reason
    ~PersistentMemoryAsync() {
      fconfig.seekp(0, std::ios::beg);
      pma_config temp{current_pages_in_disk, max_page_size_};
      filesystem::binary_write(fconfig, temp);
      fconfig.close();
    }

  public:
    /**
     * @struct Handle
     * @brief A persistent pointer to a stored data.
     * @details A FileManager::handle is a serializable object that acts like
     * a persistent pointer. It is not called 'iterator' because it is not
     * iterable.
     */
    template <typename T>
    // using T = int;
    struct Handle {
      page_id_t page_id;

      explicit Handle(const page_id_t &page_id = static_cast<page_id_t>(-1))
          : page_id(page_id) {}

      /**
       * @brief Retrieve a read-write reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      [[nodiscard]] wutong::Task<HandledReference<T>>
      ref(bool is_initializing = false) const {
        assert(!is_nullptr());
        auto &pmem = get_instance();
        slot_id_t slot_id =
            co_await pmem.acquire_page_in_slot(page_id, is_initializing);
        co_return HandledReference<T>(pmem, slot_id, page_id);
      }

      /**
       * @brief Retrieve a read-only reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      [[nodiscard]] wutong::Task<ConstHandledReference<T>> const_ref() const {
        assert(!is_nullptr());
        auto &pmem = get_instance();
        slot_id_t slot_id = co_await pmem.acquire_page_in_slot(page_id, false);
        co_return ConstHandledReference<T>(pmem, slot_id, page_id);
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

      explicit
      MutableHandle(const page_id_t &page_id = static_cast<page_id_t>(-1))
          : page_id(page_id) {}

      /**
       * @brief Retrieve a read-write reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      template <typename T>
      [[nodiscard]] wutong::Task<HandledReference<T>>
      ref(bool is_initializing = false) const {
        assert(!is_nullptr());
        auto &pmem = PersistentMemoryAsync::get_instance();
        slot_id_t slot_id =
            co_await pmem.acquire_page_in_slot(page_id, is_initializing);
        co_return HandledReference<T>(pmem, slot_id, page_id);
      }

      /**
       * @brief Retrieve a read-only reference to the chunk of persistent
       * memory.
       * @return The handled reference to the handle.
       */
      template <typename T>
      [[nodiscard]] wutong::Task<ConstHandledReference<T>> const_ref() const {
        assert(!is_nullptr());
        auto &pmem = PersistentMemoryAsync::get_instance();
        // For const_ref, is_initializing is typically false.
        slot_id_t slot_id = co_await pmem.acquire_page_in_slot(page_id, false);
        co_return ConstHandledReference<T>(pmem, slot_id, page_id);
      }

      [[nodiscard]] bool is_nullptr() const {
        return page_id == static_cast<page_id_t>(-1);
      }

      void set_nullptr() { page_id = static_cast<page_id_t>(-1); }
    };

    /**
     * @brief Obtain a handle from the memory.
     * @tparam T The type of variable to store.
     * @return A handle to the variable stored in PersistentMemoryAsync.
     */
    template <typename T> [[nodiscard]] static Handle<T> create() {
      auto &pmem = get_instance();
      page_id_t new_page_id = pmem.garbage_collector.recycle();
      if (new_page_id == static_cast<page_id_t>(-1)) { // No page from GC
        new_page_id = pmem.current_pages_in_disk++;
        if (new_page_id >= pmem.max_page_size_ - 2) {
          pmem.double_space();
        }
      }
      return Handle<T>(new_page_id);
    }

    template <typename T, typename... Args>
    [[nodiscard]] static wutong::Task<Handle<T>>
    create_and_init(Args &&...args) {
      const auto handle = create<T>();
      HandledReference<T> temp_ref = co_await handle.ref(true);
      new (temp_ref.operator->()) T(std::forward<Args>(args)...);
      co_return handle;
    }

    [[nodiscard]] static MutableHandle create_mutable() {
      auto &pmem = get_instance();
      page_id_t new_page_id = pmem.garbage_collector.recycle();
      if (new_page_id == static_cast<page_id_t>(-1)) {
        new_page_id = pmem.current_pages_in_disk++;
        if (new_page_id >= pmem.max_page_size_ - 2) {
          pmem.double_space();
        }
      }
      return MutableHandle(new_page_id);
    }

    template <typename T, typename... Args>
    [[nodiscard]] static wutong::Task<MutableHandle>
    create_mutable_and_init(Args &&...args) {
      const auto handle = create_mutable();
      HandledReference<T> temp_ref = co_await handle.ref<T>(true);
      new (temp_ref.operator->()) T(std::forward<Args>(args)...);
      co_return handle;
    }

    template <typename T>
    [[nodiscard]] static Handle<T> fetch_handle(const page_id_t &page_id) {
      assert(page_id < get_instance().current_pages_in_disk);
      return Handle<T>(page_id);
    }

    [[nodiscard]] static MutableHandle
    fetch_mutable_handle(const page_id_t &page_id) {
      assert(page_id < get_instance().current_pages_in_disk);
      return MutableHandle(page_id);
    }

    template <typename T>
    static wutong::Task<void> remove(const Handle<T> &handle) {
      if (handle.is_nullptr()) {
        co_return;
      }
      auto &pmem = get_instance();
      page_id_t page_to_remove = handle.page_id;
      pmem.remove_page_from_buffer(page_to_remove);
      pmem.garbage_collector.dump(page_to_remove);
      co_return;
    }

    static wutong::Task<void> remove(const MutableHandle &handle) {
      if (handle.is_nullptr()) {
        co_return;
      }

      auto &pmem = get_instance();
      page_id_t page_to_remove = handle.page_id;
      pmem.remove_page_from_buffer(page_to_remove);
      pmem.garbage_collector.dump(page_to_remove);
      co_return;
    }

    static page_id_t get_page_count() {
      return get_instance().current_pages_in_disk;
    }
  };
} // namespace norb