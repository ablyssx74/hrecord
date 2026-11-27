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
hrecord [start|stop] [--low|--medium|--high] [--audioonly] [--allaudio] [--realtime] [--experimental-screen-capture] [--screen-capture-rcserver-method] [--hybrid-capture] [--tiled-capture] [--direct-tiled-capture] [--direct-raw-capture] [--raw-capture] [--list-audio-inputs] [--logfps] [--window <token>]
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
mixer window, a click on **Stream live** without the add-on asks whether to download it. The download window fetches the
package for your system from hrecord's release on GitHub (the release of the same version, or the newest one), shows its
progress, and compares the file's SHA-256 checksum with the one GitHub gives for it; a file that does not match is thrown
away. **Install** then installs it with `pkgman` (which also fetches x264 if it is missing).

## hRecord Mixer (window)

`make gui` builds `hrecord_mixer`, a window that runs `hrecord --allaudio` as its engine
(keep the two programs in the same folder). It is laid out like OBS Studio. With **Advanced Options** ticked it has two
pages, **Mixer** and **Nodes**.

The **Mixer** page has a preview of what will be recorded and three docks under it:

- **Sources**: what the video shows. **Entire desktop**, **Sound only (no screen)**, or one of the open windows.
- **Audio Mixer**: every program that plays gets a strip with a level meter, a volume fader, left/right balance,
  mute and solo; the master section has a mix fader, a level meter (after the limiter), the limiter's on/off, ceiling and
  release, and a gain-reduction bar. A right-click on a fader or balance slider resets it. A right-click elsewhere in the
  mixer chooses the layout: the sources **side by side**, or **in rows**, one over the other, as OBS Studio shows them. When
  they do not fit, the mixer scrolls (its rounded scroll bar, or the mouse wheel anywhere but on a fader).
  The list of sources, the log, and the boxes and the connection list of the Nodes page scroll the same way.
- **Controls**: Start/Stop Recording, Stream live and its settings, the screen quality, and boxes for the preview, the
  log and the advanced options.

### The master equalizer

The **EQ** button in the master section opens a 15-band equalizer for the whole mix, with the bands (50 Hz to 20 kHz, one
octave wide, -15 to +15 dB) and the presets (Flat, Rock, Jazz, Bass Boost) of the one in hTV. It sits ahead of the limiter,
so it is in what you hear, in the recording and in the stream. The button is blue while the equalizer is on. On the control
channel: `eq <0|1>` and `eqband <n> <dB>` (n from 1 to 15).

### The tray icon

The mixer and the hrecord it runs are background programs: neither the Deskbar nor a dock lists them. Minimizing the window
hides it, as **Hide** does. (An hrecord started from a Terminal is not listed either: Ctrl+C or `hrecord stop` ends it.)

While the mixer runs it has an icon in the Deskbar's tray, with a red dot on it while it records. A click on it opens a
menu: **Record**, **Stop**, **Show**, **Hide** and **Quit**. A hidden mixer goes on working (and recording); **Show**, or
starting the mixer again, brings its window back. Closing the window quits the mixer, as before. The icon is the program
`hrecord_tray`, which has to be beside `hrecord_mixer` (the package puts it in `add-ons/hrecord`).

### Recording one window

On a composited desktop whose app_server can hand out a window's pixels (a RenkuOS change that is not part of every
build), the whole window is recorded, also while other windows cover it. Everywhere else the part of the screen the window
covers is recorded, with whatever is on top of it. The video has the size the window had when the recording began; a window
that is resized is fitted into it. While the window is minimized or on another workspace the last picture stays, and once it
is closed the video is black. The mouse pointer and the window's menus and pop-ups are not in the picture when it comes from
the compositor. On the command line: `--window <app_server window token>`; on the control channel: `capwindow <token>`
(-1: the whole desktop) before `record`.

### The preview

The preview can be switched off (**Show preview**) to save processor time. Where there is no composited desktop it is read
from the screen about once a second, and rests while a recording runs.

The window shows whether real-time audio is set on the sound driver (`hrecord --detect-realtime`);
the box can be changed only for a driver hrecord has no profile for.

The window starts `hrecord --allaudio --standby --control`: the sources are connected as soon as
it opens (before Record) and put back when it closes. Its **Nodes** page shows the audio nodes and
their connections: select a box to disconnect or reconnect it, take a program off the mix, or send
it into the mix as Stereo, Left or Right (hardware inputs have L and R boxes).

### Sharing with other programs (on by default)

Three things are on, and their boxes (and the Nodes page) are shown only while **Advanced Options** is ticked (unticking it
turns them on again):

- **Share this input with programs** (Nodes page): hrecord is the system's audio input (the input hub), so several programs
  (for example Guitarix and rakarrack) can use one line-in, each getting Stereo, Left or Right.
- **Offer the mix to programs that record** (master section): a second node, **hrecord Mix**, hands out what is heard: the mix
  of everything that plays, with the effects of a program such as rakarrack in it. A program that records can take it as an
  input of its own (Wine 11.20-69 and later list it in Windows programs such as Ardour as "What you hear (hrecord mix)").
- **Leave out the recording program's own sound**: a program that records the mix does not get what it plays itself (Ardour
  recording a second track does not record the first one again); what you hear is not changed.

On the control channel: `hub 1` (and `hub 0`), `mixhub 1` (and `mixhub 0`), and `mixhubself 1` to let the recording program
hear its own sound in the mix (`mixhubself 0` leaves it out, the default).

## Apps that were recorded no longer linger in Media preferences

An app's sound player disconnects from the Mixer input it first connected to when it quits.
Recording moves that app to a new Mixer input, so its own disconnect used to miss, and the
app stayed listed in Media preferences' Audio mixer. After putting an app back, hrecord now
leaves a tiny helper (`hrecord --reap ...`) that waits for the app to quit and then removes
the leftover input.

That helper stays in ProcessController (thread name "hrecord cleanup: waiting for <app> to quit")
until that app quits; it sleeps and uses no CPU. To end it earlier, run `hrecord --stop-reapers`
(the app's Mixer entry is then not removed when it quits later).

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
