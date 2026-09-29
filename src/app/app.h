#pragma once

#include "base/types.h"
#include "base/array.h"
#include "core/dir_list.h"
#include "core/sort.h"
#include "core/fuzzy.h"
#include "core/mounts.h"
#include "core/fileops.h"
#include "core/watch.h"
#include "core/desktop.h"
#include "gfx/font.h"
#include "gfx/font_db.h"
#include "gfx/thumb.h"
#include "platform/window.h"
#include "platform/worker.h"
#include "ui/ui.h"
#include "ui/theme.h"

#define MX_VERSION "0.0.19"

enum ViewMode : u8 { VIEW_DETAILS = 0, VIEW_LIST, VIEW_ICONS };
enum { MAX_TABS = 16, MAX_PLACES = 16, MAX_PINS = 32, HISTORY_CAP = 64, MAX_UNDO = 16, STAT_POOL = 8, MAX_MENU_ITEMS = 20 };

struct Place { char label[48]; char path[512]; u8 icon; };

struct Tab {
  bool        used = false;
  char        path[4096] = {};
  int         dirfd = -1;
  DirListing  listing;
  Array<u32>  order; // sorted permutation of listing indices
  Array<u32>  rows;
  u32         hidden_count = 0; // dotfiles in the listing
  SortSpec    sort;
  ViewMode    view = VIEW_DETAILS;
  Array<u8>   selected; // per listing entry
  u32         selected_count = 0;
  i32         cursor = -1, anchor = -1;
  i32         scroll = 0, scroll_x = 0; // pixels
  i32         first_vis = 0;
  bool        keep_first_vis = false;
  bool        restore_scroll = false;

  UiTextInput filter;
  bool        filtering = false;
  Array<FuzzyHit> hits;

  Array<char> hist_buf;
  Array<u32>  hist_off;
  u32         hist_pos = 0;

  float       max_name_w = 0, max_name_px = 0;
  char        err[256] = {};
  i64         list_ns = 0, sort_ns = 0;

  u32         gen = 1;
  i32         watch = -1; // index into App::watcher.watches
  i32         renaming = -1; // listing index being renamed inline, -1 none
  bool        stat_all = false;
  u32         stat_all_cursor = 0, stat_pending = 0;

  Array<u8>   thumb_state;
  u32         thumb_gen = 0;

  Array<char> trash_orig; Array<u32> trash_orig_off; Array<i64> trash_deleted;
  bool        trash_cols = false;
};

struct Layout {
  float s = 1;
  i32   pad = 0, sb_w = 0, row_h = 1, icon_sz = 0;
  bool  sidebar = true;
  bool  sidebar_fits = true;
  Rect  side = {}, side_footer = {}, content = {}, tabs = {}, crumbs = {}, header = {}, list = {}, status = {}, ops = {};

  bool  col_date = true, col_type = true;
  i32   name_x = 0, name_w = 0, size_x = 0, size_w = 0, date_x = 0, date_w = 0, type_x = 0, type_w = 0;

  i32   lcol_w = 1, rows_per_col = 1;

  i32   cell_w = 1, cell_h = 1, cols = 1, big_icon = 0;
};

struct Config {
  i32      win_w = 1100, win_h = 700;
  i32      sidebar_w = 220; // logical px
  bool     sidebar_visible = true;
  bool     show_hidden = false;
  ViewMode view = VIEW_DETAILS;
  SortSpec sort;
  float    font_px = 13.0f;
  bool     light = false;
  bool     theme_follow = true;
  bool     thumbnails = true;
  char     expanded[512] = {};
  char     terminal[128] = {};
};

enum JobKind : u32 { JOB_MOUNTS_READ = 1, JOB_MOUNTS_STAT = 2, JOB_STAT_BATCH = 3, JOB_PROPS_SCAN = 4,
                     JOB_APPS_SCAN = 5, JOB_MIME_SCAN = 6, JOB_UDISKS = 7, JOB_CLIP_SEND = 8, JOB_CLIP_RECV = 9, JOB_THUMB = 10 };

