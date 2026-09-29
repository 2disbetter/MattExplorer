#pragma once

#include "base/types.h"
#include <stdlib.h>
#include <string.h>

struct Arena {
  struct Chunk {
    Chunk* next;
    usize  cap;
    usize  used;
    u8*    mem() { return (u8*)(this + 1); }
  };

  Chunk* head          = nullptr;
  usize  default_chunk = 256 * 1024;
  usize  total_used    = 0;

  Arena() = default;
  explicit Arena(usize chunk_size) : default_chunk(chunk_size) {}
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() { release(); }

  void* push(usize size, usize align = 8) {
    Chunk* c   = head;
    usize  off = c ? (c->used + align - 1) & ~(align - 1) : 0;
    if (!c || off + size > c->cap) {
      usize cap = mx_max(default_chunk, size + align);
      c = (Chunk*)malloc(sizeof(Chunk) + cap);
      MX_ASSERT(c);
      c->next = head; c->cap = cap; c->used = 0;
      head = c;
      off  = 0;
    }
    void* p = c->mem() + off;
    c->used = off + size;
    total_used += size;
    return p;
  }
  template <class T> T* push_array(usize n) { return (T*)push(n * sizeof(T), alignof(T)); }
  template <class T> T* push_zero(usize n)  { T* p = push_array<T>(n); memset(p, 0, n * sizeof(T)); return p; }

  char* push_str(const char* s, usize n) {
    char* p = (char*)push(n + 1, 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
  }

  void reset() {
    if (!head) return;
    Chunk* keep = head;
    Chunk* c    = head->next;
    while (c) { Chunk* n = c->next; free(c); c = n; }
    keep->next = nullptr;
    keep->used = 0;
    head       = keep;
    total_used = 0;
  }
  void release() {
    while (head) { Chunk* n = head->next; free(head); head = n; }
    total_used = 0;
  }
};
