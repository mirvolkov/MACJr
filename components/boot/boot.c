/*
 * Boot shell: a file navigator in the style of DOS Navigator, one panel
 * over the SD card, starting in its root.
 *
 *   ╔═[home]══════════[/BIN]════════[14:05]═╗
 *   ║..                                    ▲
 *   ║TETRIS                      ELF  6.9 KB▒
 *   ║MK52                        ELF   14 KB■
 *   ║layout                      PNG   86 KB▒
 *   ║...                                   ▼
 *   ╚══════════════════════════════════════╝
 *
 * The top edge is the header: the current directory ("/" for the card's
 * root) in brackets, always; the Wi-Fi network on the left while
 * connected and the time on the right once the clock is set (NTP), each
 * left out while it isn't there. The clock's colon blinks once a second;
 * the header is checked twice a second. The bracketed text is white on
 * black. The scroll bar is the panel's right edge, between ╗ and ╝. While
 * a USB keyboard is plugged in (USB keyboard mode, F + А↑) the bottom edge
 * shows ╚═[USB KBRD]═…╝, the label red.
 *
 * The panel lists name, extension and file size, directories first;
 * built-in programs come first in the root. Colors by type: directories
 * white, programs (ELF, built-ins) cyan, Python scripts green, C sources
 * dark green, images yellow, sounds purple, text files light grey,
 * anything else dark grey. Hidden files are left out: FAT hidden or system
 * attribute, or a name starting with '.' (macOS metadata).
 *
 * The card's root also lists DEV, in blue after the built-ins: the
 * board's devices (/dev, devfs.h), not on the card. Inside, the header
 * shows "/DEV" and the devices are listed in blue, by name only; they are
 * only listed: В↑ doesn't open one, and the file menu has nothing to
 * delete, copy or paste there. Cx goes back to the card.
 *
 * Keys, as in the WIFI program: П→x / x→П move, ←ШГ / ШГ→ go to the first /
 * last entry, В↑ opens a directory or a file (main.c runs a program, and
 * hands a .py file to UPYTHON.ELF, a .c file to CC.ELF, a sound to
 * AUDIOPLAYER.ELF, an image to IMAGEVIEWER.ELF and a text file to
 * TEXTVIEWER.ELF), Cx goes up a directory. The selection is black on cyan,
 * as in DOS Navigator.
 *
 * F + В↑ (Alt+Enter on a USB keyboard) opens the file menu on the
 * selected entry, a window with its name in the top edge; a digit picks:
 *
 *   ╔═══════[NOTES.TXT]═══════╗
 *   ║ 3  View / run            ║   as В↑
 *   ║ 8  Delete                ║   asks first; a directory with all in it
 *   ║ 4  Copy                  ║   remembers the entry (the clipboard)
 *   ║ 5  Paste README.TXT      ║   only while something is copied
 *   ╚══════════════════════════╝
 *
 * Paste copies the remembered file or directory (with everything in it)
 * into the current directory, or into the selected directory when the
 * menu was opened on one ("Paste into X"; on ".." into the parent). A name
 * already there gets "_1", "_2", ... before its extension. Cx closes the
 * menu, and stops a copy or a delete under way (the half-copied file is
 * removed).
 *
 * The directory is saved in the db (nav.dir) whenever it changes, and the
 * entry started from it (nav.file) when a program starts; after a reboot
 * the navigator opens there again, on that entry.
 */
#include "boot.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "devfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "monitor.h"
#include "net.h"
#include "sdcard.h"
#include "tty/list.h"
#include "tty/window.h"
#include "usbdev.h"

static const char *TAG = "boot";

#define KEY_UP   KEY_MK_P_TO_X /* П→x */
#define KEY_DOWN KEY_MK_X_TO_P /* x→П */
#define KEY_HOME KEY_MK_HOME   /* ←ШГ */
#define KEY_END  KEY_MK_END    /* ШГ→ */
#define KEY_OPEN KEY_MK_ENTER  /* В↑ */
#define KEY_BACK KEY_ESC       /* Cx */
#define KEY_MENU KEY_MK_F_ENTER /* F + В↑: file menu */

#define COLS       40
#define ROW_TOP    1 /* the top edge, with the header fields */
#define ROW_LIST   2
#define LIST_ROWS  13
#define ROW_BOTTOM (ROW_LIST + LIST_ROWS)
#define COL_LIST   2 /* after the left frame */

/* Columns of an item, a space apart: name, extension, size; then the
 * scroll bar. */
#define NAME_W    26
#define EXT_W     3
#define SIZE_W    7
#define SSID_SHOW 12 /* network name in the header, at most */

/* Where the navigator was, kept in the db for the next boot. */
#define DB_KEY_DIR  "nav.dir"  /* the directory, saved when it changes */
#define DB_KEY_FILE "nav.file" /* the entry last started from it */

/* /dev as the navigator shows it: upper case, like the card's directories */
#define DEV_NAME "DEV"

#define MAX_ENTRIES 512
#define PATH_LEN    256
#define POLL_MS     500           /* header refresh: twice a second for the blinking colon */
#define TIME_VALID  (2025 - 1900) /* tm_year once NTP has set the clock */

/* CP866 */
#define G_TL '\xC9' /* ╔ */
#define G_TR '\xBB' /* ╗ */
#define G_BL '\xC8' /* ╚ */
#define G_BR '\xBC' /* ╝ */
#define G_H  '\xCD' /* ═ */
#define G_V  "\xBA" /* ║ */

