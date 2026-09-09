# Haiku Terminal Screen Recorder App.

## Build

```
make release

```

## Usage

```
hrecord [start|stop] [--low|--medium|--high] [--audioonly] [--allaudio] [--realtime] [--list-audio-inputs]
```

- `hrecord` / `hrecord start` — records the screen (MJPEG in a `.mkv`
  container) to `/boot/home/hrecord_capture.mkv`, with a Vorbis desktop-audio
  track alongside it whenever the audio tap (below) can be set up.
- `hrecord start --audioonly` — records desktop audio only (no screen
  capture) to `/boot/home/hrecord_capture.ogg`, an Ogg/Vorbis file. Both Ogg
  and Vorbis are open, royalty-free formats, so this carries none of the
  licensing baggage a proprietary audio codec would. Taps one currently
  playing app (see "How desktop-audio capture works" below).
- `hrecord start --allaudio` — taps *every* app currently playing sound and
  mixes them together, instead of just one. Works with or without
  `--audioonly` -- combine it with plain `hrecord start` to get screen
  recording with every currently-playing app mixed into its audio track,
  not just one. See "Recording every audio source at once" below.
- `hrecord start --realtime` — trims the audio-tap ring buffers and the
  buffer size requested from BSoundPlayer for lower live-monitoring
  latency, at the cost of a smaller safety margin against glitches. Works
  with or without `--allaudio`/`--audioonly`. Worth it for genuinely
  real-time use (e.g. playing an instrument live through effects, such as
  rakarrack, while recording); a casual recording doesn't need it. See
  "--realtime: lower monitoring latency" below.
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

**Resampling a source with a native rate well below the 48kHz bus rate**
(e.g. a synth running its own engine at 8kHz or 11kHz) used to need several
times as many output samples as input to upsample -- and the per-buffer
output size was originally sized for something closer to a flat "roughly
double" margin (fine for e.g. 44.1kHz -> 48kHz, not for a much lower native
rate). Undersizing that doesn't fail loudly: whatever doesn't fit stays
buffered inside the resampler's own internal state and only comes out on a
*later* call, which shows up first as growing startup latency (real audio
piling up before any of it reaches the mix) and then as chopping in that
one source specifically, once its own per-tap buffer runs dry between
those delayed, bursty catch-ups -- easy to mistake for an inherent "sample
mismatch" between sources, but actually just a fixed-size buffer that
wasn't sized for the actual rate ratio. The output buffer is now sized
from each source's real rate ratio instead of a flat guess, with a bounded
drain loop as a second line of defense against any backlog compounding
across calls.

**A source whose own producer delivers audio in bursts, not a steady
drip** -- a network radio stream doing its own internal buffering or
rebuffering being the clearest example -- can run into a *different*
problem than a rate mismatch: overflowing (dropping audio, heard as a pop
at the seam) or underrunning (zero-filled gaps, heard as a blip) a per-tap
ring buffer that's too small to absorb the burstiness, independent of
whether the sample rate itself needed converting. hrecord can't do
anything about jitter in how a source's own app delivers audio -- but it
can absorb more of it: each tapped source's ring buffer is now sized for 2
seconds of audio (up from half a second), trading a bit more live-
monitoring lag for a lot more headroom. Each source's ring also now
tracks how many bytes it's ever had to drop (overflow) or silence-fill
(underrun); if either is non-zero when a session ends, hrecord prints
which tapped app it happened to and how much, e.g.:

```
[i] Audio source "SomaFM Player": 4032 bytes dropped (arrived faster than
the mix could take them), 0 bytes silence-filled (arrived slower, or with
gaps, than the mix needed them) -- a likely cause of any popping or
dropouts heard for this source.
```

If that still shows up with real numbers after the larger buffer, it's
concrete confirmation of a burstiness mismatch for that specific source
(rather than something to keep guessing about from the recorded audio
alone) and the ring size is the next thing to tune upward.

**Encoding used to happen directly on the mixed-playback callback's own
thread**, which runs under a hard deadline set by the sound driver's own
hardware buffer depth -- e.g. Haiku's HDA driver defaults to buffers deep
enough for tens of milliseconds of slack, but a driver tuned for low
latency (a `play_buffer_frames`/`play_buffer_count` override in
`hda.settings`, the kind serious real-time audio work like rakarrack
already depends on) can bring that down to single-digit milliseconds.
Vorbis encoding is variable-latency work (FFmpeg resampling, FIFO
buffering, the actual codec call) with no business running under a
deadline that tight; missing it is audible as clicking, independent of
anything upstream, and was a real, confirmed contributor to `--allaudio`
popping under exactly that kind of driver tuning. Encoding now happens on
its own dedicated thread instead: the playback callback only mixes and
hands the result to a queue (same overflow/underrun-tracked ring buffer as
above), and a separate worker drains that queue and does the actual encode
work with no comparable timing pressure.