struct ThumbJob {
  char  path[4096];
  i64   mtime; u64 size;
  u32   want;
  u32   tab, gen, index; // whose entry asked, so its state can be updated
  Image img;
  bool  ok, from_cache;
  char  err[128];
};
struct ThumbEntry {
  u32   path_off;
  i64   mtime;
  Image img;
  Image disp; // scaled to the size last drawn
  i32   disp_size;
  i64   used_ms;
  u32   bytes;
};
struct ThumbCache {
  Array<char>       paths;
  Array<ThumbEntry> entries;
  Array<i32>        slots;
  u64   bytes = 0;
  u32   inflight = 0, frame_asked = 0;
  u32   hits = 0, misses = 0, generated = 0, cached = 0, failed = 0;
};
enum { THUMB_MAX_ENTRIES = 768, THUMB_MAX_INFLIGHT = 24, THUMB_PER_FRAME = 6 };
enum { THUMB_MAX_BYTES = 96 << 20 };

enum UdisksKind : u8 { UD_MOUNT = 0, UD_UNMOUNT, UD_POWER_OFF };
struct UdisksJob {
  u8    kind;
  char  device[128];
  char  name[64]; // for the message
  char  unmount_first[8][128]; // UD_POWER_OFF: mounted volumes to unmount before
  u32   n_unmount;
  bool  open_after, new_tab; // UD_MOUNT: navigate there once mounted
  bool  ok;
  char  mountpoint[256]; // UD_MOUNT: where it landed
  char  err[256];
};

struct ClipXfer {
  int         fd;
  bool        send;
  u32         mime; // WCLIP_* (receive)
  Array<char> data;
  char        dest[4096]; // receive: the folder to paste into
  bool        ok;
  bool        dnd;
  u32         action;
};

struct DropTarget { Rect r; char path[512]; };

enum UndoKind : u8 { UNDO_NONE = 0, UNDO_MOVE, UNDO_COPY, UNDO_TRASH, UNDO_RENAME, UNDO_NEW_FOLDER, UNDO_CHMOD, UNDO_CHOWN, UNDO_RESTORE };
enum OpJobState : u32 { JOB_QUEUED = 0, JOB_RUNNING, JOB_ASKING, JOB_DONE };

struct OpJob {
  FileOpRequest  req;
  FileOpProgress prog;
  FileOpResult   res;
  volatile u32   state;
  u32            id;
  u8             undo_kind;
  bool           is_undo;
  bool           is_redo; // a redo of an undone operation: reported as "Redid ..."
  struct RedoRecord* redo;
  char           label[128]; // "3 items", "report.pdf"
  char           where[512]; // destination folder name for the message
  i64            started_ms, rate_ms;
  u64            rate_bytes; // bytes_done when the rate was last sampled
  float          rate; // bytes per second, smoothed

  bool           elevated;
  bool           needs_password;
  bool           wrong_password; // it had one and sudo refused it
  bool           no_sudo; // sudo missing, or the user may not use it: give up
  bool           reprobed;
  char           password[256]; // zeroed as soon as it has been written to sudo
  char           sudo_msg[256]; // what sudo said on stderr, for the dialog
  pid_t          child;
};

struct OpConflict {
  char src[4096], dst[4096];
  bool src_dir, dst_dir;
  u64  src_size, dst_size;
  i64  src_mtime, dst_mtime;
};

struct OpError { char path[4096]; char reason[256]; };

struct OpsQueue {
  pthread_t     thread;
  bool          running = false;
  sem_t         work, answer;
  int           wake_fd = -1;
  OpJob*        ring[32];
  volatile u32  head = 0, tail = 0; // ops thread consumes, main produces
  OpJob* volatile asking = nullptr;
  OpConflict    conflict;
  OpError       error;
  volatile u32  asking_error = 0;
  volatile u32  answer_value = 0, answer_all = 0;
  volatile i64  last_wake_ms = 0;
  ConflictAnswer inline_policy = CONFLICT_SKIP; // without a thread (tests) conflicts resolve to this
  u32           next_id = 1;
};

