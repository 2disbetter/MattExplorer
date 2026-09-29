#include "platform/wayland/wire.h"
#include "platform/wayland/proto.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const u32 kServerIdBase = 0xFF000000u;

static void wl_fail(WlConn& c, const char* what) {
  if (!c.dead) snprintf(c.err, sizeof c.err, "%s", what);
  c.dead = true;
}

bool wl_connect(WlConn& c) {
  const char* env_fd = getenv("WAYLAND_SOCKET");
  if (env_fd && *env_fd) {
    c.fd = atoi(env_fd);
    unsetenv("WAYLAND_SOCKET");
  } else {
    const char* rt   = getenv("XDG_RUNTIME_DIR");
    const char* name = getenv("WAYLAND_DISPLAY");
    if (!name || !*name) name = "wayland-0";
    if (!rt || !*rt) { wl_fail(c, "XDG_RUNTIME_DIR not set"); return false; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (name[0] == '/') snprintf(addr.sun_path, sizeof addr.sun_path, "%s", name);
    else snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", rt, name);

    c.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c.fd < 0) { wl_fail(c, "socket() failed"); return false; }
    if (connect(c.fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
      close(c.fd); c.fd = -1;
      wl_fail(c, "connect() to compositor failed");
      return false;
    }
  }
  c.out_len = c.in_len = 0;
  c.out_fd_count = c.in_fd_count = c.in_fd_head = 0;
  c.client_iface.clear();
  c.free_ids.clear();
  c.server_iface.clear();
  c.dead = false;
  return true;
}

void wl_disconnect(WlConn& c) {
  if (c.fd >= 0) close(c.fd);
  c.fd = -1;
  for (u32 i = 0; i < c.out_fd_count; i++) close(c.out_fds[i]);
  for (u32 i = c.in_fd_head; i < c.in_fd_count; i++) close(c.in_fds[i]);
  c.out_fd_count = c.in_fd_count = c.in_fd_head = 0;
}

u32 wl_new_id(WlConn& c, u16 iface) {
  u32 id;
  if (c.free_ids.len) { id = c.free_ids.last(); c.free_ids.pop(); }
  else { id = c.client_iface.len + 2; c.client_iface.push(IF_NONE); }
  c.client_iface[id - 2] = iface;
  return id;
}

u16 wl_iface_of(WlConn& c, u32 id) {
  if (id == 1) return IF_DISPLAY;
  if (id >= kServerIdBase) {
    u32 k = id - kServerIdBase;
    return k < c.server_iface.len ? c.server_iface[k] : (u16)IF_NONE;
  }
  return (id - 2) < c.client_iface.len ? c.client_iface[id - 2] : (u16)IF_NONE;
}

void wl_bind_server_id(WlConn& c, u32 id, u16 iface) {
  if (id < kServerIdBase) return;
  u32 k = id - kServerIdBase;
  if (k >= c.server_iface.len) c.server_iface.resize_zero(k + 1);
  c.server_iface[k] = iface;
}

void wl_forget_id(WlConn& c, u32 id) {
  if (id >= 2 && id - 2 < c.client_iface.len) c.client_iface[id - 2] = IF_NONE;
}

static inline void put32(WlConn& c, u32 v) {
  memcpy(c.out + c.out_len, &v, 4);
  c.out_len += 4;
}

void wl_begin(WlConn& c, u32 object, u32 opcode) {
  if (c.out_len + WL_MAX_MSG > WL_OUT_CAP) wl_flush(c);
  c.msg_start = c.out_len;
  put32(c, object);
  put32(c, opcode); // size patched in wl_end
}

void wl_u32(WlConn& c, u32 v)   { put32(c, v); }
void wl_i32(WlConn& c, i32 v)   { put32(c, (u32)v); }
void wl_fixed(WlConn& c, double v) { put32(c, (u32)(i32)(v * 256.0)); }

void wl_str(WlConn& c, Str s) {
  u32 n = s.n + 1; // including NUL
  put32(c, n);
  memcpy(c.out + c.out_len, s.p, s.n);
  c.out[c.out_len + s.n] = 0;
  u32 padded = (n + 3) & ~3u;
  memset(c.out + c.out_len + n, 0, padded - n);
  c.out_len += padded;
}

void wl_array(WlConn& c, const void* data, u32 len) {
  put32(c, len);
  memcpy(c.out + c.out_len, data, len);
  u32 padded = (len + 3) & ~3u;
  memset(c.out + c.out_len + len, 0, padded - len);
  c.out_len += padded;
}

void wl_fd(WlConn& c, int fd) {
  if (c.out_fd_count == WL_MAX_FDS) {

    u32 keep = c.out_len - c.msg_start;
    u8  tmp[WL_MAX_MSG];
    memcpy(tmp, c.out + c.msg_start, keep);
    c.out_len = c.msg_start;
    wl_flush(c);
    memcpy(c.out, tmp, keep);
    c.msg_start = 0;
    c.out_len = keep;
  }
  c.out_fds[c.out_fd_count++] = dup(fd);
}

void wl_end(WlConn& c) {
  u32 size = c.out_len - c.msg_start;
  u32 hdr;
  memcpy(&hdr, c.out + c.msg_start + 4, 4);
  hdr = (size << 16) | (hdr & 0xFFFF);
  memcpy(c.out + c.msg_start + 4, &hdr, 4);
}

bool wl_flush(WlConn& c) {
  if (c.dead || c.fd < 0) return false;
  u32 sent = 0;
  while (sent < c.out_len) {
    struct iovec  iov = { c.out + sent, c.out_len - sent };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov    = &iov;
    mh.msg_iovlen = 1;
    alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)];
    if (sent == 0 && c.out_fd_count) {
      mh.msg_control    = cbuf;
      mh.msg_controllen = CMSG_SPACE(sizeof(int) * c.out_fd_count);
      struct cmsghdr* cm = CMSG_FIRSTHDR(&mh);
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type  = SCM_RIGHTS;
      cm->cmsg_len   = CMSG_LEN(sizeof(int) * c.out_fd_count);
      memcpy(CMSG_DATA(cm), c.out_fds, sizeof(int) * c.out_fd_count);
    }
    ssize_t n = sendmsg(c.fd, &mh, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) { struct pollfd p = { c.fd, POLLOUT, 0 }; poll(&p, 1, -1); continue; }
      wl_fail(c, "sendmsg() failed");
      return false;
    }
    if (sent == 0) {
      for (u32 i = 0; i < c.out_fd_count; i++) close(c.out_fds[i]);
      c.out_fd_count = 0;
    }
    sent += (u32)n;
  }
  c.out_len = 0;
  return true;
}

