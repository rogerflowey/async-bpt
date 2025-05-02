#pragma once

#ifndef SJTU_VECTOR_HPP
#define SJTU_VECTOR_HPP

#include <climits>
#include <cstddef>
#include <stdexcept>
#include <cstring>

namespace norb {
  template <typename T> class vector {
  private:
    static constexpr size_t STARTUP_SIZE = 16;
    static constexpr float MULTIPLIER = 2.;

    T *base_ptr =
        nullptr; // The pointer that points to the beginning to the vector.
    size_t cur_size = 0; // The number of elements within the vector at present.
    size_t cur_size_bound =
        STARTUP_SIZE; // The upperbound that the vector can hold at present.

    static T *raw_new(const size_t &count) {
      const auto allocated =
          static_cast<T *>(operator new[](sizeof(T) * count));
      // Done is necessary for the mement. Slows down **dramatically**.
      // Done needed for operator = (will call destructor first)
      return allocated;
    }

    void guard_bound(const size_t &index) const {
      if (index < 0 || index >= cur_size) {
        throw std::out_of_range("index out of range");
      }
    }

    void destruct_within(const size_t &lower, const size_t &upper) {
      for (auto i = lower; i < upper; i++) {
        base_ptr[i].~T();
      }
    }

    /**
     * duplicate other_size number of T instances to the current base_ptr
     *
     * ! base_ptr must be able to contain the data
     */
    void duplicate_vector(const vector<T> &other) {
      base_ptr = raw_new(other.cur_size_bound);
      for (size_t i = 0; i < other.cur_size; i++) {
        base_ptr[i] = T(other.base_ptr[i]);
      }
      cur_size = other.cur_size;
      cur_size_bound = other.cur_size_bound;
    }

    /**
     * grows the current storage space by constant MULTIPLIER;
     * the data and current length are not affected.
     *
     * ! This function is called when the cur_size is NO LESS THAN
     * cur_size_bound - 1
     */
    void grow_space() {
      const auto new_size_bound =
          static_cast<size_t>(cur_size_bound * MULTIPLIER);
      T *new_base_ptr = raw_new(new_size_bound);
      // First move the old data to the new data.
      memcpy(new_base_ptr, base_ptr, sizeof(T) * cur_size_bound);
      // Next, delete the old space WITHOUT DESTRUCTING.
      operator delete[](base_ptr);
      // Finally, update the data.
      base_ptr = new_base_ptr;
      cur_size_bound = new_size_bound;
    }

  public:
    /**
     * a type for actions of the elements of a vector, and you should write
     *   a class named const_iterator with same interfaces.
     */
    /**
     * you can see RandomAccessIterator at CppReference for help.
     */
    class const_iterator;
    class iterator {
      // The following code is written for the C++ type_traits library.
      // Type traits is a C++ feature for describing certain properties of a
      // type. For instance, for an iterator, iterator::value_type is the type
      // that the iterator points to. STL algorithms and containers may use
      // these type_traits (e.g. the following typedef) to work properly. In
      // particular, without the following code,
      // @code{std::sort(iter, iter1);} would not compile.
      // See these websites for more information:
      // https://en.cppreference.com/w/cpp/header/type_traits
      // About value_type:
      // https://blog.csdn.net/u014299153/article/details/72419713 About
      // iterator_category: https://en.cppreference.com/w/cpp/iterator
    public:
      using difference_type = std::ptrdiff_t;
      using value_type = T;
      using pointer = T *;
      using reference = T &;
      using iterator_category = std::output_iterator_tag;

    private:
      vector<T> *ref_vector = nullptr;
      size_t index = 0;

      // void guard_not_null() const {
      // 	if (ref_vector == nullptr) {
      // 		throw internal_safeguard_failed();
      // 	}
      // }

      void guard_bound(const size_t &new_index) const {
        if (new_index >= ref_vector->cur_size || new_index < 0) {
          throw std::out_of_range("index out of bound");
        }
      }

    public:
      iterator(vector<T> *base_vector, const size_t n)
          : ref_vector(base_vector), index(n) {}

      /**
       * return a new iterator which pointer n-next elements
       * as well as operator-
       */
      iterator operator+(const int &n) const {
        // guard_bound(index + n);
        return iterator(ref_vector, n + index);
      }
      iterator operator-(const int &n) const {
        // guard_bound(index - n);
        return iterator(ref_vector, n - index);
      }
      // return the distance between two iterators,
      // if these two iterators point to different vectors, throw
      // invaild_iterator. DONE Check whether sign should be considered here.
      int operator-(const iterator &rhs) const {
        if (ref_vector != rhs.ref_vector) {
          throw std::exception();
        }
        // guard_not_null();
        return index - rhs.index;
      }
      iterator &operator+=(const int &n) {
        // guard_not_null();
        // guard_bound(index + n);
        index += n;
        return *this;
      }
      iterator &operator-=(const int &n) {
        // guard_not_null();
        // guard_bound(index - n);
        index -= n;
        return *this;
      }

      iterator operator++(int) { // iter++
        // guard_not_null();
        // guard_bound(index + 1);
        ++index;
        return iterator(ref_vector, index - 1);
      }
      iterator &operator++() { // ++iter
        // guard_not_null();
        // guard_bound(index + 1);
        ++index;
        return *this;
      }
      iterator operator--(int) { // iter--
        // guard_not_null();
        // guard_bound(index - 1);
        --index;
        return iterator(ref_vector, index + 1);
      }
      iterator &operator--() { // --iter
        // guard_not_null();
        // guard_bound(index - 1);
        --index;
        return *this;
      }

      T &operator*() const {
        // guard_not_null();
        return *(ref_vector->base_ptr + index);
      }
      /**
       * an operator to check whether two iterators are same (pointing to the
       * same memory address).
       */
      bool operator==(const iterator &rhs) const {
        return ref_vector == rhs.ref_vector && index == rhs.index;
      }
      bool operator==(const const_iterator &rhs) const {
        return ref_vector == rhs.ref_vector && index == rhs.index;
      }
      /**
       * some other operator for iterator.
       */
      bool operator!=(const iterator &rhs) const {
        return ref_vector != rhs.ref_vector || index != rhs.index;
      }
      bool operator!=(const const_iterator &rhs) const {
        return ref_vector != rhs.ref_vector || index != rhs.index;
      }
    };

    class const_iterator {
    public:
      using difference_type = std::ptrdiff_t;
      using value_type = T;
      using pointer = T *;
      using reference = T &;
      using iterator_category = std::output_iterator_tag;

    private:
      const vector<T> *ref_vector = nullptr;
      size_t index = 0;

      // void guard_not_null() const {
      // 	if (ref_vector == nullptr) {
      // 		throw internal_safeguard_failed();
      // 	}
      // }

      void guard_bound(const size_t &new_index) const {
        if (new_index >= ref_vector->cur_size || new_index < 0) {
          throw std::out_of_range("index out of bound");
        }
      }

    public:
      const_iterator(const vector<T> *base_vector, const size_t n)
          : ref_vector(base_vector), index(n) {}

      /**
       * return a new iterator which pointer n-next elements
       * as well as operator-
       */
      const_iterator operator+(const int &n) const {
        // guard_bound(index + n);
        return const_iterator(ref_vector, n + index);
      }
      const_iterator operator-(const int &n) const {
        // guard_bound(index - n);
        return const_iterator(ref_vector, n - index);
      }
      // return the distance between two iterators,
      // if these two iterators point to different vectors, throw
      // invaild_iterator. DONE Check whether sign should be considered here.
      int operator-(const const_iterator &rhs) const {
        if (ref_vector != rhs.ref_vector) {
          throw std::exception();
        }
        // guard_not_null();
        return index - rhs.index;
      }
      const_iterator &operator+=(const int &n) {
        // guard_not_null();
        // guard_bound(index + n);
        index += n;
        return *this;
      }
      const_iterator &operator-=(const int &n) {
        // guard_not_null();
        // guard_bound(index - n);
        index -= n;
        return *this;
      }

      const_iterator operator++(int) { // iter++
        // guard_not_null();
        // guard_bound(index + 1);
        ++index;
        return const_iterator(ref_vector, index - 1);
      }
      const_iterator &operator++() { // ++iter
        // guard_not_null();
        // guard_bound(index + 1);
        ++index;
        return *this;
      }
      const_iterator operator--(int) { // iter--
        // guard_not_null();
        // guard_bound(index - 1);
        --index;
        return const_iterator(ref_vector, index + 1);
      }
      const_iterator &operator--() { // --iter
        // guard_not_null();
        // guard_bound(index - 1);
        --index;
        return *this;
      }

      T operator*() const {
        // guard_not_null();
        return *(ref_vector->base_ptr + index);
      }
      /**
       * an operator to check whether two iterators are same (pointing to the
       * same memory address).
       */
      bool operator==(const iterator &rhs) const {
        return ref_vector == rhs.ref_vector && index == rhs.index;
      }
      bool operator==(const const_iterator &rhs) const {
        return ref_vector == rhs.ref_vector && index == rhs.index;
      }
      /**
       * some other operator for iterator.
       */
      bool operator!=(const iterator &rhs) const {
        return ref_vector != rhs.ref_vector || index != rhs.index;
      }
      bool operator!=(const const_iterator &rhs) const {
        return ref_vector != rhs.ref_vector || index != rhs.index;
      }
    };

    vector() { base_ptr = raw_new(cur_size_bound); }
    vector(const vector &other) { duplicate_vector(other); }

    ~vector() {
      // Invoke the destructors of corresponding elements stored.
      destruct_within(0, cur_size);
      operator delete[](base_ptr);
    }

    vector &operator=(const vector &other) {
      if (base_ptr == other.base_ptr)
        return *this;
      // The old space needs to be removed
      destruct_within(0, cur_size);
      operator delete[](base_ptr);
      // Use the same assignment as the copy constructor
      duplicate_vector(other);
      return *this;
    }
    /**
     * assigns specified element with bounds checking
     * throw index_out_of_bound if pos is not in [0, size)
     */
    T &at(const size_t &pos) {
      guard_bound(pos);
      return base_ptr[pos];
    }
    const T &at(const size_t &pos) const {
      guard_bound(pos);
      return base_ptr[pos];
    }
    /**
     * assigns specified element with bounds checking
     * throw index_out_of_bound if pos is not in [0, size)
     * !!! Pay attentions
     *   In STL this operator does not check the boundary but I want you to do.
     */
    T &operator[](const size_t &pos) {
      guard_bound(pos);
      return base_ptr[pos];
    }
    const T &operator[](const size_t &pos) const {
      guard_bound(pos);
      return base_ptr[pos];
    }
    /**
     * access the first element.
     * throw container_is_empty if size == 0
     */
    const T &front() const {
      if (cur_size == 0) {
        throw std::logic_error("empty vector");
      }
      return base_ptr[0];
    }
    /**
     * access the last element.
     * throw container_is_empty if size == 0
     */
    const T &back() const {
      if (cur_size == 0) {
        throw std::logic_error("empty vector");
      }
      return base_ptr[cur_size - 1];
    }
    /**
     * returns an iterator to the beginning.
     */
    iterator begin() { return iterator(this, 0); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator cbegin() const { return const_iterator(this, 0); }
    /**
     * returns an iterator to the end.
     */
    iterator end() { return iterator(this, cur_size); }
    const_iterator end() const { return const_iterator(this, cur_size); }
    const_iterator cend() const { return const_iterator(this, cur_size); }
    /**
     * checks whether the container is empty
     */
    bool empty() const { return cur_size == 0; }
    /**
     * returns the number of elements
     */
    size_t size() const { return cur_size; }
    /**
     * clears the contents
     */
    void clear() {
      // Manually free all the T-elements in the vector.
      destruct_within(0, cur_size);
      cur_size = 0;
      // The bound remains the same.
    }
    /**
     * inserts value before pos
     * returns an iterator pointing to the inserted value.
     */
    iterator insert(iterator pos, const T &value) {
      //! after insert, all iterators after and including the inserted point
      //! will fail!
      // ASSUME that pos is a valid iterator.
      const auto ind = pos - begin();
      // First shift the data right 1 unit.
      for (auto from = static_cast<long long>(cur_size - 1); from >= ind;
           --from) {
        // Copy the data from `from` to index `from + 1`.
        base_ptr[from + 1] = base_ptr[from];
      }
      // Then replace the data at pos.
      base_ptr[ind] = value;
      cur_size++;
      // If the length >= bound, grow the space.
      if (cur_size >= cur_size_bound) {
        grow_space();
      }
      return pos; // The iterator stays the same though.
    }
    /**
     * inserts value at index ind.
     * after inserting, this->at(ind) == value
     * returns an iterator pointing to the inserted value.
     * throw index_out_of_bound if ind > size (in this situation ind can be size
     * because after inserting the size will increase 1.)
     */
    iterator insert(const size_t &ind, const T &value) {
      if (ind < 0 || ind >= cur_size) {
        throw std::out_of_range("index out of bound");
        ;
      }
      // First shift the data right 1 unit.
      for (auto from = static_cast<long long>(cur_size - 1); from >= ind;
           --from) {
        // Copy the data from `from` to index `from + 1`.
        base_ptr[from + 1] = base_ptr[from];
      }
      // Then replace the data at ind.
      cur_size++;
      // If the length >= bound, then grow the space.
      if (cur_size >= cur_size_bound) {
        grow_space();
      }
      base_ptr[ind] = value;
      return iterator(this, ind);
    }
    /**
     * removes the element at pos.
     * return an iterator pointing to the following element.
     * If the iterator pos refers the last element, the end() iterator is
     * returned.
     */
    iterator erase(iterator pos) {
      const auto ind = pos - begin();
      for (size_t to = ind; to < cur_size - 1; ++to) {
        base_ptr[to] = base_ptr[to + 1];
      }
      cur_size--;
      return pos;
    }
    /**
     * removes the element with index ind.
     * return an iterator pointing to the following element.
     * throw index_out_of_bound if ind >= size
     */
    iterator erase(const size_t &ind) {
      for (size_t to = ind; to < cur_size - 1; ++to) {
        base_ptr[to] = base_ptr[to + 1];
      }
      cur_size--;
      return iterator(this, ind);
    }
    /**
     * adds an element to the end.
     */
    void push_back(const T &value) {
      base_ptr[cur_size++] = T(value);
      if (cur_size >= cur_size_bound) {
        grow_space();
      }
    }
    /**
     * remove the last element from the end.
     * throw container_is_empty if size() == 0
     */
    void pop_back() {
      if (cur_size == 0) {
        throw std::logic_error("empty vector");
      }
      base_ptr[--cur_size].~T();
    }
  };

} // namespace norb

#endif