#define COLOR_FIELD "\x1b[0;97;40m" /* header text in brackets: white on black */
#define COLOR_FRAME "\x1b[0;37m"
#define COLOR_USB_KBD "\x1b[0;91;40m" /* bottom edge label: red on black */
#define USB_KBD_LABEL "USB KBRD"

typedef enum {
  KIND_UP, /* ".." */
  KIND_BUILTIN,
  KIND_DEVDIR, /* "dev" in the card's root: /dev */
  KIND_DIR,
  KIND_ELF,
  KIND_SCRIPT, /* .py, run by UPYTHON.ELF */
  KIND_SOURCE, /* .c, compiled by CC.ELF */
  KIND_IMAGE,
  KIND_SOUND,
  KIND_TEXT,  /* opened by TEXTVIEWER.ELF */
  KIND_DEVICE, /* in /dev: listed, not opened */
  KIND_OTHER,
} kind_t;

static const char *const s_kind_fg[] = {
    [KIND_UP] = "\x1b[97m",    [KIND_BUILTIN] = "\x1b[96m", [KIND_DEVDIR] = "\x1b[94m", [KIND_DIR] = "\x1b[97m",
    [KIND_DEVICE] = "\x1b[94m",
    [KIND_ELF] = "\x1b[96m",   [KIND_SCRIPT] = "\x1b[92m", [KIND_SOURCE] = "\x1b[32m", [KIND_IMAGE] = "\x1b[93m",   [KIND_SOUND] = "\x1b[95m", [KIND_TEXT] = "\x1b[37m",
    [KIND_OTHER] = "\x1b[90m",
};

typedef struct {
  char *name;
  uint32_t size;
  kind_t kind;
  int builtin; /* index in s_builtins for KIND_BUILTIN */
} entry_t;

static entry_t s_ents[MAX_ENTRIES];
static int s_count;
static bool s_scan_ok;
static char s_cwd[PATH_LEN];
static macjr_tty_list_t s_list;
static const boot_builtin_t *s_builtins;
static int s_n_builtins;

static void out(macjr_tty_t *tty, const char *s) {
  macjr_tty_write(tty, s, strlen(s));
}

/* Move to row r, column c (1-based) and print the formatted text. */
static void at(macjr_tty_t *tty, int r, int c, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void at(macjr_tty_t *tty, int r, int c, const char *fmt, ...) {
  char buf[160];
  int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", r, c);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  out(tty, buf);
}

static bool card_mounted(void) {
  uint64_t total, free_bytes;
  return sdcard_info(&total, &free_bytes) == ESP_OK;
}

static bool at_root(void) {
  return strcmp(s_cwd, SDCARD_MOUNT_POINT) == 0;
}

static bool in_dev(void) {
  return strcmp(s_cwd, DEVFS_DIR) == 0;
}

/* The directory as the user sees it: "/" for the card's root, "/DEV". */
static const char *shown_path(void) {
  if (in_dev()) {
    return "/" DEV_NAME;
  }
  const char *p = s_cwd + strlen(SDCARD_MOUNT_POINT);
  return *p ? p : "/";
}

/* ---- directory -------------------------------------------------------- */

/* The extension after the last '.', or "" (a leading dot doesn't count). */
static const char *ext_of(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && dot != name ? dot + 1 : "";
}

static kind_t file_kind(const char *name) {
  static const char *const images[] = {"PNG", "BMP", "JPG", "JPEG", "GIF"};
  static const char *const sounds[] = {"WAV", "MP3", "OGG", "FLAC"};
  static const char *const texts[] = {"TXT", "MD", "LOG", "INI", "CFG", "CSV", "JSON", "H"};
  const char *ext = ext_of(name);
  if (strcasecmp(ext, "ELF") == 0) {
    return KIND_ELF;
  }
  if (strcasecmp(ext, "PY") == 0) {
    return KIND_SCRIPT;
  }
  if (strcasecmp(ext, "C") == 0) {
    return KIND_SOURCE;
  }
  for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
    if (strcasecmp(ext, images[i]) == 0) {
      return KIND_IMAGE;
    }
  }
  for (size_t i = 0; i < sizeof(sounds) / sizeof(sounds[0]); i++) {
    if (strcasecmp(ext, sounds[i]) == 0) {
      return KIND_SOUND;
    }
  }
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    if (strcasecmp(ext, texts[i]) == 0) {
      return KIND_TEXT;
    }
  }
  return KIND_OTHER;
}

/* "..", built-ins and directories first, then files; by name in each. */
static int rank(kind_t kind) {
  return kind <= KIND_DIR ? (int)kind : KIND_DIR + 1;
}

static int by_kind_and_name(const void *a, const void *b) {
  const entry_t *x = a, *y = b;
  int d = rank(x->kind) - rank(y->kind);
  return d ? d : strcasecmp(x->name, y->name);
}

static void add(const char *name, kind_t kind, uint32_t size, int builtin) {
  if (s_count >= MAX_ENTRIES) {
    return;
  }
  char *copy = strdup(name);
  if (!copy) {
    return;
  }
  s_ents[s_count++] = (entry_t){.name = copy, .size = size, .kind = kind, .builtin = builtin};
}

static void clear_entries(void) {
  for (int i = 0; i < s_count; i++) {
    free(s_ents[i].name);
  }
  s_count = 0;
}

/* Read s_cwd into s_ents and select `select` if it is there (else keep the
 * selection's position). */
