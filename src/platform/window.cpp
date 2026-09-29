#include "platform/window.h"
#include "platform/wayland/proto.h"

#include "platform/xkb/keysym.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

i64 window_now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float px_scale(const Window& w) {
  return w.fractional_mgr ? (float)w.scale120 / 120.0f : (float)w.int_scale;
}

static WEvent& push_event(Window& w, u8 type) {
  WEvent* e = w.events.push_n(1);
  memset(e, 0, sizeof *e);
  e->type = type;
  return *e;
}

static void recompute_buffer_size(Window& w) {
  i32 lw = w.logical_w > 0 ? w.logical_w : w.want_w;
  i32 lh = w.logical_h > 0 ? w.logical_h : w.want_h;
  i32 bw, bh;
  if (w.fractional_mgr) {
    bw = (i32)(((i64)lw * w.scale120 + 60) / 120);
    bh = (i32)(((i64)lh * w.scale120 + 60) / 120);
  } else {
    bw = lw * w.int_scale;
    bh = lh * w.int_scale;
  }
  bw = mx_max(bw, 1); bh = mx_max(bh, 1);
  if (bw != w.buf_w || bh != w.buf_h) {
    w.buf_w = bw; w.buf_h = bh;
    push_event(w, WE_RESIZE);
    w.need_redraw = true;
  }
}

static void destroy_buffer(Window& w, WindowBuffer& b) {
  if (!b.id) return;
  wl_call0(w.wl, b.id, WL_BUFFER_DESTROY);
  wl_forget_id(w.wl, b.id);
  b.id = 0; b.busy = b.stale = false; b.bytes = 0; b.w = b.h = 0;
}

static void destroy_pool(Window& w) {
  WlConn& c = w.wl;
  for (WindowBuffer& b : w.bufs) destroy_buffer(w, b);
  if (w.pool) { wl_call0(c, w.pool, WL_SHM_POOL_DESTROY); wl_forget_id(c, w.pool); w.pool = 0; }
  if (w.shm_mem) { munmap(w.shm_mem, w.shm_size); w.shm_mem = nullptr; w.shm_size = 0; }
  if (w.shm_fd >= 0) { close(w.shm_fd); w.shm_fd = -1; }
  w.slot_cap = 0;
}

static bool ensure_pool_size(Window& w, usize total) {
  WlConn& c = w.wl;
  if (w.shm_mem && total <= w.shm_size) return true;
  if (!w.shm_mem) total = mx_max(total, w.slot_cap * 3); // room for the usual three buffers up front
  total = (total + 65535) & ~(usize)65535;
  if (!w.shm_mem) {
    w.shm_fd = memfd_create("mattexplorer-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (w.shm_fd < 0) return false;
    if (ftruncate(w.shm_fd, (off_t)total) != 0) { close(w.shm_fd); w.shm_fd = -1; return false; }
    w.shm_mem = (u8*)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, w.shm_fd, 0);
    if (w.shm_mem == MAP_FAILED) { w.shm_mem = nullptr; close(w.shm_fd); w.shm_fd = -1; return false; }
    w.shm_size = total;
    w.pool = wl_new_id(c, IF_SHM_POOL);
    wl_begin(c, w.shm, WL_SHM_CREATE_POOL);
    wl_u32(c, w.pool);
    wl_fd(c, w.shm_fd); // dup'ed: we keep ours for later ftruncates
    wl_i32(c, (i32)total);
    wl_end(c);
    return true;
  }
  if (ftruncate(w.shm_fd, (off_t)total) != 0) return false;
  void* m = mremap(w.shm_mem, w.shm_size, total, MREMAP_MAYMOVE);
  if (m == MAP_FAILED) return false;
  w.shm_mem  = (u8*)m;
  w.shm_size = total;
  wl_begin(c, w.pool, WL_SHM_POOL_RESIZE); wl_i32(c, (i32)total); wl_end(c);
  w.pool_grows++;
  return true;
}

static bool overlaps(usize a0, usize a1, usize b0, usize b1) { return a0 < b1 && b0 < a1; }

static bool create_buffer(Window& w, WindowBuffer& b, usize offset, i32 bw, i32 bh) {
  WlConn& c      = w.wl;
  usize   stride = (usize)bw * 4;
  usize   bytes  = stride * (usize)bh;
  if (!ensure_pool_size(w, offset + bytes)) return false;
  b.id = wl_new_id(c, IF_BUFFER);
  b.busy = b.stale = false;
  b.offset = offset; b.bytes = bytes; b.w = bw; b.h = bh;
  wl_begin(c, w.pool, WL_SHM_POOL_CREATE_BUFFER);
  wl_u32(c, b.id);
  wl_i32(c, (i32)offset);
  wl_i32(c, bw);
  wl_i32(c, bh);
  wl_i32(c, (i32)stride);
  wl_u32(c, WL_SHM_FORMAT_ARGB8888);
  wl_end(c);
  return true;
}

static WindowBuffer* acquire_buffer(Window& w, i32 bw, i32 bh) {
  usize need = (usize)bw * 4 * (usize)bh;
  if (need > w.slot_cap) w.slot_cap = need; // slots move to i * slot_cap from now on

  for (u32 i = 0; i < MX_ARRAY_COUNT(w.bufs); i++) {
    WindowBuffer& b = w.bufs[i];
    if (!b.id || b.busy || b.w != bw || b.h != bh) continue;
    WindowBuffer& ov = w.bufs[3];
    if (i < 3 && ov.id && !ov.busy) destroy_buffer(w, ov);
    return &b;
  }

  for (WindowBuffer& b : w.bufs) {
    if (!b.id || (b.w == bw && b.h == bh)) continue;
    if (b.busy) b.stale = true; else destroy_buffer(w, b);
  }

  for (u32 i = 0; i < MX_ARRAY_COUNT(w.bufs); i++) {
    WindowBuffer& b = w.bufs[i];
    if (b.id) continue;
    usize off = i * w.slot_cap;
    bool clash = false;
    for (WindowBuffer& o : w.bufs) if (o.id && overlaps(off, off + need, o.offset, o.offset + o.bytes)) { clash = true; break; }
    if (clash) continue;
    return create_buffer(w, b, off, bw, bh) ? &b : nullptr;
  }
  return nullptr;
}

