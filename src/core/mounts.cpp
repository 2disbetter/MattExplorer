#include "core/mounts.h"
#include "base/algo.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static i64 now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static u32 unescape_octal(Str s, char* out, u32 cap) {
  u32 o = 0;
  for (u32 i = 0; i < s.n && o + 1 < cap; i++) {
    char c = s.p[i];
    if (c == '\\' && i + 3 < s.n && s.p[i + 1] >= '0' && s.p[i + 1] <= '7') {
      c = (char)((s.p[i + 1] - '0') * 64 + (s.p[i + 2] - '0') * 8 + (s.p[i + 3] - '0'));
      i += 3;
    }
    out[o++] = c;
  }
  out[o] = 0;
  return o;
}

static u32 unescape_hex(const char* s, char* out, u32 cap) {
  u32 o = 0;
  for (; *s && o + 1 < cap; s++) {
    char c = *s;
    if (c == '\\' && s[1] == 'x' && s[2] && s[3]) {
      char h[3] = { s[2], s[3], 0 };
      c = (char)strtol(h, nullptr, 16);
      s += 3;
    }
    out[o++] = c;
  }
  out[o] = 0;
  return o;
}

static Str next_field(Str line, u32& pos) {
  while (pos < line.n && line.p[pos] == ' ') pos++;
  u32 start = pos;
  while (pos < line.n && line.p[pos] != ' ') pos++;
  return Str(line.p + start, pos - start);
}

static bool pseudo_fs(Str t) {
  static const char* pseudo[] = { "squashfs", "autofs", "nsfs", "tmpfs", "devtmpfs", "proc", "sysfs", "cgroup", "cgroup2",
                                  "overlay", "fuse.portal", "fuse.gvfsd-fuse", "efivarfs", "binfmt_misc", "bpf", "tracefs",
                                  "debugfs", "configfs", "securityfs", "pstore", "mqueue", "hugetlbfs", "devpts", "ramfs" };
  for (const char* p : pseudo) if (str_eq(t, p)) return true;
  return false;
}

static void read_file(const char* path, char* out, u32 cap) {
  out[0] = 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  long n = read(fd, out, cap - 1);
  close(fd);
  if (n < 0) n = 0;
  out[n] = 0;
  while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
}

static bool file_exists(const char* path) { struct stat st; return stat(path, &st) == 0; }

static u64 hash_str(const char* s) {
  u64 h = 1469598103934665603ull;
  for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
  return h;
}

static const char* dev_base(const char* dev) { const char* b = strrchr(dev, '/'); return b ? b + 1 : dev; }

void mounts_parse(Str text, MountList& out) {
  out.entries.clear();
  out.disks.clear();
  Array<u64> roots; // hash of each entry's root, for bind-mount collapsing
  u32 pos = 0;
  while (pos < text.n) {
    u32 eol = pos;
    while (eol < text.n && text.p[eol] != '\n') eol++;
    Str line(text.p + pos, eol - pos);
    pos = eol + 1;
    u32 p = 0;
    next_field(line, p); // mount id
    next_field(line, p); // parent id
    Str majmin = next_field(line, p);
    Str root   = next_field(line, p);
    Str mp     = next_field(line, p);
    next_field(line, p); // options
    for (;;) { // optional fields until "-"
      Str f = next_field(line, p);
      if (f.empty() || str_eq(f, "-")) break;
    }
    Str fstype = next_field(line, p);
    Str source = next_field(line, p);
    if (mp.empty() || fstype.empty() || source.n < 6) continue;
    if (strncmp(source.p, "/dev/", 5) != 0) continue;
    if (source.n >= 9 && strncmp(source.p, "/dev/loop", 9) == 0) continue;
    if (source.n >= 9 && strncmp(source.p, "/dev/zram", 9) == 0) continue;
    if (pseudo_fs(fstype)) continue;

    MountEntry e = {};
    unescape_octal(source, e.device, sizeof e.device);
    unescape_octal(mp, e.mountpoint, sizeof e.mountpoint);
    unescape_octal(fstype, e.fstype, sizeof e.fstype);
    e.major = (u32)strtoul(majmin.p, nullptr, 10);
    const char* colon = (const char*)memchr(majmin.p, ':', majmin.n);
    e.minor = colon ? (u32)strtoul(colon + 1, nullptr, 10) : 0;
    e.mounted = true;

    char rootbuf[256];
    unescape_octal(root, rootbuf, sizeof rootbuf);
    u64  rh  = hash_str(rootbuf);
    bool dup = false;
    for (u32 i = 0; i < out.entries.len; i++) {
      MountEntry& o = out.entries[i];
      if (o.major != e.major || o.minor != e.minor || roots[i] != rh) continue;
      dup = true;
      if (strlen(e.mountpoint) < strlen(o.mountpoint)) memcpy(o.mountpoint, e.mountpoint, sizeof o.mountpoint);
      break;
    }
    if (dup) continue;
    out.entries.push(e);
    roots.push(rh);
  }
}