static void scan(const char *select) {
  clear_entries();
  if (at_root()) {
    for (int i = 0; i < s_n_builtins; i++) {
      add(s_builtins[i].name, KIND_BUILTIN, 0, i);
    }
    add(DEV_NAME, KIND_DEVDIR, 0, -1);
  } else {
    add("..", KIND_UP, 0, -1);
  }
  DIR *dir = opendir(s_cwd);
  s_scan_ok = dir != NULL;
  if (dir) {
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
      if (de->d_name[0] == '.') {
        continue; /* "._NAME" is macOS metadata; also . and .. */
      }
      if (in_dev()) {
        /* by name only: most drivers have no stat() for their path */
        add(de->d_name, KIND_DEVICE, 0, -1);
        continue;
      }
      char path[PATH_LEN + 260];
      snprintf(path, sizeof(path), "%s/%s", s_cwd, de->d_name);
      struct stat st;
      if (stat(path, &st) != 0 || sdcard_is_hidden(path)) {
        continue;
      }
      bool is_dir = S_ISDIR(st.st_mode);
      add(de->d_name, is_dir ? KIND_DIR : file_kind(de->d_name), is_dir ? 0 : (uint32_t)st.st_size, -1);
    }
    closedir(dir);
  } else {
    ESP_LOGW(TAG, "cannot read %s", s_cwd);
  }
  qsort(s_ents, s_count, sizeof(entry_t), by_kind_and_name);

  macjr_tty_list_set_count(&s_list, s_count);
  if (select) {
    for (int i = 0; i < s_count; i++) {
      if (strcmp(s_ents[i].name, select) == 0) {
        macjr_tty_list_move(&s_list, i - s_list.sel);
        break;
      }
    }
  }
}

static void save(const char *key, const char *value) {
  esp_err_t err = db_update(key, value);
  if (err == ESP_ERR_NOT_FOUND) {
    err = db_create(key, value);
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "%s not saved: %s", key, esp_err_to_name(err));
  }
}

/* The directory saved at the last change, if it is still one on the card;
 * else the root. The entry last started from it goes to `file` ("" if
 * none). */
static void restore_dir(char *file, size_t len) {
  snprintf(s_cwd, sizeof(s_cwd), "%s", SDCARD_MOUNT_POINT);
  file[0] = '\0';
  char dir[PATH_LEN];
  size_t root = strlen(SDCARD_MOUNT_POINT);
  struct stat st;
  if (db_read(DB_KEY_DIR, dir, sizeof(dir)) != ESP_OK || strncmp(dir, SDCARD_MOUNT_POINT, root) != 0 ||
      (dir[root] != '\0' && dir[root] != '/') || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return;
  }
  snprintf(s_cwd, sizeof(s_cwd), "%s", dir);
  if (db_read(DB_KEY_FILE, file, len) != ESP_OK) {
    file[0] = '\0';
  }
}

/* Into /dev, from the card's root (not saved: a reboot starts on the card). */
static void enter_dev(void) {
  snprintf(s_cwd, sizeof(s_cwd), "%s", DEVFS_DIR);
  s_list.sel = 0;
  s_list.top = 0;
  scan(NULL);
}

/* Into directory `name`, selecting its first entry. */
static void enter_dir(const char *name) {
  size_t len = strlen(s_cwd);
  if (len + 1 + strlen(name) >= sizeof(s_cwd)) {
    ESP_LOGW(TAG, "path too long: %s/%s", s_cwd, name);
    return;
  }
  snprintf(s_cwd + len, sizeof(s_cwd) - len, "/%s", name);
  s_list.sel = 0;
  s_list.top = 0;
  scan(NULL);
  save(DB_KEY_DIR, s_cwd);
}

/* Up one directory, selecting the one we came from. */
static void leave_dir(void) {
  if (at_root()) {
    return;
  }
  if (in_dev()) {
    snprintf(s_cwd, sizeof(s_cwd), "%s", SDCARD_MOUNT_POINT);
    scan(DEV_NAME);
    return;
  }
  char *slash = strrchr(s_cwd, '/');
  char came_from[PATH_LEN];
  snprintf(came_from, sizeof(came_from), "%s", slash + 1);
  *slash = '\0';
  scan(came_from);
  save(DB_KEY_DIR, s_cwd);
}

/* ---- drawing ---------------------------------------------------------- */

/* "123 B", "4.5 KB", "120 KB", "2.3 MB": at most SIZE_W characters. */
static void format_size(uint32_t bytes, char *out, size_t len) {
  static const char *const units[] = {"KB", "MB", "GB"};
  if (bytes < 1024) {
    snprintf(out, len, "%lu B", (unsigned long)bytes);
    return;
  }
  uint64_t scale = 1024;
  int u = 0;
  while (u < 2 && bytes >= scale * 1024) {
    scale *= 1024;
    u++;
  }
  unsigned long tenths = (unsigned long)((uint64_t)bytes * 10 / scale);
  if (tenths < 100) {
    snprintf(out, len, "%lu.%lu %s", tenths / 10, tenths % 10, units[u]);
  } else {
    snprintf(out, len, "%lu %s", tenths / 10, units[u]);
  }
}

/* `src` cut to `width` characters; a name that doesn't fit ends in '~'. */
static void fit(char *dst, const char *src, size_t src_len, int width) {
  if ((int)src_len > width) {
    memcpy(dst, src, width - 1);
    dst[width - 1] = '~';
    dst[width] = '\0';
  } else {
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
  }
}

