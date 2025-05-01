#pragma once

#include <file_utils.hpp>
#include <iostream>
#include <string>

namespace norb {
  /**
   * @class FiledConfig
   * @brief An RAII helper class for storing unstructured config data.
   */
  class FiledConfig {
  private:
    using pos_t_ = unsigned long;
    template <typename val_t_> struct RAII_Tracker;

    static FiledConfig &get_instance() {
      static FiledConfig instance;
      return instance;
    }

    static std::string file_path;
    std::fstream fconfig;
    bool write_only;
    pos_t_ global_cur = 0;

    FiledConfig() {
      filesystem::fassert(file_path);
      fconfig = std::fstream(file_path,
                             std::ios::binary | std::ios::in | std::ios::out);
      write_only = filesystem::is_empty(fconfig);
    }
    ~FiledConfig() { fconfig.close(); }

    template <typename val_t_> struct RAII_Tracker {
    private:
      pos_t_ cur;

    public:
      val_t_ val;
      explicit RAII_Tracker(const val_t_ &default_value) : val(default_value) {
        auto &singleton = get_instance();
        cur = singleton.global_cur;
        singleton.global_cur += sizeof(pos_t_);
        if (!singleton.write_only) {
          // read from config
          singleton.fconfig.seekg(cur);
          filesystem::binary_read(singleton.fconfig, val);
        }
        assert(singleton.fconfig.good());
      }

      ~RAII_Tracker() {
        auto &singleton = get_instance();
        singleton.fconfig.seekp(cur);
        filesystem::binary_write(singleton.fconfig, val);
      }
    };

  public:
    static void set_file_path(const std::string &path) {
      file_path = path;
    }

    template <typename val_t_>
    static RAII_Tracker<val_t_> track(const val_t_ &default_value) {
      return RAII_Tracker<val_t_>(default_value);
    }
  };

  inline std::string FiledConfig::file_path = "./default.config";
} // namespace norb
