# hrecord: research and design history

This is the full investigation log behind hrecord's screen-capture and
audio-capture design — every mode tried, what real-world testing found,
what got fixed, and what's still open. `readme.md` covers day-to-day usage;
this file is for understanding *why* it works the way it does, or for
picking up an open question below.

See also `research/README.md` and `research/directwindow_probe.cpp` for
the standalone (not part of hrecord itself) probe that first established
whether `BDirectWindow` was usable at all on real hardware.

## Screen capture: the modes, in the order they were tried

The default path today is tiled reads through a directly-mapped
framebuffer pointer when that's safe (see `readme.md`). Getting there took
several rounds of testing, summarized here roughly in chronological order.

### The original default: one plain full-screen read (`--raw-capture`)

The simplest possible code path: one `BScreen::ReadBitmap()` call covering
the whole screen, every frame, no tiling, no window tracking, no caching.
This was the *only* capture mode for most of this project's early life,
and every other mode here was originally compared against it.

**The first real fix, before any of the modes below existed:** capturing
and MJPEG-encoding the full screen every frame at 30fps, uncapped, was
consistently pegging a full CPU core — enough that the mouse itself would
visibly lag, since Haiku's own input/compositing work was fighting hrecord
for that core. The actual dominant cost turned out not to be the encoder
at all: every captured frame was pulled via `BScreen::GetBitmap()`, which
allocates a brand-new `BBitmap` — and the shared-memory area `app_server`
backs it with — from scratch on every single call. That per-frame
allocate/IPC/free cycle ran the same way regardless of quality profile,
which is why lowering fps/resolution/quality alone never fixed the lag:
the capture side was never touched by any of that. hrecord now allocates
one `BBitmap` up front and refills it in place every frame via
`BScreen::ReadBitmap()` instead, removing that allocation entirely. The
quality profiles (see `readme.md`) still matter for the encode side and
output size, but this capture-side fix is what actually addressed the
sluggish, laggy-mouse symptom.

**Investigated and (at the time) ruled out: `BDirectWindow`.** Real-hardware
testing showed `SupportsWindowMode()` returning `false` — windowed direct
connection wasn't available on that driver at all, and the only
alternative, full-screen exclusive mode, would take over the whole
display, incompatible with a background recorder. This is what `research/
README.md`'s own standalone probe was built to answer definitively. It
turned out not to be the final word — see "`--direct-tiled-capture`" and
"`--direct-raw-capture`" far below, much later in this project's life.

### `--experimental-screen-capture`: window-aware capture

A plain whole-screen read reruns `sws_scale`'s colorspace conversion over
every pixel, every frame, regardless of how much actually changed. That's
simple and always correct, but wasteful on a desktop where windows only
cover part of the screen.

**First version only fixed half the problem.** It skipped the `sws_scale`
conversion cost for the static background (cached, reused every frame,
only window regions plus a small cursor region freshly converted), but
still called `BScreen::ReadBitmap()` for the *whole screen* every frame
regardless, on the assumption `BScreen` had no partial-capture primitive.
Real-world testing showed that assumption was the actual problem: `htop`
showed `app_server` pinned near 100% of one core, identically with or
without this mode. The `sws_scale` cost this targeted was real, but small
next to the cost of the *read* itself.

