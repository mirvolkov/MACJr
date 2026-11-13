/*
 * AUDIOPLAYER.ELF -- plays a WAV file named by argv[1] on the speaker.
 *
 * Integer PCM 8/16/24/32-bit, any channel count (mixed to mono), at the file's
 * sample rate (parsed sequentially -- no seeking, which the loader doesn't
 * export). Samples are mixed to mono 16-bit and streamed through a handful of
 * buffers that take turns on the speaker queue (speaker_write()'s callback
 * frees one when it has played). The speaker's master volume still applies.
 * Cx stops.
 *
 * The navigator launches this with the chosen file as argv[1] (В↑ on a sound
 * file; main.c routes sounds here, like *.py goes to UPYTHON).
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "macjr.h"
#include "keyboard.h"
#include "speaker.h"

#define NBUF        4
#define BUF_SAMPLES 1152          /* mono samples per buffer */

static int16_t *s_bufs;           /* NBUF * BUF_SAMPLES mono int16 */
static int s_slot;
static volatile uint32_t s_written, s_done;
static volatile bool s_stop;
static int s_kbd = -1;
static macjr_tty_t *s_tty;

/* ---- tty helpers ---- */
static void say(const char *s) {
    macjr_tty_write(s_tty, s, strlen(s));
    macjr_display_flush(s_tty);
}

static void progress(unsigned cur, unsigned total) { /* seconds */
    char b[48];
    if (total) {
        snprintf(b, sizeof b, "\r %u:%02u / %u:%02u    ", cur / 60, cur % 60, total / 60, total % 60);
    } else {
        snprintf(b, sizeof b, "\r %u:%02u    ", cur / 60, cur % 60);
    }
    say(b);
}

/* ---- file helpers (posix only; the loader exports open/read/close/stat) ---- */
static ssize_t read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static bool skip_bytes(int fd, uint32_t n) {
    uint8_t tmp[256];
    while (n > 0) {
        size_t want = n < sizeof(tmp) ? n : sizeof(tmp);
        ssize_t r = read(fd, tmp, want);
        if (r <= 0) {
            return false;
        }
        n -= (uint32_t)r;
    }
    return true;
}

/* ---- streaming engine ---- */
static void on_done(void *ctx, esp_err_t r) {
    (void)ctx;
    (void)r;
    s_done++;
}

static void poll_keys(void) {
    uint8_t k;
    while (s_kbd >= 0 && read(s_kbd, &k, 1) == 1) {
        if (!KBD_IS_CHORD(k) && k == KEY_ESC) {
            s_stop = true;
        }
    }
}

/* Queue n mono samples; blocks (polling keys) until a buffer frees. */
static bool feed(const int16_t *s, int n) {
    if (n <= 0) {
        return true;
    }
    if (n > BUF_SAMPLES) {
        n = BUF_SAMPLES;
    }
    while (s_written - s_done >= NBUF) {
        poll_keys();
        if (s_stop) {
            return false;
        }
        sys_delay(3);
    }
    int16_t *out = s_bufs + (size_t)s_slot * BUF_SAMPLES;
    memcpy(out, s, (size_t)n * sizeof(int16_t));
    s_slot = (s_slot + 1) % NBUF;
    while (speaker_write(out, (size_t)n, on_done, NULL) == ESP_ERR_NO_MEM) {
        poll_keys();
        if (s_stop) {
            return false;
        }
        sys_delay(5);
    }
    s_written++;
    return true;
}

static void drain(void) {
    while (s_done < s_written) {
        sys_delay(5);
    }
}

/* ---- WAV ---- */
#define WAV_FMT_PCM 1
#define WAV_FMT_EXT 0xFFFEu

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct {
    uint16_t format, channels;
    uint32_t rate;
    uint16_t block_align, bits;
} wav_fmt_t;

