#pragma once
#include "exceptions.hpp"

#include <climits>
#include <cstddef>

#include <iostream>

namespace sjtu {
  /**
   * a data container like std::vector
   * store data in a successive memory and support random access.
   */
  template<typename T>
  class vector {
  private:
    T *data;
    int capacity;
    int _size;
    static constexpr int SIZE = sizeof(T);
    static constexpr int DEFAULT_SIZE = 16;
    static constexpr float EXPAND_RATE = 2;
    static constexpr float SHRINK_RATE = 0.25;


    void free_resource() {
      for (int i = 0; i < _size; ++i) {
        data[i].~T();
      }
      operator delete(data);
    }

    void check_expand() {
      if (_size == capacity) {
        capacity = int(EXPAND_RATE * capacity);
        T *new_data = static_cast<T *>(operator new(capacity * SIZE));
        memcpy(new_data, data, _size * SIZE);

        operator delete(data);
        data = new_data;
      }
    }

    /*
    void check_shrink() {
      if(_size<=int(capacity*SHRINK_RATE)) {
        capacity=(int(capacity*SHRINK_RATE));
        T** new_data = new T*[capacity];
        for(int i=0;i<_size;++i) {
          new_data[i] = data[i];
        }
        delete[] data;
        data = new_data;
      }
    }
    */
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
      // Type traits is a C++ feature for describing certain properties of a type.
      // For instance, for an iterator, iterator::value_type is the type that the
      // iterator points to.
      // STL algorithms and containers may use these type_traits (e.g. the following
      // typedef) to work properly. In particular, without the following code,
      // @code{std::sort(iter, iter1);} would not compile.
      // See these websites for more information:
      // https://en.cppreference.com/w/cpp/header/type_traits
      // About value_type: https://blog.csdn.net/u014299153/article/details/72419713
      // About iterator_category: https://en.cppreference.com/w/cpp/iterator
    public:
      using difference_type = std::ptrdiff_t;
      using value_type = T;
      using pointer = T *;
      using reference = T &;
      using iterator_category = std::output_iterator_tag;

    private:
      int pos = -1;
      vector &parent;

      iterator(int pos, vector &parent): pos(pos), parent(parent) {
      }

      friend class vector;
      friend class const_iterator;

    public:
      /**
       * return a new iterator which pointer n-next elements
       * as well as operator-
       */
      iterator operator+(const int &n) const {
        iterator new_it(pos, parent);
        new_it += n;
        return new_it;
      }

      iterator operator-(const int &n) const {
        iterator new_it(pos, parent);
        new_it -= n;
        return new_it;
      }

      // return the distance between two iterators,
      // if these two iterators point to different vectors, throw invaild_iterator.
      int operator-(const iterator &rhs) const {
        if (&parent != &rhs.parent) {

          throw invalid_iterator();
        }
        return pos - rhs.pos;
      }

      iterator &operator+=(const int &n) {
        pos = pos + n;
        if (pos > parent._size) {
          pos = parent._size;
        }
        if (pos < 0) {
          pos = 0;
        }
        return *this;
      }

      iterator &operator-=(const int &n) {
        pos = pos - n;
        if (pos > parent._size) {
          pos = parent._size;
        }
        if (pos < 0) {
          pos = 0;
        }
        return *this;
      }

      iterator operator++(int) {
        iterator temp_it(pos, parent);
        *this += 1;
        return temp_it;
      }

      iterator &operator++() {
        *this += 1;
        return *this;
      }

      iterator operator--(int) {
        iterator temp_it(pos, parent);
        *this -= 1;
        return temp_it;
      }

      iterator &operator--() {
        *this += 1;
        return *this;
      }

      T &operator*() const {
        return parent[pos];
      }

      /**
       * a operator to check whether two iterators are same (pointing to the same memory address).
       */
      bool operator==(const iterator &rhs) const {
        return pos == rhs.pos && (&parent == &(rhs.parent));
      }

      bool operator==(const const_iterator &rhs) const {
        return pos == rhs.pos && (&parent == &(rhs.parent));
      }

      /**
       * some other operator for iterator.
       */
      bool operator!=(const iterator &rhs) const {
        return !(*this == rhs);
      }

