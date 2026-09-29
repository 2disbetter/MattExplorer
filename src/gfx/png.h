#pragma once

#include "base/types.h"
#include "base/array.h"

struct Image {
  u32* px = nullptr; // premultiplied ARGB8888, w * h, malloc'ed
  i32  w = 0, h = 0;
};
void image_free(Image& img);

bool png_info(const u8* data, u32 n, i32* w, i32* h);

bool png_decode(const u8* data, u32 n, Image& out, u32 max_pixels, char* err, u32 ecap);

bool png_text(const u8* data, u32 n, const char* key, char* out, u32 cap);

void png_encode(const Image& img, const char* const* kv, u32 npairs, Array<u8>& out);

void image_scale_down(const Image& src, i32 max_w, i32 max_h, Image& dst);

void image_resample(const Image& src, i32 w, i32 h, Image& dst);

void image_orient(const Image& src, u32 orientation, Image& dst);
