/*
 * The /dev directory (see devfs.h): a VFS entry at DEVFS_DIR with the
 * directory calls only. The devices are its list of names; a path under
 * DEVFS_DIR that no driver claims ends up here and isn't found.
 */
#include "devfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "devfs";

typedef struct {
  char name[DEVFS_NAME_MAX];
  bool is_dir;
} devfs_ent_t;

typedef struct {
  DIR dir; /* the VFS's part comes first */
  int next;
  struct dirent ent;
} dev_dir_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static devfs_ent_t s_devs[DEVFS_MAX];
static int s_count;
static bool s_registered;

/* "" or "/": the directory itself. */
static bool is_root(const char *path) {
  return path[0] == '\0' || strcmp(path, "/") == 0;
}

/* The device "/name" is, or -1. */
static int find(const char *path) {
  if (path[0] != '/') {
    return -1;
  }
  for (int i = 0; i < s_count; i++) {
    if (strcmp(s_devs[i].name, path + 1) == 0) {
      return i;
    }
  }
  return -1;
}

static int dir_stat(void *ctx, const char *path, struct stat *st) {
  (void)ctx;
  memset(st, 0, sizeof(*st));
  if (is_root(path)) {
    st->st_mode = S_IFDIR | 0555;
    return 0;
  }
  taskENTER_CRITICAL(&s_mux);
  int i = find(path);
  bool is_dir = i >= 0 && s_devs[i].is_dir;
  taskEXIT_CRITICAL(&s_mux);
  if (i < 0) {
    errno = ENOENT;
    return -1;
  }
  st->st_mode = is_dir ? S_IFDIR | 0555 : S_IFCHR | 0666;
  return 0;
}

static int dir_access(void *ctx, const char *path, int amode) {
  (void)ctx;
  if (is_root(path) && !(amode & W_OK)) {
    return 0;
  }
  errno = is_root(path) ? EACCES : ENOENT;
  return -1;
}

static int dev_open(void *ctx, const char *path, int flags, int mode) {
  (void)ctx;
  (void)flags;
  (void)mode;
  errno = is_root(path) ? EISDIR : ENOENT;
  return -1;
}

static DIR *dir_open(void *ctx, const char *name) {
  (void)ctx;
  if (!is_root(name)) {
    errno = ENOENT;
    return NULL;
  }
  dev_dir_t *d = calloc(1, sizeof(*d));
  if (!d) {
    errno = ENOMEM;
    return NULL;
  }
  return &d->dir;
}

static int dir_read_r(void *ctx, DIR *pdir, struct dirent *entry, struct dirent **out) {
  (void)ctx;
  dev_dir_t *d = (dev_dir_t *)pdir;
  taskENTER_CRITICAL(&s_mux);
  if (d->next >= s_count) {
    taskEXIT_CRITICAL(&s_mux);
    *out = NULL;
    return 0;
  }
  const devfs_ent_t *dev = &s_devs[d->next];
  memset(entry, 0, sizeof(*entry));
  entry->d_ino = (ino_t)(d->next + 1);
  entry->d_type = dev->is_dir ? DT_DIR : DT_CHR;
  memcpy(entry->d_name, dev->name, sizeof(dev->name));
  d->next++;
  taskEXIT_CRITICAL(&s_mux);
  *out = entry;
  return 0;
}

static struct dirent *dir_read(void *ctx, DIR *pdir) {
  dev_dir_t *d = (dev_dir_t *)pdir;
  struct dirent *out;
  dir_read_r(ctx, pdir, &d->ent, &out);
  return out;
}

static long dir_tell(void *ctx, DIR *pdir) {
  (void)ctx;
  return ((dev_dir_t *)pdir)->next;
}

static void dir_seek(void *ctx, DIR *pdir, long offset) {
  (void)ctx;
  ((dev_dir_t *)pdir)->next = offset < 0 ? 0 : (int)offset;
}

static int dir_close(void *ctx, DIR *pdir) {
  (void)ctx;
  free(pdir);
  return 0;
}

static const esp_vfs_dir_ops_t s_dir_ops = {
  .stat_p = dir_stat,
  .access_p = dir_access,
  .opendir_p = dir_open,
  .readdir_p = dir_read,
  .readdir_r_p = dir_read_r,
  .telldir_p = dir_tell,
  .seekdir_p = dir_seek,
  .closedir_p = dir_close,
};

static const esp_vfs_fs_ops_t s_ops = {
  .open_p = dev_open,
  .dir = &s_dir_ops,
};

static esp_err_t add(const char *name, bool is_dir) {
  if (!name || !name[0] || strchr(name, '/') || strlen(name) >= DEVFS_NAME_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = ESP_OK;
  taskENTER_CRITICAL(&s_mux);
  char path[DEVFS_NAME_MAX + 1] = "/";
  strcat(path, name);
  if (find(path) >= 0) {
    /* already listed */
  } else if (s_count == DEVFS_MAX) {
    ret = ESP_ERR_NO_MEM;
  } else {
    strcpy(s_devs[s_count].name, name);
    s_devs[s_count].is_dir = is_dir;
    s_count++;
  }
  taskEXIT_CRITICAL(&s_mux);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s not listed: %s", name, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t devfs_add(const char *name, bool is_dir) {
  if (!s_registered) {
    esp_err_t err = esp_vfs_register_fs(DEVFS_DIR, &s_ops, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, NULL);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "cannot register %s: %s", DEVFS_DIR, esp_err_to_name(err));
      return err;
    }
    s_registered = true;
  }
  return add(name, is_dir);
}