static void item_text(int index, char *text, size_t len, void *ctx) {
  const entry_t *e = &s_ents[index];
  char name[NAME_W + 1], ext[EXT_W + 1] = "", size[SIZE_W + 8] = "";

  const char *dot_ext =
      e->kind == KIND_UP || e->kind == KIND_BUILTIN || e->kind == KIND_DEVDIR || e->kind == KIND_DEVICE
          ? ""
          : ext_of(e->name);
  size_t base_len = *dot_ext ? (size_t)(dot_ext - 1 - e->name) : strlen(e->name);
  fit(name, e->name, base_len, NAME_W);
  snprintf(ext, sizeof(ext), "%s", dot_ext); /* cut to 3 */

  if (e->kind == KIND_BUILTIN) {
    snprintf(size, sizeof(size), "BUILTIN");
  } else if (e->kind != KIND_UP && e->kind != KIND_DIR && e->kind != KIND_DEVDIR && e->kind != KIND_DEVICE) {
    format_size(e->size, size, sizeof(size));
  }
  snprintf(text, len, "%s%-*s %-*s %*s", s_kind_fg[e->kind], NAME_W, name, EXT_W, ext, SIZE_W, size);
}

/* A frame row: corners `l` and `r`, ═ between. */
static void frame_row(char *buf, char l, char r) {
  memset(buf, G_H, COLS);
  buf[0] = l;
  buf[COLS - 1] = r;
  buf[COLS] = '\0';
}

/* The top edge: ╔═[network]═══[path]═══[hh:mm]═╗. mark[i] is set for the
 * cells inside the brackets, drawn in COLOR_FIELD. Also what the bottom
 * edge shows, so a change there is noticed the same way. */
typedef struct {
  char text[COLS + 1];
  char mark[COLS];
  bool usb_kbd; /* bottom edge: ╚═[USB KBRD]═══╝ while a USB keyboard is plugged in */
} header_t;

static void put_field(header_t *h, int col, const char *s, int len) {
  h->text[col] = '[';
  memcpy(h->text + col + 1, s, len);
  memset(h->mark + col + 1, 1, len);
  h->text[col + len + 1] = ']';
}

/* "[network]" on the left while connected, "[hh:mm]" on the right once
 * the clock is set, "[path]" centered between them (moved aside or cut
 * from the left to keep clear). */
static void header_build(header_t *h) {
  frame_row(h->text, G_TL, G_TR);
  memset(h->mark, 0, sizeof(h->mark));
  h->usb_kbd = usb_kbd_connected();
  int lo = 2, hi = COLS - 2; /* fields go in [lo, hi), ═ around them */

  char ssid[NET_SSID_MAX + 1];
  if (net_wifi_status(ssid, NULL)) {
    for (char *c = ssid; *c; c++) {
      if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7F) {
        *c = '?'; /* the font has no UTF-8 */
      }
    }
    int len = (int)strnlen(ssid, SSID_SHOW);
    put_field(h, lo, ssid, len);
    lo += len + 3;
  }
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  if (tm.tm_year >= TIME_VALID) {
    char clock[8];
    int len = (int)strftime(clock, sizeof(clock), "%H:%M", &tm);
    if (tm.tm_sec % 2) {
      clock[2] = ' '; /* the colon blinks once a second, like a watch */
    }
    hi -= len + 2;
    put_field(h, hi, clock, len);
    hi--;
  }

  const char *path = shown_path();
  int len = (int)strlen(path);
  int room = hi - lo - 2;
  char shown[COLS + 1];
  if (len > room) {
    len = room;
    snprintf(shown, sizeof(shown), "..%s", path + strlen(path) - (len - 2));
  } else {
    snprintf(shown, sizeof(shown), "%s", path);
  }
  int col = (COLS - (len + 2)) / 2;
  if (col < lo) {
    col = lo;
  } else if (col + len + 2 > hi) {
    col = hi - len - 2;
  }
  put_field(h, col, shown, len);
}

static void draw_header(macjr_tty_t *tty, const header_t *h) {
  char buf[COLS * 16];
  int n = snprintf(buf, sizeof(buf), "\x1b[%d;1H", ROW_TOP);
  int cur = -1;
  for (int i = 0; i < COLS; i++) {
    if (h->mark[i] != cur) {
      cur = h->mark[i];
      n += snprintf(buf + n, sizeof(buf) - n, "%s", cur ? COLOR_FIELD : COLOR_FRAME);
    }
    buf[n++] = h->text[i];
  }
  n += snprintf(buf + n, sizeof(buf) - n, "\x1b[0m");
  macjr_tty_write(tty, buf, n);
}

/* ╚═[USB KBRD]══…══╝ while a USB keyboard is plugged in (F + А↑ on), else plain. */
static void draw_bottom(macjr_tty_t *tty, const header_t *h) {
  char row[COLS + 1];
  frame_row(row, G_BL, G_BR);
  if (h->usb_kbd) {
    at(tty, ROW_BOTTOM, 1, COLOR_FRAME "%.2s[" COLOR_USB_KBD USB_KBD_LABEL COLOR_FRAME "]%s\x1b[0m", row,
       row + 2 + sizeof(USB_KBD_LABEL) + 1);
  } else {
    at(tty, ROW_BOTTOM, 1, COLOR_FRAME "%s\x1b[0m", row);
  }
}

static void draw_frame(macjr_tty_t *tty, const header_t *h) {
  for (int r = ROW_LIST; r < ROW_BOTTOM; r++) {
    at(tty, r, 1, COLOR_FRAME G_V);
  }
  draw_bottom(tty, h);
}

