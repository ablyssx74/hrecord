# hrecord_x264 — the H.264 add-on of hrecord

Streaming to YouTube, Twitch and the like needs H.264 video, and Haiku's ffmpeg has no H.264 encoder (x264 is GPL).
So the encoder is a separate small library that hrecord loads when it is installed:

- `hrecord_x264.h` is the whole interface between the two (plain C).
- `hrecord_x264.c` wraps x264. This add-on is GPL licensed, hrecord stays MIT.
- hrecord looks for `hrecord_x264` in `/boot/system/add-ons/hrecord/`, `~/config/non-packaged/add-ons/hrecord/` and `~/config/add-ons/hrecord/`.

Build with `make` (needs the `x264_devel` package).
