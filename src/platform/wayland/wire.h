#pragma once

#include "base/types.h"
#include "base/array.h"
#include "base/str.h"

enum { WL_MAX_MSG = 4096, WL_OUT_CAP = 16384, WL_IN_CAP = 16384, WL_MAX_FDS = 28, WL_MAX_POLL_FDS = 8 };

struct pollfd;

struct WlConn;

struct WlReader {
  const u8* p;
  u32       len;
  u32       pos;
  WlConn*   c;
  bool      ok;

  u32    u32v();
  i32    i32v();
  double fixed();
  Str    str(); // points into the receive buffer; copy if you keep it
  Str    array(); // raw bytes, same lifetime
  int    fd(); // takes ownership; -1 if none queued
};

typedef void (*WlHandler)(void* ctx, u32 object, u16 iface, u16 opcode, WlReader& rd);

struct WlConn {
  int  fd = -1;
  u8   out[WL_OUT_CAP];
  u32  out_len = 0;
  u32  msg_start = 0;
  int  out_fds[WL_MAX_FDS];
  u32  out_fd_count = 0;
  u8   in[WL_IN_CAP];
  u32  in_len = 0;
  int  in_fds[WL_MAX_FDS * 2];
  u32  in_fd_head = 0, in_fd_count = 0;
  Array<u16> client_iface; // [id - 2] -> interface tag, IF_NONE when free
  Array<u32> free_ids;
  Array<u16> server_iface; // [id - 0xFF000000]
  bool dead = false;
  char err[160];
};

bool wl_connect(WlConn& c); // $WAYLAND_SOCKET or $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
void wl_disconnect(WlConn& c);

u32  wl_new_id(WlConn& c, u16 iface); // allocate a client object id
u16  wl_iface_of(WlConn& c, u32 id);
void wl_bind_server_id(WlConn& c, u32 id, u16 iface); // for new_id args in events
void wl_forget_id(WlConn& c, u32 id);

void wl_begin(WlConn& c, u32 object, u32 opcode);
void wl_u32(WlConn& c, u32 v);
void wl_i32(WlConn& c, i32 v);
void wl_fixed(WlConn& c, double v);
void wl_str(WlConn& c, Str s);
void wl_array(WlConn& c, const void* data, u32 len);
void wl_fd(WlConn& c, int fd); // fd is dup'ed; caller keeps its own
void wl_end(WlConn& c);
bool wl_flush(WlConn& c);

static inline void wl_call0(WlConn& c, u32 object, u32 opcode) { wl_begin(c, object, opcode); wl_end(c); }

bool wl_dispatch(WlConn& c, WlHandler h, void* ctx);

bool wl_wait_dispatch(WlConn& c, WlHandler h, void* ctx, int timeout_ms);

bool wl_wait_dispatch_fds(WlConn& c, WlHandler h, void* ctx, int timeout_ms, struct pollfd* extra, u32 nextra);

bool wl_roundtrip(WlConn& c, WlHandler h, void* ctx);
