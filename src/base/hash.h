#pragma once

#include "base/types.h"

u32  crc32_update(u32 crc, const u8* p, usize n); // start with 0; the running value is the plain CRC
u32  adler32_update(u32 adler, const u8* p, usize n); // start with 1

struct Md5 {
  u32 a, b, c, d;
  u64 len;
  u8  buf[64];
  u32 buf_len;
};
void md5_init(Md5& m);
void md5_update(Md5& m, const u8* p, usize n);
void md5_final(Md5& m, u8 digest[16]);

void md5_hex(const void* data, usize n, char out[33]);
