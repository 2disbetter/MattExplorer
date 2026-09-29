#include "gfx/inflate.h"
#include "base/hash.h"

#include <stdio.h>
#include <string.h>

namespace {

enum { FAST_BITS = 9, MAX_BITS = 15, MAX_SYMS = 288 };

struct Huff {
  u16 fast[1 << FAST_BITS]; // (symbol << 4) | length, 0 = look further
  u16 count[MAX_BITS + 1]; // codes of each length
  u16 symbol[MAX_SYMS]; // symbols in canonical order
  u16 first_code[MAX_BITS + 1], first_index[MAX_BITS + 1];
};

struct Bits {
  const u8* p; const u8* end;
  u64 acc; u32 n;
};
inline bool truncated(const Bits& b) { return b.p - b.n / 8 > b.end; }

inline void refill(Bits& b) {
  while (b.n <= 56) {
    u32 byte = b.p < b.end ? *b.p : 0;
    b.p++;
    b.acc |= (u64)byte << b.n;
    b.n += 8;
  }
}
inline u32 peek(Bits& b, u32 k) { if (b.n < k) refill(b); return (u32)(b.acc & ((1ull << k) - 1)); }
inline void drop(Bits& b, u32 k) { b.acc >>= k; b.n -= k; }
inline u32 bits(Bits& b, u32 k) { if (!k) return 0; u32 v = peek(b, k); drop(b, k); return v; }

bool huff_build(Huff& h, const u8* lengths, u32 n) {
  memset(h.count, 0, sizeof h.count);
  for (u32 i = 0; i < n; i++) h.count[lengths[i]]++;
  h.count[0] = 0;
  u32 left = 1;
  for (u32 len = 1; len <= MAX_BITS; len++) { left <<= 1; if (h.count[len] > left) return false; left -= h.count[len]; }
  u16 offs[MAX_BITS + 2]; offs[1] = 0;
  for (u32 len = 1; len < MAX_BITS; len++) offs[len + 1] = (u16)(offs[len] + h.count[len]);
  for (u32 i = 0; i < n; i++) if (lengths[i]) h.symbol[offs[lengths[i]]++] = (u16)i;

  u32 code = 0, index = 0;
  for (u32 len = 1; len <= MAX_BITS; len++) {
    h.first_code[len] = (u16)code; h.first_index[len] = (u16)index;
    code = (code + h.count[len]) << 1;
    index += h.count[len];
  }
  memset(h.fast, 0, sizeof h.fast);
  u32 idx = 0;
  for (u32 len = 1; len <= FAST_BITS; len++) {
    for (u32 k = 0; k < h.count[len]; k++, idx++) {
      u32 c = h.first_code[len] + k;
      u32 rev = 0; for (u32 i = 0; i < len; i++) rev |= ((c >> i) & 1) << (len - 1 - i);
      for (u32 fill = rev; fill < (1u << FAST_BITS); fill += 1u << len) h.fast[fill] = (u16)((h.symbol[idx] << 4) | len);
    }
  }
  return true;
}

inline i32 huff_decode(Bits& b, const Huff& h) {
  u32 v = peek(b, FAST_BITS);
  u16 e = h.fast[v];
  if (e) { drop(b, e & 15); return e >> 4; }

  u32 code = 0;
  for (u32 len = 1; len <= MAX_BITS; len++) {
    code = (code << 1) | bits(b, 1);
    i32 rel = (i32)code - (i32)h.first_code[len];
    if (rel >= 0 && rel < (i32)h.count[len]) return h.symbol[h.first_index[len] + rel];
  }
  return -1;
}

const u16 kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const u8  kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const u16 kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const u8  kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
const u8  kCodeOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

struct State { Bits b; u8* out; u32 cap; u32 pos; const char* err; };

bool inflate_block(State& s, const Huff& lit, const Huff& dist) {
  for (;;) {
    i32 sym = huff_decode(s.b, lit);
    if (sym < 0) { s.err = "bad literal/length code"; return false; }
    if (sym < 256) { if (s.pos >= s.cap) { s.err = "output buffer full"; return false; } s.out[s.pos++] = (u8)sym; continue; }
    if (sym == 256) return true;
    sym -= 257;
    if (sym >= 29) { s.err = "bad length code"; return false; }
    u32 len = kLenBase[sym] + bits(s.b, kLenExtra[sym]);
    i32 ds = huff_decode(s.b, dist);
    if (ds < 0 || ds >= 30) { s.err = "bad distance code"; return false; }
    u32 d = kDistBase[ds] + bits(s.b, kDistExtra[ds]);
    if (d > s.pos) { s.err = "distance before the start"; return false; }
    if (s.pos + len > s.cap) { s.err = "output buffer full"; return false; }
    u8* dst = s.out + s.pos; const u8* src = dst - d;
    if (d >= len) memcpy(dst, src, len);
    else for (u32 i = 0; i < len; i++) dst[i] = src[i]; // overlapping: byte by byte
    s.pos += len;
  }
}

bool inflate_fixed(State& s) {
  u8 lengths[MAX_SYMS];
  u32 i = 0;
  for (; i < 144; i++) lengths[i] = 8;
  for (; i < 256; i++) lengths[i] = 9;
  for (; i < 280; i++) lengths[i] = 7;
  for (; i < 288; i++) lengths[i] = 8;
  Huff lit, dist;
  huff_build(lit, lengths, 288);
  u8 dl[30]; memset(dl, 5, sizeof dl);
  huff_build(dist, dl, 30);
  return inflate_block(s, lit, dist);
}

bool inflate_dynamic(State& s) {
  u32 hlit = bits(s.b, 5) + 257, hdist = bits(s.b, 5) + 1, hclen = bits(s.b, 4) + 4;
  if (hlit > 286 || hdist > 30) { s.err = "bad table sizes"; return false; }
  u8 cl[19] = {};
  for (u32 i = 0; i < hclen; i++) cl[kCodeOrder[i]] = (u8)bits(s.b, 3);
  Huff code;
  if (!huff_build(code, cl, 19)) { s.err = "bad code-length table"; return false; }
  u8 lengths[MAX_SYMS + 32] = {};
  u32 n = 0, total = hlit + hdist;
  while (n < total) {
    i32 sym = huff_decode(s.b, code);
    if (sym < 0) { s.err = "bad code-length code"; return false; }
    if (sym < 16) { lengths[n++] = (u8)sym; continue; }
    u32 rep, val = 0;
    if (sym == 16) { if (!n) { s.err = "repeat with no previous length"; return false; } val = lengths[n - 1]; rep = 3 + bits(s.b, 2); }
    else if (sym == 17) rep = 3 + bits(s.b, 3);
    else rep = 11 + bits(s.b, 7);
    if (n + rep > total) { s.err = "too many code lengths"; return false; }
    while (rep--) lengths[n++] = (u8)val;
  }
  if (lengths[256] == 0) { s.err = "no end-of-block code"; return false; }
  Huff lit, dist;
  if (!huff_build(lit, lengths, hlit)) { s.err = "bad literal table"; return false; }
  if (!huff_build(dist, lengths + hlit, hdist)) { s.err = "bad distance table"; return false; }
  return inflate_block(s, lit, dist);
}

bool inflate_stored(State& s) {
  drop(s.b, s.b.n & 7); // to a byte boundary

  const u8* p = s.b.p - (s.b.n / 8);
  s.b.acc = 0; s.b.n = 0; s.b.p = p;
  if (s.b.end - s.b.p < 4) { s.err = "truncated stored block"; return false; }
  u32 len = s.b.p[0] | (s.b.p[1] << 8), nlen = s.b.p[2] | (s.b.p[3] << 8);
  s.b.p += 4;
  if ((len ^ 0xFFFF) != nlen) { s.err = "stored length check failed"; return false; }
  if ((u32)(s.b.end - s.b.p) < len) { s.err = "truncated stored block"; return false; }
  if (s.pos + len > s.cap) { s.err = "output buffer full"; return false; }
  memcpy(s.out + s.pos, s.b.p, len);
  s.pos += len; s.b.p += len;
  return true;
}

} // namespace