static void ensure_pointer(Window& w, bool want) {
  WlConn& c = w.wl;
  if (want && !w.pointer) {
    w.pointer = wl_new_id(c, IF_POINTER);
    wl_begin(c, w.seat, WL_SEAT_GET_POINTER); wl_u32(c, w.pointer); wl_end(c);
    if (w.cursor_mgr) {
      w.cursor_device = wl_new_id(c, IF_CURSOR_SHAPE_DEVICE);
      wl_begin(c, w.cursor_mgr, WP_CURSOR_SHAPE_MGR_GET_POINTER);
      wl_u32(c, w.cursor_device); wl_u32(c, w.pointer); wl_end(c);
    }
  } else if (!want && w.pointer) {
    if (w.cursor_device) { wl_call0(c, w.cursor_device, WP_CURSOR_SHAPE_DEVICE_DESTROY); wl_forget_id(c, w.cursor_device); w.cursor_device = 0; }
    if (w.seat_version >= 3) wl_call0(c, w.pointer, WL_POINTER_RELEASE);
    wl_forget_id(c, w.pointer);
    w.pointer = 0;
    w.ptr_inside = false;
  }
}

static void ensure_keyboard(Window& w, bool want) {
  WlConn& c = w.wl;
  if (want && !w.keyboard) {
    w.keyboard = wl_new_id(c, IF_KEYBOARD);
    wl_begin(c, w.seat, WL_SEAT_GET_KEYBOARD); wl_u32(c, w.keyboard); wl_end(c);
  } else if (!want && w.keyboard) {
    if (w.seat_version >= 3) wl_call0(c, w.keyboard, WL_KEYBOARD_RELEASE);
    wl_forget_id(c, w.keyboard);
    w.keyboard = 0;
    w.focused = false;
  }
}

static void keymap_arrived(Window& w) {
  u64 h = 1469598103934665603ull;
  for (u32 i = 0; i < w.keymap_size; i++) { h ^= (u8)w.keymap[i]; h *= 1099511628211ull; }
  if (h != w.keymap_hash || !w.xkb.ok) {
    w.keymap_hash = h;
    u32 n = w.keymap_size;
    while (n && w.keymap[n - 1] == 0) n--;
    keymap_parse(w.xkb, Str(w.keymap, n));
  }
  if (!w.compose_tried) { w.compose_tried = true; compose_load_locale(w.compose); }
  compose_reset(w.compose_state);
  w.repeat_code = 0;
  push_event(w, WE_KEYMAP);
}

static u32 mod_bit_of(u32 ks) {
  if (ks == XKB_KEY_Shift_L || ks == XKB_KEY_Shift_R) return XKB_MOD_SHIFT;
  if (ks == XKB_KEY_Control_L || ks == XKB_KEY_Control_R) return XKB_MOD_CONTROL;
  if (ks == XKB_KEY_Alt_L || ks == XKB_KEY_Alt_R || ks == XKB_KEY_Meta_L || ks == XKB_KEY_Meta_R) return XKB_MOD_MOD1;
  return 0;
}

static void translate_key(Window& w, WEvent& e) {
  memcpy(e.mods, w.mods, sizeof e.mods);
  e.keysym = 0; e.consumed = 0; e.text_len = 0;
  if (!w.xkb.ok) return;
  u32 mods = w.mods[0] | w.mods[1] | w.mods[2];
  e.keysym = keymap_key_sym(w.xkb, e.code, mods, w.mods[3], &e.consumed);
  if (!e.pressed || !e.keysym || keysym_is_modifier(e.keysym)) return;
  u32 sym = e.keysym;
  const char* text = nullptr;
  switch (compose_feed(w.compose, w.compose_state, sym)) {
    case COMPOSE_NOTHING: break;
    case COMPOSE_COMPOSED: { u32 rs; text = compose_result(w.compose, w.compose_state, &rs); if (rs) sym = rs; break; }
    default: return; // composing or cancelled: nothing typed yet
  }
  if ((mods & XKB_MOD_CONTROL) && !(e.consumed & XKB_MOD_CONTROL)) return; // Ctrl+X is a shortcut, not text
  if (text && text[0]) {
    u32 n = (u32)strlen(text);
    if (n <= sizeof e.text) { memcpy(e.text, text, n); e.text_len = (u8)n; }
    return;
  }
  if (keysym_is_dead(sym)) return;
  u32 cp = keysym_to_utf32(sym);
  if (cp >= 0x20 && cp != 0x7f) e.text_len = (u8)utf8_encode(cp, e.text);
}

static void key_pressed(Window& w, u32 code, u32 serial) {
  w.repeat_code = 0;
  if (!w.focused || w.repeat_rate <= 0 || !keymap_key_repeats(w.xkb, code)) return;
  w.repeat_code    = code;
  w.repeat_serial  = serial;
  w.repeat_next_ms = window_now_ms() + w.repeat_delay;
}

static void pump_repeat(Window& w) {
  if (!w.repeat_code || !w.focused) return;
  i64 now = window_now_ms();
  if (now < w.repeat_next_ms) return;
  WEvent& e = push_event(w, WE_KEY);
  e.code = w.repeat_code; e.pressed = 1; e.repeat = 1; e.serial = w.repeat_serial; e.time = (u32)now;
  translate_key(w, e);
  i64 interval = mx_max((i64)1, 1000 / (i64)mx_max(w.repeat_rate, 1));
  w.repeat_next_ms = mx_max(w.repeat_next_ms + interval, now + interval / 2);
}

static void flush_scroll(Window& w) {
  if (!w.axis_pending) return;
  WEvent& e = push_event(w, WE_SCROLL);
  memcpy(e.mods, w.mods, sizeof e.mods); // Ctrl-wheel zooms, Shift-wheel scrolls sideways
  e.mods[0] |= w.key_mods;
  e.x = w.ptr_x; e.y = w.ptr_y;
  e.dx = w.axis_dx; e.dy = w.axis_dy;
  w.axis_dx = w.axis_dy = 0;
  w.axis_pending = false;
}

