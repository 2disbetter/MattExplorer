#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

struct MountEntry { // one volume
  char name[64]; // label ("Data"), else the partition name ("nvme0n1p2")
  char device[128]; // "/dev/nvme0n1p2", "/dev/mapper/cryptroot"
  char mountpoint[256]; // "" when not mounted
  char fstype[32]; // "btrfs", "swap" for swap, "" unknown
  char disk[32];
  char part[32];
  u32  major = 0, minor = 0;
  u64  total = 0, used = 0, free = 0;
  u64  raw_size = 0; // unmounted / swap: size from /sys/block
  bool mounted = false, removable = false, statted = false, stat_failed = false;
  bool system = false; // EFI / boot partition, swap: shown, never counted
  bool counted = false;
  i64  stat_ns = 0;
};

struct DiskEntry {
  char name[32]; // kernel name ("nvme0n1")
  char model[64]; // "Samsung SSD 980 PRO 1TB", "" if sysfs has none
  char label[64];
  bool removable = false;
  u64  size = 0; // whole device, from /sys/block
  u64  total = 0, used = 0, free = 0; // aggregate of counted volumes that have answered statvfs
  u32  volumes = 0; // data volumes (filesystems) counted
  u32  first = 0, count = 0; // its entries, contiguous in MountList::entries
  bool any_statted = false, any_pending = false;
};

struct MountList {
  Array<MountEntry> entries; // grouped by disk, in disk order
  Array<DiskEntry>  disks;
  i64 read_ns = 0;
};

void mounts_parse(Str mountinfo, MountList& out);

void mounts_classify(MountList& l);

void mounts_group(MountList& l);

void mounts_aggregate(MountList& l);

bool mounts_read(MountList& out);

void mounts_stat_one(MountEntry& e);

void mounts_carry_stats(const MountList& old, MountList& fresh);

bool mounts_merge_stat(MountList& l, const MountEntry& result);

const char* disk_primary_mount(const MountList& l, const DiskEntry& d);

static inline float mount_used_fraction(const MountEntry& e) { return e.total ? (float)((double)e.used / (double)e.total) : 0.0f; }
static inline float disk_used_fraction(const DiskEntry& d)   { return d.total ? (float)((double)d.used / (double)d.total) : 0.0f; }

bool mounts_disk_removable(const char* sys_block, const char* name);

void mounts_demo(MountList& out);

int  mounts_open_watch();
void mounts_rearm(int fd);
