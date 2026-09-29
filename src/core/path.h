#pragma once

#include "base/types.h"

void path_normalize(const char* in, char* out, u32 cap);

const char* path_base(const char* path);