static void drag_end(Window& w) {
  WlConn& c = w.wl;
  if (w.drag_source) { wl_call0(c, w.drag_source, WL_DATA_SOURCE_DESTROY); wl_forget_id(c, w.drag_source); w.drag_source = 0; }
  if (w.drag_icon_viewport) { wl_call0(c, w.drag_icon_viewport, WP_VIEWPORT_DESTROY); wl_forget_id(c, w.drag_icon_viewport); w.drag_icon_viewport = 0; }
  if (w.drag_icon_buf) { wl_call0(c, w.drag_icon_buf, WL_BUFFER_DESTROY); wl_forget_id(c, w.drag_icon_buf); w.drag_icon_buf = 0; }
  if (w.drag_icon) { wl_call0(c, w.drag_icon, WL_SURFACE_DESTROY); wl_forget_id(c, w.drag_icon); w.drag_icon = 0; }
  if (w.drag_icon_pool) { wl_call0(c, w.drag_icon_pool, WL_SHM_POOL_DESTROY); wl_forget_id(c, w.drag_icon_pool); w.drag_icon_pool = 0; }
  if (w.drag_icon_mem) { munmap(w.drag_icon_mem, w.drag_icon_bytes); w.drag_icon_mem = nullptr; w.drag_icon_bytes = 0; }
  if (w.drag_icon_fd >= 0) { close(w.drag_icon_fd); w.drag_icon_fd = -1; }
  w.dragging = w.drag_dropped = w.drag_accepted = false; w.drag_action = 0;
}

static void on_registry_global(Window& w, u32 name, Str iface, u32 version) {
  WlConn& c = w.wl;
  u16 tag = IF_NONE; u32 want_ver = 1; u32* slot = nullptr;
  if      (str_eq(iface, "wl_compositor"))                 { tag = IF_COMPOSITOR;       want_ver = WL_VER_COMPOSITOR;   slot = &w.compositor; }
  else if (str_eq(iface, "wl_shm"))                        { tag = IF_SHM;              want_ver = WL_VER_SHM;          slot = &w.shm; }
  else if (str_eq(iface, "xdg_wm_base"))                   { tag = IF_XDG_WM_BASE;      want_ver = WL_VER_XDG_WM_BASE;  slot = &w.wm_base; }
  else if (str_eq(iface, "wl_seat"))                       { tag = IF_SEAT;             want_ver = WL_VER_SEAT;         slot = &w.seat; }
  else if (str_eq(iface, "wp_fractional_scale_manager_v1")){ tag = IF_FRACTIONAL_MGR;   want_ver = WL_VER_FRACTIONAL;   slot = &w.fractional_mgr; }
  else if (str_eq(iface, "wp_viewporter"))                 { tag = IF_VIEWPORTER;       want_ver = WL_VER_VIEWPORTER;   slot = &w.viewporter; }
  else if (str_eq(iface, "wp_cursor_shape_manager_v1"))    { tag = IF_CURSOR_SHAPE_MGR; want_ver = WL_VER_CURSOR_SHAPE; slot = &w.cursor_mgr; }
  else if (str_eq(iface, "wl_data_device_manager"))        { tag = IF_DATA_DEVICE_MGR;  want_ver = WL_VER_DATA_DEVICE_MGR; slot = &w.data_mgr; }
  if (!slot || *slot) return; // not interesting, or already bound (first seat wins)

  u32 ver = mx_min(want_ver, version);
  u32 id  = wl_new_id(c, tag);
  wl_begin(c, w.registry, WL_REGISTRY_BIND);
  wl_u32(c, name);
  wl_str(c, iface);
  wl_u32(c, ver);
  wl_u32(c, id);
  wl_end(c);
  *slot = id;
  if (tag == IF_COMPOSITOR) w.compositor_version = ver;
  if (tag == IF_SEAT) w.seat_version = ver;
  if (tag == IF_DATA_DEVICE_MGR) w.data_mgr_version = ver;
}