static void draw(macjr_tty_t *tty, const header_t *header) {
  out(tty, "\x1b[?25l\x1b[0m\x1b[2J");
  draw_header(tty, header);
  draw_frame(tty, header);
  macjr_tty_list_draw(tty, &s_list);
  if (!s_scan_ok) {
    at(tty, ROW_LIST + 4, COL_LIST + 2, "\x1b[0;91m%s",
       card_mounted() ? "Cannot read this directory" : "No SD card");
    at(tty, ROW_LIST + 6, COL_LIST + 2, "\x1b[0;90mPress any key to look again\x1b[0m");
  } else if (s_count == 0) {
    at(tty, ROW_LIST + 4, COL_LIST + 2, "\x1b[0;90mEmpty\x1b[0m");
  }
  macjr_display_flush(tty);
}


/* ---- the shell -------------------------------------------------------- */

/* Wait up to POLL_MS for a key. The key, or -1 if none came. */
static int wait_key(int fd) {
  if (fd < 0) {
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    return -1;
  }
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  struct timeval tv = {.tv_sec = POLL_MS / 1000, .tv_usec = (POLL_MS % 1000) * 1000};
  if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
    return -1;
  }
  uint8_t key;
  return read(fd, &key, 1) == 1 ? key : -1;
}

/* ---- opening ---------------------------------------------------------- */

/* В↑ on the selected entry: into a directory, or pick a program or a file
 * for main.c to start. True if picked. */
static bool open_selected(char *path, size_t path_len, const boot_builtin_t **builtin) {
  if (s_list.sel < 0 || s_list.sel >= s_count) {
    return false;
  }
  const entry_t *e = &s_ents[s_list.sel];
  if (e->kind == KIND_UP) {
    leave_dir();
  } else if (e->kind == KIND_DIR) {
    enter_dir(e->name);
  } else if (e->kind == KIND_DEVDIR) {
    enter_dev();
  } else if (e->kind == KIND_BUILTIN) {
    *builtin = &s_builtins[e->builtin];
    save(DB_KEY_FILE, e->name);
    ESP_LOGI(TAG, "%s (built in) selected", e->name);
    return true;
  } else if (e->kind != KIND_OTHER && e->kind != KIND_DEVICE) {
    *builtin = NULL;
    snprintf(path, path_len, "%s/%s", s_cwd, e->name);
    save(DB_KEY_FILE, e->name);
    ESP_LOGI(TAG, "%s selected", path);
    return true;
  }
  return false;
}

/* ---- file operations (F + В↑) ---------------------------------------- */

#define MENU_W    32
#define MSG_W     34
#define COPY_BUF  4096
#define SHOW_STEP (64 * 1024) /* progress redrawn every 64 KB */

typedef enum { OP_EXEC, OP_DEL, OP_COPY, OP_PASTE } op_t;

static char s_clip[PATH_LEN + 260];    /* full path of the copied entry; "" = nothing copied */
static char s_pa[PATH_LEN + 260];      /* working paths of a copy or a delete, */
static char s_pb[PATH_LEN + 260];      /* grown and cut back as it goes down */

/* 0-9 for a digit key (MK-52 or a USB keyboard's ASCII), else -1. */
static int digit_of(int key) {
  static const uint8_t mk[10] = {KEY_MK_0, KEY_MK_1, KEY_MK_2, KEY_MK_3, KEY_MK_4,
                                 KEY_MK_5, KEY_MK_6, KEY_MK_7, KEY_MK_8, KEY_MK_9};
  if (key >= '0' && key <= '9') {
    return key - '0';
  }
  for (int i = 0; i < 10; i++) {
    if (key == mk[i]) {
      return i;
    }
  }
  return -1;
}

static const char *base_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* A modal window: draws itself over the navigator. */
typedef void (*modal_draw_t)(macjr_tty_t *tty, const void *ctx);

static void modal_show(macjr_tty_t *tty, modal_draw_t fn, const void *ctx, bool with_nav) {
  if (with_nav) {
    header_t h;
    header_build(&h);
    draw(tty, &h);
  }
  fn(tty, ctx);
  macjr_display_flush(tty);
}

/* The next key for a modal window (not a chord); KEY_ESC with no
 * keyboard. While the system monitor is open the keys are its. */
static int modal_key(macjr_tty_t *tty, int fd, modal_draw_t fn, const void *ctx) {
  (void)tty;
  (void)fn;
  (void)ctx;
  if (fd < 0) {
    return KEY_ESC;
  }
  for (;;) {
    int key = wait_key(fd);
    if (key >= 0 && !KBD_IS_CHORD(key) && !monitor_is_open()) {
      return key;
    }
  }
}

/* A message: a title and up to three centered lines. */
typedef struct {
  const char *title;
  const char *fg[3];
  char line[3][COLS];
} msg_t;

static void msg_draw(macjr_tty_t *tty, const void *ctx) {
  const msg_t *m = ctx;
  macjr_tty_window_t win;
  macjr_tty_window_center(tty, &win, MSG_W, 7);
  macjr_tty_window_draw(tty, &win, m->title);
  for (int i = 0; i < 3; i++) {
    macjr_tty_window_text(tty, &win, i + 1, m->fg[i], true, m->line[i]);
  }
}

/* Show an error until a key is pressed. */
static void error_box(macjr_tty_t *tty, int fd, const char *what, const char *name, const char *why) {
  msg_t m = {.title = what, .fg = {"97", "93", "90"}};
  snprintf(m.line[0], COLS, "%s", name);
  snprintf(m.line[1], COLS, "%s", why);
  snprintf(m.line[2], COLS, "Press any key");
  modal_show(tty, msg_draw, &m, false);
  modal_key(tty, fd, msg_draw, &m);
}

