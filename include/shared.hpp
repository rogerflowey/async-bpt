#pragma once

#include <stdexcept>
#define NOT_IMPLEMENTED {throw std::runtime_error("Not implemented");}

namespace norb {
  using page_id_t = unsigned long;
  using slot_id_t = unsigned long;
  using page_size_t = unsigned long;
  using mem_size_t = unsigned long;

  // constexpr page_size_t MEMORY_SIZE = 4096 * 1248;
  // constexpr page_size_t PAGE_SIZE = 4096;
  // constexpr page_id_t LRU_K_INDEX = 3;
  constexpr page_size_t MEMORY_SIZE = 64 * 3;
  constexpr page_size_t PAGE_SIZE = 64;
  constexpr page_id_t LRU_K_INDEX = 2;
  const std::string PMEM_FILE_NAME = "persistent_memory.db";
}