static void window_handler(void* ctx, u32 object, u16 iface, u16 op, WlReader& rd) {
  Window& w = *(Window*)ctx;
  WlConn& c = w.wl;

  switch (iface) {
    case IF_REGISTRY:
      if (op == WL_REGISTRY_EV_GLOBAL) {
        u32 name = rd.u32v(); Str name_s = rd.str(); u32 ver = rd.u32v();
        if (rd.ok) on_registry_global(w, name, name_s, ver);
      }
      break;

    case IF_CALLBACK:
      if (object == w.frame_cb) { w.frame_pending = false; w.frame_cb = 0; }
      break;

    case IF_BUFFER:
      if (op == WL_BUFFER_EV_RELEASE)
        for (WindowBuffer& b : w.bufs) if (b.id == object) { b.busy = false; if (b.stale) destroy_buffer(w, b); }
      break;

    case IF_XDG_WM_BASE:
      if (op == XDG_WM_BASE_EV_PING) { u32 serial = rd.u32v(); wl_begin(c, w.wm_base, XDG_WM_BASE_PONG); wl_u32(c, serial); wl_end(c); }
      break;

    case IF_XDG_TOPLEVEL:
      if (op == XDG_TOPLEVEL_EV_CONFIGURE) {
        i32 tw = rd.i32v(), th = rd.i32v();
        Str states = rd.array();
        if (!rd.ok) break;
        if (tw > 0 && th > 0) { w.logical_w = tw; w.logical_h = th; }
        w.maximized = w.fullscreen = w.activated = w.resizing = false;
        for (u32 i = 0; i + 4 <= states.n; i += 4) {
          u32 s; memcpy(&s, states.p + i, 4);
          if (s == XDG_TOPLEVEL_STATE_MAXIMIZED)  w.maximized  = true;
          if (s == XDG_TOPLEVEL_STATE_FULLSCREEN) w.fullscreen = true;
          if (s == XDG_TOPLEVEL_STATE_RESIZING)   w.resizing   = true;
          if (s == XDG_TOPLEVEL_STATE_ACTIVATED)  w.activated  = true;
        }
      } else if (op == XDG_TOPLEVEL_EV_CLOSE) {
        w.closed = true;
        push_event(w, WE_CLOSE);
      }
      break;

    case IF_XDG_SURFACE:
      if (op == XDG_SURFACE_EV_CONFIGURE) {

        w.ack_serial  = rd.u32v();
        w.ack_pending = true;
        w.configures++;
        if (w.logical_w <= 0) { w.logical_w = w.want_w; w.logical_h = w.want_h; }
        w.configured = true;
        w.need_redraw = true;
        recompute_buffer_size(w);
      }
      break;

    case IF_FRACTIONAL:
      if (op == WP_FRACTIONAL_EV_PREFERRED_SCALE) {
        u32 s = rd.u32v();
        if (s && s != w.scale120) { w.scale120 = s; push_event(w, WE_SCALE); recompute_buffer_size(w); w.need_redraw = true; }
      }
      break;

    case IF_SURFACE:
      if (op == WL_SURFACE_EV_PREFERRED_BUFFER_SCALE && !w.fractional_mgr) {
        i32 f = rd.i32v();
        if (f > 0 && f != w.int_scale) { w.int_scale = f; push_event(w, WE_SCALE); recompute_buffer_size(w); w.need_redraw = true; }
      }
      break;

    case IF_SEAT:
      if (op == WL_SEAT_EV_CAPABILITIES) {
        u32 caps = rd.u32v();
        ensure_pointer(w, caps & WL_SEAT_CAP_POINTER);
        ensure_keyboard(w, caps & WL_SEAT_CAP_KEYBOARD);
      }
      break;

    case IF_POINTER: {
      float s = px_scale(w);
      switch (op) {
        case WL_POINTER_EV_ENTER: {
          u32 serial = rd.u32v(); rd.u32v();
          w.ptr_x = (float)rd.fixed() * s; w.ptr_y = (float)rd.fixed() * s;
          w.ptr_inside = true; w.ptr_enter_serial = serial; w.cursor_shape = 0;
          window_set_cursor(w, CURSOR_DEFAULT);
          WEvent& e = push_event(w, WE_POINTER_ENTER); e.x = w.ptr_x; e.y = w.ptr_y; e.serial = serial;
          break;
        }
        case WL_POINTER_EV_LEAVE:
          w.ptr_inside = false;
          push_event(w, WE_POINTER_LEAVE);
          break;
        case WL_POINTER_EV_MOTION: {
          u32 t = rd.u32v();
          w.ptr_x = (float)rd.fixed() * s; w.ptr_y = (float)rd.fixed() * s;
          WEvent& e = push_event(w, WE_POINTER_MOTION); e.x = w.ptr_x; e.y = w.ptr_y; e.time = t;
          break;
        }
        case WL_POINTER_EV_BUTTON: {
          WEvent& e = push_event(w, WE_POINTER_BUTTON);
          e.serial = rd.u32v(); e.time = rd.u32v(); e.code = rd.u32v(); e.pressed = rd.u32v() == 1;
          e.x = w.ptr_x; e.y = w.ptr_y;
          memcpy(e.mods, w.mods, sizeof e.mods);
          e.mods[0] |= w.key_mods; // ... and what the modifier keys themselves said
          if (e.pressed) { w.input_serial = e.serial; w.pointer_serial = e.serial; }
          break;
        }
        case WL_POINTER_EV_AXIS: {
          rd.u32v(); u32 axis = rd.u32v(); float v = (float)rd.fixed() * s;
          if (axis == WL_POINTER_AXIS_VERTICAL) w.axis_dy += v; else w.axis_dx += v;
          w.axis_pending = true;
          if (w.seat_version < 5) flush_scroll(w); // no frame events before v5
          break;
        }
        case WL_POINTER_EV_FRAME:
          flush_scroll(w);
          break;
        default: break;
      }
      break;
    }

    case IF_KEYBOARD:
      switch (op) {
        case WL_KEYBOARD_EV_KEYMAP: {
          u32 format = rd.u32v(); int fd = rd.fd(); u32 size = rd.u32v();
          if (fd >= 0) {
            if (w.keymap) { munmap(w.keymap, w.keymap_size); w.keymap = nullptr; w.keymap_size = 0; }
            if (format == WL_KEYBOARD_KEYMAP_XKB_V1 && size) {
              void* m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
              if (m != MAP_FAILED) { w.keymap = (char*)m; w.keymap_size = size; keymap_arrived(w); }
            }
            close(fd);
          }
          break;
        }
        case WL_KEYBOARD_EV_ENTER: {

          rd.u32v(); rd.u32v();
          Str keys = rd.array();
          w.focused = true; w.repeat_code = 0; w.key_mods = 0;
          if (w.xkb.ok) for (u32 i = 0; i + 4 <= keys.n; i += 4) {
            u32 code; memcpy(&code, keys.p + i, 4);
            u32 ks = keymap_key_sym(w.xkb, code, 0, w.mods[3]);
            w.key_mods |= mod_bit_of(ks);
          }
          compose_reset(w.compose_state);
          push_event(w, WE_FOCUS_IN);
          break;
        }
        case WL_KEYBOARD_EV_LEAVE: w.focused = false; w.repeat_code = 0; w.key_mods = 0; compose_reset(w.compose_state); push_event(w, WE_FOCUS_OUT); break;
        case WL_KEYBOARD_EV_KEY: {
          WEvent& e = push_event(w, WE_KEY);
          e.serial = rd.u32v(); e.time = rd.u32v(); e.code = rd.u32v(); e.pressed = rd.u32v() == 1;
          translate_key(w, e);
          {
            u32 bit = mod_bit_of(e.keysym);
            if (bit) { if (e.pressed) w.key_mods |= bit; else w.key_mods &= ~bit; }
          }
          if (e.pressed) { w.input_serial = e.serial; key_pressed(w, e.code, e.serial); }
          else if (e.code == w.repeat_code) w.repeat_code = 0;
          break;
        }
        case WL_KEYBOARD_EV_MODIFIERS: {
          rd.u32v();
          for (u32 i = 0; i < 4; i++) w.mods[i] = rd.u32v();
          WEvent& e = push_event(w, WE_MODIFIERS);
          memcpy(e.mods, w.mods, sizeof e.mods);
          break;
        }
        case WL_KEYBOARD_EV_REPEAT_INFO: w.repeat_rate = rd.i32v(); w.repeat_delay = rd.i32v(); break;
        default: break;
      }
      break;

    case IF_DATA_DEVICE:
      switch (op) {
        case WL_DATA_DEVICE_EV_DATA_OFFER: {
          u32 id = rd.u32v();
          wl_bind_server_id(c, id, IF_DATA_OFFER);
          w.pending_offer = id; w.pending_mimes = 0;
          break;
        }
        case WL_DATA_DEVICE_EV_SELECTION: {
          u32 id = rd.u32v();
          if (w.sel_offer && w.sel_offer != id) { wl_call0(c, w.sel_offer, WL_DATA_OFFER_DESTROY); wl_bind_server_id(c, w.sel_offer, IF_NONE); }
          w.sel_offer = id;
          w.sel_mimes = (id && id == w.pending_offer) ? w.pending_mimes : 0;
          if (id == w.pending_offer) w.pending_offer = 0;
          w.sel_ours = id != 0 && w.data_source != 0;
          push_event(w, WE_SELECTION);
          break;
        }
        case WL_DATA_DEVICE_EV_ENTER: { // something dragged over us
          u32 serial = rd.u32v(); rd.u32v();
          float s = px_scale(w);
          float x = (float)rd.fixed() * s, y = (float)rd.fixed() * s;
          u32 id = rd.u32v();
          if (w.dnd_offer && w.dnd_offer != id) { wl_call0(c, w.dnd_offer, WL_DATA_OFFER_DESTROY); wl_bind_server_id(c, w.dnd_offer, IF_NONE); }
          w.dnd_offer = id;
          w.dnd_mimes = (id && id == w.pending_offer) ? w.pending_mimes : 0;
          if (id == w.pending_offer) w.pending_offer = 0;
          w.dnd_serial = serial; w.dnd_x = x; w.dnd_y = y; w.dnd_inside = true; w.dnd_dropped = false; w.dnd_action = 0;
          WEvent& e = push_event(w, WE_DND_ENTER); e.x = x; e.y = y; e.serial = serial;
          break;
        }
        case WL_DATA_DEVICE_EV_MOTION: {
          rd.u32v();
          float s = px_scale(w);
          w.dnd_x = (float)rd.fixed() * s; w.dnd_y = (float)rd.fixed() * s;
          WEvent& e = push_event(w, WE_DND_MOTION); e.x = w.dnd_x; e.y = w.dnd_y;
          break;
        }
        case WL_DATA_DEVICE_EV_LEAVE:
          w.dnd_inside = false;
          if (w.dnd_offer && !w.dnd_dropped) { wl_call0(c, w.dnd_offer, WL_DATA_OFFER_DESTROY); wl_bind_server_id(c, w.dnd_offer, IF_NONE); w.dnd_offer = 0; }
          push_event(w, WE_DND_LEAVE);
          break;
        case WL_DATA_DEVICE_EV_DROP:
          w.dnd_inside = false; w.dnd_dropped = true;
          push_event(w, WE_DND_DROP);
          break;
        default: break;
      }
      break;

    case IF_DATA_OFFER:
      if (op == WL_DATA_OFFER_EV_OFFER) {
        Str mime = rd.str();
        u32 bit = str_eq(mime, "text/uri-list") ? WCLIP_URI_LIST : str_eq(mime, "x-special/gnome-copied-files") ? WCLIP_GNOME_FILES
                : (str_eq(mime, "text/plain") || str_eq(mime, "text/plain;charset=utf-8") || str_eq(mime, "UTF8_STRING")) ? WCLIP_TEXT : 0;
        if (object == w.pending_offer) w.pending_mimes |= bit;
        else if (object == w.sel_offer) w.sel_mimes |= bit;
        else if (object == w.dnd_offer) w.dnd_mimes |= bit;
      } else if (op == WL_DATA_OFFER_EV_SOURCE_ACTIONS) { u32 acts = rd.u32v(); if (object == w.dnd_offer) w.dnd_source_actions = acts; }
      else if (op == WL_DATA_OFFER_EV_ACTION) { u32 act = rd.u32v(); if (object == w.dnd_offer) w.dnd_action = act; }
      break;

    case IF_DATA_SOURCE:
      switch (op) {
        case WL_DATA_SOURCE_EV_SEND: {
          Str mime = rd.str(); int fd = rd.fd();
          if (fd < 0) break;
          if (object != w.data_source && object != w.drag_source) { close(fd); break; } // a source we already dropped
          WEvent& e = push_event(w, WE_DATA_SEND);
          e.code = (u32)fd; e.aux = object == w.drag_source ? 1 : 0;
          snprintf(w.send_mime, sizeof w.send_mime, "%.*s", (int)mx_min(mime.n, (u32)sizeof w.send_mime - 1), mime.p);
          break;
        }
        case WL_DATA_SOURCE_EV_TARGET: { Str mime = rd.str(); if (object == w.drag_source) w.drag_accepted = mime.n > 0; break; }
        case WL_DATA_SOURCE_EV_ACTION: { u32 act = rd.u32v(); if (object == w.drag_source) w.drag_action = act; break; }
        case WL_DATA_SOURCE_EV_DND_DROP_PERFORMED: if (object == w.drag_source) w.drag_dropped = true; break;
        case WL_DATA_SOURCE_EV_DND_FINISHED:
          if (object == w.drag_source) { u32 act = w.drag_action; drag_end(w); WEvent& e = push_event(w, WE_DRAG_END); e.aux = 1; e.code = act; }
          break;
        case WL_DATA_SOURCE_EV_CANCELLED:
          if (object == w.data_source) {
            wl_call0(c, w.data_source, WL_DATA_SOURCE_DESTROY); wl_forget_id(c, w.data_source);
            w.data_source = 0; w.sel_ours = false;
            push_event(w, WE_DATA_CANCELLED);
          } else if (object == w.drag_source) { drag_end(w); WEvent& e = push_event(w, WE_DRAG_END); e.aux = 0; e.code = 0; }
          else { wl_call0(c, object, WL_DATA_SOURCE_DESTROY); wl_forget_id(c, object); }
          break;
        default: break;
      }
      break;

    default: break;
  }
}

