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