struct UndoRecord {
  u8          kind = UNDO_NONE;
  bool        elevated = false; // done as root: undoing needs root too
  Array<char> journal;
  Array<u32>  joff;
  char        label[128] = {};
};

struct RedoRecord {
  u8          kind = UNDO_NONE;
  bool        elevated = false;
  Array<char> journal;  Array<u32> joff; // the original operation's journal
  Array<char> ujournal; Array<u32> ujoff; // the undo job's journal
  char        label[128] = {};
};

struct Clip {
  bool        cut = false;
  bool        text = false;
  Array<char> paths;
  Array<u32>  off;
  char        dir[4096] = {}; // where they were cut / copied from
  u32 count() const { return off.len; }
  const char* path(u32 i) const { return paths.data + off[i]; }
  void clear() { paths.clear(); off.clear(); cut = false; text = false; dir[0] = 0; }
  void add(const char* p) { u32 n = (u32)strlen(p); off.push(paths.len); memcpy(paths.push_n(n + 1), p, n + 1); }
};

enum DialogKind : u8 { DLG_NONE = 0, DLG_CONFLICT, DLG_CONFIRM_DELETE, DLG_TRASH_FAILED, DLG_ERROR, DLG_PROPERTIES, DLG_SUDO, DLG_OP_ERROR, DLG_SETTINGS };
enum { DIALOG_LINES = 5, DIALOG_BUTTONS = 4 };
struct Dialog {
  DialogKind  kind = DLG_NONE;
  char        title[128] = {};
  char        lines[DIALOG_LINES][256] = {};
  u32         nlines = 0;
  const char* buttons[DIALOG_BUTTONS] = {};
  u32         nbuttons = 0, focus = 0, cancel = 0;
  i32         hover = -1; // button under the mouse this frame
  const char* check_label = nullptr;
  bool        checked = false, hover_check = false;
  OpJob*      job = nullptr; // DLG_CONFLICT: the job waiting
  FileOpRequest pending;
  u8          pending_undo = UNDO_NONE;
  char        pending_label[128] = {};
  bool        pending_is_undo = false, pending_is_redo = false;
  struct RedoRecord* pending_redo = nullptr; // DLG_SUDO for an undo job: its redo record, handed on
  bool        error_last = false;
  UiTextInput password; // DLG_SUDO
};

enum MenuAction : u8 {
  MA_NONE = 0, MA_OPEN, MA_OPEN_TAB, MA_CUT, MA_COPY, MA_PASTE, MA_RENAME, MA_TRASH, MA_DELETE, MA_NEW_FOLDER, MA_PIN, MA_UNDO,
  MA_SELECT_ALL, MA_REFRESH, MA_PROPERTIES,

  MA_OPEN_WITH_PAGE,
  MA_OPEN_WITH, // item arg = index into Menu::apps
  MA_RUN, // execute the file itself
  MA_TERMINAL,
  MA_RESTORE, // Trash place: put the selection back where it came from
  MA_EMPTY_TRASH,
  MA_REDO,
  MA_MOUNT, MA_UNMOUNT, MA_POWER_OFF, MA_OPEN_MOUNT, MA_OPEN_MOUNT_TAB, // device menus
  MA_COPY_PATH, // the path(s) to the clipboard as text
  MA_PLACE_OPEN, MA_PLACE_OPEN_TAB, // a sidebar place's menu
};
enum MenuKind : u8 { MENU_ROWS = 0, MENU_OPEN_WITH, MENU_DEVICE, MENU_PLACE };

struct PropsScan {
  FileOpRequest  req;
  FileOpProgress prog;
  FileOpResult   res;
  u32            gen;
};