bool window_open(Window& w, i32 width, i32 height, const char* title, const char* app_id) {
  WlConn& c = w.wl;
  if (!wl_connect(c)) return false;
  w.want_w = width; w.want_h = height;

  w.registry = wl_new_id(c, IF_REGISTRY);
  wl_begin(c, 1, WL_DISPLAY_GET_REGISTRY); wl_u32(c, w.registry); wl_end(c);
  if (!wl_roundtrip(c, window_handler, &w)) return false; // globals
  if (!wl_roundtrip(c, window_handler, &w)) return false; // seat capabilities, shm formats

  if (!w.compositor || !w.shm || !w.wm_base) {
    snprintf(c.err, sizeof c.err, "compositor lacks %s", !w.compositor ? "wl_compositor" : !w.shm ? "wl_shm" : "xdg_wm_base");
    return false;
  }

  w.surface = wl_new_id(c, IF_SURFACE);
  wl_begin(c, w.compositor, WL_COMPOSITOR_CREATE_SURFACE); wl_u32(c, w.surface); wl_end(c);

  if (w.data_mgr && w.seat) {
    w.data_device = wl_new_id(c, IF_DATA_DEVICE);
    wl_begin(c, w.data_mgr, WL_DATA_DEVICE_MGR_GET_DATA_DEVICE); wl_u32(c, w.data_device); wl_u32(c, w.seat); wl_end(c);
  }

  if (w.fractional_mgr) {
    w.fractional = wl_new_id(c, IF_FRACTIONAL);
    wl_begin(c, w.fractional_mgr, WP_FRACTIONAL_MGR_GET_FRACTIONAL_SCALE); wl_u32(c, w.fractional); wl_u32(c, w.surface); wl_end(c);
  }
  if (w.viewporter) {
    w.viewport = wl_new_id(c, IF_VIEWPORT);
    wl_begin(c, w.viewporter, WP_VIEWPORTER_GET_VIEWPORT); wl_u32(c, w.viewport); wl_u32(c, w.surface); wl_end(c);
  }

  w.xdg_surface = wl_new_id(c, IF_XDG_SURFACE);
  wl_begin(c, w.wm_base, XDG_WM_BASE_GET_XDG_SURFACE); wl_u32(c, w.xdg_surface); wl_u32(c, w.surface); wl_end(c);
  w.toplevel = wl_new_id(c, IF_XDG_TOPLEVEL);
  wl_begin(c, w.xdg_surface, XDG_SURFACE_GET_TOPLEVEL); wl_u32(c, w.toplevel); wl_end(c);

  wl_begin(c, w.toplevel, XDG_TOPLEVEL_SET_TITLE);  wl_str(c, title);  wl_end(c);
  wl_begin(c, w.toplevel, XDG_TOPLEVEL_SET_APP_ID); wl_str(c, app_id); wl_end(c);
  wl_begin(c, w.toplevel, XDG_TOPLEVEL_SET_MIN_SIZE); wl_i32(c, 320); wl_i32(c, 240); wl_end(c);
  wl_call0(c, w.surface, WL_SURFACE_COMMIT); // no buffer yet: asks for the first configure

  while (!w.configured) {
    if (!wl_wait_dispatch(c, window_handler, &w, 5000)) return false;
    if (c.dead) return false;
  }
  return true;
}