/* Parse up to the "data" chunk by reading forward; leaves fd at the samples. */
static bool wav_parse(int fd, wav_fmt_t *fmt, uint32_t *data_size) {
    uint8_t b[40];
    if (read_full(fd, b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        return false;
    }
    bool have_fmt = false;
    for (;;) {
        if (read_full(fd, b, 8) != 8) {
            return false;
        }
        uint32_t len = le32(b + 4);
        if (!memcmp(b, "fmt ", 4)) {
            if (len < 16) {
                return false;
            }
            uint32_t n = len < sizeof(b) ? len : (uint32_t)sizeof(b);
            if (read_full(fd, b, n) != (ssize_t)n) {
                return false;
            }
            fmt->format = le16(b);
            fmt->channels = le16(b + 2);
            fmt->rate = le32(b + 4);
            fmt->block_align = le16(b + 12);
            fmt->bits = le16(b + 14);
            if (fmt->format == WAV_FMT_EXT && n >= 26) {
                fmt->format = le16(b + 24);
            }
            if (!skip_bytes(fd, (len - n) + (len & 1))) {
                return false;
            }
            have_fmt = true;
        } else if (!memcmp(b, "data", 4)) {
            if (!have_fmt) {
                return false;
            }
            *data_size = len;
            return true;
        } else if (!skip_bytes(fd, len + (len & 1))) {
            return false;
        }
    }
}

static int16_t wav_mono(const uint8_t *p, const wav_fmt_t *fmt) {
    int bytes = fmt->bits / 8;
    int32_t sum = 0;
    for (int c = 0; c < fmt->channels; c++, p += bytes) {
        sum += bytes == 1 ? ((int32_t)p[0] - 128) * 256 : (int16_t)le16(p + bytes - 2);
    }
    return (int16_t)(sum / fmt->channels);
}

static int play_wav(int fd) {
    wav_fmt_t fmt = {0};
    uint32_t data_size = 0;
    if (!wav_parse(fd, &fmt, &data_size)) {
        say("Not a supported WAV file.\n");
        return 1;
    }
    if (fmt.format != WAV_FMT_PCM || !fmt.channels || !fmt.rate || fmt.bits % 8 || fmt.bits < 8 ||
        fmt.bits > 32 || fmt.block_align != fmt.channels * fmt.bits / 8) {
        say("Unsupported WAV format (need integer PCM).\n");
        return 1;
    }
    char hdr[64];
    snprintf(hdr, sizeof hdr, "%lu Hz  %uch  %u-bit WAV\n\n", (unsigned long)fmt.rate, fmt.channels, fmt.bits);
    say(hdr);

    uint8_t *raw = malloc((size_t)BUF_SAMPLES * fmt.block_align);
    int16_t *mono = malloc((size_t)BUF_SAMPLES * sizeof(int16_t));
    if (!raw || !mono) {
        free(raw);
        free(mono);
        say("Out of memory.\n");
        return 1;
    }
    speaker_set_sample_rate(fmt.rate);

    uint32_t total_s = (data_size != 0xFFFFFFFFu) ? data_size / fmt.block_align / fmt.rate : 0;
    uint32_t played = 0, last = 0xffffffffu, left = data_size;
    for (;;) {
        if (s_stop) {
            break;
        }
        size_t want = (size_t)BUF_SAMPLES * fmt.block_align;
        if (data_size != 0xFFFFFFFFu) {
            if (left == 0) {
                break;
            }
            if (want > left) {
                want = left;
            }
        }
        size_t frames = (size_t)read_full(fd, raw, want) / fmt.block_align;
        if (frames == 0) {
            break;
        }
        if (data_size != 0xFFFFFFFFu) {
            left -= (uint32_t)frames * fmt.block_align;
        }
        for (size_t i = 0; i < frames; i++) {
            mono[i] = wav_mono(raw + i * fmt.block_align, &fmt);
        }
        if (!feed(mono, (int)frames)) {
            break;
        }
        played += (uint32_t)frames;
        if (played / fmt.rate != last) {
            last = played / fmt.rate;
            progress(last, total_s);
        }
    }
    free(raw);
    free(mono);
    return 0;
}

/* case-insensitive suffix test (no strcasecmp dependency) */
static bool ends_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) {
        return false;
    }
    s += ls - lf;
    for (size_t i = 0; i < lf; i++) {
        char a = s[i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) {
            return false;
        }
    }
    return true;
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
    s_tty = tty;
    macjr_tty_header(tty, "Audio Player");
    if (argc < 2 || !argv[1]) {
        say("\nUsage: AUDIOPLAYER <file.wav>\n");
        return 1;
    }
    const char *path = argv[1];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char line[80];
    snprintf(line, sizeof line, "\n%s\n", base);
    say(line);

    s_bufs = malloc((size_t)NBUF * BUF_SAMPLES * sizeof(int16_t));
    if (!s_bufs) {
        say("Out of memory.\n");
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        say("Cannot open the file.\n");
        free(s_bufs);
        return 1;
    }
    s_kbd = open(KBD_DEV_PATH, O_RDONLY);
    if (s_kbd >= 0) {
        fcntl(s_kbd, F_SETFL, O_NONBLOCK);
    }
    uint32_t orig_rate = speaker_sample_rate();
    say("Cx to stop\n\n");

    int rc;
    if (ends_ci(path, ".wav")) {
        rc = play_wav(fd);
    } else {
        say("Only WAV is supported.\n");
        rc = 1;
    }

    drain();
    speaker_set_sample_rate(orig_rate);
    say(s_stop ? "\nStopped.\n" : "\nDone.\n");

    if (s_kbd >= 0) {
        close(s_kbd);
    }
    close(fd);
    free(s_bufs);
    return rc;
}