/* A copy or a delete under way: progress on the screen, Cx stops it. */
typedef struct {
  macjr_tty_t *tty;
  int fd;
  const char *verb; /* "Copying", "Deleting" */
  bool stopped;
  int err;          /* errno of the failure, 0 if none */
  char failed[COLS]; /* the name it failed on */
} job_t;

static void job_show(job_t *j, const char *name, uint64_t done) {
  msg_t m = {.title = j->verb, .fg = {"97", "93", "90"}};
  snprintf(m.line[0], COLS, "%s", name);
  if (done) {
    format_size(done > UINT32_MAX ? UINT32_MAX : (uint32_t)done, m.line[1], COLS);
  }
  snprintf(m.line[2], COLS, "Cx stops");
  msg_draw(j->tty, &m);
  macjr_display_flush(j->tty);
}

/* True once Cx has been pressed (other keys are dropped). */
static bool job_stopped(job_t *j) {
  if (j->stopped || j->fd < 0) {
    return j->stopped;
  }
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(j->fd, &rfds);
    struct timeval tv = {0, 0};
    uint8_t key;
    if (select(j->fd + 1, &rfds, NULL, NULL, &tv) <= 0 || read(j->fd, &key, 1) != 1) {
      return false;
    }
    if (key == KEY_ESC && !monitor_is_open()) {
      j->stopped = true;
      return true;
    }
  }
}

static bool job_fail(job_t *j, const char *path) {
  j->err = errno ? errno : EIO;
  snprintf(j->failed, sizeof(j->failed), "%s", base_of(path));
  return false;
}

/* Add "/name" to a working path, or false if it gets too long. */
static bool path_push(char *path, const char *name) {
  size_t len = strlen(path);
  if (len + 1 + strlen(name) >= sizeof(s_pa)) {
    errno = ENAMETOOLONG;
    return false;
  }
  snprintf(path + len, sizeof(s_pa) - len, "/%s", name);
  return true;
}

static void path_pop(char *path) {
  *strrchr(path, '/') = '\0';
}

/* Copy file s_pa to s_pb; a partial copy is removed. */
static bool copy_file(job_t *j, uint8_t *buf) {
  job_show(j, base_of(s_pa), 0);
  int in = open(s_pa, O_RDONLY);
  if (in < 0) {
    return job_fail(j, s_pa);
  }
  int out_fd = open(s_pb, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out_fd < 0) {
    job_fail(j, s_pb);
    close(in);
    return false;
  }
  bool ok = true;
  uint64_t done = 0, shown = 0;
  for (;;) {
    ssize_t n = read(in, buf, COPY_BUF);
    if (n == 0) {
      break;
    }
    if (n < 0 || write(out_fd, buf, n) != n) {
      if (n >= 0 && errno == 0) {
        errno = ENOSPC; /* a short write: the card is full */
      }
      ok = job_fail(j, n < 0 ? s_pa : s_pb);
      break;
    }
    done += n;
    if (done - shown >= SHOW_STEP) {
      shown = done;
      job_show(j, base_of(s_pa), done);
    }
    if (job_stopped(j)) {
      ok = false;
      break;
    }
  }
  close(in);
  if (close(out_fd) != 0 && ok) {
    ok = job_fail(j, s_pb);
  }
  if (!ok) {
    unlink(s_pb);
  }
  return ok;
}

/* Copy s_pa (a file, or a directory with everything in it) to s_pb. */
static bool copy_tree(job_t *j, uint8_t *buf) {
  if (!is_dir(s_pa)) {
    return copy_file(j, buf);
  }
  job_show(j, base_of(s_pa), 0);
  if (mkdir(s_pb, 0755) != 0 && errno != EEXIST) {
    return job_fail(j, s_pb);
  }
  DIR *dir = opendir(s_pa);
  if (!dir) {
    return job_fail(j, s_pa);
  }
  bool ok = true;
  struct dirent *de;
  while (ok && (de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    if (!path_push(s_pa, de->d_name) || !path_push(s_pb, de->d_name)) {
      ok = job_fail(j, de->d_name);
      break;
    }
    ok = copy_tree(j, buf) && !job_stopped(j);
    path_pop(s_pa);
    path_pop(s_pb);
  }
  closedir(dir);
  return ok;
}

/* Delete s_pa: a file, or a directory with everything in it. */
static bool delete_tree(job_t *j) {
  job_show(j, base_of(s_pa), 0);
  if (!is_dir(s_pa)) {
    return unlink(s_pa) == 0 || job_fail(j, s_pa);
  }
  for (;;) {
    /* read the directory again after each removal: never delete under a
     * readdir that is still going */
    DIR *dir = opendir(s_pa);
    if (!dir) {
      return job_fail(j, s_pa);
    }
    char name[260] = "";
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
      if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
        snprintf(name, sizeof(name), "%s", de->d_name);
        break;
      }
    }
    closedir(dir);
    if (!name[0]) {
      break;
    }
    if (!path_push(s_pa, name)) {
      return job_fail(j, name);
    }
    bool ok = delete_tree(j);
    path_pop(s_pa);
    if (!ok || job_stopped(j)) {
      return false;
    }
  }
  return rmdir(s_pa) == 0 || job_fail(j, s_pa);
}

/* After a failure: what went wrong, until a key. */
static void job_report(job_t *j, const char *what) {
  if (j->stopped) {
    return;
  }
  error_box(j->tty, j->fd, what, j->failed, strerror(j->err));
}

