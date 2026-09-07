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
  track alongside it whenever the audio tee (below) can be set up.
- `hrecord start --audioonly` — records desktop audio only (no screen
  capture) to `/boot/home/hrecord_capture.ogg`, an Ogg/Vorbis file. Both Ogg
  and Vorbis are open, royalty-free formats, so this carries none of the
  licensing baggage a proprietary audio codec would.
- `hrecord stop` — signals a running recording instance to stop and finalize
  its output file.
- `hrecord --list-audio-inputs` — prints every audio node the media_server
  currently knows about (Mixer, sound card, effects, etc.) — mostly useful
  for confirming a System Mixer and sound card are actually present.

## How desktop-audio capture works

Haiku's System Mixer only ever exposes a single output, already wired
directly to the sound card — there's no built-in "Stereo Mix"-style tap to
just listen in on. So on startup hrecord:

1. Finds the Mixer and the sound card (`BMediaRoster::GetAudioMixer()` /
   `GetAudioOutput()`).
2. Disconnects the Mixer's existing direct connection to the sound card.
3. Registers its own pass-through Media Kit node (`AudioTeeNode`) and
   rewires the graph as Mixer → hrecord's tee → sound card, using the exact
   format the Mixer and sound card had already agreed on.
4. From then on, every audio buffer the Mixer produces is forwarded to the
   sound card completely unchanged (so what you hear doesn't change) while
   a copy is handed to hrecord's Vorbis encoder.
5. On a clean shutdown (Ctrl+C or `hrecord stop`), the tee is removed and
   the Mixer is reconnected directly to the sound card, exactly as it was
   found.

This only ever touches hrecord's own local audio pipeline — the same signal
already being sent to your speakers — so there's nothing here that
intercepts audio you couldn't otherwise hear yourself.

**Caveat:** the restore-on-exit step only runs on a normal shutdown. If
hrecord is killed with `kill -9` or crashes while the tee is spliced in, the
Mixer is left connected to hrecord instead of the sound card and system
audio will go silent until something reconnects it — use Media preferences'
"Restart Media Services" (or an equivalent `media_server` restart) to
recover in that case.

If `SetupDesktopAudioTee` can't find a Mixer or sound card at all (e.g. no
audio hardware/driver present), hrecord records video only and says so, or
fails outright for `--audioonly` since there'd be nothing to record.
