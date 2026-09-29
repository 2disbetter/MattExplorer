#include "base/hash.h"
#include <string.h>

u32 crc32_update(u32 crc, const u8* p, usize n) {
  u32 t[256];
  for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
  crc = ~crc;
  for (usize i = 0; i < n; i++) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

u32 adler32_update(u32 adler, const u8* p, usize n) {
  u32 a = adler & 0xFFFF, b = adler >> 16;
  while (n) {
    usize k = n < 5552 ? n : 5552; // largest block before the sums can overflow
    n -= k;
    while (k--) { a += *p++; b += a; }
    a %= 65521; b %= 65521;
  }
  return (b << 16) | a;
}

static const u32 kMd5K[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const u8 kMd5R[64] = {
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static inline u32 rotl(u32 x, u32 c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(Md5& m, const u8* p) {
  u32 w[16];
  for (u32 i = 0; i < 16; i++) w[i] = (u32)p[4 * i] | ((u32)p[4 * i + 1] << 8) | ((u32)p[4 * i + 2] << 16) | ((u32)p[4 * i + 3] << 24);
  u32 a = m.a, b = m.b, c = m.c, d = m.d;
  for (u32 i = 0; i < 64; i++) {
    u32 f, g;
    if (i < 16)      { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
    else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
    u32 t = d; d = c; c = b;
    b = b + rotl(a + f + kMd5K[i] + w[g], kMd5R[i]);
    a = t;
  }
  m.a += a; m.b += b; m.c += c; m.d += d;
}

void md5_init(Md5& m) { m.a = 0x67452301; m.b = 0xefcdab89; m.c = 0x98badcfe; m.d = 0x10325476; m.len = 0; m.buf_len = 0; }

void md5_update(Md5& m, const u8* p, usize n) {
  m.len += n;
  if (m.buf_len) {
    usize take = n < 64 - m.buf_len ? n : 64 - m.buf_len;
    memcpy(m.buf + m.buf_len, p, take);
    m.buf_len += (u32)take; p += take; n -= take;
    if (m.buf_len < 64) return;
    md5_block(m, m.buf);
    m.buf_len = 0;
  }
  while (n >= 64) { md5_block(m, p); p += 64; n -= 64; }
  if (n) { memcpy(m.buf, p, n); m.buf_len = (u32)n; }
}

void md5_final(Md5& m, u8 digest[16]) {
  u64 bits = m.len * 8;
  u8 pad = 0x80;
  md5_update(m, &pad, 1);
  u8 zero = 0;
  while (m.buf_len != 56) md5_update(m, &zero, 1);
  u8 lenb[8];
  for (u32 i = 0; i < 8; i++) lenb[i] = (u8)(bits >> (8 * i));
  md5_update(m, lenb, 8);
  u32 v[4] = { m.a, m.b, m.c, m.d };
  for (u32 i = 0; i < 4; i++) for (u32 k = 0; k < 4; k++) digest[4 * i + k] = (u8)(v[i] >> (8 * k));
}

void md5_hex(const void* data, usize n, char out[33]) {
  Md5 m; md5_init(m); md5_update(m, (const u8*)data, n);
  u8 d[16]; md5_final(m, d);
  static const char hex[] = "0123456789abcdef";
  for (u32 i = 0; i < 16; i++) { out[2 * i] = hex[d[i] >> 4]; out[2 * i + 1] = hex[d[i] & 15]; }
  out[32] = 0;
}
