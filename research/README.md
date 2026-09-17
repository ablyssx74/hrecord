# Research: BDirectWindow as a faster capture path

## Result: ruled out, on real hardware

`./directwindow_probe` on real Haiku hardware (the same machine whose
`--logfps` numbers are documented in the main readme's "Real hardware vs.
virtual machines" section) reported `SupportsWindowMode(): false` --
windowed `BDirectWindow` isn't available on this driver at all. The only
alternative, `SetFullScreen(true)`, takes over the entire display, which
defeats the point of a background recorder (it needs the user's other
windows to stay usable while it captures them). So this path is a dead
end here, bounded by Haiku's own driver/`app_server` architecture on this
hardware -- not something reachable from hrecord's own code, through this
API or any other. hrecord's actual capture code was never touched based
on this; the default tiled capture (real hardware) / `--raw-capture` (VM
guest) split documented in the main readme stands as the practical
answer. Kept here, unbuilt by hrecord's own Makefile, as a record of what
was tried and why, so this doesn't get re-investigated from scratch later
on the same hardware class.

Exploratory only -- not part of hrecord itself, not built by hrecord's own
`make`/`make release`. See `directwindow_probe.cpp`'s own top comment for
the full rationale; short version: hrecord's `--logfps` diagnostics showed
real Haiku hardware spending hundreds of ms/frame in the
`BScreen::GetBitmap()`/`ReadBitmap()` IPC call every capture mode goes
through, while the same call costs single-digit ms inside a Haiku VM guest.
`BDirectWindow` is a real Haiku API for getting a direct, shared-memory
pointer into the actual frame buffer, bypassing that IPC path entirely --
but whether it can see the *whole desktop* (not just its own window) safely
on real hardware is unverified, driver-dependent, and worth testing before
touching hrecord's own capture code at all.

## Build

```
cd research
make
```

Already links `-lgame` -- `BDirectWindow`'s own symbols live in Haiku's
separate Game Kit library, not `libbe` itself (confirmed by a real link
failure on real hardware).

## Run

```
./directwindow_probe                    # safe: connection info + timing
./directwindow_probe --desktop-read-test # also tests reading past this
                                          # window's own bounds -- read the
                                          # warning in directwindow_probe.cpp
                                          # first; save other work first
```

Please run both on real hardware and inside a VM guest (same as the
`--logfps` tests), and paste back the full output from each -- that's what
decides whether integrating this into hrecord is even possible, and if so,
what it would need to handle.