void mounts_classify(MountList& l) {
  for (MountEntry& e : l.entries) {
    e.system = false;
    if (!strcmp(e.fstype, "swap")) { e.system = true; continue; }
    if (!e.mounted) continue;

    bool boot_path = !strcmp(e.mountpoint, "/boot") || !strncmp(e.mountpoint, "/boot/", 6) || !strcmp(e.mountpoint, "/efi");
    if (boot_path && (!strcmp(e.fstype, "vfat") || !strcmp(e.fstype, "msdos"))) e.system = true;
  }
}

struct EntryKey { u32 disk_rank; u32 index; };

void mounts_group(MountList& l) {
  Array<DiskEntry> known = static_cast<Array<DiskEntry>&&>(l.disks); // keep model / size / removable across regrouping
  l.disks = Array<DiskEntry>();

  Array<DiskEntry> disks;
  for (MountEntry& e : l.entries) {
    if (!e.disk[0]) snprintf(e.disk, sizeof e.disk, "%s", dev_base(e.device)); // unknown: the device stands for itself
    bool seen = false;
    for (DiskEntry& d : disks) if (!strcmp(d.name, e.disk)) { seen = true; if (e.removable) d.removable = true; break; }
    if (seen) continue;
    DiskEntry d = {};
    snprintf(d.name, sizeof d.name, "%s", e.disk);
    d.removable = e.removable;
    for (const DiskEntry& k : known) if (!strcmp(k.name, d.name)) { memcpy(d.model, k.model, sizeof d.model); d.size = k.size; d.removable |= k.removable; break; }
    disks.push(d);
  }
  auto disk_rank = [&](const DiskEntry& d) -> i64 {
    i64 r = d.removable ? 1000000 : 0;
    for (const MountEntry& e : l.entries) if (!strcmp(e.disk, d.name) && e.mounted && !strcmp(e.mountpoint, "/")) return r - 1;
    return r;
  };
  Array<u32> dorder; dorder.resize(disks.len);
  for (u32 i = 0; i < disks.len; i++) dorder[i] = i;
  Array<u32> dtmp; dtmp.resize(disks.len / 2 + 1);
  auto dless = [&](u32 a, u32 b) {
    i64 ra = disk_rank(disks[a]), rb = disk_rank(disks[b]);
    if (ra != rb) return ra < rb;
    return strcmp(disks[a].name, disks[b].name) < 0;
  };
  merge_sort(dorder.data, dtmp.data, dorder.len, dless);
  for (u32 i = 0; i < dorder.len; i++) l.disks.push(disks[dorder[i]]);

  Array<u32> eorder; eorder.resize(l.entries.len);
  for (u32 i = 0; i < eorder.len; i++) eorder[i] = i;
  Array<u32> etmp; etmp.resize(l.entries.len / 2 + 1);
  auto rank_of_disk = [&](const char* name) { for (u32 i = 0; i < l.disks.len; i++) if (!strcmp(l.disks[i].name, name)) return i; return l.disks.len; };
  auto eless = [&](u32 a, u32 b) {
    const MountEntry& x = l.entries[a]; const MountEntry& y = l.entries[b];
    u32 dx = rank_of_disk(x.disk), dy = rank_of_disk(y.disk);
    if (dx != dy) return dx < dy;
    if (x.mounted != y.mounted) return x.mounted;
    if (x.system != y.system) return !x.system;
    usize lx = strlen(x.mountpoint), ly = strlen(y.mountpoint);
    if (lx != ly) return lx < ly;
    int c = strcmp(x.mountpoint, y.mountpoint);
    if (c) return c < 0;
    return strcmp(x.name, y.name) < 0;
  };
  merge_sort(eorder.data, etmp.data, eorder.len, eless);
  Array<MountEntry> sorted;
  for (u32 i : eorder) sorted.push(l.entries[i]);
  l.entries = static_cast<Array<MountEntry>&&>(sorted);

  for (DiskEntry& d : l.disks) {
    d.first = 0; d.count = 0; d.volumes = 0;
    bool started = false;
    for (u32 i = 0; i < l.entries.len; i++) {
      MountEntry& e = l.entries[i];
      if (strcmp(e.disk, d.name)) { if (started) break; continue; }
      if (!started) { started = true; d.first = i; }
      d.count++;
      e.counted = false;
      if (!e.mounted || e.system) continue;
      bool first_of_fs = true;
      for (u32 k = d.first; k < i; k++) if (l.entries[k].counted && !strcmp(l.entries[k].device, e.device)) { first_of_fs = false; break; }
      if (first_of_fs) { e.counted = true; d.volumes++; }
    }

    const MountEntry* only = nullptr; u32 counted = 0;
    for (u32 i = d.first; i < d.first + d.count; i++) if (l.entries[i].counted) { only = &l.entries[i]; counted++; }
    bool labelled = counted == 1 && strcmp(only->name, dev_base(only->device)) != 0;
    if (labelled && (d.removable || !d.model[0])) snprintf(d.label, sizeof d.label, "%s", only->name);
    else if (d.model[0]) snprintf(d.label, sizeof d.label, "%s", d.model);
    else if (labelled) snprintf(d.label, sizeof d.label, "%s", only->name);
    else snprintf(d.label, sizeof d.label, "%s", d.name);
  }
  mounts_aggregate(l);
}