/* `base` in `dir` as s_pb, or with "_1", "_2", ... before its extension if
 * that name is taken. False if all are. */
static bool free_name(const char *dir, const char *base) {
  const char *dot = strrchr(base, '.');
  int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
  for (int n = 0; n < 100; n++) {
    if (n == 0) {
      snprintf(s_pb, sizeof(s_pb), "%s/%s", dir, base);
    } else {
      snprintf(s_pb, sizeof(s_pb), "%s/%.*s_%d%s", dir, stem, base, n, base + stem);
    }
    if (!exists(s_pb)) {
      return true;
    }
  }
  return false;
}

static void do_delete(macjr_tty_t *tty, int fd, const entry_t *e) {
  bool dir = e->kind == KIND_DIR;
  msg_t m = {.title = "Delete", .fg = {"97", dir ? "93" : "90", "90"}};
  snprintf(m.line[0], COLS, "%s", e->name);
  snprintf(m.line[1], COLS, "%s", dir ? "and everything in it" : "");
  snprintf(m.line[2], COLS, "8 delete    Cx cancel");
  modal_show(tty, msg_draw, &m, true);
  for (;;) {
    int key = modal_key(tty, fd, msg_draw, &m);
    if (key == KEY_ESC) {
      return;
    }
    if (digit_of(key) == 8 || key == KEY_OPEN) {
      break;
    }
  }
  job_t j = {.tty = tty, .fd = fd, .verb = "Deleting"};
  snprintf(s_pa, sizeof(s_pa), "%s/%s", s_cwd, e->name);
  errno = 0;
  bool ok = delete_tree(&j);
  /* macOS leaves "._NAME" next to a file it wrote: it goes too */
  snprintf(s_pa, sizeof(s_pa), "%s/._%s", s_cwd, e->name);
  unlink(s_pa);
  ESP_LOGI(TAG, "delete %s/%s: %s", s_cwd, e->name, ok ? "done" : j.stopped ? "stopped" : strerror(j.err));
  if (!ok) {
    job_report(&j, "Cannot delete");
  }
  if (s_clip[0] && !exists(s_clip)) {
    s_clip[0] = '\0'; /* what was copied is gone */
  }
}

/* Paste the clipboard into directory `dir`; the new name goes to `made`. */
static void do_paste(macjr_tty_t *tty, int fd, const char *dir, char *made, size_t made_len) {
  made[0] = '\0';
  const char *base = base_of(s_clip);
  if (!exists(s_clip)) {
    error_box(tty, fd, "Cannot paste", base, "It is not there any more");
    s_clip[0] = '\0';
    return;
  }
  size_t clen = strlen(s_clip);
  if (is_dir(s_clip) && strncmp(dir, s_clip, clen) == 0 && (dir[clen] == '\0' || dir[clen] == '/')) {
    error_box(tty, fd, "Cannot paste", base, "Not into itself");
    return;
  }
  if (!free_name(dir, base)) {
    error_box(tty, fd, "Cannot paste", base, "Name taken");
    return;
  }
  uint8_t *buf = malloc(COPY_BUF);
  if (!buf) {
    error_box(tty, fd, "Cannot paste", base, "Out of memory");
    return;
  }
  job_t j = {.tty = tty, .fd = fd, .verb = "Copying"};
  snprintf(s_pa, sizeof(s_pa), "%s", s_clip);
  char target[PATH_LEN + 260];
  snprintf(target, sizeof(target), "%s", s_pb);
  modal_show(tty, msg_draw, &(msg_t){.title = "Copying"}, true);
  errno = 0;
  bool ok = copy_tree(&j, buf);
  free(buf);
  ESP_LOGI(TAG, "copy %s -> %s: %s", s_clip, target, ok ? "done" : j.stopped ? "stopped" : strerror(j.err));
  if (!ok && is_dir(target)) {
    /* a directory copy that didn't finish: take away what was made */
    job_t undo = {.tty = tty, .fd = -1, .verb = "Removing"};
    snprintf(s_pa, sizeof(s_pa), "%s", target);
    delete_tree(&undo);
  }
  if (ok) {
    snprintf(made, made_len, "%s", base_of(target));
  } else {
    job_report(&j, "Cannot copy");
  }
}

/* The menu: the selected entry's name and the operations it allows. */
typedef struct {
  char title[COLS];
  int n;
  op_t op[4];
  char text[4][COLS];
} menu_t;

static void menu_draw(macjr_tty_t *tty, const void *ctx) {
  const menu_t *m = ctx;
  macjr_tty_window_t win;
  macjr_tty_window_center(tty, &win, MENU_W, m->n + 2);
  macjr_tty_window_draw(tty, &win, m->title);
  for (int i = 0; i < m->n; i++) {
    macjr_tty_window_text(tty, &win, i, "97", false, m->text[i]);
  }
}

static void menu_add(menu_t *m, op_t op, const char *fmt, const char *arg) {
  m->op[m->n] = op;
  snprintf(m->text[m->n], COLS, fmt, arg);
  m->n++;
}

/* F + В↑: the file menu on the selected entry. True if it picked a
 * program or a file to open (as В↑ would). */
