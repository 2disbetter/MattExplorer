#pragma once

#include "base/types.h"
#include <stdlib.h>
#include <string.h>

template <class T>
struct Array {
  T*  data = nullptr;
  u32 len  = 0;
  u32 cap  = 0;

  Array() = default;
  Array(const Array&) = delete;
  Array& operator=(const Array&) = delete;
  Array(Array&& o) noexcept : data(o.data), len(o.len), cap(o.cap) { o.data = nullptr; o.len = o.cap = 0; }
  Array& operator=(Array&& o) noexcept {
    if (this != &o) { free(data); data = o.data; len = o.len; cap = o.cap; o.data = nullptr; o.len = o.cap = 0; }
    return *this;
  }
  ~Array() { free(data); }

  T&       operator[](u32 i)       { MX_ASSERT(i < len); return data[i]; }
  const T& operator[](u32 i) const { MX_ASSERT(i < len); return data[i]; }
  T*       begin()       { return data; }
  T*       end()         { return data + len; }
  const T* begin() const { return data; }
  const T* end()   const { return data + len; }
  T&       last()        { MX_ASSERT(len); return data[len - 1]; }

  void reserve(u32 n) {
    if (n <= cap) return;
    u32 c = cap ? cap : 16;
    while (c < n) c *= 2;
    T* p = (T*)realloc(data, (usize)c * sizeof(T));
    MX_ASSERT(p);
    data = p;
    cap  = c;
  }
  void resize(u32 n)       { reserve(n); len = n; }
  void resize_zero(u32 n)  { u32 old = len; resize(n); if (n > old) memset(data + old, 0, (usize)(n - old) * sizeof(T)); }
  void push(const T& v)    { T tmp = v; if (len == cap) reserve(len + 1); data[len++] = tmp; } // tmp: v may alias data
  T*   push_n(u32 n)       { reserve(len + n); T* p = data + len; len += n; return p; }
  void pop()               { MX_ASSERT(len); len--; }
  void remove_at(u32 i) { for (u32 k = i; k + 1 < len; k++) data[k] = data[k + 1]; if (i < len) len--; } // keeps order
  void clear()             { len = 0; }
  void release()           { free(data); data = nullptr; len = cap = 0; }
  usize bytes() const      { return (usize)cap * sizeof(T); }
};