void mounts_aggregate(MountList& l) {
  for (DiskEntry& d : l.disks) {
    d.total = d.used = d.free = 0;
    d.any_statted = d.any_pending = false;
    for (u32 i = d.first; i < d.first + d.count && i < l.entries.len; i++) {
      const MountEntry& e = l.entries[i];
      if (!e.counted) continue;
      if (e.statted) { d.total += e.total; d.used += e.used; d.free += e.free; d.any_statted = true; }
      else if (!e.stat_failed) d.any_pending = true;
    }
  }
}

const char* disk_primary_mount(const MountList& l, const DiskEntry& d) {
  for (u32 i = d.first; i < d.first + d.count && i < l.entries.len; i++) if (l.entries[i].counted) return l.entries[i].mountpoint;
  for (u32 i = d.first; i < d.first + d.count && i < l.entries.len; i++) if (l.entries[i].mounted) return l.entries[i].mountpoint;
  return "";
}

static void resolve_block(const char* name, char* part, u32 pcap, char* disk, u32 dcap, u32 depth = 0) {
  char path[512], real[PATH_MAX];
  snprintf(path, sizeof path, "/sys/class/block/%s/partition", name);
  if (file_exists(path)) {
    snprintf(part, pcap, "%s", name);
    snprintf(path, sizeof path, "/sys/class/block/%s/..", name);
    if (realpath(path, real)) snprintf(disk, dcap, "%s", dev_base(real));
    else snprintf(disk, dcap, "%s", name);
    return;
  }
  snprintf(path, sizeof path, "/sys/class/block/%s/slaves", name);
  if (DIR* d = opendir(path)) {
    char slave[64] = {}; u32 n = 0;
    while (struct dirent* de = readdir(d)) {
      if (de->d_name[0] == '.') continue;
      if (!n) snprintf(slave, sizeof slave, "%s", de->d_name);
      n++;
    }
    closedir(d);
    if (n == 1 && depth < 8) { resolve_block(slave, part, pcap, disk, dcap, depth + 1); return; }
  }
  snprintf(part, pcap, "%s", name);
  snprintf(disk, dcap, "%s", name);
}

static void resolve_entry_disk(MountEntry& e) {
  char real[PATH_MAX];
  const char* r = realpath(e.device, real) ? real : e.device;
  resolve_block(dev_base(r), e.part, sizeof e.part, e.disk, sizeof e.disk);
}

