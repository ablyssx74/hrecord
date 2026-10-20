# Haiku Recorder / Streamer - Screen, Desktop Audio & Real-Time Mixing

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
- `hrecord start --allaudio` mixes every source at its own gain, scaled by 1/sqrt(N)
  (-3 dB each time the number of sources doubles), and a lookahead peak limiter keeps
  the mix under a ceiling of -1 dBFS, so several loud sources no longer clip. Options:
  `--meter` prints each source's peak and the mix's peak before and after the limiter once a
  second, with a summary at the end; `--ceiling <dB>` sets the limiter's ceiling;
  `--no-limiter` turns it off; `--mix-gain <dB>` raises or lowers the whole mix;
  `--source-gain <n>=<dB>` sets the gain of the n-th source (numbered as they are tapped);
  `--meter-ms <n>` sets how often the meter line is printed (default 1000);
  `--control` reads commands from stdin while recording (`gain <n> <dB>`, `mute <n> <0|1>`,
  `solo <n> <0|1>`, `pan <n> <-1..1>`, `mixgain <dB>`, `ceiling <dB>`, `release <ms>`,
  `limiter <0|1>`, `quit`), which is how the mixer window below drives it.
- With `--allaudio`, programs that start playing after the recording began join the mix by themselves, and
  those that quit leave it (their number is then free for the next one to start).
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

## Live streaming (`--stream`)

`hrecord start --stream <address>` also sends the screen and the sound to a server while it records
to the file (the file is unaffected if the stream breaks). The address is `rtmp://` or `rtmps://`
(YouTube, Twitch, Facebook, ... : the server's address with your stream key at the end) or
`rtsp://` (pushed to an RTSP server such as MediaMTX). Options: `--stream-bitrate <kbps>`
(2500), `--stream-audio-bitrate <kbps>` (128), `--stream-preset <x264 preset>` (veryfast; a
faster one costs less CPU) and `--stream-keyint <seconds>` (2).

Streaming needs H.264 video, which Haiku's ffmpeg does not have (x264 is GPL). It is done by a
small add-on, `stream/` (`make` there; needs the `x264_devel` package), that is installed as its own
package, `hrecord_stream` (it is GPL, hrecord stays MIT), and that hrecord loads from
`/boot/system/add-ons/hrecord/`, `~/config/non-packaged/add-ons/hrecord/` or
`~/config/add-ons/hrecord/` when it is installed. `hrecord --detect-stream` tells whether it is. In the
mixer window, a click on **Stream live** without the add-on opens the download page of the newest release.

## hRecord Mixer (window)

`make gui` builds `hrecord_mixer`, a window that runs `hrecord --allaudio` as its engine
(keep the two programs in the same folder). Press Record and every tapped source gets a
strip with a level meter, a volume fader, left/right balance, mute and solo; the master
section has a mix fader, before/after limiter meters, the limiter's on/off, ceiling and
release, and a gain-reduction bar. A right-click on a fader or balance slider resets it.
The window shows whether real-time audio is set on the sound driver (`hrecord --detect-realtime`);
the box can be changed only for a driver hrecord has no profile for.

## Apps that were recorded no longer linger in Media preferences

An app's sound player disconnects from the Mixer input it first connected to when it quits.
Recording moves that app to a new Mixer input, so its own disconnect used to miss, and the
app stayed listed in Media preferences' Audio mixer. After putting an app back, hrecord now
leaves a tiny helper (`hrecord --reap ...`) that waits for the app to quit and then removes
the leftover input.

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