static bool file_menu(macjr_tty_t *tty, int fd, char *path, size_t path_len, const boot_builtin_t **builtin) {
  const entry_t *e = s_list.sel >= 0 && s_list.sel < s_count ? &s_ents[s_list.sel] : NULL;
  bool file_op = e && e->kind >= KIND_DIR && e->kind != KIND_DEVICE; /* not "..", built-ins, /dev */
  bool can_paste = s_clip[0] && !in_dev() && !(e && e->kind == KIND_DEVDIR);

  /* where a paste goes: into the selected directory, else here */
  char dest[PATH_LEN + 260];
  snprintf(dest, sizeof(dest), "%s", s_cwd);
  if (e && e->kind == KIND_DIR) {
    snprintf(dest, sizeof(dest), "%s/%s", s_cwd, e->name);
  } else if (e && e->kind == KIND_UP) {
    *strrchr(dest, '/') = '\0';
  }

  menu_t m = {0};
  snprintf(m.title, sizeof(m.title), "%s", e ? e->name : shown_path());
  if (e && e->kind != KIND_OTHER && e->kind != KIND_DEVICE) {
    bool dir = e->kind == KIND_DIR || e->kind == KIND_UP || e->kind == KIND_DEVDIR;
    menu_add(&m, OP_EXEC, "%s", dir ? "3  Open" : "3  View / run");
  }
  if (file_op) {
    menu_add(&m, OP_DEL, "%s", "8  Delete");
    menu_add(&m, OP_COPY, "%s", "4  Copy");
  }
  if (can_paste) {
    if (e && (e->kind == KIND_DIR || e->kind == KIND_UP)) {
      menu_add(&m, OP_PASTE, "5  Paste into %s", e->name);
    } else {
      menu_add(&m, OP_PASTE, "5  Paste %s", base_of(s_clip));
    }
  }
  if (m.n == 0) {
    return false;
  }

  static const int key_of[] = {[OP_EXEC] = 3, [OP_DEL] = 8, [OP_COPY] = 4, [OP_PASTE] = 5};
  modal_show(tty, menu_draw, &m, false);
  int op = -1;
  while (op < 0) {
    int key = modal_key(tty, fd, menu_draw, &m);
    if (key == KEY_ESC) {
      return false;
    }
    for (int i = 0; i < m.n; i++) {
      if (digit_of(key) == key_of[m.op[i]]) {
        op = m.op[i];
      }
    }
  }

  switch ((op_t)op) {
    case OP_EXEC:
      return open_selected(path, path_len, builtin);
    case OP_DEL:
      do_delete(tty, fd, e);
      scan(NULL);
      break;
    case OP_COPY:
      snprintf(s_clip, sizeof(s_clip), "%s/%s", s_cwd, e->name);
      ESP_LOGI(TAG, "copied %s", s_clip);
      break;
    case OP_PASTE: {
      char made[260];
      do_paste(tty, fd, dest, made, sizeof(made));
      /* show the copy if it landed here */
      scan(made[0] && strcmp(dest, s_cwd) == 0 ? made : (e ? e->name : NULL));
      break;
    }
  }
  return false;
}


void boot_select(macjr_tty_t *tty, const boot_builtin_t *builtins, int n_builtins, char *path, size_t path_len,
                 const boot_builtin_t **builtin) {
  s_builtins = builtins;
  s_n_builtins = n_builtins;
  char keep[PATH_LEN] = "";
  if (!s_cwd[0]) {
    /* first call since boot: back where the navigator was */
    macjr_tty_list_init(&s_list, ROW_LIST, COL_LIST, COLS - 1, LIST_ROWS, item_text, NULL);
    restore_dir(keep, sizeof(keep));
  } else if (s_list.sel >= 0 && s_list.sel < s_count) {
    /* The card may have changed while the program ran: read it again and
     * stay on the same entry. */
    snprintf(keep, sizeof(keep), "%s", s_ents[s_list.sel].name);
  }
  scan(keep[0] ? keep : NULL);

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed", KBD_DEV_PATH);
  }
  header_t header;
  header_build(&header);
  draw(tty, &header);

  for (;;) {
    int key = wait_key(fd);
    if (monitor_is_open()) {
      key = -1; /* the keys are the monitor's */
    }
    header_t now;
    header_build(&now);
    if (memcmp(&now, &header, sizeof(now)) != 0) {
      header = now;
      draw_header(tty, &header);
      draw_bottom(tty, &header);
      macjr_display_flush(tty);
    }
    if (key < 0 || (KBD_IS_CHORD(key) && key != KEY_MENU)) {
      continue;
    }
    if (!s_scan_ok) {
      if (in_dev() || (!at_root() && !card_mounted())) {
        snprintf(s_cwd, sizeof(s_cwd), "%s", SDCARD_MOUNT_POINT);
      }
      scan(NULL);
      header_build(&header);
      draw(tty, &header);
      continue;
    }

    switch (key) {
      case KEY_UP:
        macjr_tty_list_move(&s_list, -1);
        break;
      case KEY_DOWN:
        macjr_tty_list_move(&s_list, 1);
        break;
      case KEY_HOME:
        macjr_tty_list_move(&s_list, -s_list.count);
        break;
      case KEY_END:
        macjr_tty_list_move(&s_list, s_list.count);
        break;
      case KEY_BACK:
        leave_dir();
        break;
      case KEY_OPEN:
        if (open_selected(path, path_len, builtin)) {
          goto picked;
        }
        break;
      case KEY_MENU:
        if (file_menu(tty, fd, path, path_len, builtin)) {
          goto picked;
        }
        break;
      default:
        continue;
    }
    header_build(&header); /* the directory may have changed */
    draw(tty, &header);
  }

picked:
  if (fd >= 0) {
    close(fd);
  }
}