static bool skip_block_name(const char* n) {
  return !strncmp(n, "loop", 4) || !strncmp(n, "zram", 4) || !strncmp(n, "ram", 3) || !strncmp(n, "dm-", 3) || !strncmp(n, "md", 2);
}

bool mounts_disk_removable(const char* sys_block, const char* name) {
  char path[512], val[16];
  snprintf(path, sizeof path, "%s/%s/removable", sys_block, name);
  read_file(path, val, sizeof val);
  if (val[0] == '1') return true;
  snprintf(path, sizeof path, "%s/%s", sys_block, name);
  char target[1024];
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n <= 0) return false;
  target[n] = 0;
  return strstr(target, "/usb") || strstr(target, "/mmc_host") || strstr(target, "/firewire") || strstr(target, "/thunderbolt");
}
static bool disk_removable(const char* name) { return mounts_disk_removable("/sys/block", name); }

static void fill_disk_info(DiskEntry& d) {
  char path[512], val[256], vendor[128];
  snprintf(path, sizeof path, "/sys/block/%s/device/model", d.name);
  read_file(path, val, sizeof val);
  snprintf(path, sizeof path, "/sys/block/%s/device/vendor", d.name);
  read_file(path, vendor, sizeof vendor);
  if (val[0]) {
    if (vendor[0] && strcmp(vendor, "ATA") && strncmp(val, vendor, strlen(vendor))) snprintf(d.model, sizeof d.model, "%s %s", vendor, val);
    else snprintf(d.model, sizeof d.model, "%s", val);
  }
  if (disk_removable(d.name)) d.removable = true;
  snprintf(path, sizeof path, "/sys/block/%s/size", d.name);
  read_file(path, val, sizeof val);
  d.size = strtoull(val, nullptr, 10) * 512;
}

static void attach_labels(MountList& l) {
  DIR* d = opendir("/dev/disk/by-label");
  if (!d) return;
  Array<char> dev_real; Array<u32> dev_off;
  for (MountEntry& e : l.entries) {
    char buf[PATH_MAX];
    const char* r = realpath(e.device, buf) ? buf : e.device;
    u32 n = (u32)strlen(r);
    dev_off.push(dev_real.len);
    memcpy(dev_real.push_n(n + 1), r, n + 1);
  }
  while (struct dirent* de = readdir(d)) {
    if (de->d_name[0] == '.') continue;
    char link[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof link, "/dev/disk/by-label/%s", de->d_name);
    if (!realpath(link, target)) continue;
    for (u32 i = 0; i < l.entries.len; i++) {
      if (strcmp(dev_real.data + dev_off[i], target)) continue;
      unescape_hex(de->d_name, l.entries[i].name, sizeof l.entries[i].name);
    }
  }
  closedir(d);
}

static void add_swaps(MountList& l) {
  char buf[4096];
  read_file("/proc/swaps", buf, sizeof buf);
  char* line = strchr(buf, '\n'); // skip the header
  while (line && *line) {
    line++;
    char* end = strchr(line, '\n');
    if (end) *end = 0;
    if (!strncmp(line, "/dev/", 5)) {
      char* sp = strpbrk(line, " \t");
      if (sp) *sp = 0;
      MountEntry e = {};
      snprintf(e.device, sizeof e.device, "%s", line);
      snprintf(e.fstype, sizeof e.fstype, "swap");
      snprintf(e.name, sizeof e.name, "%s", dev_base(line));
      resolve_entry_disk(e);
      char path[512], val[64];
      snprintf(path, sizeof path, "/sys/class/block/%s/size", dev_base(line));
      read_file(path, val, sizeof val);
      e.raw_size = strtoull(val, nullptr, 10) * 512;
      e.system = true;
      l.entries.push(e);
    }
    line = end;
  }
}

