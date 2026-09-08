# Haiku Terminal Screen Recorder App.

## Build

```
make release

```

## Usage

```
hrecord [start|stop] [--audioonly] [--list-audio-inputs]
```

- `hrecord` / `hrecord start` — records the screen (MJPEG in a `.mkv`
  container) to `/boot/home/hrecord_capture.mkv`, with a Vorbis desktop-audio
  track alongside it whenever the audio tap (below) can be set up.
- `hrecord start --audioonly` — records desktop audio only (no screen
  capture) to `/boot/home/hrecord_capture.ogg`, an Ogg/Vorbis file. Both Ogg
  and Vorbis are open, royalty-free formats, so this carries none of the
  licensing baggage a proprietary audio codec would.
- `hrecord stop` — signals a running recording instance to stop and finalize
  its output file.
- `hrecord --list-audio-inputs` — lists the apps currently feeding the
  System Mixer (i.e. currently playing sound). `--audioonly` needs at least
  one.

## How desktop-audio capture works

An earlier version of hrecord spliced a Media Kit node between the System
Mixer's *output* and the sound card — the single, shared connection
everything downstream depends on. That reproducibly crashed Haiku's own
Mixer control thread on real hardware, so it's been replaced with a
structurally different, lower-risk approach:

1. Find one app currently playing sound (i.e. connected to the Mixer).
2. Briefly stop it, redirect its connection through hrecord's own Media Kit
   node instead of straight to the Mixer, then reconnect hrecord's node to a
   *fresh* Mixer input and restart the app.
3. From then on, every buffer the app produces is copied to hrecord's Vorbis
   encoder and immediately forwarded on to the Mixer unchanged — so what you
   hear doesn't change, aside from the one extra hop.
4. On a clean shutdown (Ctrl+C or `hrecord stop`), the tap is torn out and
   the app is reconnected directly to the Mixer, exactly as it was found.

The Mixer's input side is inherently dynamic — apps connect and disconnect
from it constantly as they start and stop playing sound — so it's built to
handle this kind of churn, unlike its single, rarely-touched output
connection to hardware. This only ever touches hrecord's own local audio
pipeline — the same signal already being sent to your speakers — so there's
nothing here that intercepts audio you couldn't otherwise hear yourself.

**Caveat:** the restore-on-exit step only runs on a normal shutdown. If
hrecord is killed with `kill -9` or crashes while the tap is spliced in, the
hijacked app is left connected to hrecord instead of the Mixer and will stop
being audible until it's restarted (or, if needed, Media preferences'
"Restart Media Services").

If nothing is currently playing when hrecord starts, it records video only
(with a warning) in the default mode, or fails outright for `--audioonly`
since there'd be nothing to capture.
