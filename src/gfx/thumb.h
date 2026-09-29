#pragma once

#include "base/types.h"
#include "gfx/png.h"

enum { THUMB_NORMAL = 128, THUMB_LARGE = 256 };

u32  thumb_uri(const char* path, char* out, u32 cap);

bool thumb_cache_dir(char* out, u32 cap);

bool thumb_get(const char* path, i64 mtime, u64 size, u32 want, const char* app_dir, Image& out, char* err, u32 ecap, bool* from_cache = nullptr);

bool thumb_candidate_name(const char* name);