      bool operator!=(const const_iterator &rhs) const {
        return !(*this == rhs);
      }
    };

    /**
     * has same function as iterator, just for a const object.
     */
    class const_iterator {
    public:
      using difference_type = std::ptrdiff_t;
      using value_type = T;
      using pointer = T *;
      using reference = T &;
      using iterator_category = std::output_iterator_tag;

    private:
      int pos = -1;
      const vector &parent;

      const_iterator(int pos, const vector &parent): pos(pos), parent(parent) {
      }

      friend class vector;
      friend class iterator;

    public:
      /**
       * return a new iterator which pointer n-next elements
       * as well as operator-
       */
      const_iterator operator+(const int &n) const {
        const_iterator new_it(pos, parent);
        new_it += n;
        return new_it;
      }

      const_iterator operator-(const int &n) const {
        const_iterator new_it(pos, parent);
        new_it -= n;
        return new_it;
      }

      // return the distance between two iterators,
      // if these two iterators point to different vectors, throw invaild_iterator.
      int operator-(const const_iterator &rhs) const {
        if (&parent != &rhs.parent) {

          throw invalid_iterator();
        }
        return pos - rhs.pos;
      }

      const_iterator &operator+=(const int &n) {
        if (pos + n > parent._size) {
          pos = parent._size;
        } else {
          pos = pos + n;
        }
        return *this;
      }

      const_iterator &operator-=(const int &n) {
        if (pos - n < 0) {
          pos = 0;
        } else {
          pos = pos - n;
        }
        return *this;
      }

      const_iterator operator++(int) {
        const_iterator temp_it = const_iterator(pos, parent);
        *this += 1;
        return temp_it;
      }

      const_iterator &operator++() {
        *this += 1;
        return *this;
      }

      const_iterator operator--(int) {
        const_iterator temp_it = const_iterator(pos, parent);
        *this -= 1;
        return temp_it;
      }

      const_iterator &operator--() {
        *this += 1;
        return *this;
      }

      const T &operator*() const {
        return parent[pos];
      }

      /**
       * a operator to check whether two iterators are same (pointing to the same memory address).
       */
      bool operator==(const iterator &rhs) const {
        return pos == rhs.pos && (&parent == &(rhs.parent));
      }

      bool operator==(const const_iterator &rhs) const {
        return pos == rhs.pos && (&parent == &(rhs.parent));
      }

      /**
       * some other operator for iterator.
       */
      bool operator!=(const iterator &rhs) const {
        return !(*this == rhs);
      }

      bool operator!=(const const_iterator &rhs) const {
        return !(*this == rhs);
      }
    };

    /**
     * At least two: default constructor, copy constructor
     */
    vector() {
      data = static_cast<T *>(operator new(DEFAULT_SIZE * SIZE));
      capacity = DEFAULT_SIZE;
      _size = 0;
    }

    vector(const vector &other) {
      data = static_cast<T *>(operator new(other.capacity * SIZE));
      capacity = other.capacity;
      _size = other.size();
      for (int i = 0; i < _size; ++i) {
        new(data + i)T(other[i]);
      }
    }

    vector(vector &&other)  noexcept {
      data = other.data;
      capacity = other.capacity;
      _size = other._size;

      other.data = static_cast<T *>(operator new(DEFAULT_SIZE * SIZE));
      other.capacity = DEFAULT_SIZE;
      other._size = 0;
    }

    /**
     */
    ~vector() {
      free_resource();
    }

    /**
     */
    vector &operator=(const vector &other) {
      if (this == &other) {
        return *this;
      }
      free_resource();

      capacity = other.capacity;
      _size = other.size();
      data = static_cast<T *>(operator new(SIZE * capacity));
      for (int i = 0; i < _size; ++i) {
        new(data + i) T(other[i]);
      }
      return *this;
    }

    /**
     * assigns specified element with bounds checking
     * throw index_out_of_bound if pos is not in [0, size)
     */
    T &at(const size_t &pos) {
      if (pos >= _size) {
        throw index_out_of_bound();
      }
      return data[pos];
    }

