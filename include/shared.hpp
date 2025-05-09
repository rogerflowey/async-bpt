#pragma once

#include <stdexcept>
#define NOT_IMPLEMENTED {throw std::runtime_error("Not implemented");}

// #define USE_SMALL_BATCH

namespace norb {
  using page_id_t = unsigned long;
  using slot_id_t = unsigned long;
  using page_size_t = unsigned long;
  using mem_size_t = unsigned long;

#ifdef USE_SMALL_BATCH
  constexpr page_size_t MEMORY_SIZE = 256 * 1024;
  constexpr page_size_t PAGE_SIZE = 256;
  constexpr page_id_t LRU_K_INDEX = 2;
  constexpr size_t OVERWRITE_BLOCK_SIZE = 8;
#else
  constexpr page_size_t MEMORY_SIZE = 4096 * 1248;
  constexpr page_size_t PAGE_SIZE = 4096;
  constexpr page_id_t LRU_K_INDEX = 20;
#endif

  const std::string PMEM_FILE_NAME = "persistent_memory.db";
} // namespace norb