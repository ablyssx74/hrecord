# Haiku Terminal Screen Recorder App.

## Build

```
make release

```

## Usage

```
hrecord [start|stop] [--low|--medium|--high] [--audioonly [--allaudio]] [--list-audio-inputs]
```

- `hrecord` / `hrecord start` — records the screen (MJPEG in a `.mkv`
  container) to `/boot/home/hrecord_capture.mkv`, with a Vorbis desktop-audio
  track alongside it whenever the audio tap (below) can be set up.
- `hrecord start --audioonly` — records desktop audio only (no screen
  capture) to `/boot/home/hrecord_capture.ogg`, an Ogg/Vorbis file. Both Ogg
  and Vorbis are open, royalty-free formats, so this carries none of the
  licensing baggage a proprietary audio codec would. Taps one currently
  playing app (see "How desktop-audio capture works" below).
- `hrecord start --audioonly --allaudio` — same as above, but taps *every*
  app currently playing sound and mixes them together, instead of just one.
  See "Recording every audio source at once" below.
- `hrecord stop` — signals a running recording instance to stop and finalize
  its output file.
- `hrecord --list-audio-inputs` — lists the apps currently feeding the
  System Mixer (i.e. currently playing sound). `--audioonly` needs at least
  one.

## Screen recording quality profiles

Capturing and MJPEG-encoding the full screen every frame at 30fps, uncapped,
was consistently pegging a full CPU core — enough that the mouse itself
would visibly lag, since Haiku's own input/compositing work was fighting
hrecord for that core.

**The actual dominant cost turned out not to be the encoder at all.** Every
captured frame was pulled via `BScreen::GetBitmap()`, which allocates a
brand-new `BBitmap` — and the shared-memory area `app_server` backs it
with — from scratch on every single call. That per-frame allocate/IPC/free
cycle, at full native resolution, ran the same way regardless of profile,
which is why lowering fps/resolution/quality alone didn't fix the lag: the
capture side was never touched by any of that. hrecord now allocates one
`BBitmap` up front and refills it in place every frame via
`BScreen::ReadBitmap()` instead, which removes that allocation entirely.
The three profiles below still matter for the encode side (and for output
size), but the capture-side fix is what actually addresses the sluggish,
laggy-mouse symptom.

Three profiles trade recording quality for headroom:

| Profile | Flag | FPS | Max resolution (longest edge) | Notes |
|---|---|---|---|---|
| Low | `--low` | 15 | 1280px | Cheapest scaling algorithm, most compression. Best choice on slower hardware or when you just need a legible reference recording. |
| Medium | `--medium` (default) | 24 | 1600px | Balanced; a reasonable default for most machines. |
| High | `--high` | 30 | native (no downscale) | Full native resolution and framerate, sharpest output, most CPU. |

`hrecord start` with no profile flag uses Medium. All three profiles also
benefit from MJPEG slice-threading, which spreads the actual JPEG encode
work across available CPU cores regardless of which profile is picked —
that part isn't something the flags affect, it's on for every recording.

`--audioonly` recordings aren't affected by these flags — there's no video
being captured to scale or encode.

**Practical ceiling:** even with the per-frame `BBitmap` allocation removed,
some mouse-cursor lag remains while recording, at any profile. Haiku's
`app_server` is a non-compositing window server — it draws windows and the
cursor directly, rather than compositing pre-rendered layers the way most
modern desktops do — so a `BScreen::ReadBitmap()` capture request still has
to be serviced by the same code path doing that drawing, and briefly
contends with it every time hrecord asks for a frame. That's an
architectural property of `app_server` itself, not something fixable from
outside it. The profiles get you the rest of the way there by controlling
how often and how expensively that contention happens.

## How desktop-audio capture works

Two earlier approaches tried to put hrecord's own Media Kit node back into
the playback graph as a genuine producer — first spliced between the System
Mixer's *output* and the sound card (crashed Haiku's own Mixer control
thread, reproducibly, three times), then between one app's output and a
fresh Mixer input (didn't crash, but the audio never became audible despite
being delivered without error — consistent with something in the Mixer's
own internal buffer routing not fully recognizing hrecord's connection,
which isn't fixable from outside Haiku's own Mixer source).

