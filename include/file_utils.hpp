#pragma once

#include <cassert>
#include <cstring>
#include <fstream>

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

  // Returns whether the three given variables are in ascending order,
  // non-strictly.
  template <typename T_> bool ascend(T_ begin, T_ val, T_ end) {
    return begin <= val && val <= end;
  }

  inline bool ascend(const char *begin, const char *val, const char *end) {
    return strcmp(begin, val) <= 0 && strcmp(val, end) <= 0;
  }
} // namespace norb