u32 WlReader::u32v() {
  if (!ok || pos + 4 > len) { ok = false; return 0; }
  u32 v; memcpy(&v, p + pos, 4); pos += 4; return v;
}
i32    WlReader::i32v()  { return (i32)u32v(); }
double WlReader::fixed() { return (double)(i32)u32v() / 256.0; }

Str WlReader::str() {
  u32 n = u32v();
  if (!ok) return Str();
  u32 padded = (n + 3) & ~3u;
  if (n == 0 || pos + padded > len) { if (n) ok = false; return Str(); }
  Str s((const char*)p + pos, n - 1);
  pos += padded;
  return s;
}

Str WlReader::array() {
  u32 n = u32v();
  if (!ok) return Str();
  u32 padded = (n + 3) & ~3u;
  if (pos + padded > len) { ok = false; return Str(); }
  Str s((const char*)p + pos, n);
  pos += padded;
  return s;
}

int WlReader::fd() {
  if (c->in_fd_head >= c->in_fd_count) return -1;
  int f = c->in_fds[c->in_fd_head++];
  if (c->in_fd_head == c->in_fd_count) c->in_fd_head = c->in_fd_count = 0;
  return f;
}

static bool wl_read(WlConn& c) {
  if (c.in_len >= WL_IN_CAP) { wl_fail(c, "receive buffer full"); return false; }
  struct iovec  iov = { c.in + c.in_len, WL_IN_CAP - c.in_len };
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)];
  mh.msg_iov        = &iov;
  mh.msg_iovlen     = 1;
  mh.msg_control    = cbuf;
  mh.msg_controllen = sizeof cbuf;

  ssize_t n;
  for (;;) {
    n = recvmsg(c.fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  if (n < 0) {
    if (errno == EAGAIN) return true;
    wl_fail(c, "recvmsg() failed");
    return false;
  }
  if (n == 0) { wl_fail(c, "compositor closed the connection"); return false; }

  for (struct cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
    if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
    u32 count = (u32)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    const int* fds = (const int*)CMSG_DATA(cm);
    for (u32 i = 0; i < count; i++) {
      if (c.in_fd_count < MX_ARRAY_COUNT(c.in_fds)) c.in_fds[c.in_fd_count++] = fds[i];
      else close(fds[i]);
    }
  }
  c.in_len += (u32)n;
  return true;
}

static void handle_display_event(WlConn& c, u16 opcode, WlReader& rd) {
  if (opcode == WL_DISPLAY_EV_ERROR) {
    u32 obj  = rd.u32v();
    u32 code = rd.u32v();
    Str msg  = rd.str();
    char buf[160];
    snprintf(buf, sizeof buf, "protocol error on object %u (iface %u) code %u: %.*s", obj, wl_iface_of(c, obj),
             code, (int)mx_min(msg.n, 100u), msg.p ? msg.p : "");
    wl_fail(c, buf);
  } else if (opcode == WL_DISPLAY_EV_DELETE_ID) {
    u32 id = rd.u32v();
    if (id >= 2 && id - 2 < c.client_iface.len) {
      c.client_iface[id - 2] = IF_NONE;
      c.free_ids.push(id);
    }
  }
}

static int dispatch_buffered(WlConn& c, WlHandler h, void* ctx) {
  u32 pos = 0, count = 0;
  while (c.in_len - pos >= 8) {
    u32 id, hdr;
    memcpy(&id, c.in + pos, 4);
    memcpy(&hdr, c.in + pos + 4, 4);
    u32 size = hdr >> 16, opcode = hdr & 0xFFFF;
    if (size < 8 || size > WL_MAX_MSG || (size & 3)) { wl_fail(c, "malformed message header"); return -1; }
    if (c.in_len - pos < size) break;

    WlReader rd = { c.in + pos + 8, size - 8, 0, &c, true };
    if (id == 1) handle_display_event(c, (u16)opcode, rd);
    else {
      u16 iface = wl_iface_of(c, id);
      if (iface != IF_NONE) h(ctx, id, iface, (u16)opcode, rd);
    }
    pos += size;
    count++;
    if (c.dead) return -1;
  }
  if (pos) {
    memmove(c.in, c.in + pos, c.in_len - pos);
    c.in_len -= pos;
  }
  return (int)count;
}

bool wl_dispatch(WlConn& c, WlHandler h, void* ctx) {
  if (c.dead) return false;
  if (!wl_read(c)) return false;
  return dispatch_buffered(c, h, ctx) >= 0;
}

bool wl_wait_dispatch_fds(WlConn& c, WlHandler h, void* ctx, int timeout_ms, struct pollfd* extra, u32 nextra) {
  if (c.dead) return false;
  if (!wl_flush(c)) return false;
  for (u32 i = 0; i < nextra; i++) extra[i].revents = 0;
  int n = dispatch_buffered(c, h, ctx); // anything already buffered counts
  if (n < 0) return false;
  if (n > 0) timeout_ms = 0;
  struct pollfd fds[1 + WL_MAX_POLL_FDS];
  nextra = mx_min(nextra, (u32)WL_MAX_POLL_FDS);
  fds[0] = { c.fd, POLLIN, 0 };
  for (u32 i = 0; i < nextra; i++) fds[1 + i] = extra[i];
  int r;
  do r = poll(fds, 1 + nextra, timeout_ms); while (r < 0 && errno == EINTR);
  if (r < 0) { wl_fail(c, "poll() failed"); return false; }
  for (u32 i = 0; i < nextra; i++) extra[i].revents = fds[1 + i].revents;
  if (r == 0) return true; // timeout, nothing to do
  if (fds[0].revents & (POLLHUP | POLLERR)) { wl_fail(c, "compositor hung up"); return false; }
  if (fds[0].revents & POLLIN) return wl_dispatch(c, h, ctx);
  return true;
}

bool wl_wait_dispatch(WlConn& c, WlHandler h, void* ctx, int timeout_ms) {
  return wl_wait_dispatch_fds(c, h, ctx, timeout_ms, nullptr, 0);
}

struct RoundtripCtx { WlHandler h; void* ctx; u32 cb; bool done; };

static void roundtrip_handler(void* vctx, u32 object, u16 iface, u16 opcode, WlReader& rd) {
  RoundtripCtx* r = (RoundtripCtx*)vctx;
  if (object == r->cb && iface == IF_CALLBACK && opcode == WL_CALLBACK_EV_DONE) { r->done = true; return; }
  r->h(r->ctx, object, iface, opcode, rd);
}

bool wl_roundtrip(WlConn& c, WlHandler h, void* ctx) {
  RoundtripCtx r = { h, ctx, wl_new_id(c, IF_CALLBACK), false };
  wl_begin(c, 1, WL_DISPLAY_SYNC);
  wl_u32(c, r.cb);
  wl_end(c);
  if (!wl_flush(c)) return false;
  while (!r.done) {
    if (!wl_wait_dispatch(c, roundtrip_handler, &r, 5000)) return false;
  }
  return true;
}