struct NameList {
  Array<char> names;
  Array<u32>  off;
  Array<u32>  ids;
  u32 count() const { return off.len; }
  const char* name(u32 i) const { return names.data + off[i]; }
  void add(const char* n, u32 id) { off.push(names.len); u32 l = (u32)strlen(n); memcpy(names.push_n(l + 1), n, l + 1); ids.push(id); }
  void clear() { names.clear(); off.clear(); ids.clear(); }
};

struct Props {
  FileOpRequest paths; // the items

  UiTextInput owner_in, group_in;
  i32   focus_field = 0; // 0 the buttons, 1 owner, 2 group
  bool  popup_hide = false; // Esc closed the popup until the text changes
  i32   popup_sel = -1;
  NameList users, groups;
  char  my_name[64] = {}, my_group[64] = {};
  bool  owners_differ = false;
  u32   count = 0, n_files = 0, n_dirs = 0, n_links = 0, n_others = 0; // among the items themselves
  char  name[256] = {}, location[4096] = {}, kind[64] = {}, target[4096] = {};
  bool  is_dir = false, is_link = false, any_dir = false;
  bool  foreign = false; // some item is owned by someone else: a chmod needs root
  u32   mode = 0, new_mode = 0; // permission bits of the first item; what the grid shows
  bool  modes_differ = false;
  u32   uid = 0, gid = 0;
  char  owner[64] = {}, group[64] = {};
  u64   size = 0, alloc = 0; // of the items themselves
  i64   mtime_ns = 0, atime_ns = 0, btime_ns = 0;
  bool  has_btime = false;
  u64   ino = 0; u32 nlink = 0;
  bool  recursive = false; // apply the permission changes inside the folders too
  bool  folder_itself = false; // opened on the folder shown, not on a selection
  PropsScan* scan = nullptr; // in flight (nullptr: none, or done)
  u32   scan_gen = 0;
  bool  scanned = false, scan_cancelled = false;
  u64   tot_bytes = 0, tot_alloc = 0;
  u32   tot_files = 0, tot_dirs = 0, tot_links = 0, tot_unreadable = 0;
};

struct Settings {
  UiTextInput terminal_in;
  i32   focus_field = 0; // 0 the buttons, 1 the terminal field
};

struct MenuItem { const char* label; const char* shortcut; i8 icon; u8 action; bool enabled, separator; u16 arg; };
struct Menu {
  bool     open = false;
  u8       kind = MENU_ROWS;
  i32      x = 0, y = 0; // anchor (buffer pixels)
  MenuItem items[MAX_MENU_ITEMS];
  u32      count = 0;
  i32      hover = -1, focus = -1;
  i32      row = -1;
  char     text[4][160];

  u32      apps[16]; u32 napps = 0;
  char     mime[128] = {};

  i32      dev_entry = -1, dev_disk = -1;

  char     place_path[512] = {};
  bool     place_trash = false, place_pinned = false;
};

struct App {
  Window     w;
  FontDb     db;
  Text       text, small;
  Ui         ui;
  UiTheme    theme_dark, theme_light;
  float      cur_px = 0, cur_small_px = 0;
  char       font_spec[160] = {};
  Config     cfg;
  bool       cfg_dirty = false;
  i64        cfg_save_at = 0;

  Tab        tabs[MAX_TABS];
  u32        tab_order[MAX_TABS] = {};
  u32        num_tabs = 0, active = 0;

  Place      places[MAX_PLACES];
  u32        num_places = 0;
  Place      pins[MAX_PINS];
  u32        num_pins = 0;
  i32        side_scroll = 0;

  WorkerPool pool;
  MountList  mounts, mounts_fresh;
  bool       mounts_reading = false, mounts_again = false, mounts_read_once = false;
  u32        mounts_stats_pending = 0;
  int        mountinfo_fd = -1;
  i64        mounts_next_ms = 0, mounts_last_pri_ms = 0;

