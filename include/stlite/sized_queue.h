#pragma once

#include <exception>

template<typename T,size_t MAX_SIZE>
struct queue {
  T data[MAX_SIZE]{};
  int _front=0;
  int _back=0;

  static int prev(int n) {
    return n-1<0?n-1+MAX_SIZE:n-1;
  }
  static int next(int n) {
    return n+1>=MAX_SIZE?n+1-MAX_SIZE:n+1;
  }

  void push(const T& value) {
    data[_back] = value;
    _back = next(_back);
  }

  T pop() {
    T temp = data[_front];
    _front = next(_front);
    return temp;
  }
  [[nodiscard]] bool empty() const {
    return _back==_front;
  }
  [[nodiscard]] size_t size() const {
    long long temp = _back-_front;
    if(temp<0) {
      temp+=MAX_SIZE;
    }
    return temp;
  }
};
