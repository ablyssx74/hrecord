# Research: BDirectWindow as a faster capture path

## Result: BDirectWindow connects successfully -- the accelerant patch alone was sufficient

Long story, worth recording in full since it took several wrong turns to get
here:

1. **First real-hardware run** (unpatched nvidia-haiku accelerant):
   `SupportsWindowMode(): false`. Traced to `ToHaikuMode()` in the
   accelerant never setting `display_mode.flags` at all -- fixed upstream
   (see the accelerant's own commit) by setting `B_PARALLEL_ACCESS`, which
   is honest: this driver's framebuffer is a plain linear CPU-mapped
   buffer, the same architecture the flag describes.
2. **After that fix**: `SupportsWindowMode()` correctly returned `true`,
   both on the patched real-hardware accelerant and (unpatched) in a VM
   guest -- but `DirectConnected()` never fired on either, timing out
   after 5s with no crash, no hang, no error. Looked exactly like
   `app_server` silently refusing the connection.
3. **Long detour into `app_server` itself**: built a custom `app_server`
   (full nightly image, real effort -- see the `haiku` fork's own commit
   history) with `debug_printf` logging at every decision point in the
   connect handshake (`ServerWindow::_EnableDirectWindowMode()`,
   `Desktop::_ShowWindow()`, `ServerWindow::HandleDirectConnection()`,
   `DirectWindowInfo::SetState()`). Every single one succeeded, every
   time, including the initial `B_DIRECT_START` call -- `SetState()`'s own
   client-acknowledgment semaphore handshake (500ms timeout) never failed
   once. That's only possible if the client's daemon thread was already
   waking up and calling `DirectConnected()` successfully.
4. **The actual bug, found by reading `headers/os/game/DirectWindow.h`
   directly**: `B_DIRECT_START = 0`. It is *not* an independent bit flag --
   `B_DIRECT_START`/`B_DIRECT_STOP`/`B_DIRECT_MODIFY` are mutually-exclusive
   *values* (0/1/2) packed into the low 4 bits of `buffer_state`
   (`B_DIRECT_MODE_MASK = 15`), meant to be extracted and compared, never
   tested with a bare `&` -- `buffer_state & B_DIRECT_START` is `& 0`,
   always false, regardless of what actually happened. This probe's own
   `DirectConnected()` override had exactly that bug. `app_server`'s own
   internal code and Haiku's own client-side `_DirectDaemon()` both
   correctly use `(buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_START`.
   Fixed the same way here.

**No `app_server` changes were ever actually required.** The whole
`app_server`-debugging detour was real, useful work (it's what proved
`DirectConnected()` was firing, which is what pointed at the probe's own
check being the broken piece rather than anything server-side) -- but the
fix that matters is entirely contained in the accelerant's
`B_PARALLEL_ACCESS` patch plus this probe's own bitmask bug. A stock,
unmodified `app_server` works fine.

**Still open**: the probe's `--desktop-read-test` -- the actual question
this whole investigation exists to answer (does the direct buffer pointer
really cover the *whole* desktop, safely, not just this window's own
region) -- hasn't been re-run since the bitmask fix. That's the next,
genuinely final step before concluding whether this is usable as a real
hrecord capture path.

Exploratory only -- not part of hrecord itself, not built by hrecord's own
`make`/`make release`. See `directwindow_probe.cpp`'s own top comment for
the full original rationale.

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
./directwindow_probe                    # connection info + timing
./directwindow_probe --desktop-read-test # tests reading past this window's
                                          # own bounds -- read the warning
                                          # in directwindow_probe.cpp first;
                                          # save other work first
```

Please run both again now that the bitmask bug is fixed, on real hardware
and in a VM guest -- that's what actually decides whether integrating this
into hrecord is possible, and if so, what it would need to handle.
