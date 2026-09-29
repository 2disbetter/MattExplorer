#pragma once

#include "base/types.h"
#include <string.h>

template <class T, class Less>
static void insertion_sort(T* a, u32 n, Less& less) {
  for (u32 i = 1; i < n; i++) {
    T   v = a[i];
    u32 j = i;
    while (j > 0 && less(v, a[j - 1])) { a[j] = a[j - 1]; j--; }
    a[j] = v;
  }
}

template <class T, class Less>
static void merge_sort(T* a, T* tmp, u32 n, Less& less) {
  if (n <= 24) { insertion_sort(a, n, less); return; }
  u32 mid = n / 2;
  merge_sort(a, tmp, mid, less);
  merge_sort(a + mid, tmp, n - mid, less);
  if (!less(a[mid], a[mid - 1])) return; // halves already in order
  memcpy(tmp, a, (usize)mid * sizeof(T));
  u32 i = 0, j = mid, k = 0;
  while (i < mid && j < n) a[k++] = less(a[j], tmp[i]) ? a[j++] : tmp[i++];
  while (i < mid) a[k++] = tmp[i++];
}
