# Haiku Recorder - Screen, Desktop Audio & Real-Time Mixing

## Build

```
make release

```

## Usage

```
hrecord [start|stop] [--low|--medium|--high] [--audioonly] [--allaudio] [--realtime] [--experimental] [--experimental-screen-capture] [--list-audio-inputs]
```

- `hrecord` / `hrecord start` — records the screen (MJPEG in a `.mkv`
  container) to `/boot/home/hrecord_capture_YYYYMMDD_HHMMSS.mkv`, with a
  Vorbis desktop-audio track alongside it whenever the audio tap (below) can
  be set up.
- `hrecord start --audioonly` — records desktop audio only (no screen
  capture) to `/boot/home/hrecord_capture_YYYYMMDD_HHMMSS.ogg`, an
  Ogg/Vorbis file. Both Ogg and Vorbis are open, royalty-free formats, so
  this carries none of the licensing baggage a proprietary audio codec
  would. Taps one currently playing app (see "How desktop-audio capture
  works" below).

Every run's filename is stamped with its own start time (local time,
`YYYYMMDD_HHMMSS`), so starting a new recording never silently overwrites
whatever an earlier run left behind in `/boot/home`.
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
- `hrecord start --experimental` — caps how long a tap can hold a source's
  buffer back, relative to that source's own buffer size, instead of the
  flat 50ms `--realtime` otherwise uses for every source alike. Unconfirmed
  experiment aimed at very small hardware buffer settings. See
  "--experimental: capping buffer holds relative to the source's own
  buffer size" below.
