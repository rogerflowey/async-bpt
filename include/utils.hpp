#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include "shared.hpp"

namespace norb {

  namespace filesystem {
    // Check whether a fstream is empty.
    inline bool is_empty(std::fstream &f) {
      const std::streampos pos = f.tellg();
      // Change Note: using seek(0) on an empty file will disrupt the fstream f.
      f.seekg(0, std::ios::beg);
      const auto begin = f.tellg();
      f.seekg(0, std::ios::end);
      const auto end = f.tellg();
      f.seekg(pos);
      assert(f.good());
      return begin == end;
    }

    // Create file when it doesn't exist.
    // ! Can only be called when no other fstreams are pointing to that file !
    inline void fassert(const std::string &path) {
      std::fstream f;
      f.open(path, std::ios::app);
      f.close();
    }

    // Get the size of a file in bytes
    inline unsigned long get_size(const std::string &path) {
      std::fstream f(path, std::ios::in | std::ios::ate);
      return f.tellg();
    }

    // Binary read from a file.
    template <typename T_> void binary_read(std::fstream &f, T_ &item) {
      f.read(reinterpret_cast<char *>(&item), sizeof(item));
    }

    // Binary write into a file.
    template <typename T_> void binary_write(std::fstream &f, T_ &item) {
      f.write(reinterpret_cast<char *>(&item), sizeof(item));
    }

    // Binary write into a file.
    template <typename T_>
    void binary_write(std::fstream &f, T_ &item, const int size_) {
      f.write(reinterpret_cast<char *>(&item), size_);
    }

    // Clear all texts within a stream and reopen it.
    inline void trunc(std::fstream &f, const std::string &file_name,
                      const std::_Ios_Openmode mode) {
      f.close();
      remove(file_name.c_str());
      fassert(file_name);
      f.open(file_name, mode);
    }
  } // namespace filesystem

  namespace hash {
    // a collection of commonly used hashing algorithms
    using hashed_t_ = uint32_t;

    inline hashed_t_ basic_hash(const std::string &str) {
      constexpr hashed_t_ MOD = 4294967029;
      hashed_t_ hash = 0;
      for (auto i : str) {
        hash += static_cast<hashed_t_>(i);
        hash = (hash << 16) + hash;
        hash %= MOD;
      }
      return hash;
    }

    inline hashed_t_ fnv1a_hash(const std::string &str) {
      // FNV constants for 32-bit hash
      constexpr hashed_t_ fnv_prime = 16777619U;
      constexpr hashed_t_ fnv_offset_basis = 2166136261U;

      hashed_t_ hash = fnv_offset_basis;

      // Iterate over bytes as unsigned char
      for (unsigned char c : str) {
        hash ^= static_cast<hashed_t_>(c); // XOR with byte
        hash *= fnv_prime;                 // Multiply by prime
      }
      return hash;
    }

    inline hashed_t_ djb2_hash(const std::string &str) {
      hashed_t_ hash = 5381; // Initial magic constant
      // Iterate over bytes as unsigned char
      for (unsigned char c : str) {
        // hash = hash * 33 + c
        hash = ((hash << 5) + hash) + static_cast<hashed_t_>(c);
      }
      return hash;
    }
  } // namespace hash

  // simple array-related utils
  namespace array {
    template <typename T_>
    void insert_at(T_ *array, const size_t &array_size, const size_t &pos,
                   const T_ &new_val) {
      memmove(array + pos + 1, array + pos, sizeof(T_) * (array_size - pos));
      array[pos] = new_val;
    }

    template <typename T_>
    void remove_at(T_ *array, const size_t &array_size, const size_t &pos) {
      array[pos].~T_();
      memmove(array + pos, array + pos + 1,
              sizeof(T_) * (array_size - pos - 1));
      if constexpr (!std::is_trivially_destructible_v<T_>)
        memset(array + (array_size - 1), 0, sizeof(T_));
    }

    template <typename T_>
    void migrate(T_ *dest, T_ *src, const size_t &migrate_count) {
      memcpy(dest, src, sizeof(T_) * migrate_count);
      if constexpr (!std::is_trivially_destructible_v<T_>)
        memset(src, 0, sizeof(T_) * migrate_count);
    }

    template <typename T_>
    bool equals(const sjtu::vector<T_> &a, const sjtu::vector<T_> &b) {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
      }
      return true;
    }
  } // namespace array

  // unstructured stuff
  namespace chore {
    // Returns whether the three given variables are in ascending order,
    // non-strictly.
    template <typename T_> bool ascend(T_ begin, T_ val, T_ end) {
      return begin <= val && val <= end;
    }

    inline bool ascend(const char *begin, const char *val, const char *end) {
      return strcmp(begin, val) <= 0 && strcmp(val, end) <= 0;
    }

    inline void remove_associated() {
      std::remove(PMEM_FILE_NAME.c_str());
      std::remove((PMEM_FILE_NAME + ".config").c_str());
      std::remove("persistent.config");
    }
  }; // namespace chore
} // namespace norb

struct nothing{};