    const T &at(const size_t &pos) const {
      if (pos >= _size) {
        throw index_out_of_bound();
      }
      return data[pos];
    }

    /**
     * assigns specified element with bounds checking
     * throw index_out_of_bound if pos is not in [0, size)
     * !!! Pay attentions
     *   In STL this operator does not check the boundary but I want you to do.
     */
    T &operator[](const size_t &pos) {
      return at(pos);
    }

    const T &operator[](const size_t &pos) const {
      return at(pos);
    }

    /**
     * access the first element.
     * throw container_is_empty if size == 0
     */
    const T &front() const {
      if (_size == 0) {
        throw container_is_empty();
      }
      return data[0];
    }

    /**
     * access the last element.
     * throw container_is_empty if size == 0
     */
    const T &back() const {
      if (_size == 0) {
        throw container_is_empty();
      }
      return data[_size - 1];
    }
    T &back() {
      if (_size == 0) {
        throw container_is_empty();
      }
      return data[_size - 1];
    }

    /**
     * returns an iterator to the beginning.
     */
    iterator begin() {
      return iterator(0, *this);
    }

    const_iterator begin() const {
      return const_iterator(0, *this);
    }

    const_iterator cbegin() const {
      return const_iterator(0, *this);
    }

    /**
     * returns an iterator to the end.
     */
    iterator end() {
      return iterator(_size, *this);
    }

    const_iterator end() const {
      return const_iterator(_size, *this);
    }

    const_iterator cend() const {
      return const_iterator(_size, *this);
    }

    /**
     * checks whether the container is empty
     */
    bool empty() const {
      return _size == 0;
    }

    /**
     * returns the number of elements
     */
    size_t size() const {
      return _size;
    }

    /**
     * clears the contents
     */
    void clear() {
      for (int i = 0; i < _size; ++i) {
        data[i].~T();
      }
      _size = 0;
    }

    /**
     * inserts value before pos
     * returns an iterator pointing to the inserted value.
     */
    iterator insert(iterator pos, const T &value) {
      return insert(pos.pos, value);
    }

    /**
     * inserts value at index ind.
     * after inserting, this->at(ind) == value
     * returns an iterator pointing to the inserted value.
     * throw index_out_of_bound if ind > size (in this situation ind can be size because after inserting the size will increase 1.)
     */
    iterator insert(const size_t &ind, const T &value) {
      if (ind > _size) {
        throw index_out_of_bound();
      }
      _size += 1;
      check_expand();
      //for(int i=_size-2;i>=ind && i>=0;--i) {
      //	data[i+1]=data[i];
      //}
      memmove(data + ind + 1, data + ind, (_size - 1 - ind) * SIZE);
      new(data + ind) T(value);
      return iterator(ind, *this);
    }

    /**
     * removes the element at pos.
     * return an iterator pointing to the following element.
     * If the iterator pos refers the last element, the end() iterator is returned.
     */
    iterator erase(iterator pos) {
      return erase(pos.pos);
    }

    /**
     * removes the element with index ind.
     * return an iterator pointing to the following element.
     * throw index_out_of_bound if ind >= size
     */
    iterator erase(const size_t &ind) {
      if (ind >= _size) {
        throw index_out_of_bound();
      }
      _size -= 1;
      //check_shrink();
      data[ind].~T();

      memmove(data + ind, data + ind + 1, (_size - ind) * SIZE);
      return iterator(ind, *this);
    }

    /**
     * adds an element to the end.
     */
    void push_back(const T &value) {
      check_expand();
      new(data + _size) T(value);
      _size += 1;
    }

    void push_back(T &&value) {
      check_expand();
      new(data + _size) T(std::move(value));
      _size += 1;
    }

    template<typename... Args>
    void emplace_back(Args&&... args) {
      check_expand();
      new(data + _size) T(std::forward<Args>(args)...);
      _size += 1;
    }

    /**
     * remove the last element from the end.
     * throw container_is_empty if size() == 0
     */
    void pop_back() {
      if (_size == 0) {
        throw container_is_empty();
      }
      _size -= 1;
      data[_size].~T();
      //check_shrink();
    }
  };
}