u32 inflate(const u8* in, u32 n, u8* out, u32 cap, bool zlib_header, char* err, u32 ecap) {
  State s = {};
  s.out = out; s.cap = cap; s.pos = 0; s.err = nullptr;
  const u8* p = in; const u8* end = in + n;
  if (zlib_header) {
    if (n < 6) { snprintf(err, ecap, "zlib stream too short"); return 0; }
    u32 cmf = p[0], flg = p[1];
    if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 != 0) { snprintf(err, ecap, "not a zlib stream"); return 0; }
    if (flg & 0x20) { snprintf(err, ecap, "preset dictionary not supported"); return 0; }
    p += 2; end -= 4; // the Adler-32 trailer
  }
  s.b.p = p; s.b.end = end; s.b.acc = 0; s.b.n = 0;
  for (;;) {
    u32 last = bits(s.b, 1), type = bits(s.b, 2);
    bool ok = type == 0 ? inflate_stored(s) : type == 1 ? inflate_fixed(s) : type == 2 ? inflate_dynamic(s) : false;
    if (!ok) { snprintf(err, ecap, "%s", s.err ? s.err : "bad block type"); return 0; }
    if (truncated(s.b)) { snprintf(err, ecap, "input truncated"); return 0; }
    if (last) break;
  }
  if (zlib_header) {
    u32 want = ((u32)end[0] << 24) | ((u32)end[1] << 16) | ((u32)end[2] << 8) | end[3];
    if (adler32_update(1, out, s.pos) != want) { snprintf(err, ecap, "Adler-32 mismatch"); return 0; }
  }
  return s.pos;
}
