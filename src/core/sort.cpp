#include "core/sort.h"
#include "base/algo.h"

int natural_compare(Str a, Str b) {
  u32 i = 0, j = 0;
  while (i < a.n && j < b.n) {
    char ca = a.p[i], cb = b.p[j];

    if (is_digit(ca) && is_digit(cb)) {
      while (i < a.n && a.p[i] == '0') i++; // leading zeros don't count
      while (j < b.n && b.p[j] == '0') j++;
      u32 si = i; while (i < a.n && is_digit(a.p[i])) i++;
      u32 sj = j; while (j < b.n && is_digit(b.p[j])) j++;
      u32 la = i - si, lb = j - sj;
      if (la != lb) return la < lb ? -1 : 1; // more digits = bigger number
      int r = memcmp(a.p + si, b.p + sj, la);
      if (r) return r < 0 ? -1 : 1;
      continue;
    }

    u8 la = (u8)ascii_lower(ca), lb = (u8)ascii_lower(cb);
    if (la != lb) return la < lb ? -1 : 1;
    i++; j++;
  }
  if (i < a.n) return 1;
  if (j < b.n) return -1;
  return str_cmp(a, b); // total order: "01" vs "1"
}

struct KeyTable {
  Array<char> bytes;
  Array<u32>  off;
  Array<u16>  len;
};

static void build_key(Str s, Array<char>& out) {
  u32 i = 0;
  while (i < s.n) {
    char c = s.p[i];
    if (is_digit(c)) {
      while (i < s.n && s.p[i] == '0') i++;
      u32 st = i;
      while (i < s.n && is_digit(s.p[i])) i++;
      u32   n = i - st; // <= NAME_MAX, fits a byte
      char* d = out.push_n(2 + n);
      d[0] = '0';
      d[1] = (char)(u8)n;
      memcpy(d + 2, s.p + st, n);
    } else {
      out.push(ascii_lower(c));
      i++;
    }
  }
}

static inline u64 be_prefix(const char* p, u32 n, u32 at = 0) {
  u64 v = 0;
  for (u32 i = 0; i < 8 && at + i < n; i++) v |= (u64)(u8)p[at + i] << (56 - 8 * i);
  return v;
}

static inline u64 ext_prefix(Str name) {
  Str e = str_extension(name);
  char tmp[8] = {};
  u32  m = e.n < 8 ? e.n : 8;
  for (u32 i = 0; i < m; i++) tmp[i] = ascii_lower(e.p[i]);
  return be_prefix(tmp, 8);
}

struct SortItem {
  u64 p0, p1; // first 16 key bytes
  u64 ext; // first 8 bytes of lowercased extension (SORT_KIND)
  u32 index;
  u32 key_off;
  u16 key_len;
  u8  group; // 0 = directory, 1 = file (when dirs_first)
  u8  _pad;
};

void sort_listing(const DirListing& l, SortSpec s, Array<u32>& order) {
  u32 n = l.count();
  order.resize(n);
  for (u32 i = 0; i < n; i++) order[i] = i;
  if (n < 2) return;

  KeyTable keys;
  keys.bytes.reserve(l.names.len + n * 2);

  Array<SortItem> items;
  items.resize(n);
  for (u32 i = 0; i < n; i++) {
    u32 start = keys.bytes.len;
    build_key(l.name(i), keys.bytes);
    SortItem& it = items[i];
    it.p0      = be_prefix(keys.bytes.data + start, keys.bytes.len - start, 0);
    it.p1      = be_prefix(keys.bytes.data + start, keys.bytes.len - start, 8);
    it.ext     = s.key == SORT_KIND ? ext_prefix(l.name(i)) : 0;
    it.index   = i;
    it.key_off = start;
    it.key_len = (u16)(keys.bytes.len - start);
    it.group   = (s.dirs_first && !l.is_dir(i)) ? 1 : 0;
  }

  const char* kb = keys.bytes.data;

  auto name_cmp = [&](const SortItem& a, const SortItem& b) -> int {
    if (a.p0 != b.p0) return a.p0 < b.p0 ? -1 : 1;
    if (a.p1 != b.p1) return a.p1 < b.p1 ? -1 : 1;
    if (a.key_len > 16 || b.key_len > 16) {
      u32 la = a.key_len > 16 ? a.key_len - 16 : 0, lb = b.key_len > 16 ? b.key_len - 16 : 0;
      int r = memcmp(kb + a.key_off + 16, kb + b.key_off + 16, la < lb ? la : lb);
      if (r) return r;
      if (la != lb) return la < lb ? -1 : 1;
    }
    return str_cmp(l.name(a.index), l.name(b.index)); // total order
  };

  auto less = [&](const SortItem& a, const SortItem& b) -> bool {
    if (a.group != b.group) return a.group < b.group;
    int c = 0;
    switch (s.key) {
      case SORT_NAME:  c = name_cmp(a, b); break;
      case SORT_SIZE:  c = (l.size[a.index] > l.size[b.index]) - (l.size[a.index] < l.size[b.index]); break;
      case SORT_MTIME: c = (l.mtime_ns[a.index] > l.mtime_ns[b.index]) - (l.mtime_ns[a.index] < l.mtime_ns[b.index]); break;
      case SORT_KIND:  c = (a.ext > b.ext) - (a.ext < b.ext); break;
    }
    if (c == 0 && s.key != SORT_NAME) c = name_cmp(a, b);
    return s.ascending ? c < 0 : c > 0;
  };

  Array<SortItem> tmp;
  tmp.resize(n / 2 + 1);
  merge_sort(items.data, tmp.data, n, less);
  for (u32 i = 0; i < n; i++) order[i] = items[i].index;
}
