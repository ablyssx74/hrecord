# Haiku Terminal Screen Recorder App.

## Build

```
make release

```

## Usage

```
hrecord [start|stop] [--audioonly]
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

Desktop-audio capture only works where the sound hardware/driver actually
provides a loopback capture input; hrecord does not attempt to tap Haiku's
System Mixer output directly (it only ever exposes a single output, already
wired to the speakers), so there's nothing here that intercepts audio you
wouldn't otherwise be able to record locally.
