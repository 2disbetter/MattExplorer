#pragma once

#include "base/types.h"
#include "base/array.h"
#include "platform/wayland/wire.h"
#include "platform/xkb/keymap.h"
#include "platform/xkb/compose.h"
#include "gfx/canvas.h"

enum WEventType : u8 {
  WE_NONE = 0,
  WE_RESIZE, // buffer size changed; see w.buf_w / buf_h
  WE_SCALE, // w.scale120 changed
  WE_CLOSE, // compositor asked us to close
  WE_POINTER_ENTER, WE_POINTER_LEAVE,
  WE_POINTER_MOTION, // x, y
  WE_POINTER_BUTTON,
  WE_SCROLL, // dx, dy in pixels (positive = down / right)
  WE_FOCUS_IN, WE_FOCUS_OUT,
  WE_KEYMAP,
  WE_KEY,
  WE_MODIFIERS, // mods[0..3] = depressed, latched, locked, group
  WE_FD,
  WE_SELECTION,
  WE_DATA_SEND,
  WE_DATA_CANCELLED, // our clipboard offer was replaced by another client's
  WE_DND_ENTER,
  WE_DND_MOTION, // ... and moved: x, y
  WE_DND_LEAVE, // ... and left (no drop)
  WE_DND_DROP, // ... and was dropped: receive it, then finish
  WE_DRAG_END,
};

struct WEvent {
  u8    type;
  u8    pressed;
  u8    repeat; // WE_KEY: synthesised auto-repeat (pressed is 1)
  u8    text_len; // WE_KEY: bytes valid in text[]
  u32   code; // WE_KEY: evdev keycode; WE_POINTER_BUTTON: BTN_*
  float x, y;
  float dx, dy;
  u32   time;
  u32   serial;
  u32   mods[4];
  u32   keysym; // WE_KEY: resolved keysym, 0 if none or no keymap
  u32   consumed;
  u32   aux; // WE_FD: revents
  char  text[12];
};

static inline u32 wevent_mods(const WEvent& e) { return e.mods[0] | e.mods[1] | e.mods[2]; }

struct WindowBuffer { u32 id; bool busy, stale; usize offset, bytes; i32 w, h; };

struct WindowFd { int fd; short events; };

enum { WCLIP_URI_LIST = 1, WCLIP_GNOME_FILES = 2, WCLIP_TEXT = 4 };

enum { WL_DND_NONE = 0, WL_DND_COPY = 1, WL_DND_MOVE = 2, WL_DND_ASK = 4 };

struct Window {
  WlConn wl;

  u32 registry = 0, compositor = 0, shm = 0, wm_base = 0, seat = 0;
  u32 fractional_mgr = 0, viewporter = 0, cursor_mgr = 0, data_mgr = 0;
  u32 compositor_version = 0, seat_version = 0, data_mgr_version = 0;

  u32 surface = 0, xdg_surface = 0, toplevel = 0, viewport = 0, fractional = 0;
  u32 pointer = 0, keyboard = 0, cursor_device = 0, frame_cb = 0;

  u32  data_device = 0, data_source = 0;
  u32  sel_offer = 0, sel_mimes = 0; // server-side offer object, WCLIP_* bits
  u32  pending_offer = 0, pending_mimes = 0;
  u32  dnd_offer = 0; // an offer dragged over us (not accepted yet: M6b)
  bool sel_ours = false;
  u32  key_mods = 0;

  u32  input_serial = 0;
  u32  pointer_serial = 0;
  char send_mime[64] = {}; // WE_DATA_SEND

  u32  drag_source = 0, drag_icon = 0, drag_icon_buf = 0, drag_icon_pool = 0, drag_icon_viewport = 0;
  int  drag_icon_fd = -1; u8* drag_icon_mem = nullptr; usize drag_icon_bytes = 0;
  bool dragging = false, drag_dropped = false, drag_accepted = false;
  u32  drag_action = 0;
  u32  dnd_mimes = 0, dnd_serial = 0, dnd_action = 0, dnd_source_actions = 0;
  bool dnd_inside = false, dnd_dropped = false;
  float dnd_x = 0, dnd_y = 0; // buffer pixels

  i32 want_w = 0, want_h = 0; // our preferred logical size
  i32 logical_w = 0, logical_h = 0;
  i32 buf_w = 0, buf_h = 0; // pixel size of the buffers
  u32 scale120 = 120; // 120 = 1.0
  i32 int_scale = 1; // fallback when no fractional scale protocol
  bool maximized = false, fullscreen = false, activated = false, resizing = false;

  bool configured = false, closed = false;
  bool need_redraw = false, frame_pending = false;
  u32  ack_serial = 0;
  bool ack_pending = false;
  u32  configures = 0, pool_grows = 0; // stats for --debug

  int          shm_fd = -1;
  u8*          shm_mem = nullptr;
  usize        shm_size = 0;
  u32          pool = 0;
  usize        slot_cap = 0;
  WindowBuffer bufs[4] = {};
  u32          current_buf = 0;

  WindowFd fds[8] = {};
  u32      num_fds = 0;

  float ptr_x = 0, ptr_y = 0;
  u32   ptr_enter_serial = 0;
  bool  ptr_inside = false;
  float axis_dx = 0, axis_dy = 0;
  bool  axis_pending = false;
  u32   cursor_shape = 0;
  char* keymap = nullptr; // mmap'ed xkb_v1 text, NUL-terminated by the compositor
  u32   keymap_size = 0;
  u64   keymap_hash = 0;
  i32   repeat_rate = 25, repeat_delay = 600;
  u32   mods[4] = {};
  bool  focused = false;
  XkbKeymap    xkb;
  ComposeTable compose;
  ComposeState compose_state;
  bool         compose_tried = false;

  u32   repeat_code = 0;
  u32   repeat_serial = 0;
  i64   repeat_next_ms = 0;
  Array<WEvent> events;
};

i64 window_now_ms();

bool window_open(Window& w, i32 width, i32 height, const char* title, const char* app_id);
void window_close(Window& w);

bool window_pump(Window& w, int timeout_ms);

bool window_add_fd(Window& w, int fd, short events);
void window_remove_fd(Window& w, int fd);

bool window_acquire(Window& w, Canvas& out);

void window_present(Window& w, i32 dx, i32 dy, i32 dw, i32 dh);

void window_set_cursor(Window& w, u32 shape); // CursorShape from proto.h
void window_set_title(Window& w, const char* title);

bool window_clip_offer(Window& w, u32 mimes);

int  window_clip_receive(Window& w, u32 mime);
void window_clip_release(Window& w); // drop our source (exit)

bool window_drag_start(Window& w, u32 mimes, u32 actions, const u32* icon_px, i32 icon_w, i32 icon_h);

void window_dnd_accept(Window& w, u32 mime, u32 actions, u32 preferred);

int  window_dnd_receive(Window& w, u32 mime);

void window_dnd_finish(Window& w, bool accepted);
