# 15. A WAV player

MACJr learned to play sound files. `AUDIOPLAYER.ELF` takes a file on its
command line and streams it to the speaker; in the navigator, select a `.wav`
and press В↑.

## The speaker as a stream

The speaker HAL already had the right primitive:

```c
esp_err_t speaker_write(const int16_t *samples, size_t count,
                        speaker_write_cb_t on_complete, void *ctx);
```

Queue a block of mono 16-bit PCM, get a callback when it has finished playing.
The firmware now exports three calls to programs -- `speaker_write`,
`speaker_set_sample_rate`, `speaker_sample_rate` -- and the player does the rest
in a few buffers that take turns on the speaker's queue.

Flow control needs no semaphore. The player keeps a ring of four buffers and
two counters: `s_written`, which it bumps after each `speaker_write`, and
`s_done`, which the completion callback bumps. Each counter has exactly one
writer, so no lock is required; the player simply waits while
`s_written - s_done` reaches four, and polls the keyboard in that wait so Cx
stops the track at once.

## WAV without seeking

The loader exports `open`/`read`/`close` and `stat`, but not `lseek`. A RIFF
file is normally parsed by *seeking* past the chunks you don't want -- so the
player reads them and throws them away instead, always walking forward:
`RIFF`/`WAVE`, then each chunk's 8-byte header, keeping `fmt `, skipping the
rest, until `data`. From there any integer PCM (8/16/24/32-bit, any channel
count) is mixed down to mono at the file's own sample rate, which the speaker
is retuned to for the duration and set back afterwards. The master volume still
applies.

## Routing from the navigator

The navigator already coloured sound files purple but did nothing when you
pressed В↑ on one. Now В↑ on a sound returns its path, and `main.c` hands it to
`AUDIOPLAYER.ELF` as `argv[1]` -- the very path a `.py` file takes to
`UPYTHON.ELF`. One line in the shell, no new machinery:

```c
} else if (is_audio(path)) {
  exec_launch_with(tty, PLAYER_PATH, path);
}
```

## The MP3 that wasn't

For a while the player also decoded MP3, with the public-domain **minimp3**
compiled straight into the ELF (28 KB, against 5 KB for WAV alone): read the
whole file into PSRAM, decode frame by frame, mix to mono, feed the same
stream. It worked -- but MP3 only earns its keep on real music, and the project
is about small, legible pieces, so it came back out. The player is WAV only,
and the streaming pipeline it left behind is the part worth keeping.

**Result:** a 5 KB WAV player, launched from the navigator or by name, that
streams any integer-PCM WAV to the speaker through a four-buffer pipeline, with
a stop key and a running clock.