void window_close(Window& w) {
  WlConn& c = w.wl;
  if (c.fd >= 0 && !c.dead) {
    destroy_pool(w);
    if (w.frame_cb) w.frame_cb = 0;
    window_clip_release(w);
    drag_end(w);
    if (w.dnd_offer) { wl_call0(c, w.dnd_offer, WL_DATA_OFFER_DESTROY); w.dnd_offer = 0; }
    if (w.sel_offer) { wl_call0(c, w.sel_offer, WL_DATA_OFFER_DESTROY); w.sel_offer = 0; }
    if (w.data_device) { if (w.data_mgr_version >= 2) wl_call0(c, w.data_device, WL_DATA_DEVICE_RELEASE); w.data_device = 0; }
    ensure_pointer(w, false);
    ensure_keyboard(w, false);
    if (w.fractional) wl_call0(c, w.fractional, WP_FRACTIONAL_DESTROY);
    if (w.viewport)   wl_call0(c, w.viewport, WP_VIEWPORT_DESTROY);
    if (w.toplevel)   wl_call0(c, w.toplevel, XDG_TOPLEVEL_DESTROY);
    if (w.xdg_surface) wl_call0(c, w.xdg_surface, XDG_SURFACE_DESTROY);
    if (w.surface)    wl_call0(c, w.surface, WL_SURFACE_DESTROY);
    wl_flush(c);
  }
  if (w.keymap) { munmap(w.keymap, w.keymap_size); w.keymap = nullptr; w.keymap_size = 0; }
  wl_disconnect(c);
}

bool window_add_fd(Window& w, int fd, short events) {
  if (w.num_fds >= MX_ARRAY_COUNT(w.fds)) return false;
  w.fds[w.num_fds++] = { fd, events };
  return true;
}

void window_remove_fd(Window& w, int fd) {
  for (u32 i = 0; i < w.num_fds; i++)
    if (w.fds[i].fd == fd) { w.fds[i] = w.fds[w.num_fds - 1]; w.num_fds--; return; }
}

bool window_pump(Window& w, int timeout_ms) {
  w.events.clear();
  if (w.wl.dead) return false;
  if (w.need_redraw && !w.frame_pending) timeout_ms = 0; // a frame is waiting to be drawn: don't block
  if (w.repeat_code && w.focused) {
    i64 wait = w.repeat_next_ms - window_now_ms();
    int t = (int)mx_clamp(wait, (i64)0, (i64)1000000);
    if (timeout_ms < 0 || t < timeout_ms) timeout_ms = t;
  }
  struct pollfd extra[MX_ARRAY_COUNT(w.fds)];
  for (u32 i = 0; i < w.num_fds; i++) extra[i] = { w.fds[i].fd, w.fds[i].events, 0 };
  if (!wl_wait_dispatch_fds(w.wl, window_handler, &w, timeout_ms, extra, w.num_fds)) return false;
  for (u32 i = 0; i < w.num_fds; i++) {
    if (!extra[i].revents) continue;
    WEvent& e = push_event(w, WE_FD);
    e.code = (u32)extra[i].fd;
    e.aux  = (u32)(u16)extra[i].revents;
  }
  pump_repeat(w);
  return !w.closed;
}