- `hrecord start --experimental-screen-capture` — reuses a cached,
  already-converted background between frames instead of reconverting the
  whole screen every time, only freshly capturing the screen regions
  actually covered by a window (or the mouse cursor) each frame. No effect
  under `--audioonly` (there's no video to capture). Unconfirmed
  experiment; the default full-frame capture path is completely unaffected
  unless this flag is passed. See "--experimental-screen-capture:
  window-aware capture" below.
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

## `--experimental-screen-capture`: window-aware capture

The default path reads the whole screen and reruns `sws_scale`'s
colorspace conversion over every pixel, every single frame, regardless of
how much of the screen actually changed since the last one. That's simple
and always correct, but wasteful on a desktop where windows only cover
part of the screen -- the bare wallpaper/Deskbar area gets reconverted for
no reason, every frame.

**First version only fixed half the problem.** It skipped the `sws_scale`
conversion cost for the static background (a cached, already-converted
background reused every frame, only the screen regions actually covered
by a window -- plus a small region around the mouse cursor, see below --
freshly converted each time), but it still called `BScreen::ReadBitmap()`
for the *whole screen* every single frame regardless, on the assumption
that `BScreen` had no partial-capture primitive. Real-world testing
showed that assumption was the actual problem: `htop` showed `app_server`
itself pinned near 100% of one core, identically with or without this
mode, and a brief improvement in mouse responsiveness faded after about a
second back to the same sluggishness the default path has. The `sws_scale`
cost this mode targeted was real, but small next to the cost of the
*read* itself -- reading is where nearly all the CPU time actually goes.

**BScreen does support a genuine partial capture, via `ReadBitmap()`'s own
`bounds` parameter** -- confirmed by looking at how
[RemoteControl](https://github.com/HaikuArchives/RemoteControl)'s
`RCServer` (a free Haiku screen-sharing tool one real user found stayed
low-CPU while still delivering a live, responsive capture) achieves that:
it never reads the whole screen either, calling `BScreen::GetBitmap()`
with a small bounds rectangle for one tile at a time, cycling through the
whole screen tile by tile. This mode now does the same thing per tracked
window (and the cursor) instead of per uniform tile: each region gets its
own small, exactly-sized `BScreen::GetBitmap()` call, and the *whole
screen* is only ever read when the window layout actually changes (the
same "background rebuild" trigger as before). That's the fix that
actually reaches the real bottleneck the `sws_scale`-only version never
could.

**Two things this deliberately does *not* do, because doing them would be
wrong, not just less efficient:**

1. It never skips re-capturing a window just because its *frame*
   (position/size) hasn't moved. A window's content changes for reasons
   that have nothing to do with its frame -- a blinking cursor, scrolling
   text, a VU meter, a video playing -- so every tracked window's own
   region is freshly captured and converted every single frame,
   unconditionally, via its own small `BScreen::GetBitmap()` call. Only
   the true background (the area no window covers) is cached, and only
   until the window layout itself changes (a window opens, closes,
   minimizes, restores, moves, resizes, or changes z-order).
2. It never ignores the mouse cursor just because it isn't a window.
   `BScreen` bakes the cursor into whatever it captures, but nothing in
   the window list reports the cursor's own position, so a generous
   fixed-size box around wherever it currently is gets its own fresh
   capture every frame too, exactly like a window's own region --
   otherwise the cursor would visibly freeze in place except when some
   window's frame happened to change.

Window tracking uses the same private Haiku Window Kit API
[hDesktop](https://github.com/ablyssx74/hDesktop) uses for its own
minimize/maximize/open/close detection: `BPrivate::get_window_order()`
enumerates window tokens for the active workspace, and `get_window_info()`
resolves each token to its frame and state (minimized, which workspaces,
window feel). Minimized windows and non-normal windows (menus, tooltips,
the Deskbar itself) are excluded, matching what a viewer would actually
expect "the apps" to mean.

**Window-border artifacts, also found in real-world testing, and a first
mitigation.** Scaling a small region in isolation, with no visibility into
the real pixels just outside it, can produce a visibly different result
right at its own edge than the same algorithm would produce as part of
one continuous full-frame scale -- a soft seam/halo right around window
borders. Each window/cursor region's own scale now deliberately uses
`SWS_FAST_BILINEAR` regardless of the recording profile's own algorithm
(`SWS_BILINEAR`/`SWS_BICUBIC` under `--medium`/`--high`): a cheaper,
sharper filter doesn't blend across that boundary the same way a wider
sampling kernel does, trading a slightly less smooth look on downscaled
window content specifically (not the recording as a whole) for less of
this specific artifact. Unconfirmed whether this fully resolves it pending
re-testing; if visible artifacts remain, the next thing to try is padding
each region's *capture* rectangle by a few extra pixels (giving the
scaler real neighboring context) while still only pasting the original,
unpadded rectangle into the output frame.

**A trade-off worth knowing:** while a window is actively being dragged or
resized, its frame changes on every single frame by definition, so the
background gets rebuilt every frame during that -- no visual artifacts,
just no speedup for that specific moment. That's the correct choice (never
risk a stale background sliver under a moving window), not a bug.

**Unconfirmed pending real-world testing**, including assumptions worth
flagging if something looks visibly wrong:

- Overlapping windows are composited in the order `get_window_order()`
  returns them, assumed front-to-back (topmost first) and reversed before
  painting. If two overlapping windows composite with the wrong one on
  top, this assumption is inverted -- the fix is a one-line change to stop
  reversing that order.
- `client_window_info`'s frame fields are read as `window_left`/
  `window_top`/`window_right`/`window_bottom`. If this doesn't match the
  actual struct on a given Haiku build, it'll fail to compile rather than
  silently misbehave -- the fix is matching whatever field names that
  build's `<WindowInfo.h>` actually declares.
- Each region is captured into a bitmap sized to exactly match it (via
  `GetBitmap()`'s own auto-sizing), rather than read into a reused, larger
  canvas at some computed offset -- deliberately, to sidestep an
  unconfirmed question about whether `BScreen` positions a partial capture
  at the destination bitmap's own origin or at the requested rectangle's
  absolute screen position. With an exact-sized destination those two
  possibilities are identical, so it doesn't matter which one is actually
  true -- but it does mean a small `BBitmap` allocation happens per
  region, per frame, rather than reusing one long-lived buffer the way the
  default path's own screen-sized bitmap does. RemoteControl's own design
  does the same thing, continuously, for tiles as small as 100x100px, so
  this is expected to be cheap -- small allocations were never the
  bottleneck the very first fix in this project (removing a *screen-sized*
  per-frame allocation) addressed; only a large one was.

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

**Fixed: hijacked apps staying listed in Media preferences' Audio mixer
after being fully closed**, even on a clean shutdown -- confirmed not to
happen on a Haiku session that's never run hrecord, so this was hrecord's
own bug, not the pre-existing Haiku quirk below. Every Media Kit roster
call that hands back a `media_node` (`GetNodeFor()`, `GetAudioMixer()`)
hands out a reference the caller owns and must explicitly release with
`ReleaseNode()` -- hrecord was acquiring one for each hijacked app
(`GetNodeFor()`, to find and control it) and for the Mixer itself
(`GetAudioMixer()`, to query its connected/free inputs) but never
releasing either, on every single hijack. `media_server` kept the
underlying node considered "in use" by hrecord's own already-exited
process indefinitely, which is consistent with what stayed visible even
after the app itself was closed. Every acquisition now has a matching
release, including on every internal failure path (not just the
success/teardown path).

**Follow-up from real-world testing:** confirmed fixed for the reported
case (stop hrecord, then close the apps -- no zombie entries). But an app
closed *before* stopping hrecord (i.e. disappearing mid-session, while
still hijacked) can still leave a zombie entry, alongside Haiku's own
Media Kit printing a cascade of `Bad port ID`/`GetNodeFor failed`
diagnostics when hrecord's restore code tries to reconnect an app that's
no longer there. hrecord itself doesn't crash in this case (it finishes
and writes its output normally) and this appears tied to a broader,
still-open reliability question -- see "Known open issues" below.

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

`--realtime` trims two things, in both single-tap and `--allaudio` mode.
With pacing already preventing a growing backlog, the ring buffers'
steady-state fill tracks genuine jitter, not a queue that needs seconds of
headroom -- so trading away some of that headroom for a lower latency
ceiling is a reasonable bet for a real-time use case:

| | Default | `--realtime` |
|---|---|---|
| Audio-tap ring buffer (`--allaudio`, per source) | 2s | 0.07s |
| Audio-tap ring buffer (single-tap) | 0.5s | 0.07s |
| BSoundPlayer buffer size requested | (default) | ~128 frames |

The BSoundPlayer buffer size is only a hint -- the Mixer can renegotiate it
away -- but matching an already-tuned driver's own scale gives it the best
chance of being honored. `--realtime` only touches hrecord's own
buffering, not your sound driver's own settings -- pair it with a driver
already tuned for low latency (the `hda.settings` section above) for it to
actually matter; on default driver settings there's a hardware buffer
floor `--realtime` can't get under.

These numbers were reached in two steps, worth knowing about since the
first one went badly enough to explain here rather than just quietly
picking better numbers. They started out behind a separate
`--experimental` flag (256 frames / 0.03s rings) alongside a *tighter*
"catch-up snooze cap" inside the pacing logic -- reasoning that a smaller
cap meant a large corrective sleep could never itself become a latency
spike. In testing it did the opposite: overflow on three simultaneously
tapped sources went from at-or-near zero (clean) to hundreds of thousands,
even tens of millions, of bytes dropped -- heard as constant crackling on
everything, not just a little extra lag. The mechanism: that cap doesn't
just bound a single sleep, it bounds *how much drift pacing can correct
per buffer*. A source running even slightly ahead of real time needs to
fully correct before the next buffer arrives to stay caught up; a tighter
cap makes that take more calls, and the also-shrunk ring left far less
room to absorb the gap while it did. The two changes compounded instead of
adding. Fixed by decoupling them: the snooze cap is a single, generous
50ms in every mode now (its actual job -- stop one anomalous burst from
blocking this thread too long -- never needed to scale with a latency
target), leaving ring buffer size as the real, and only, latency dial.
Retested at a gentler 0.07s (up from 0.03s) with three simultaneous
sources and confirmed clean, so that's `--realtime`'s own default now.

**Buffer size retuned again, 256 frames -> 128.** Same real user,
retuning their own driver further still (`play_buffer_frames` 128,
Cortex reporting ~7ms of latency) -- confirmed lower latency, with only
occasional clicks/pops rather than a clean zero. `--realtime`'s own
BSoundPlayer buffer-size hint was updated to match (128 frames, up from
256) for the same reason as the original 256: matching the scale of an
already-tuned driver gives the Mixer the best chance of actually
honoring the hint. `--experimental` was revived (previously a no-op
after its old tuning got folded into `--realtime`'s own defaults) to
try to close the remaining occasional clicks/pops at this tighter
setting -- see its own section below.

### `--experimental`: capping buffer holds relative to the source's own buffer size

`PaceToRealTime`'s backpressure (see above) works by deliberately
delaying `buffer->Recycle()`, holding a buffer back for up to 50ms as
the signal that tells a fast producer to slow down. That 50ms cap was
picked as a generous, mode-independent safety valve, not tuned against
any particular buffer size -- fine at 256 frames (~5.3ms @ 48kHz,
roughly a 9x hold-to-period ratio), but at 128 frames (~2.7ms) that
ratio nearly doubles to ~18x. A hold that much longer than a source's
own natural buffer period is plausibly enough to strain a small
hardware buffer's own headroom, heard as occasional clicks/pops even
though nothing is being dropped or logged as an error.

`--experimental` caps the hold at roughly 2x *that source's own* buffer
duration instead of the flat 50ms, whenever that's tighter -- a 128-frame
buffer gets roughly a 5.3ms cap. This is deliberately not the same
mistake as the v1.8.1 regression documented above: a flat, *smaller*
cap applied to every source alike is what broke correction speed there.
This cap only tightens for sources whose own buffers are small and
frequent to begin with, and for exactly those sources, correction
*opportunity* scales right along with the tighter cap, since
`BufferReceived` fires again just as often. A source with large,
infrequent buffers keeps the full 50ms, identical to every other mode.

This exact idea was tried once before, gated behind `--experimental`,
for a different symptom entirely: a Rakarrack-side
`SoundPlayNode::FillNextBuffer: RequestBuffer failed` flood. That one
turned out to be caused by stale `media_server` state left over from
earlier testing, not by pacing timing, so the change was reverted as
unnecessary for that bug (`--experimental` went back to being a no-op).
That finding doesn't rule this mechanism out for a *different* symptom
-- audible clicking during monitoring, not a Media Kit error message --
so it's being tried again on its own merits rather than treated as
already disproven. Unconfirmed pending real-world testing.

**The periodic (`t+Ns: source #N backlog: ...`) logging that used to print
every ~2 seconds has been removed.** It was added specifically to catch a
source slowly drifting ahead of real time over the course of a session --
useful diagnostic noise while chasing the "known open issues" below, but it
did its job: real-world testing with it in place showed hrecord's own
ring backlog staying flat and nearly identical across both laggy and
lag-free runs, which is what pointed the case-1 root cause at the tapped
app itself (see below) rather than anything in hrecord's pipeline. With
that confirmed, the extra per-callback logging overhead isn't earning its
keep anymore. The one-time startup snapshot (`[i] Mixed playback: first
callback ...`) stays -- it's cheap (runs once) and still useful for
diagnosing startup delay.

**Known open issues, likely related:** two things reported from testing
against still-open apps across repeated runs, which may share one root
cause rather than being two separate bugs:

1. Repeatedly hijacking the same app in quick succession has,
   intermittently, produced Haiku's own Media Kit printing
   `Bad port ID`/`GetNodeFor failed` diagnostics during teardown,
   alongside a large stretch of silence-filled audio mid-session --
   consistent with the hijacked app's own connection having gone away
   before hrecord tried to restore it. hrecord itself doesn't crash when
   this happens (it finishes and writes output normally), and it doesn't
   reproduce every time -- sporadic across otherwise-identical repeated
   runs of the same single tapped app.
2. When an app disappears mid-session like that, the zombie
   Media-preferences-entry problem above can still occur, since there's
   nothing left to cleanly restore by the time hrecord's teardown runs.

**Not an hrecord issue, resolved: a "2nd-instance lag" finding
previously listed here.** Re-running hrecord against apps still connected
from a previous run (stopped hrecord, apps left open, started hrecord
again without restarting them) showed noticeably more lag on the second
run than the first. Comparing the periodic backlog log between a run with
barely any noticeable lag and a very similar run where lag was clearly
audible: the numbers were essentially identical -- the same source sat at
its ring's full capacity (a fixed ~0.07s) for the entire session in
*both* runs, and the overflow/underrun counts at teardown were nearly the
same either way. With nothing distinguishing the two runs anywhere in
hrecord's tap -> ring -> mix -> BSoundPlayer pipeline, the isolating test
was to restart only the tapped app (Rakarrack) between runs, leaving
hrecord itself untouched -- and that alone made the lag go away. So this
lived entirely inside the tapped app's own internal state, not hrecord's
(hrecord only ever sees whatever audio data an app hands it, never that
app's own input-to-output round trip, and apparently that round trip
degraded across repeated hijack/restore cycles for at least this
particular app). No longer listed as an hrecord issue above; if it ever
shows up again with some other tapped app, restarting that app between
runs is the workaround.

**Cases 1 and 2 above, and the resolved 2nd-instance-lag finding, got a
shared data point: the `SoundPlayNode` flood was leftover zombie Media
Server state, not a persistent hrecord-triggered bug.** After an
`hrecord --allaudio --realtime --audioonly` session hijacked and restored
Rakarrack, a later launch of Rakarrack started printing Haiku's own
`SoundPlayNode::FillNextBuffer: RequestBuffer failed` -- the Media Kit's
internal `BSoundPlayer` implementation failing to push a buffer through a
connection the Mixer still considered live. A controlled test (Rakarrack
running standalone and quiet, *then* starting hrecord) initially found
this flooding continuously for as long as hrecord's tap stayed connected,
stopping the moment hrecord stopped. The actual explanation turned out to
be simpler: that test's Media Server had already accumulated zombie node
state from earlier testing sessions (see the reference-leak bug above and
the "stale" Mixer connection issue below) -- a node roster already in a
bad state doesn't settle a new connection cleanly, however long you wait.
With Media Server freshly restarted (no zombie entries left in Media
preferences' Audio mixer beforehand), the same test instead shows
`RequestBuffer failed` for a few seconds right after hrecord's tap
connects, then settles into normal operation for the rest of the
session -- ordinary Media Kit connection warm-up, not a sustained
failure. Considered resolved: keep Media Server clean (restart it,
per the known issue below, if zombie entries ever show up) and this
doesn't reproduce as a persistent problem.

A settle-delay widening (`Disconnect`/`Connect` in
`HijackAppIntoTap`/`UndoHijack`/`RestoreHijackedApp`, 20ms -> 100ms) and a
`--experimental` flag capping how long a tap could hold a buffer back
were both tried as fixes before this was understood -- the settle-delay
widening is harmless to keep (closes a real asymmetry regardless) and
stays; the `--experimental` buffer-hold cap turned out unnecessary once
the real cause was identified, so it's been reverted and `--experimental`
is back to being a no-op, same as before this investigation started.
Cases 1 and 2 above were reported against test sessions carrying the same
kind of accumulated zombie state -- plausibly the same explanation
applies to both, though that's not separately re-confirmed against a
freshly restarted Media Server the way the `SoundPlayNode` flood was.

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

## Credits

hrecord's use of Haiku's Media Kit (`BMediaRoster`, node hijacking, buffer
tapping) built on groundwork the author laid earlier while porting
[JAMin](https://jamin.sourceforge.net/) (the JACK Audio Mastering
interface) to Haiku, a project developed with help from Google AI. That
prior hands-on experience with `BMediaRoster`/`BBufferConsumer`/node
lifecycle behavior informed how hrecord's own audio tapping was designed.
