#include "core/fuzzy.h"
#include "base/algo.h"

static inline bool is_sep(char c) {
  return c == ' ' || c == '_' || c == '-' || c == '.' || c == '/' || c == '(' || c == '[';
}

static inline int bonus_at(Str t, u32 i) {
  if (i == 0) return 12; // start of the name
  char prev = t.p[i - 1], cur = t.p[i];
  if (is_sep(prev)) return 8; // after . _ - space
  if (is_lower(prev) && is_upper(cur)) return 6; // camelCase boundary
  if (!is_digit(prev) && is_digit(cur)) return 4; // start of a number
  return 0;
}

int fuzzy_score(Str pat, Str text, u16* positions) {
  if (pat.n == 0) return 1;
  if (pat.n > text.n) return 0;

  u32 pi = 0, end = 0;
  for (u32 ti = 0; ti < text.n && pi < pat.n; ti++) {
    if (ascii_lower(text.p[ti]) == ascii_lower(pat.p[pi])) { pi++; end = ti; }
  }
  if (pi < pat.n) return 0;

  u32 start = end;
  for (i32 ti = (i32)end, pj = (i32)pat.n - 1; ti >= 0 && pj >= 0; ti--) {
    if (ascii_lower(text.p[ti]) == ascii_lower(pat.p[pj])) { start = (u32)ti; pj--; }
  }

  int  score  = 0;
  bool in_run = false;
  u32  pk     = 0;
  for (u32 ti = start; ti <= end && pk < pat.n; ti++) {
    if (ascii_lower(text.p[ti]) == ascii_lower(pat.p[pk])) {
      score += 16 + bonus_at(text, ti);
      if (in_run) score += 8; // consecutive run
      if (is_upper(pat.p[pk]) && pat.p[pk] == text.p[ti]) score += 2; // exact case
      if (positions) positions[pk] = (u16)ti;
      pk++;
      in_run = true;
    } else {
      score -= in_run ? 3 : 1; // gap start / gap continue
      in_run = false;
    }
  }
  score -= (int)(start / 2); // earlier is better
  score -= (int)mx_min(text.n, 64u) / 8; // shorter is better
  return score > 0 ? score : 1;
}

void fuzzy_filter(const DirListing& l, Str pattern, Array<FuzzyHit>& out) {
  out.clear();
  u32 n = l.count();
  if (pattern.n == 0) {
    out.resize(n);
    for (u32 i = 0; i < n; i++) out[i] = FuzzyHit{i, 1};
    return;
  }
  for (u32 i = 0; i < n; i++) {
    int s = fuzzy_score(pattern, l.name(i), nullptr);
    if (s) out.push(FuzzyHit{i, s});
  }
  if (out.len < 2) return;
  Array<FuzzyHit> tmp;
  tmp.resize(out.len / 2 + 1);
  auto less = [](const FuzzyHit& a, const FuzzyHit& b) { return a.score > b.score; };
  merge_sort(out.data, tmp.data, out.len, less);
}
