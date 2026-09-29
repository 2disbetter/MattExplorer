#pragma once

#include "base/types.h"
#include "gfx/png.h"

bool jpeg_info(const u8* data, u32 n, i32* w, i32* h, bool* progressive, u32* orientation);

bool jpeg_decode(const u8* data, u32 n, Image& out, u32 denom, u32 max_pixels, char* err, u32 ecap);
