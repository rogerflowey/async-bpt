#pragma once

namespace norb {
  /**
   * @brief A queue where new elements overwrite the oldest ones.
   * @tparam val_t_ The type of variable to hold.
   * @tparam capacity_ The capacity of the queue.
   */
  template <typename val_t_, unsigned int capacity_> class LoopedQueue {
  private:
    val_t_ q_[capacity_]{};
    int cur_ = 0;
    int size_ = 0;

  public:
    LoopedQueue() = default;
    [[nodiscard]] int size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    void insert(const val_t_ &val) {
      q_[cur_++] = val;
      cur_ %= capacity_;
      if (size_ < capacity_)
        ++size_;
    }
    const val_t_ &back() const {
      if (size_ < capacity_)
        return q_[0];
      return q_[(size_ - 1 + capacity_) % capacity_];
    }
  };
} // namespace norb