static void add_unmounted(MountList& l) {
  DIR* d = opendir("/sys/block");
  if (!d) return;
  while (struct dirent* de = readdir(d)) {
    const char* disk = de->d_name;
    if (disk[0] == '.' || skip_block_name(disk)) continue;
    char path[512], val[64];
    snprintf(path, sizeof path, "/sys/block/%s/size", disk);
    read_file(path, val, sizeof val);
    if (!strtoull(val, nullptr, 10)) continue; // no medium
    snprintf(path, sizeof path, "/sys/block/%s/hidden", disk);
    read_file(path, val, sizeof val);
    if (val[0] == '1') continue;

    char ppath[512];
    snprintf(ppath, sizeof ppath, "/sys/block/%s", disk);
    u32 nparts = 0;
    if (DIR* pd = opendir(ppath)) {
      while (struct dirent* pe = readdir(pd)) {
        if (strncmp(pe->d_name, disk, strlen(disk))) continue;
        snprintf(path, sizeof path, "/sys/block/%s/%s/partition", disk, pe->d_name);
        if (!file_exists(path)) continue;
        nparts++;
        bool used = false;
        for (MountEntry& e : l.entries) if (!strcmp(e.part, pe->d_name)) { used = true; break; }
        if (used) continue;
        MountEntry e = {};
        snprintf(e.device, sizeof e.device, "/dev/%s", pe->d_name);
        snprintf(e.name, sizeof e.name, "%s", pe->d_name);
        snprintf(e.part, sizeof e.part, "%s", pe->d_name);
        snprintf(e.disk, sizeof e.disk, "%s", disk);
        snprintf(path, sizeof path, "/sys/block/%s/%s/size", disk, pe->d_name);
        read_file(path, val, sizeof val);
        e.raw_size = strtoull(val, nullptr, 10) * 512;
        l.entries.push(e);
      }
      closedir(pd);
    }
    if (!nparts) { // unpartitioned: the whole device is the volume
      bool used = false;
      for (MountEntry& e : l.entries) if (!strcmp(e.part, disk)) { used = true; break; }
      if (!used) {
        MountEntry e = {};
        snprintf(e.device, sizeof e.device, "/dev/%s", disk);
        snprintf(e.name, sizeof e.name, "%s", disk);
        snprintf(e.part, sizeof e.part, "%s", disk);
        snprintf(e.disk, sizeof e.disk, "%s", disk);
        snprintf(path, sizeof path, "/sys/block/%s/size", disk);
        read_file(path, val, sizeof val);
        e.raw_size = strtoull(val, nullptr, 10) * 512;
        l.entries.push(e);
      }
    }
  }
  closedir(d);

  for (MountEntry& e : l.entries) e.removable = disk_removable(e.disk);
}

