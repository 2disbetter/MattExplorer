#pragma once
#include "core/dir_list.h"

enum SortKey : u8 { SORT_NAME = 0, SORT_SIZE, SORT_MTIME, SORT_KIND };

struct SortSpec {
  SortKey key        = SORT_NAME;
  bool    ascending  = true;
  bool    dirs_first = true;
};

int natural_compare(Str a, Str b);

void sort_listing(const DirListing& l, SortSpec spec, Array<u32>& order);