  StatBatch* stat_pool[STAT_POOL] = {};
  u32        stat_free = 0; // bitmask of free batches
  bool       sync_stat = true; // no pool: statx inline as in M4 (tests, --no-workers)

  OpsQueue   ops;
  Array<OpJob*> jobs; // queued, running and finished-but-unreported, in order
  UndoRecord undo[MAX_UNDO];
  u32        undo_count = 0;
  RedoRecord redo[MAX_UNDO];
  u32        redo_count = 0;
  Clip       clip;
  bool       clip_published = false; // our source owns the Wayland selection
  u32        clip_recv_pending = 0; // a paste is waiting for another client's data

  MimeDb     mime;
  AppDb      apps;
  bool       mime_ready = false, apps_ready = false, mime_scanning = false, apps_scanning = false;
  UiTheme    theme_omarchy;
  bool       theme_synced = false; // theme_omarchy holds the Omarchy theme's colours
  bool       theme_is_light = false; // ... and it is a light one
  char       theme_name[64] = {};
  i32        theme_watch = -1; // index into watcher.watches on ~/.config/omarchy/current
  u32        udisks_pending = 0;
  u32        launches = 0; // stats
  ThumbCache thumbs;
  bool       no_thumbs = false; // --no-thumbs

  Clip       drag; // what we are dragging
  i32        drag_press_row = -1;
  float      drag_press_x = 0, drag_press_y = 0;
  u32        drags = 0, drops = 0; // stats

  bool       band_active = false, band_add = false; // band_add: Ctrl held: adds to the selection made before
  float      band_x0 = 0, band_y0 = 0; // press point, in content coordinates (scroll removed)
  Array<u8>  band_base;
  i32        tab_drag = -1;
  float      tab_drag_x = 0, tab_drag_grab = 0; // press x, and the pointer offset inside the tab
  bool       tab_dragging = false;
  bool       dnd_over = false; // something is dragged over the window
  char       dnd_target[4096] = {}; // the folder it would land in ("" none)
  i32        dnd_row = -1;
  Rect       dnd_rect = {};
  u32        dnd_mime = 0; // the format we accepted
  Array<DropTarget> drop_targets;
  Dialog     dialog;
  Props      props;
  Settings   settings;
  bool       no_thumbs_arg = false; // --no-thumbs: thumbnails off however the setting is
  Menu       menu;
  i64        sudo_ok_ms;
  char       exe_path[4096];
  char       user_name[64]; // who we run as, for the password prompt
  bool       no_sudo_run;
  Watcher    watcher;
  i64        watch_due_ms = 0;
  u32        ops_done = 0, ops_failed = 0; // stats

  bool       path_editing = false;
  UiTextInput path_input;
  UiTextInput rename_input;
  i32        pending_rename_row = -1;
  i64        pending_rename_at = 0;
  bool       status_error = false;

  Layout     L;
  bool       light = false, debug = false, no_workers = false, demo_devices = false;
  bool       running = true;
  u32        frames = 0;
  i64        draw_ns = 0;
  char       status_msg[256] = {};
  i64        status_msg_until = 0;
};

