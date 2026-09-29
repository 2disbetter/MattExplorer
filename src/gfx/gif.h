#pragma once

#include "base/types.h"
#include "gfx/png.h"

bool gif_info(const u8* data, u32 n, i32* w, i32* h);
bool gif_decode(const u8* data, u32 n, Image& out, u32 max_pixels, char* err, u32 ecap);