**The actual root cause of both the popping and the multi-second delay,
found from real timing/overflow diagnostics rather than more guessing:**
`AudioTapNode` was built untimed on purpose (`SetTimeSource(nullptr)`,
`B_RECORDING` run mode) -- it just hands off whatever the hijacked app
gives it, whenever, with no pacing of its own. That's fine in single-tap
mode: nothing downstream cares about wall-clock timing, the encoder just
processes whatever arrives. But `--allaudio` puts something real-time-paced
downstream of it for the first time -- `MixedPlaybackCallback`, tied to
the hardware's own clock -- and diagnostics confirmed some tapped apps
(rakarrack running with tight, low-latency buffers was the clearest case)
push audio to the Mixer *faster than real time*: one session showed
**~18 million bytes (≈47 seconds of audio) continuously dropped**, not a
one-time startup burst. With nothing pushing back on that, it just piled
up in the per-source ring buffer -- either overflowing continuously (heard
as popping) or, with a big enough ring to absorb it, settling into a
fixed, ever-present backlog instead (heard as a long delay before
anything's heard, staying exactly that far behind afterward). Widening the
ring earlier didn't fix either -- it just traded one symptom for the
other, which is why it measured *worse*, not better, in some tests.

The real fix is backpressure: each tap now paces its own consumption of
incoming buffers to match real time, snoozing briefly whenever it's
running ahead. That delays `Recycle()`-ing the buffer back, which is
exactly the signal Media Kit uses to let a producer know it can send more
-- so an over-fast producer now gets throttled the same way it naturally
would be if it were still connected straight to the Mixer, instead of
being allowed to run ahead unchecked. (This intentionally doesn't just
give the node a real time source instead, which is the more "normal" way
Media Kit nodes stay paced -- `SetTimeSource(nullptr)` was chosen earlier
in this project specifically to dodge a reproducible
`BTimeSource::RealTimeFor` crash in a different, abandoned approach, and
revisiting that wasn't worth the risk here.)

The timing/backlog diagnostics that found this are still logged on every
`--allaudio` run:

```
[i] BSoundPlayer::Start() returned 83ms after tap setup began.
[i] Mixed playback: first callback 88ms after tap setup began.
    source #1 backlog already queued: 16384 bytes (~0.0106667s)
```

Confirmed fixed in testing: with pacing in place, backlog and delay both
dropped to milliseconds (from as much as several seconds before), and
overflow drops went to zero -- rakarrack and a second source now record
together in near-real-time with no popping.

## `--realtime`: lower monitoring latency

Pacing removed the *runaway* backlog, but the hijack-and-relay path itself
(tap -> per-source ring -> mixed-playback ring -> Mixer) still has more
hops than a direct connection to the Mixer ever did, and the default
buffer sizes were chosen for safety margin against jitter, not for the
lowest latency possible. For a genuinely real-time use case -- monitoring
a live instrument through effects, e.g. rakarrack, while playing -- that
remaining margin is latency worth trimming; a casual recording doesn't
need to.

`--realtime` trims two things, in both single-tap and `--allaudio` mode:

- The audio-tap ring buffer(s): down from 2s to 0.1s in `--allaudio` mode
  (per source), and from 0.5s to 0.1s in single-tap mode. With pacing
  already preventing a growing backlog, these rings' steady-state fill
  tracks genuine jitter, not a queue that needs seconds of headroom --
  `--realtime` trades some of that jitter headroom for a lower latency
  ceiling.
- The buffer size requested from BSoundPlayer: down to ~1024 frames
  (deliberately the same scale as the `hda.settings` low-latency example
  above, ~5-20ms depending on rate) from a larger default. It's only a
  hint -- the Mixer can renegotiate it away -- but matching an
  already-tuned driver's own scale gives it the best chance of being
  honored.

`--realtime` only touches hrecord's own buffering. It doesn't change your
sound driver's own settings -- pair it with a driver already tuned for low
latency (the `hda.settings` section above) for it to actually matter; on
default driver settings there's a hardware buffer floor `--realtime` can't
get under.

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
