# Haiku Terminal Screen Recorder App.

## Build

```
make release

```

## Usage

```
hrecord [start|stop] [--low|--medium|--high] [--audioonly] [--allaudio] [--realtime] [--experimental] [--list-audio-inputs]
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
  experiment aimed at small-buffer producers whose own BSoundPlayer buffer
  pool can start failing to reclaim buffers when held back that long. See
  "--experimental: capping buffer holds relative to the source's own
  buffer size" below.
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
| BSoundPlayer buffer size requested | (default) | ~256 frames |

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
`--experimental` was a no-op for a while after that (its old tuning had
been folded into `--realtime`'s defaults, so passing it did nothing) --
it's since been repurposed for a new, unrelated experiment; see below.

### `--experimental`: capping buffer holds relative to the source's own buffer size

A completely different symptom, found later: with `--realtime` engaged,
Haiku's own `SoundPlayNode::FillNextBuffer: RequestBuffer failed` started
flooding the terminal of a tapped app (rakarrack) for as long as its
connection to hrecord's tap was held -- starting the instant hrecord
started, stopping the instant hrecord stopped, confirmed reproducible
across repeated on/off cycles. That's Haiku's own Media Kit code (inside
`libmedia.so`'s `BSoundPlayer` implementation), not rakarrack's and not
hrecord's -- it means a producer's request for a free buffer from its own
`BBufferGroup` failed.

The likely mechanism: pacing's backpressure (see above) works by
deliberately delaying `buffer->Recycle()` by up to the 50ms cap, as the
signal that tells a fast producer to slow down. That's fine for a
producer with a normal-size buffer pool. rakarrack's own output runs on
very small buffers (~256 frames, ~5.3ms @ 48kHz), and Haiku's
`BBufferGroup`/`SoundPlayNode` pools are typically only 2-3 buffers deep --
holding even one buffer back for anywhere close to 50ms while the
producer tries to reclaim a fresh one roughly every 5ms can starve a pool
that small, and every subsequent request fails until the held buffer
finally comes back. Continuous, for as long as pacing keeps holding
buffers that long -- matching what was observed exactly.

`--experimental` caps the hold at roughly 2x *that source's own* buffer
duration instead of the flat 50ms, whenever that's tighter. This is
deliberately not the same mistake as the regression above: a flat,
*smaller* cap applied to every source alike is what broke correction
speed there. This cap only tightens for sources whose own buffers are
small and frequent to begin with (rakarrack's ~5.3ms buffers get roughly
a 10.6ms cap) -- and for exactly those sources, correction *opportunity*
scales right along with the tighter cap, since `BufferReceived` fires
again just as often. A source with large, infrequent buffers keeps the
full 50ms, identical to every other mode, so this shouldn't touch
behavior for anything but small-buffer producers like rakarrack.

Not folded into `--realtime`'s own defaults (unlike the ring/buffer-size
tuning above) -- kept as its own opt-in flag since it's unconfirmed
pending real-world testing.

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

**Known open issues, likely related:** three things reported from testing
against still-open apps across repeated runs, which may share one root
cause rather than being three separate bugs:

1. Re-running hrecord against apps still connected from a previous run
   (stopped hrecord, apps left open, started hrecord again without
   restarting them) shows noticeably more lag on the second run than the
   first, even though the startup backlog/overflow numbers logged look
   similar between the two.
2. The same repeated-hijack scenario has, intermittently, produced Haiku's
   own Media Kit printing `Bad port ID`/`GetNodeFor failed` diagnostics
   during teardown, alongside a large stretch of silence-filled audio
   mid-session -- consistent with the hijacked app's own connection having
   gone away before hrecord tried to restore it. hrecord itself doesn't
   crash when this happens (it finishes and writes output normally), and
   it doesn't reproduce every time -- sporadic across otherwise-identical
   repeated runs of the same single tapped app.
3. When an app disappears mid-session like that (case 2), the zombie
   Media-preferences-entry problem above can still occur, since there's
   nothing left to cleanly restore by the time hrecord's teardown runs.

The common thread across all three: repeatedly hijacking the same app in
quick succession, rather than anything specific to `--allaudio` or the
number of tapped sources (case 1 reproduces with one source as readily as
three). A settle-delay fix (matching `HijackAppIntoTap`'s own pause after
Disconnect, previously missing from the mirror-image restore path) didn't
resolve case 1.

**Case 1 is confirmed: it isn't hrecord.** Comparing the periodic backlog
log between a run with barely any noticeable lag and a very similar run
where lag was clearly audible: the numbers were essentially identical --
the same source sat at its ring's full capacity (a fixed ~0.07s) for the
entire session in *both* runs, and the overflow/underrun counts at
teardown were nearly the same either way. With nothing distinguishing the
two runs anywhere in hrecord's tap -> ring -> mix -> BSoundPlayer
pipeline, the isolating test was to restart only the tapped app (Rakarrack)
between runs, leaving hrecord itself untouched -- and that alone made the
lag go away. So case 1 lives entirely inside the tapped app's own internal
state (hrecord only ever sees whatever audio data an app hands it, never
that app's own input-to-output round trip, and apparently that round trip
itself degrades across repeated hijack/restore cycles for at least this
app). There's nothing in hrecord's own code to fix here; restarting the
tapped app between runs is the workaround. It's plausible the same
app-side degradation also explains cases 2 and 3 below, though that's not
separately confirmed.

**Cases 2 and 3 got a second data point from a different app's side --
which turned out to point somewhere more specific than first thought.**
After an `hrecord --allaudio --realtime --audioonly` session hijacked and
restored Rakarrack, a later launch of Rakarrack started printing Haiku's
own `SoundPlayNode::FillNextBuffer: RequestBuffer failed` -- the Media
Kit's internal `BSoundPlayer` implementation failing to push a buffer
through a connection the Mixer still considered live. A controlled
follow-up test (Rakarrack already running standalone and quiet, *then*
starting hrecord) pinned down the timing precisely: the flood starts the
instant hrecord starts, stops the instant hrecord stops, and resumes the
instant hrecord restarts -- tracking hrecord's live connection window
exactly, not persisting once hrecord actually exits. That rules out
stale post-disconnect `media_server` state (what the 100ms settle-delay
change above was aimed at) as the cause of *this* symptom specifically --
it's continuous for as long as the tap is connected, not a one-time
reconnect-moment glitch.

The likely mechanism instead: pacing's backpressure (see above) works by
deliberately delaying `buffer->Recycle()`, holding a buffer back for up
to 50ms as the signal that tells a fast producer to slow down. Rakarrack's
own output runs on very small buffers (~256 frames, ~5.3ms @ 48kHz), and
Haiku's `BBufferGroup`/`SoundPlayNode` pools are typically only 2-3
buffers deep -- holding even one buffer back for anywhere close to 50ms
while the producer tries to reclaim a fresh one roughly every 5ms can
starve a pool that small, continuously, for as long as pacing keeps doing
it. Testing also found no audio at all for a while immediately after
stopping hrecord (the Mixer reconnect completing, but Rakarrack staying
silent afterward) -- consistent with `SoundPlayNode`'s own buffer-pool
bookkeeping not recovering cleanly from a long run of failed requests,
and quite possibly the same underlying mechanism behind the original
"restarting Rakarrack fixes the 2nd-instance lag" finding in case 1
above, rather than a fully separate issue.

See `--experimental` below for the fix being tried for this specific
symptom.

Following this, the settle delay between `Disconnect` and `Connect` in
`HijackAppIntoTap`/`UndoHijack`/`RestoreHijackedApp` (previously a flat
20ms) was widened to 100ms, on the theory that the Mixer's own internal
connection state may not have been fully settling in the shorter window
before hrecord reused it. Unconfirmed pending real-world retesting --
recorded here as the current experiment in progress, not a verified fix.

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
