#pragma once

#include <compare>
#include <type_traits>
#include <utility>

namespace norb {
  /**
   * @class Pair
   * @brief A class template similar to std::pair.
   * @tparam first_t_ Type of the first element.
   * @tparam second_t_ Type of the second element.
   */
  template <typename first_t_, typename second_t_> class Pair {
  public:
    first_t_ first;
    second_t_ second;
    Pair() = default;
    Pair(first_t_ first, second_t_ second) : first(first), second(second) {}

    Pair(const Pair &other) = default;
    Pair(Pair &&other) = default;
    Pair &operator=(const Pair &other) = default;

    auto operator<=>(const Pair &other) const = default;
    bool operator==(const Pair &other) const = default;
  };

  template <typename first_t_, typename second_t_>
  constexpr Pair<std::decay_t<first_t_>, std::decay_t<second_t_>>
  make_pair(first_t_ &&first, second_t_ &&second) {
    return Pair<std::decay_t<first_t_>, std::decay_t<second_t_>>(
        std::forward<first_t_>(first), std::forward<second_t_>(second));
  }
} // namespace norb