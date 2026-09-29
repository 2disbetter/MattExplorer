#pragma once

#include "base/types.h"

u32 inflate(const u8* in, u32 n, u8* out, u32 cap, bool zlib_header, char* err, u32 ecap);
