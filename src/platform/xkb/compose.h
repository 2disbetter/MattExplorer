#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

struct ComposeNode {
  u32 keysym;
  u32 lo, hi; // siblings with smaller / larger keysym (0 = none)
  u32 eq; // next node in the sequence (internal nodes)
  u32 utf8; // leaf: offset into ComposeTable::utf8 (0 = no string)
  u32 sym; // leaf: result keysym (0 = none)
};

struct ComposeTable {
  Array<ComposeNode> nodes; // [0] is a sentinel; the root's first node is 1
  Array<char>        utf8; // NUL-terminated results, offset 0 is ""
  u32                sequences = 0;
  char               source[256] = {};
};

enum ComposeStatus : u8 {
  COMPOSE_NOTHING, // keysym is not part of any sequence: handle it normally
  COMPOSE_COMPOSING, // swallowed, sequence in progress
  COMPOSE_COMPOSED, // swallowed, sequence complete: see compose_result
  COMPOSE_CANCELLED, // swallowed, sequence broken
};

struct ComposeState { u32 node = 0; bool composed = false; };

bool compose_load_locale(ComposeTable& t);

bool compose_load_file(ComposeTable& t, const char* path);
void compose_parse(ComposeTable& t, Str text, const char* locale_file);
void compose_clear(ComposeTable& t);

static inline bool compose_empty(const ComposeTable& t) { return t.nodes.len <= 1; }
static inline void compose_reset(ComposeState& s) { s.node = 0; s.composed = false; }
ComposeStatus compose_feed(const ComposeTable& t, ComposeState& s, u32 keysym);

const char* compose_result(const ComposeTable& t, const ComposeState& s, u32* sym);
