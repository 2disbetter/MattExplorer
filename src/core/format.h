#pragma once

#include "base/types.h"

const char* fmt_size(u64 v, char* buf, u32 cap);

const char* fmt_time(i64 unix_ns, char* buf, u32 cap);

const char* fmt_count(u64 v, char* buf, u32 cap);