The current approach doesn't try to be a producer at all:

1. Find one app currently playing sound (i.e. connected to the Mixer).
2. Briefly stop it, redirect its connection through hrecord's own capture
   node instead of straight to the Mixer, then restart the app.
3. Every buffer the app produces is handed to hrecord's Vorbis encoder *and*
   copied into a small ring buffer.
4. A `BSoundPlayer` drains that ring buffer to actually produce sound,
   connecting to the System Mixer through Haiku's own well-tested playback
   path — the same one every ordinary sound-playing app already uses
   successfully — instead of hrecord's own connection code.
5. On a clean shutdown (Ctrl+C or `hrecord stop`), playback stops, the tap
   is torn out, and the app is reconnected directly to the Mixer, exactly as
   it was found.

This only ever touches hrecord's own local audio pipeline — the same signal
already being sent to your speakers — so there's nothing here that
intercepts audio you couldn't otherwise hear yourself.

**Caveat:** the restore-on-exit step only runs on a normal shutdown. If
hrecord is killed with `kill -9` or crashes while the tap is spliced in, the
hijacked app is left connected to hrecord instead of the Mixer and will stop
being audible until it's restarted (or, if needed, Media preferences'
"Restart Media Services").

If nothing is currently playing when hrecord starts, it records video only
(with a warning) in the default mode, or fails outright for `--audioonly`
since there'd be nothing to capture.

## Recording every audio source at once (`--allaudio`)

The single-tap approach above hijacks exactly one app's own connection to
the Mixer. `--allaudio` repeats that same hijack, completely unmodified,
once for *every* app currently playing sound — but that leaves N
independent streams, each in whatever raw format its own app happened to
negotiate (sample rate, channel count, and sample encoding can all differ
between apps). Rather than trying to splice into the Mixer's own internal
mixing (the approach that crashed Haiku's Mixer control thread early on,
reproducibly, and was abandoned for exactly that reason — see above), each
tapped source is independently resampled to one fixed format (48kHz stereo
float) and then summed together entirely in hrecord's own code. One shared
`BSoundPlayer` plays back that combined mix and feeds it to the Vorbis
encoder, instead of one per source.

The mix is a plain average (divide the sum by however many sources are
tapped), not a straight sum — since every individual source is already
within its own valid range, an average of N sources can never clip, at the
cost of getting quieter as more apps join in. That's a deliberate tradeoff:
a guaranteed-safe mix over a louder one that occasionally distorts.

If any individual app can't be tapped (stale Mixer state, a failed
connection), it's skipped with a warning and the rest continue; the whole
thing only fails if *no* source could be tapped at all. On shutdown, every
tapped app is restored directly to the Mixer, the same way the single-tap
path already does.

**Not yet tested on real Haiku hardware.** This was built and reviewed
carefully against Haiku's Media Kit source, but --allaudio hasn't actually
been run yet — if the mix sounds off (too quiet, distorted, or silent),
that's the first thing to report back.

## Known issue: "stale" Mixer connection

Occasionally (usually after repeatedly closing and reopening whatever app is
providing audio), the System Mixer ends up reporting a connected input that
no longer actually resolves to a live app:

```
BMediaRoster::NodeIDFor: failed (error 0xffffffff)
[-] Error: The System Mixer is reporting an audio connection hrecord can't
actually find or reach. ...
```

This is Haiku's Mixer holding onto a stale/ghost entry left behind by an app
that disappeared (closed, killed, or crashed) without cleanly disconnecting
first — state that lives inside `media_server` itself, not anything
hrecord's own process (which starts fresh and holds no state between runs)
can have caused or can clean up from the outside. hrecord detects this and
tells you so rather than failing with a bare, unexplained error. The fix is
the same one Haiku's own Media preferences offers for this exact situation:
open Media preferences and click "Restart Media Services", then try again.