**`BScreen` does support a genuine partial capture**, via
`ReadBitmap()`/`GetBitmap()`'s own `bounds` parameter — confirmed by
looking at how [RemoteControl](https://github.com/HaikuArchives/RemoteControl)'s
`RCServer` achieves a responsive live capture at low CPU cost: it calls
`BScreen::GetBitmap()` with a small bounds rectangle for one tile at a
time. This mode does the same per tracked window (and the cursor)
instead of per uniform tile: each region gets its own small, exactly-sized
`GetBitmap()` call, and the whole screen is only read when the window
layout actually changes.

**Two things this deliberately does *not* do:**

1. It never skips re-capturing a window just because its frame hasn't
   moved — a window's content changes for reasons that have nothing to do
   with its frame (blinking cursor, scrolling text, a VU meter, video).
   Every tracked window's own region is freshly captured every single
   frame, unconditionally. Only the true background is cached, until the
   window layout itself changes.
2. It never ignores the mouse cursor just because it isn't a window — a
   generous fixed-size box around wherever it currently is gets its own
   fresh capture every frame too.

Window tracking uses the same private Haiku Window Kit API
[hDesktop](https://github.com/ablyssx74/hDesktop) uses for its own
minimize/maximize/open/close detection: `BPrivate::get_window_order()`
enumerates window tokens for the active workspace, `get_window_info()`
resolves each to its frame/state. Minimized and non-normal windows (menus,
tooltips, the Deskbar) are excluded.

**Real-world confirmation: the bounded-read fix is a genuine win.** One
real user's testing described mouse responsiveness as roughly 50% better
than a plain full-screen read, "feels like what using [RemoteControl's]
RClient would behave as" — small, frequent capture requests interleave
with `app_server`'s other work far better than one big request per frame.
`htop` still showed `app_server` pinned near 95% of one core either way —
this mode was never expected to lower that number, only to change how the
cost is *felt* while recording.

**Window-border artifacts, found and fixed at the actual cause.** Scaling
a small region in isolation, with no visibility into pixels just outside
it, produces a visibly different result at its own edge than the same
algorithm would produce as part of one continuous full-frame scale — a
soft seam/halo around window borders. Switching each region's own
`sws_scale` call to `SWS_FAST_BILINEAR` didn't fix it (confirmed by
real-world retest). The actual fix: capture still happens per region, but
each region's raw pixels are pasted into the persistent capture buffer
with a plain `memcpy` — no color-space conversion — and `sws_scale` runs
exactly once per frame, over the whole composited buffer. No seam because
there's no longer more than one scale operation per frame.

**A trade-off, not a bug:** while a window is being dragged or resized,
its frame changes every frame by definition, so the background rebuilds
every frame during that — no artifacts, just no speedup for that moment.

**Mouse-trail ghosting, confirmed and fixed.** Real-world testing (inside
QEMU) found a visible cursor trail in this mode, and in `--hybrid-capture`
and `--screen-capture-rcserver-method` too — every mode with dedicated
cursor handling. `--tiled-capture` never showed it, which pointed at the
cause: those three modes all refreshed a small box around the cursor's
*current* position only. When the cursor moved far enough between frames
that old and new boxes stopped overlapping, the previous frame's rendered
cursor pixels — baked into the shared buffer by `BScreen` itself — were
never revisited (the background there is cached), so they persisted as a
visible ghost. Fixed by also refreshing wherever the cursor *was* last
frame, as its own separate small read (not merged into one larger
rectangle, which could balloon if the cursor jumped a long distance in
one frame, e.g. a multi-monitor warp). All affected modes share one
`RefreshCursorRegion()` helper now.

**Unconfirmed assumptions worth knowing about:**

- Overlapping windows are composited in the order `get_window_order()`
  returns them, assumed front-to-back (topmost first) and reversed before
  painting. If two overlapping windows composite with the wrong one on
  top, this assumption is inverted — the fix is a one-line change to stop
  reversing that order.
- `client_window_info`'s frame fields are read as `window_left`/
  `window_top`/`window_right`/`window_bottom`. A mismatch against a given
  Haiku build's actual struct would fail to compile, not silently
  misbehave.
- Each region is captured into an exactly-sized bitmap rather than a
  reused buffer at a computed offset, sidestepping an unconfirmed question
  about whether `BScreen` positions a partial capture at the destination's
  own origin or the requested rectangle's absolute screen position — with
  an exact-sized destination, both possibilities are identical.

### `--screen-capture-rcserver-method`: blind uniform-tile capture

Built to answer a direct question: does `--experimental-screen-capture`'s
speedup depend on knowing which regions are windows, or would any scheme
reading the screen in small bounded pieces get a similar win? This mode
throws away window-kit knowledge entirely and uses the technique
RCServer is actually built on (read from its source,
`sources/RCServer/ScreenShot.cpp`/`ScreenServer.cpp`): divide the screen
into a fixed 100x100px grid (matching RCServer's own default
`mSquareSize`), and round-robin through it continuously.

**How this differs from `--experimental-screen-capture`:**

- No window tracking, no private Window Kit API, no concept of "window"
  at all — just a uniform grid. Doesn't depend on `client_window_info`'s
  field layout matching between Haiku builds.
- Change detection is byte-level: each freshly read tile is `memcmp()`'d
  against what's cached there (RCServer's own `CompareBitmaps` technique),
  and only pasted into the buffer if it actually differs. This only ever
  skips the *paste*, never the *read* — `GetBitmap()` still asks
  `app_server` for the tile's current pixels first.
