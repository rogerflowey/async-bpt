#pragma once

#include "utils.hpp"
#include <exception>
#include <string>

// Namespace norb for Norb.
namespace norb {
  template <typename T_> class FiledNaiveList {
  public:
    explicit FiledNaiveList(const std::string &f_name_) {
      f_name = f_name_;
      filesystem::fassert(f_name);
      f_stream.open(f_name, file_open_mode);
      assert(f_stream.good());
      f_stream.seekg(0);
      if (!filesystem::is_empty(f_stream)) {
        // Read the size from the disk.
        filesystem::binary_read(f_stream, size_);
      } else {
        // Write a zero to the disk.
        f_stream.seekp(0);
        filesystem::binary_write(f_stream, size_);
      }
      assert(f_stream.good());
    }
    explicit FiledNaiveList(const char *f_name_): FiledNaiveList(std::string(f_name_)) {}

    ~FiledNaiveList() { f_stream.close(); }

    T_ get(const int index) const {
      assert(f_stream.good());
      if (index >= size_ || index < 0) {
        throw std::range_error("norb::FiledNaiveList: INDEX OUT OF RANGE!");
      }
      f_stream.seekg(getPos(index), std::ios::beg);
      T_ ret;
      filesystem::binary_read(f_stream, ret);
      assert(f_stream.good());
      return ret;
    }

    T_ set(const int index, T_ to) {
      assert(f_stream.good());
      if (index < 0) {
        throw std::range_error("norb::FiledNaiveList: INDEX OUT OF RANGE!");
      }
      if (index + 1 > size_) {
        size_ = std::max(size_, index + 1);
        f_stream.seekp(0);
        filesystem::binary_write(f_stream, size_);
      }
      f_stream.seekp(getPos(index), std::ios::beg);
      filesystem::binary_write(f_stream, to);
      assert(f_stream.good());
      return to;
    }

    int size() const { return size_; }

    T_ push_back(T_ to) {
      assert(f_stream.good());
      const auto ret = set(size_, to);
      assert(f_stream.good());
      return ret;
    }

  private:
    static constexpr auto file_open_mode =
        std::ios::binary | std::ios::in | std::ios::out;
    static constexpr int sizeof_t = sizeof(T_);

    int size_ = 0;
    std::string f_name;
    mutable std::fstream f_stream;

    int getPos(const int index) const { return sizeof(int) + sizeof_t * index; }
  };
} // namespace norb