bool mounts_read(MountList& out) {
  i64 t0 = now_ns();
  int fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
  if (fd < 0) { out.entries.clear(); out.disks.clear(); return false; }
  Array<char> text;
  for (;;) {
    text.reserve(text.len + 16384);
    long n = read(fd, text.data + text.len, text.cap - text.len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    text.len += (u32)n;
  }
  close(fd);
  mounts_parse(Str(text.data, text.len), out);
  for (MountEntry& e : out.entries) resolve_entry_disk(e);
  attach_labels(out);
  for (MountEntry& e : out.entries) if (!e.name[0]) snprintf(e.name, sizeof e.name, "%s", dev_base(e.device));
  add_swaps(out);
  add_unmounted(out);
  mounts_classify(out);
  mounts_group(out);
  for (DiskEntry& d : out.disks) fill_disk_info(d);
  mounts_group(out); // again: labels depend on the model, order on removable
  out.read_ns = now_ns() - t0;
  return true;
}

void mounts_stat_one(MountEntry& e) {
  if (!e.mounted) return;
  i64 t0 = now_ns();
  struct statvfs sv;
  int r;
  do r = statvfs(e.mountpoint, &sv); while (r < 0 && errno == EINTR);
  e.stat_ns = now_ns() - t0;
  if (r != 0) { e.stat_failed = true; e.statted = false; return; }
  u64 fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
  e.total = (u64)sv.f_blocks * fr;
  e.used  = (u64)(sv.f_blocks - sv.f_bfree) * fr;
  e.free  = (u64)sv.f_bavail * fr;
  e.statted = true;
  e.stat_failed = false;
}

void mounts_carry_stats(const MountList& old, MountList& fresh) {
  for (MountEntry& e : fresh.entries) {
    if (!e.mounted) continue;
    for (const MountEntry& o : old.entries) {
      if (!o.statted || strcmp(o.device, e.device) || strcmp(o.mountpoint, e.mountpoint)) continue;
      e.total = o.total; e.used = o.used; e.free = o.free; e.statted = true; e.stat_ns = o.stat_ns;
      break;
    }
  }
  mounts_aggregate(fresh);
}

bool mounts_merge_stat(MountList& l, const MountEntry& r) {
  bool found = false;
  for (MountEntry& m : l.entries) {
    if (strcmp(m.device, r.device) || strcmp(m.mountpoint, r.mountpoint)) continue;
    m.total = r.total; m.used = r.used; m.free = r.free; m.statted = r.statted; m.stat_failed = r.stat_failed; m.stat_ns = r.stat_ns;
    found = true;
  }
  if (found) mounts_aggregate(l);
  return found;
}

void mounts_demo(MountList& out) {
  static const char kInfo[] =
    "30 1 0:38 /@ / rw,relatime - btrfs /dev/nvme0n1p2 rw,ssd,subvol=/@\n"
    "31 30 259:1 / /boot rw,relatime - vfat /dev/nvme0n1p1 rw\n"
    "32 30 0:39 /@home /home rw,relatime - btrfs /dev/nvme0n1p2 rw,subvol=/@home\n"
    "33 30 0:40 /@pkg /var/cache/pacman/pkg rw,relatime - btrfs /dev/nvme0n1p2 rw\n"
    "34 30 0:41 /@log /var/log rw,relatime - btrfs /dev/nvme0n1p2 rw\n"
    "60 30 8:1 / /run/media/matt/Gideon rw,nosuid - ext4 /dev/sda1 rw\n";
  mounts_parse(kInfo, out);
  const u64 G = 1ull << 30;
  for (MountEntry& e : out.entries) {
    const char* base = dev_base(e.device);
    bool nvme = !strncmp(base, "nvme", 4);
    snprintf(e.disk, sizeof e.disk, "%s", nvme ? "nvme0n1" : "sda");
    snprintf(e.part, sizeof e.part, "%s", base);
    e.removable = !nvme;
    if (!strcmp(e.fstype, "btrfs")) { snprintf(e.name, sizeof e.name, "root"); e.total = 929 * G; e.used = 503 * G; e.free = 426 * G; }
    else if (!strcmp(e.fstype, "vfat")) { snprintf(e.name, sizeof e.name, "%s", base); e.total = 2 * G; e.used = G / 5; e.free = 2 * G - G / 5 - G / 50; }
    else { snprintf(e.name, sizeof e.name, "Gideon"); e.total = 228 * G; e.used = 38 * G; e.free = 190 * G; }
    e.statted = true;
  }
  MountEntry swap = {};
  snprintf(swap.device, sizeof swap.device, "/dev/nvme0n1p3");
  snprintf(swap.name, sizeof swap.name, "nvme0n1p3");
  snprintf(swap.fstype, sizeof swap.fstype, "swap");
  snprintf(swap.disk, sizeof swap.disk, "nvme0n1"); snprintf(swap.part, sizeof swap.part, "nvme0n1p3");
  swap.raw_size = 16 * G; swap.system = true;
  out.entries.push(swap);
  MountEntry win = {};
  snprintf(win.device, sizeof win.device, "/dev/nvme0n1p4");
  snprintf(win.name, sizeof win.name, "nvme0n1p4");
  snprintf(win.disk, sizeof win.disk, "nvme0n1"); snprintf(win.part, sizeof win.part, "nvme0n1p4");
  win.raw_size = 120 * G;
  out.entries.push(win);
  mounts_classify(out);
  mounts_group(out);
  for (DiskEntry& d : out.disks) {
    if (!strcmp(d.name, "nvme0n1")) { snprintf(d.model, sizeof d.model, "Samsung SSD 980 PRO 1TB"); d.size = 1000204886016ull; }
    else { snprintf(d.model, sizeof d.model, "Samsung Portable SSD T7"); d.size = 250059350016ull; d.removable = true; }
  }
  mounts_group(out);
}

int mounts_open_watch() {
  return open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
}

void mounts_rearm(int fd) {

  char buf[4096];
  lseek(fd, 0, SEEK_SET);
  while (read(fd, buf, sizeof buf) > 0) {}
}