- The trade-off, confirmed in real-world testing: tiles cycle round-robin
  rather than all being current every frame, so any tile can be up to a
  full cycle old. A static desktop looks perfect, but dragging a window
  visibly broke it into fragments — both where the window used to be and
  where it currently is sat half-stale until their tiles came back around.

The cursor gets an unconditional refresh every frame on top of the tile
rotation (same shared `RefreshCursorRegion()` helper), for the same
reason `--experimental-screen-capture` needs it.

Can't be combined with `--experimental-screen-capture` — `--experimental-
screen-capture` wins if both are passed, since it had real-world
confirmation first.

**Real-world confirmation.** Mouse responsiveness described as "almost, if
not identical to native mouse motion" — by far the best of the capture
modes on that front at the time — and `app_server` CPU cost stayed
remarkably low. A static desktop recorded "fairly decent." The one real
problem: dragging a window visibly fragmented it, exactly the round-robin
staleness trade-off predicts.

**Chasing motion: a mitigation for fragmentation, confirmed helping.**

1. *Spatial contagion.* Whenever a tile's `memcmp` finds it changed, its
   four immediate neighbors get refreshed right away too, rather than
   waiting for their own rotation turn — a changed tile is likely at the
   leading/trailing edge of whatever just moved.
2. *Adaptive burst.* An internal multiplier scales the per-frame tile
   budget up while a meaningful fraction of sampled tiles are actually
   changing (real motion happening now), decaying back down once things
   go quiet.

Real-world re-test: fragmentation noticeably less, slow dragging showed
none at all, mouse responsiveness stayed "same, good and fluid"
throughout. Neither addition makes a freshness *guarantee* the way the
window-aware mode's per-frame refresh does, though — fast dragging can
likely still show some residual fragmenting.

**High-churn fallback.** Testing against a full-screen animated visualizer
(projectM) surfaced near-continuous, screen-wide change, where almost no
tile is skippable — the recording effectively updated only 2-3 times per
second even at target fps, because the round-robin was spending hundreds
of small `GetBitmap()` calls (each with fixed per-call overhead) to cover
ground one plain full-screen read would cover in one call. The fix: when
a scan pass finds more than 60% of sampled tiles changed, the next ~1
second of frames falls back to a plain full-screen `ReadBitmap()`
(identical cost to `--raw-capture`), then resumes tiling and re-triggers
if the screen's still that busy. Real-world re-test against projectM: "a
little better," not a full fix — this mode's real strength is confirmed
to be static desktop/window content, not full-screen continuous
animation, which is a fair characterization of what blind uniform-tile
capture is suited for.

**Unconfirmed, still open:**

- Whether the round-robin cadence (whole grid once per second, before any
  burst) is a good baseline, too slow, or needlessly fast — chosen as a
  starting point, not measured.
- Whether `memcmp`'s own cost across every tile ends up cheaper or more
  expensive than `--experimental-screen-capture`'s unconditional-paste
  approach during active motion.
- Whether fast (not slow) window dragging still shows residual
  fragmentation — only slow dragging has been confirmed fragment-free.

### `--hybrid-capture`: window-aware capture, rcserver-sized reads

A direct test of a hypothesis raised after comparing the two modes above:
why did `--screen-capture-rcserver-method`'s mouse responsiveness beat
`--experimental-screen-capture`'s so clearly, when both do bounded reads?
Likely answer: `--experimental-screen-capture`'s reads aren't actually
bounded in *size* — a tracked window's entire rectangle is one
`GetBitmap()` call, so a large/maximized window means one large read every
frame, exactly the `app_server` contention the design was meant to avoid.
`--screen-capture-rcserver-method` never issues a call bigger than a fixed
100x100 tile.

**The design: identical to `--experimental-screen-capture`, one change.**
Same window tracking, same per-frame full-window refresh, same shared
background, same single full-frame `sws_scale`. The only change: each
tracked window's rectangle is read through a grid of small, fixed-size
tiles instead of one call sized to the whole window.

**Real-world tested.** This was actually the *first* "hybrid" design
tried, before `--tiled-capture` existed. The verdict: felt like a mix of
`--experimental-screen-capture` and `--screen-capture-rcserver-method` —
accurately, since that's what it is — which is what led to trying a hybrid
of `--raw-capture` and `--screen-capture-rcserver-method` instead (dropping
window tracking entirely rather than keeping it), which became
`--tiled-capture`. This mode shared the mouse-trail ghosting artifact too,
fixed the same way. Not directly compared against `--tiled-capture` head
to head on real hardware.

**Open questions:**

- Whether call *size* (not just count or window-tracking presence) was
  actually what drove the responsiveness difference.
- Whether chunking a window's read into many small tiles costs more in
  aggregate IPC overhead than one large read, for typical window sizes.
- Whether `app_server` CPU-load characteristics carry over the same way
  once window tracking is layered back in.

### `--tiled-capture`: promoted to the default (before the direct pointer)

Started as a more isolated test of the same hypothesis `--hybrid-capture`
tests: does it need window tracking at all, or was that just carrying
`--experimental-screen-capture`'s complexity along for the ride? Dropping
window tracking entirely, real-world testing confirmed it — "very happy
with it," enough to promote it from opt-in experiment to the default, no
flag needed.

**The design:** every frame, the whole screen refreshed through the same
small, fixed-size tile grid `--hybrid-capture` uses for windows, applied
to the full screen instead — no window-kit API, no background caching, no
persistent state between frames. Every tile read fresh and pasted every
frame, matching the original default's own "every pixel current, every
frame" guarantee. Two consequences:

- **No round-robin staleness, no drag-fragmentation** — nothing to be
  behind on.
- **No dedicated cursor handling needed** — every tile refreshes every
  frame regardless, so nothing can go stale. This is what pointed at the
  mouse-trail ghosting bug in every other mode (see above) — `--tiled-
  capture` never showed it.

**The trade-off against `--hybrid-capture`:** can't skip anything a cached
background could — the entire screen is re-read every frame rather than
only tracked-window regions, with empty wallpaper/Deskbar area cached
until layout changes. Still unconfirmed which of the two actually feels
better in practice on a desktop with a lot of empty area.

### `--direct-raw-capture`: `--raw-capture` through a raw framebuffer pointer

`--direct-tiled-capture` (below) found the direct pointer only modestly
beat plain `BScreen` reads in its first round, pointing at framebuffer
memory-read cost (not `app_server`'s IPC overhead) as the real bottleneck.
But `--raw-capture` had already shown, through plain `BScreen`, that call
*count* matters independently: one big `BScreen::ReadBitmap()` measured
meaningfully faster than the tiled default for the same total bytes,
purely from being one call instead of many small ones. This mode combines
both: one direct-pointer read, covering the whole screen in a single tight
copy loop, no tiling, no per-call overhead at all.

Mechanically this reuses the same direct-pointer branch
`--direct-tiled-capture` exercises per tile, called once for the whole
screen instead — same `SetupDirectCapture()`, same four-step verification
(see `readme.md`). Falls back to exactly `--raw-capture`'s own
`screen.ReadBitmap()` when the direct pointer isn't verified safe.

**Confirmed via real-world testing, across two rounds** (same hardware,
same two rounds `--direct-tiled-capture` went through — see its own
section in `readme.md`):

- *Round one* (plain `memcpy`): ~402ms/frame — meaningfully faster than
  `--direct-tiled-capture`'s own ~642ms at the time, confirming call count
  was a real, independent factor. Less window-drag tearing was visible
  even at that similar total time to `--raw-capture`'s own `BScreen`
  number, consistent with one tight, uninterrupted copy landing closer to
  atomic than either many small tile calls or `BScreen::ReadBitmap()`'s
  own internals.
- *Round two* (SSE4.1 streaming-load fast path, see `readme.md`): ~43ms/
  frame — the fastest capture path measured in this project. No tearing
  at all in real dragged-window testing.

Kept as its own explicit opt-in rather than promoted alongside
`--direct-tiled-capture`, despite measuring faster: the tiled default's
many-small-reads shape has the most real-world mileage in this project
(every other mode above builds on it), and the final gap (~43ms vs.
~63ms) is small enough in absolute terms that it didn't obviously justify
a bigger architectural shift for the default every user gets.

### `--raw-capture`, revisited: still the fallback of last resort

Kept available in case the tiled default's own extra per-frame IPC
overhead (many small calls instead of one big one) ever turns out to
matter more than the responsiveness win it was confirmed to bring — on
different hardware, a heavily loaded system, or anything untested so far.

## Real hardware vs. virtual machines

`--logfps`'s capture-vs-encode+write split makes a real difference
visible: how much of Haiku's own `app_server` contention is a real-
hardware-only cost, versus something a VM guest doesn't run into.

**On real hardware** (before the direct-pointer default), capture was the
overwhelming majority of every frame's cost, at every quality profile:
~730-760ms/frame under tiled `BScreen` capture (nearly all capture, not
encode+write), ~400-425ms/frame with `--raw-capture`'s single whole-screen
read — consistent across Low/Medium/High, since only the encode side (a
consistently small 15-60ms) responds to profile choice at all. At the
time, the tiled default was still the better trade-off on real hardware
despite `--raw-capture`'s lower raw frame time, since `--raw-capture`
pushed overall frame rate low enough (well under 3fps) that the mouse
cursor became practically unusable in the recording.

**Inside a VM**, that bottleneck largely disappears. A Haiku VM guest's
software-rendered graphics stack services capture requests in single-digit
milliseconds instead of hundreds — one real-world run landed at ~20.6fps
average against Medium's 24fps target, capture averaging ~2.7ms/frame,
only ~5% of frames over budget. In a VM, `--raw-capture` was the clear
winner at the time: one capture call per frame instead of tiled capture's
~200+ small reads, with none of the real-hardware mouse-responsiveness
trade-off.

This was Haiku's `app_server` non-compositing architecture at work: it
draws windows and the cursor directly rather than compositing
pre-rendered layers, so a `BScreen::ReadBitmap()` request had to be
serviced by the same code path doing that drawing, briefly contending
with it every time hrecord asked for a frame. This was treated as an
architectural property of `app_server` itself, not fixable from outside
it — true for anything going through `BScreen`, and still true today for
the fallback path. What changed is `--direct-tiled-capture`/`--direct-raw-
capture` sidestepping `BScreen`/`app_server` entirely when verified safe —
see `readme.md` for the real numbers that came from that.

## `--allaudio`: mixing every source at once

The single-tap approach hijacks exactly one app's connection to the
Mixer. `--allaudio` repeats that hijack, unmodified, for *every* app
currently playing — but that leaves N independent streams, each in
whatever raw format its own app negotiated (sample rate, channel count,
encoding can all differ). Rather than splicing into the Mixer's own
internal mixing (the approach that crashed Haiku's Mixer control thread
early on, reproducibly, and was abandoned for exactly that reason), each
tapped source is independently resampled to one fixed format (48kHz
stereo float) and summed in hrecord's own code. One shared `BSoundPlayer`
plays back the combined mix.

The mix is a plain average, not a straight sum — since every source is
already within its own valid range, an average of N sources can never
clip, at the cost of getting quieter as more apps join. A deliberate
trade-off: guaranteed-safe over louder-but-occasionally-distorting.

If any app can't be tapped, it's skipped with a warning; the whole thing
only fails if *no* source could be tapped.

**Resampling a source with a native rate well below the 48kHz bus rate**
(e.g. a synth at 8kHz or 11kHz) used to need several times as many output
samples as input, and the per-buffer output size was originally sized for
something closer to a flat "roughly double" margin — fine for 44.1kHz ->
48kHz, not for a much lower rate. Undersizing doesn't fail loudly:
whatever doesn't fit stays buffered inside the resampler's own state and
comes out on a *later* call, showing up first as growing startup latency
and then as chopping once that source's own per-tap buffer runs dry
between delayed, bursty catch-ups — easy to mistake for a "sample
mismatch" between sources, but actually a fixed-size buffer sized for the
wrong rate ratio. The output buffer is now sized from each source's real
rate ratio, with a bounded drain loop as a second line of defense.

**A source whose producer delivers audio in bursts, not a steady drip**
(a network radio stream doing its own rebuffering is the clearest
example) can overflow (dropped audio, heard as a pop) or underrun
(zero-filled gaps, heard as a blip) a per-tap ring buffer too small to
absorb the burstiness — independent of whether the sample rate needed
converting. Each tapped source's ring is now sized for 2 seconds (up from
half a second), and tracks bytes dropped/silence-filled; if either is
non-zero at teardown, hrecord prints which app and how much, e.g.:

```
[i] Audio source "SomaFM Player": 4032 bytes dropped (arrived faster than
the mix could take them), 0 bytes silence-filled (arrived slower, or with
gaps, than the mix needed them) -- a likely cause of any popping or
dropouts heard for this source.
```

**Encoding used to happen directly on the mixed-playback callback's own
thread**, which runs under a hard deadline set by the sound driver's own
hardware buffer depth — a driver tuned for low latency (e.g.
`play_buffer_frames` overridden in `hda.settings`, the kind real-time
audio work like rakarrack depends on) can bring that down to single-digit
milliseconds. Vorbis encoding is variable-latency work with no business
running under that tight a deadline; missing it is audible as clicking,
confirmed as a real contributor to `--allaudio` popping under exactly
that driver tuning. Encoding now happens on its own dedicated thread:
the playback callback only mixes and hands off to a queue, a separate
worker drains it and encodes with no comparable timing pressure.

**The actual root cause of both popping and multi-second delay**, found
from real timing/overflow diagnostics: `AudioTapNode` was built untimed
on purpose (`SetTimeSource(nullptr)`, `B_RECORDING` run mode) — it just
hands off whatever the hijacked app gives it, no pacing of its own. Fine
in single-tap mode, but `--allaudio` puts something real-time-paced
downstream for the first time (`MixedPlaybackCallback`, tied to the
hardware clock), and diagnostics confirmed some tapped apps push audio
*faster than real time* — one session showed ~18 million bytes (≈47
seconds of audio) continuously dropped, not a one-time burst. Widening
the ring earlier just traded one symptom (popping) for the other
(growing delay), which is why it measured *worse* in some tests.

The real fix is backpressure: each tap now paces its own consumption to
match real time, snoozing briefly when running ahead. That delays
`Recycle()`-ing the buffer back — the exact signal Media Kit uses to let
a producer know it can send more — so an over-fast producer gets
throttled the same way it naturally would connected straight to the
Mixer. (Deliberately not just giving the node a real time source instead,
the more "normal" Media Kit approach — `SetTimeSource(nullptr)` was
chosen earlier specifically to dodge a reproducible `BTimeSource::
RealTimeFor` crash in a different, abandoned approach.)

Confirmed fixed in testing: with pacing in place, backlog and delay both
dropped to milliseconds (from as much as several seconds before), overflow
drops went to zero.

## Real-time audio: how the numbers were reached

**This used to require `--realtime` (and, for a while, a separate
`--experimental` flag) on every recording.** Once both were confirmed
working — including the exact use case that motivated them (monitoring a
live instrument through effects while playing) — requiring two flags on a
system already tuned for real-time audio stopped making sense. hrecord now
reads the driver's own settings file itself at startup:

1. Scans `/dev/audio/hmulti/` for the active driver.
2. If cataloged and its settings file has a genuinely active (uncommented)
   play buffer frame count set, that's taken as authoritative and hrecord
   matches it exactly. No flag needed.
3. `--realtime` remains a manual fallback (128-frame generic guess) for
   drivers not yet cataloged (six so far: `hda`, `auich`, `es1370`,
   `echo`, `emuxki`, `ice1712` — see
   [RealTimeGUI](https://github.com/ablyssx74/RealTimeGUI)'s own driver
   catalog, this project's companion app for editing these same files).

Real-time mode trims ring buffers (2s -> 0.07s single-tap and `--allaudio`
per source) and the `BSoundPlayer` buffer-size hint, in both single-tap
and `--allaudio` mode.

**These numbers were reached in two steps, the first going badly enough
to explain here.** They started behind the old `--experimental` flag
(256 frames / 0.03s rings) alongside a *tighter* pacing "catch-up snooze
cap" — reasoning that a smaller cap meant a large corrective sleep could
never become a latency spike. Testing found the opposite: overflow on
three simultaneously tapped sources went from at-or-near zero to hundreds
of thousands, even tens of millions, of bytes dropped — constant
crackling, not just extra lag. The cap didn't just bound one sleep, it
bounded how much drift pacing could correct *per buffer* — a source
running ahead needs to fully correct before the next buffer arrives, a
tighter cap makes that take more calls, and the also-shrunk ring left far
less room to absorb the gap meanwhile. The two changes compounded. Fixed
by decoupling them: the snooze cap is a flat, generous 50ms baseline in
every mode now; ring buffer size is the real latency dial, retested at a
gentler 0.07s and confirmed clean.

**Buffer size retuned again, 256 frames -> 128** after a real user tuned
their own driver further (`play_buffer_frames` 128, ~7ms Cortex-reported
latency) — confirmed lower latency with only occasional clicks rather
than a clean zero. Now that the exact frame count auto-detects from the
driver's settings file, 128 only applies as `--realtime`'s own generic
fallback.

### The pacing hold cap

`PaceToRealTime`'s backpressure works by deliberately delaying
`buffer->Recycle()`, holding a buffer back up to 50ms as the signal that
tells a fast producer to slow down. That flat cap was fine at 256 frames
(~5.3ms @ 48kHz, ~9x hold-to-period ratio), but at 128 frames (~2.7ms)
that ratio nearly doubles to ~18x — plausibly enough to strain a small
hardware buffer's own headroom, heard as occasional clicks even with
nothing dropped or logged as an error.

The hold is now *also* capped at roughly 2x that source's own buffer
duration, whenever tighter than the flat 50ms — deliberately not the same
mistake as the regression above, since this only tightens for sources
whose own buffers are small and frequent to begin with, and correction
*opportunity* scales right along with it (`BufferReceived` fires just as
often). A source with large, infrequent buffers keeps the full 50ms.

This exact idea was tried once before, gated behind the old
`--experimental` flag, for a different symptom entirely (a Rakarrack-side
`SoundPlayNode::FillNextBuffer: RequestBuffer failed` flood) — that one
turned out to be caused by stale `media_server` state, not pacing timing,
so it was reverted as unnecessary at the time. Real-world testing at
48kHz/128-frame buffers with Rakarrack came back clean, so this is now
unconditional, always-on behavior.

## Audio: known open issues

Two things reported from testing against still-open apps across repeated
runs, which may share one root cause:

1. Repeatedly hijacking the same app in quick succession has,
   intermittently, produced Haiku's own Media Kit printing `Bad port
   ID`/`GetNodeFor failed` diagnostics during teardown, alongside
   silence-filled audio mid-session — consistent with the hijacked app's
   connection having gone away before hrecord tried to restore it.
   hrecord itself doesn't crash (finishes and writes output normally),
   and it doesn't reproduce every time.
2. When an app disappears mid-session like that, a zombie
   Media-preferences entry can occur, since there's nothing left to
   cleanly restore by teardown.

**Fixed: hijacked apps staying listed in Media preferences after being
fully closed.** Confirmed hrecord's own bug (doesn't happen on a session
that's never run hrecord): every Media Kit roster call handing back a
`media_node` (`GetNodeFor()`, `GetAudioMixer()`) hands out a reference the
caller must explicitly release with `ReleaseNode()` — hrecord was
acquiring one per hijacked app and for the Mixer itself but never
releasing either. `media_server` kept the node considered "in use" by
hrecord's already-exited process indefinitely. Every acquisition now has a
matching release, including on every internal failure path.

**Resolved: a "2nd-instance lag" finding previously listed here.**
Re-running hrecord against apps still connected from a previous run
showed noticeably more lag the second time. Comparing periodic backlog
logs between a barely-laggy run and a clearly-laggy one: the numbers were
essentially identical in both. Restarting only the tapped app (Rakarrack)
between runs, leaving hrecord untouched, made the lag go away — so this
lived entirely inside the tapped app's own internal state, not hrecord's.
Workaround if it recurs with some other app: restart that app between
runs.

**Resolved: the `SoundPlayNode` flood was leftover zombie Media Server
state, not a persistent hrecord bug.** A controlled test initially found
Haiku's own `SoundPlayNode::FillNextBuffer: RequestBuffer failed`
flooding continuously while hrecord's tap stayed connected. The real
explanation: that test's Media Server had already accumulated zombie node
state from earlier sessions (see the reference-leak fix above and "Known
issue: stale Mixer connection" in `readme.md`) — a roster already in a bad
state doesn't settle a new connection cleanly, however long you wait.
With Media Server freshly restarted, the same test instead shows
`RequestBuffer failed` for a few seconds right after connecting, then
settles — ordinary connection warm-up, not sustained failure. Considered
resolved: keep Media Server clean and this doesn't reproduce.

A settle-delay widening (20ms -> 100ms in the hijack/restore connect
calls) and an `--experimental`-gated buffer-hold cap were both tried as
fixes before this was understood — the settle-delay widening is harmless
and stays; the buffer-hold cap turned out unnecessary once the real cause
was found, so `--experimental` is back to being a no-op.