bool window_acquire(Window& w, Canvas& out) {
  if (!w.configured || w.wl.dead) return false;
  WindowBuffer* b = acquire_buffer(w, w.buf_w, w.buf_h);
  if (!b) return false;
  w.current_buf = (u32)(b - w.bufs);
  out.px     = (u32*)(w.shm_mem + b->offset);
  out.w      = w.buf_w;
  out.h      = w.buf_h;
  out.stride = w.buf_w;
  canvas_reset_clip(out);
  return true;
}

void window_present(Window& w, i32 dx, i32 dy, i32 dw, i32 dh) {
  WlConn& c = w.wl;
  WindowBuffer& b = w.bufs[w.current_buf];
  b.busy = true;

  if (w.ack_pending) {
    wl_begin(c, w.xdg_surface, XDG_SURFACE_ACK_CONFIGURE); wl_u32(c, w.ack_serial); wl_end(c);
    w.ack_pending = false;
  }
  wl_begin(c, w.surface, WL_SURFACE_ATTACH); wl_u32(c, b.id); wl_i32(c, 0); wl_i32(c, 0); wl_end(c);
  if (w.viewport) {
    wl_begin(c, w.viewport, WP_VIEWPORT_SET_DESTINATION); wl_i32(c, w.logical_w); wl_i32(c, w.logical_h); wl_end(c);
  } else if (w.compositor_version >= 3) {
    wl_begin(c, w.surface, WL_SURFACE_SET_BUFFER_SCALE); wl_i32(c, w.int_scale); wl_end(c);
  }
  if (w.compositor_version >= 4) {
    wl_begin(c, w.surface, WL_SURFACE_DAMAGE_BUFFER); wl_i32(c, dx); wl_i32(c, dy); wl_i32(c, dw); wl_i32(c, dh); wl_end(c);
  } else {
    wl_begin(c, w.surface, WL_SURFACE_DAMAGE); wl_i32(c, 0); wl_i32(c, 0); wl_i32(c, 1 << 30); wl_i32(c, 1 << 30); wl_end(c);
  }
  w.frame_cb = wl_new_id(c, IF_CALLBACK);
  wl_begin(c, w.surface, WL_SURFACE_FRAME); wl_u32(c, w.frame_cb); wl_end(c);
  w.frame_pending = true;
  wl_call0(c, w.surface, WL_SURFACE_COMMIT);
  w.need_redraw = false;
  wl_flush(c);
}

void window_set_cursor(Window& w, u32 shape) {
  if (!w.cursor_device || !w.ptr_inside || shape == w.cursor_shape) return;
  wl_begin(w.wl, w.cursor_device, WP_CURSOR_SHAPE_DEVICE_SET_SHAPE);
  wl_u32(w.wl, w.ptr_enter_serial);
  wl_u32(w.wl, shape);
  wl_end(w.wl);
  w.cursor_shape = shape;
}

void window_set_title(Window& w, const char* title) {
  wl_begin(w.wl, w.toplevel, XDG_TOPLEVEL_SET_TITLE); wl_str(w.wl, title); wl_end(w.wl);
}

static const char* const kClipMimes[] = { "text/uri-list", "x-special/gnome-copied-files", "text/plain;charset=utf-8", "text/plain" };
static const u32 kClipBits[] = { WCLIP_URI_LIST, WCLIP_GNOME_FILES, WCLIP_TEXT, WCLIP_TEXT };

bool window_clip_offer(Window& w, u32 mimes) {
  WlConn& c = w.wl;
  if (!w.data_device || c.dead || !w.input_serial) return false;
  u32 old = w.data_source;
  w.data_source = wl_new_id(c, IF_DATA_SOURCE);
  wl_begin(c, w.data_mgr, WL_DATA_DEVICE_MGR_CREATE_DATA_SOURCE); wl_u32(c, w.data_source); wl_end(c);
  for (u32 i = 0; i < MX_ARRAY_COUNT(kClipMimes); i++)
    if (mimes & kClipBits[i]) { wl_begin(c, w.data_source, WL_DATA_SOURCE_OFFER); wl_str(c, kClipMimes[i]); wl_end(c); }
  wl_begin(c, w.data_device, WL_DATA_DEVICE_SET_SELECTION); wl_u32(c, w.data_source); wl_u32(c, w.input_serial); wl_end(c);
  if (old) { wl_call0(c, old, WL_DATA_SOURCE_DESTROY); wl_forget_id(c, old); }
  w.sel_ours = true;
  wl_flush(c);
  return true;
}

int window_clip_receive(Window& w, u32 mime) {
  WlConn& c = w.wl;
  if (!w.sel_offer || w.sel_ours || c.dead || !(w.sel_mimes & mime)) return -1;
  const char* name = nullptr;
  for (u32 i = 0; i < MX_ARRAY_COUNT(kClipMimes) && !name; i++) if (kClipBits[i] == mime) name = kClipMimes[i];
  if (!name) return -1;
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0) return -1;
  wl_begin(c, w.sel_offer, WL_DATA_OFFER_RECEIVE); wl_str(c, name); wl_fd(c, p[1]); wl_end(c);
  wl_flush(c);
  close(p[1]);
  return p[0];
}

void window_clip_release(Window& w) {
  WlConn& c = w.wl;
  if (!w.data_source || c.dead) { w.data_source = 0; return; }
  wl_call0(c, w.data_source, WL_DATA_SOURCE_DESTROY); wl_forget_id(c, w.data_source);
  w.data_source = 0; w.sel_ours = false;
}