Tab&  tab_active(App& a);
i32   tab_open(App& a, const char* path, bool make_active); // new tab; returns its position or -1
void  tab_close(App& a, u32 pos);
void  tab_activate(App& a, u32 pos);
bool  navigate(App& a, Tab& t, const char* path, bool push_history);
void  reload(App& a, Tab& t);
void  go_parent(App& a, Tab& t);
void  go_back(App& a, Tab& t);
void  go_forward(App& a, Tab& t);
bool  can_go_back(const Tab& t);
bool  can_go_forward(const Tab& t);
void  open_row(App& a, Tab& t, i32 row, bool new_tab);
void  resort(App& a, Tab& t);
void  rebuild_rows(App& a, Tab& t);
void  set_sort(App& a, Tab& t, SortKey key, bool toggle_dir);
void  set_view(App& a, Tab& t, ViewMode v);
void  filter_changed(App& a, Tab& t);
void  filter_clear(App& a, Tab& t);
void  clear_selection(App& a, Tab& t);
void  select_all(App& a, Tab& t);
void  select_range(Tab& t, i32 from, i32 to);
void  set_cursor(App& a, Tab& t, i32 row, bool extend, bool toggle);
void  ensure_visible(App& a, Tab& t, i32 row);
void  set_status(App& a, const char* msg);
void  set_status_error(App& a, const char* msg); // red, stays until the next action
void  refresh_tab(App& a, Tab& t, const char* stale, u32 stale_n, bool drop_stats); // re-read, keep selection / cursor / scroll
void  refresh_path(App& a, const char* dir, const char* stale = nullptr); // every tab showing `dir`
void  tab_gone(App& a, Tab& t); // its folder vanished: climb to an ancestor
void  select_names(App& a, Tab& t, const char* const* names, u32 n); // select these, cursor on the first
u32   selected_paths(App& a, Tab& t, FileOpRequest& into); // append the selection's absolute paths
void  entry_path(const Tab& t, u32 i, char* out, u32 cap);
void  tab_watch(App& a, Tab& t); // (re)attach the inotify watch to t.path
void  stat_request(App& a, Tab& t, u32 i); // statx entry i: inline, or into the frame's batch
void  stat_flush(App& a); // submit the frame's batch and pump sort-all batches
void  stat_job_done(App& a, StatBatch* b);
void  trash_cols_reset(Tab& t); // t.path changed: is it a trash, and clear the columns
const char* trash_original(const Tab& t, u32 i);
void  load_places(App& a);
bool  toggle_pin(App& a, const char* path);
void  window_title(App& a);
const char* tab_title(const Tab& t);
u8    entry_icon(const DirListing& l, u32 i);
const char* entry_kind(const DirListing& l, u32 i, char* buf, u32 cap);
void  expand_path(const char* in, char* out, u32 cap); // ~ and relative paths

void  shell_layout(App& a, i32 cw, i32 ch);
void  shell_frame(App& a, Canvas& c); // one frame: layout, widgets, drawing, pointer input
void  shell_key(App& a, const WEvent& e); // keyboard input
void  shell_timers(App& a, i64 now_ms);
i64   shell_next_wake(const App& a); // earliest timer the shell wants (0 = none)
void  shell_focus_changed(App& a);

Rect  cell_rect(const App& a, const Tab& t, i32 row);
i32   cell_at(const App& a, const Tab& t, float x, float y);
void  content_size(const App& a, const Tab& t, i32* w, i32* h);
i32   rows_per_page(const App& a, const Tab& t);

bool  disk_expanded(const App& a, const char* disk);
void  disk_toggle(App& a, const char* disk);
void  rename_begin(App& a, Tab& t, u32 listing_index);
void  rename_commit(App& a, Tab& t);
void  rename_cancel(App& a, Tab& t);
void  menu_open(App& a, Tab& t, i32 row, i32 x, i32 y);
void  menu_open_with(App& a, Tab& t, i32 row, i32 x, i32 y); // the applications page
void  menu_open_device(App& a, i32 entry, i32 disk, i32 x, i32 y); // a volume's or a disk's menu (sidebar)
void  menu_open_place(App& a, const char* path, bool pinned, i32 x, i32 y);
void  settings_open(App& a);
void  settings_apply_thumbs(App& a); // a.no_thumbs from the setting and --no-thumbs
void  menu_close(App& a);
void  path_complete(App& a); // Tab in the path editor
void  theme_sync_load(App& a); // read the Omarchy theme (or leave the built-in ones)

