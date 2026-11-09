/*
 * Boot shell: a file navigator in the style of DOS Navigator, one panel
 * over the SD card, starting in its root.
 *
 *   ╔═[home]══════════[/BIN]════════[14:05]═╗
 *   ║..                                    ▲
 *   ║HELLO                       ELF  3.1 KB▒
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
 * black. The scroll bar is the panel's right edge, between ╗ and ╝.
 *
 * The panel lists name, extension and file size, directories first;
 * built-in programs come first in the root. Colors by type: directories
 * white, programs (ELF, built-ins) cyan, Python scripts green, images
 * yellow, sounds purple, anything else grey. Hidden files are left out: FAT hidden or system
 * attribute, or a name starting with '.' (macOS metadata).
 *
 * Keys, as in the WIFI program: П→x / x→П move, В↑ opens a directory,
 * starts a program or runs a script (main.c hands a .py file to
 * UPYTHON.ELF), Cx goes up a directory. Images and sounds do nothing
 * yet. The selection is black on cyan, as in DOS Navigator.
 *
 * The directory is saved in the db (nav.dir) whenever it changes, and the
 * entry started from it (nav.file) when a program starts; after a reboot
 * the navigator opens there again, on that entry.
 */
#include "boot.h"

#include <dirent.h>
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
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "monitor.h"
#include "net.h"
#include "sdcard.h"
#include "tty/list.h"

static const char *TAG = "boot";

#define KEY_UP   KEY_MK_P_TO_X /* П→x */
#define KEY_DOWN KEY_MK_X_TO_P /* x→П */
#define KEY_OPEN KEY_MK_ENTER  /* В↑ */
#define KEY_BACK KEY_ESC       /* Cx */

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

typedef enum {
  KIND_UP, /* ".." */
  KIND_BUILTIN,
  KIND_DIR,
  KIND_ELF,
  KIND_SCRIPT, /* .py, run by UPYTHON.ELF */
  KIND_IMAGE,
  KIND_SOUND,
  KIND_OTHER,
} kind_t;

static const char *const s_kind_fg[] = {
    [KIND_UP] = "\x1b[97m",    [KIND_BUILTIN] = "\x1b[96m", [KIND_DIR] = "\x1b[97m",
    [KIND_ELF] = "\x1b[96m",   [KIND_SCRIPT] = "\x1b[92m", [KIND_IMAGE] = "\x1b[93m",   [KIND_SOUND] = "\x1b[95m",
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

/* The directory as the user sees it: "/" for the card's root. */
static const char *shown_path(void) {
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
  const char *ext = ext_of(name);
  if (strcasecmp(ext, "ELF") == 0) {
    return KIND_ELF;
  }
  if (strcasecmp(ext, "PY") == 0) {
    return KIND_SCRIPT;
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

  const char *dot_ext = e->kind == KIND_UP || e->kind == KIND_BUILTIN ? "" : ext_of(e->name);
  size_t base_len = *dot_ext ? (size_t)(dot_ext - 1 - e->name) : strlen(e->name);
  fit(name, e->name, base_len, NAME_W);
  snprintf(ext, sizeof(ext), "%s", dot_ext); /* cut to 3 */

  if (e->kind == KIND_BUILTIN) {
    snprintf(size, sizeof(size), "BUILTIN");
  } else if (e->kind != KIND_UP && e->kind != KIND_DIR) {
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
 * cells inside the brackets, drawn in COLOR_FIELD. */
typedef struct {
  char text[COLS + 1];
  char mark[COLS];
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

static void draw_frame(macjr_tty_t *tty) {
  for (int r = ROW_LIST; r < ROW_BOTTOM; r++) {
    at(tty, r, 1, COLOR_FRAME G_V);
  }
  char row[COLS + 1];
  frame_row(row, G_BL, G_BR);
  at(tty, ROW_BOTTOM, 1, COLOR_FRAME "%s\x1b[0m", row);
}

static void draw(macjr_tty_t *tty, const header_t *header) {
  out(tty, "\x1b[?25l\x1b[0m\x1b[2J");
  draw_header(tty, header);
  draw_frame(tty);
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
  bool monitor_was_open = false;

  for (;;) {
    int key = wait_key(fd);
    if (monitor_is_open()) {
      monitor_was_open = true; /* it owns the screen; keys are for it */
      continue;
    }
    header_t now;
    header_build(&now);
    if (monitor_was_open) {
      monitor_was_open = false;
      header = now;
      draw(tty, &header);
    } else if (memcmp(&now, &header, sizeof(now)) != 0) {
      header = now;
      draw_header(tty, &header);
      macjr_display_flush(tty);
    }
    if (key < 0 || KBD_IS_CHORD(key)) {
      continue;
    }
    if (!s_scan_ok) {
      if (!at_root() && !card_mounted()) {
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
      case KEY_BACK:
        leave_dir();
        break;
      case KEY_OPEN: {
        if (s_list.sel < 0) {
          break;
        }
        const entry_t *e = &s_ents[s_list.sel];
        if (e->kind == KIND_UP) {
          leave_dir();
        } else if (e->kind == KIND_DIR) {
          enter_dir(e->name);
        } else if (e->kind == KIND_BUILTIN) {
          *builtin = &s_builtins[e->builtin];
          save(DB_KEY_FILE, e->name);
          ESP_LOGI(TAG, "%s (built in) selected", e->name);
          goto picked;
        } else if (e->kind == KIND_ELF || e->kind == KIND_SCRIPT) {
          *builtin = NULL;
          snprintf(path, path_len, "%s/%s", s_cwd, e->name);
          save(DB_KEY_FILE, e->name);
          ESP_LOGI(TAG, "%s selected", path);
          goto picked;
        }
        break; /* images and sounds: later */
      }
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