bool window_drag_start(Window& w, u32 mimes, u32 actions, const u32* icon_px, i32 icon_w, i32 icon_h) {
  WlConn& c = w.wl;
  if (!w.data_device || c.dead || !w.pointer_serial || w.dragging) return false;
  drag_end(w);
  w.drag_source = wl_new_id(c, IF_DATA_SOURCE);
  wl_begin(c, w.data_mgr, WL_DATA_DEVICE_MGR_CREATE_DATA_SOURCE); wl_u32(c, w.drag_source); wl_end(c);
  for (u32 i = 0; i < MX_ARRAY_COUNT(kClipMimes); i++)
    if (mimes & kClipBits[i]) { wl_begin(c, w.drag_source, WL_DATA_SOURCE_OFFER); wl_str(c, kClipMimes[i]); wl_end(c); }
  if (w.data_mgr_version >= 3) { wl_begin(c, w.drag_source, WL_DATA_SOURCE_SET_ACTIONS); wl_u32(c, actions); wl_end(c); }

  if (icon_px && icon_w > 0 && icon_h > 0) {
    usize bytes = (usize)icon_w * icon_h * 4;
    w.drag_icon_fd = memfd_create("mattexplorer-drag", MFD_CLOEXEC);
    if (w.drag_icon_fd >= 0 && ftruncate(w.drag_icon_fd, (off_t)bytes) == 0) {
      w.drag_icon_mem = (u8*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, w.drag_icon_fd, 0);
      if (w.drag_icon_mem == MAP_FAILED) w.drag_icon_mem = nullptr;
    }
    if (w.drag_icon_mem) {
      w.drag_icon_bytes = bytes;
      memcpy(w.drag_icon_mem, icon_px, bytes);
      w.drag_icon_pool = wl_new_id(c, IF_SHM_POOL);
      wl_begin(c, w.shm, WL_SHM_CREATE_POOL); wl_u32(c, w.drag_icon_pool); wl_fd(c, w.drag_icon_fd); wl_i32(c, (i32)bytes); wl_end(c);
      w.drag_icon_buf = wl_new_id(c, IF_BUFFER);
      wl_begin(c, w.drag_icon_pool, WL_SHM_POOL_CREATE_BUFFER); wl_u32(c, w.drag_icon_buf); wl_i32(c, 0); wl_i32(c, icon_w); wl_i32(c, icon_h); wl_i32(c, icon_w * 4); wl_u32(c, WL_SHM_FORMAT_ARGB8888); wl_end(c);
      w.drag_icon = wl_new_id(c, IF_SURFACE);
      wl_begin(c, w.compositor, WL_COMPOSITOR_CREATE_SURFACE); wl_u32(c, w.drag_icon); wl_end(c);
      if (w.viewporter) {
        float s = px_scale(w);
        w.drag_icon_viewport = wl_new_id(c, IF_VIEWPORT);
        wl_begin(c, w.viewporter, WP_VIEWPORTER_GET_VIEWPORT); wl_u32(c, w.drag_icon_viewport); wl_u32(c, w.drag_icon); wl_end(c);
        wl_begin(c, w.drag_icon_viewport, WP_VIEWPORT_SET_DESTINATION); wl_i32(c, mx_max(1, (i32)((float)icon_w / s + 0.5f))); wl_i32(c, mx_max(1, (i32)((float)icon_h / s + 0.5f))); wl_end(c);
      }
    } else { if (w.drag_icon_fd >= 0) { close(w.drag_icon_fd); w.drag_icon_fd = -1; } }
  }
  wl_begin(c, w.data_device, WL_DATA_DEVICE_START_DRAG);
  wl_u32(c, w.drag_source); wl_u32(c, w.surface); wl_u32(c, w.drag_icon); wl_u32(c, w.pointer_serial);
  wl_end(c);
  if (w.drag_icon) {
    wl_begin(c, w.drag_icon, WL_SURFACE_ATTACH); wl_u32(c, w.drag_icon_buf); wl_i32(c, 0); wl_i32(c, 0); wl_end(c);
    if (w.compositor_version >= 4) { wl_begin(c, w.drag_icon, WL_SURFACE_DAMAGE_BUFFER); wl_i32(c, 0); wl_i32(c, 0); wl_i32(c, icon_w); wl_i32(c, icon_h); wl_end(c); }
    else { wl_begin(c, w.drag_icon, WL_SURFACE_DAMAGE); wl_i32(c, 0); wl_i32(c, 0); wl_i32(c, icon_w); wl_i32(c, icon_h); wl_end(c); }
    wl_call0(c, w.drag_icon, WL_SURFACE_COMMIT);
  }
  w.dragging = true; w.drag_dropped = w.drag_accepted = false; w.drag_action = 0;
  wl_flush(c);
  return true;
}

void window_dnd_accept(Window& w, u32 mime, u32 actions, u32 preferred) {
  WlConn& c = w.wl;
  if (!w.dnd_offer || c.dead) return;
  const char* name = nullptr;
  for (u32 i = 0; i < MX_ARRAY_COUNT(kClipMimes) && !name; i++) if (mime && kClipBits[i] == mime && (w.dnd_mimes & mime)) name = kClipMimes[i];
  wl_begin(c, w.dnd_offer, WL_DATA_OFFER_ACCEPT); wl_u32(c, w.dnd_serial);
  if (name) wl_str(c, name); else wl_u32(c, 0); // a null string: nothing accepted
  wl_end(c);
  if (w.data_mgr_version >= 3) { wl_begin(c, w.dnd_offer, WL_DATA_OFFER_SET_ACTIONS); wl_u32(c, name ? actions : 0); wl_u32(c, name ? preferred : 0); wl_end(c); }
  wl_flush(c);
}

int window_dnd_receive(Window& w, u32 mime) {
  WlConn& c = w.wl;
  if (!w.dnd_offer || c.dead || !(w.dnd_mimes & mime)) return -1;
  const char* name = nullptr;
  for (u32 i = 0; i < MX_ARRAY_COUNT(kClipMimes) && !name; i++) if (kClipBits[i] == mime) name = kClipMimes[i];
  int p[2];
  if (!name || pipe2(p, O_CLOEXEC) != 0) return -1;
  wl_begin(c, w.dnd_offer, WL_DATA_OFFER_RECEIVE); wl_str(c, name); wl_fd(c, p[1]); wl_end(c);
  wl_flush(c);
  close(p[1]);
  return p[0];
}

void window_dnd_finish(Window& w, bool accepted) {
  WlConn& c = w.wl;
  if (!w.dnd_offer || c.dead) { w.dnd_offer = 0; return; }
  if (accepted && w.dnd_dropped && w.data_mgr_version >= 3 && w.dnd_action != 0) wl_call0(c, w.dnd_offer, WL_DATA_OFFER_FINISH);
  wl_call0(c, w.dnd_offer, WL_DATA_OFFER_DESTROY);
  wl_bind_server_id(c, w.dnd_offer, IF_NONE);
  w.dnd_offer = 0; w.dnd_mimes = 0; w.dnd_dropped = false; w.dnd_inside = false;
  wl_flush(c);
}