void  drag_check(App& a);
void  drag_ended(App& a, bool finished, u32 action); // WE_DRAG_END
void  dnd_event(App& a, u8 type, float x, float y); // WE_DND_ENTER / MOTION / LEAVE / DROP
void  dnd_drop_paths(App& a, const char* dest, const Clip& paths, u32 action); // what a drop does once the data is in (tests call it)
bool  dnd_target_at(App& a, float x, float y, char* out, u32 cap, i32* row, Rect* side); // the folder under a point
const UiTheme& theme_current(const App& a);
void  dialog_close(App& a);
void  dialog_confirm(App& a); // press the first button (tests)
void  dialog_error(App& a, const char* title, const char* line); // a modal DLG_ERROR with one OK button
void  props_open(App& a, Tab& t, bool folder_itself); // Properties of the selection, or of the folder shown
void  props_scan_done(App& a, PropsScan* s); // the pool finished a size walk

bool  ops_start(App& a);
void  ops_stop(App& a);
OpJob* ops_submit(App& a, FileOpRequest& req, u8 undo_kind, const char* label, bool is_undo = false,
                  bool elevated = false, const char* password = nullptr, RedoRecord* redo = nullptr, bool is_redo = false); // takes the request's arrays
void   sudo_dialog(App& a, OpJob* j);
int    helper_main(const char* request_file);
void  ops_cancel(App& a, OpJob* j);
void  ops_poll(App& a);
void  ops_answer(App& a, ConflictAnswer ans, bool all); // the conflict dialog's answer
void  ops_answer_error(App& a, ErrorAnswer ans, bool all);
bool  ops_busy(const App& a);

void  op_copy_or_cut(App& a, Tab& t, bool cut);
void  op_paste(App& a, Tab& t);
void  op_trash(App& a, Tab& t);
void  op_delete(App& a, Tab& t, bool confirmed);
void  op_new_folder(App& a, Tab& t);
void  op_undo(App& a);
void  op_redo(App& a);
void  redo_record_free(RedoRecord* r);
void  op_paste_paths(App& a, Tab& t, const Clip& c); // copy / move the clip's paths into t.path
void  op_paste_into(App& a, const char* dest, const Clip& c);
bool  clip_available(const App& a); // something to paste: ours, or another client's file list
void  clip_publish(App& a); // offer a.clip on the Wayland clipboard
void  clip_send(App& a, int fd, const char* mime, const Clip& src); // another client asked for it (WE_DATA_SEND)
void  clip_recv_done(App& a, ClipXfer* x);
void  clip_send_done(App& a, ClipXfer* x);
void  clip_cancelled(App& a); // our offer was replaced (WE_DATA_CANCELLED)

void  open_files(App& a, Tab& t, i32 app_index);
void  run_file(App& a, Tab& t); // execute the cursor's file
void  open_terminal(App& a, const char* dir);
const char* entry_mime(App& a, Tab& t, u32 i, char* buf, u32 cap);
bool  ensure_apps(App& a);

bool  in_trash(const Tab& t); // t shows a trash's files/ directory
void  op_restore(App& a, Tab& t);
void  op_empty_trash(App& a, Tab& t, bool confirmed);
void  op_empty_trash_dir(App& a, const char* files_dir, bool confirmed); // the trash whose files/ directory this is

void  device_mount(App& a, u32 entry, bool open_after, bool new_tab);
void  device_unmount(App& a, u32 entry);
void  device_power_off(App& a, u32 disk);
void  udisks_done(App& a, UdisksJob* j);

void  config_load(App& a);
void  config_save(App& a);
void  config_mark_dirty(App& a);
void  pins_load(App& a);
void  pins_save(App& a);

const Image* thumb_for(App& a, Tab& t, u32 i, i32 size);
void  thumb_job_done(App& a, ThumbJob* j);
void  thumb_frame_begin(App& a); // resets the per-frame request budget
void  thumbs_clear(App& a);

void  mounts_request_refresh(App& a);
void  apps_request_scan(App& a); // MIME + applications on the pool (inline without one)
void  udisks_submit(App& a, UdisksJob* j);
void  clip_xfer_submit(App& a, ClipXfer* x);
void  pool_drain(App& a);
