#pragma once

#include "core/dir_list.h"

int fuzzy_score(Str pattern, Str text, u16* positions);

struct FuzzyHit { u32 index; i32 score; };

void fuzzy_filter(const DirListing& l, Str pattern, Array<FuzzyHit>& out);
