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
  container) to `/boot/home/hrecord_capture.mkv`. If the machine's audio
  hardware/driver exposes a desktop-audio loopback input (e.g. "Stereo
  Mix"/"What U Hear"), a Vorbis audio track is recorded alongside it in the
  same file; otherwise it falls back to video only and says so.
- `hrecord start --audioonly` — records desktop audio only (no screen
  capture) to `/boot/home/hrecord_capture.ogg`, an Ogg/Vorbis file. Both Ogg
  and Vorbis are open, royalty-free formats, so this carries none of the
  licensing baggage a proprietary audio codec would.
- `hrecord stop` — signals a running recording instance to stop and finalize
  its output file.
- `hrecord --list-audio-inputs` — prints every audio-producing node the
  media_server currently knows about, flagging any whose name matches
  hrecord's loopback heuristic. Run this if `--audioonly` (or the audio track
  in a normal recording) reports no loopback input found, to see what your
  driver actually calls its inputs.

Desktop-audio capture only works where the sound hardware/driver actually
provides a loopback capture input; hrecord does not attempt to tap Haiku's
System Mixer output directly (it only ever exposes a single output, already
wired to the speakers), so there's nothing here that intercepts audio you
wouldn't otherwise be able to record locally. Not every audio chip/driver
combination exposes such an input — if `--list-audio-inputs` shows nothing
that looks like a loopback device, this build of hrecord has no way to
record desktop audio on that machine.
