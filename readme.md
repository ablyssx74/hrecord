# Haiku Recorder - Screen, Desktop Audio & Real-Time Mixing

Record your desktop to mkv format with or without audio.
Or just record one audio source or multiple audio sources to ogg file.
Supports recording and mixing audio in Real-Time.

For the full design history, every capture mode tried, and every bug
fixed along the way, see [`research.md`](research.md).

## Build

```
make release
```

## Usage

```
hrecord [start|stop] [--low|--medium|--high] [--audioonly] [--allaudio] [--realtime] [--experimental-screen-capture] [--screen-capture-rcserver-method] [--hybrid-capture] [--tiled-capture] [--direct-tiled-capture] [--direct-raw-capture] [--raw-capture] [--list-audio-inputs] [--logfps]
```

- `hrecord` / `hrecord start` — records the screen (MJPEG in a `.mkv`
  container) to `/boot/home/hrecord_capture_YYYYMMDD_HHMMSS.mkv`, with a
  Vorbis desktop-audio track alongside it whenever the audio tap (below)
  can be set up. Every run's filename is stamped with its own start time,
  so a new recording never overwrites an earlier one.
- `hrecord start --audioonly` — desktop audio only, no screen capture, to
  `/boot/home/hrecord_capture_YYYYMMDD_HHMMSS.ogg` (open, royalty-free
  Ogg/Vorbis).
- `hrecord start --allaudio` — mixes *every* currently-playing app's audio
  together, instead of just one. Combine with plain `hrecord start` for
  screen recording with every source mixed in.
- `hrecord start --realtime` — tighter audio buffers for live monitoring
  (e.g. playing through effects while recording). Usually **not needed**
  — hrecord auto-detects an already-tuned driver's own buffer settings at
  startup for six common drivers (`hda`, `auich`, `es1370`, `echo`,
  `emuxki`, `ice1712`). Pass it manually on other drivers.
- `hrecord start --logfps` — prints actual capture rate once per second
  (frames written vs. target fps) plus a capture/encode+write time split,
  useful for telling a genuinely slow capture or disk write apart from
  normal playback smoothness. No effect under `--audioonly`.
- `hrecord stop` — stops a running recording and finalizes its file.
- `hrecord --list-audio-inputs` — lists apps currently playing sound
  (`--audioonly` needs at least one).
- `--experimental-screen-capture`, `--screen-capture-rcserver-method`,
  `--hybrid-capture` — alternative, experimental capture engines from
  earlier in this project, kept available for comparison. Mutually
  exclusive with each other and with the modes below. See `research.md`
  for what each one does.
- `--tiled-capture`, `--direct-tiled-capture` — inert confirmation flags
  for the current default (see "Screen capture" below); passing them
  doesn't change anything.
- `--direct-raw-capture` — one direct-pointer read of the whole screen per
  frame instead of tiled reads. Measured the fastest capture mode in this
  project (~43ms/frame) but kept as an explicit opt-in rather than
  promoted — see `research.md` for why.
- `--raw-capture` — the original one-read-per-frame fallback: no tiling,
  no direct pointer, simplest and most predictable if the modes above
  ever misbehave on your hardware.

## Screen capture

Screen capture defaults to tiled reads — the whole screen refreshed every
frame through small, fixed-size tiles rather than one big read — and
automatically connects a `BDirectWindow` to read those tiles straight
through its raw framebuffer pointer whenever that's verified safe on the
machine running it (checked once at startup: driver support, pixel
format, a signal-guarded test read, and a byte-for-byte match against a
real screen read).

**Confirmed on real hardware:** the direct pointer, combined with an
SSE4.1 `MOVNTDQA` streaming-load fast path for reading the framebuffer
(real GPU-mapped memory is typically write-combined, which ordinary CPU
reads handle poorly), cuts capture time roughly **10x — from ~640-760ms
per frame down to ~43-63ms per frame** — and eliminates window-drag
tearing entirely. The CPU-instruction check is a genuine runtime
capability check (`__builtin_cpu_supports("sse4.1")`), not an assumption,
so older CPUs simply never take that path.

If any of the safety checks fail — unsupported driver, connection
timeout, wrong pixel format, or the test read itself doesn't check out —
hrecord falls back automatically and silently to the exact same plain
`BScreen`-based tiled reads it always shipped. No flag needed either way,
and there's no unsafe path: only a faster one when it's available.

Full investigation — every capture mode tried before landing here, why
each one was tried, and the real-world numbers behind each decision — is
in [`research.md`](research.md).

## Screen recording quality profiles

| Profile | Flag | FPS | Max resolution (longest edge) | Notes |
|---|---|---|---|---|
| Low | `--low` | 15 | 1280px | Cheapest scaling, most compression. Best on slower hardware. |
| Medium | `--medium` (default) | 24 | 1600px | Balanced; a reasonable default for most machines. |
| High | `--high` | 30 | native (no downscale) | Full native resolution and framerate, sharpest output, most CPU. |

`hrecord start` with no profile flag uses Medium. All three profiles use
MJPEG slice-threading (spreads encode work across CPU cores) regardless of
which is picked. `--audioonly` recordings aren't affected by these flags.

## How desktop-audio capture works

1. Find one app currently playing sound (connected to the System Mixer).
2. Briefly stop it, redirect its connection through hrecord's own capture
   node instead of straight to the Mixer, then restart the app.
3. Every buffer the app produces goes to hrecord's Vorbis encoder *and*
   into a small ring buffer.
4. A `BSoundPlayer` drains that ring buffer to actually produce sound,
   through Haiku's own well-tested playback path — the same one every
   ordinary sound-playing app uses.
5. On a clean shutdown (Ctrl+C or `hrecord stop`), playback stops, the tap
   is torn out, and the app is reconnected directly to the Mixer.

This only touches hrecord's own local audio pipeline — the same signal
already going to your speakers — so nothing here intercepts audio you
couldn't otherwise hear yourself. If hrecord is killed with `kill -9` or
crashes mid-session, the hijacked app stays connected to hrecord instead
of the Mixer until restarted (or "Restart Media Services" in Media
preferences).

If nothing is playing when hrecord starts, it records video only (with a
warning) in the default mode, or fails outright under `--audioonly`.

`--allaudio` and `--realtime`/auto-detection are covered above; the full
design history (buffer sizing, pacing/backpressure fixes, and open
issues) is in [`research.md`](research.md).

## Known issue: "stale" Mixer connection

Occasionally (usually after repeatedly closing and reopening whatever app
is providing audio), the System Mixer ends up reporting a connected input
that no longer actually resolves to a live app:

```
BMediaRoster::NodeIDFor: failed (error 0xffffffff)
[-] Error: The System Mixer is reporting an audio connection hrecord can't
actually find or reach. ...
```

This is Haiku's Mixer holding a stale/ghost entry left behind by an app
that disappeared without cleanly disconnecting — state inside
`media_server` itself, not anything hrecord's own process (which starts
fresh every run) caused or can clean up from the outside. hrecord detects
this and tells you so rather than failing with a bare error. Fix: open
Media preferences and click "Restart Media Services", then try again.

## Credits

hrecord's use of Haiku's Media Kit (`BMediaRoster`, node hijacking, buffer
tapping) built on groundwork the author laid earlier while porting
[JAMin](https://jamin.sourceforge.net/) (the JACK Audio Mastering
interface) to Haiku, a project developed with help from Google AI. That
prior hands-on experience with `BMediaRoster`/`BBufferConsumer`/node
lifecycle behavior informed how hrecord's own audio tapping was designed.
