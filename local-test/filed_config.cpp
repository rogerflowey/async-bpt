#include "stlite/filed_config.hpp"
#include <iostream>

using norb::FiledConfig;

int main() {
  FiledConfig::set_file_path("./test.master.config");

  auto int_config = FiledConfig::track(5);
  struct Point {
    int x, y;
    Point(const int &x, const int &y) : x(x), y(y) {}
    bool operator==(const Point &other) const {
      return x == other.x && y == other.y;
    }
  };
  auto point_config = FiledConfig::track(Point{0, 0});
  // point_config.val = {1, 3};

  assert(int_config.val == 5);
  assert(point_config.val == Point(1, 3));
}