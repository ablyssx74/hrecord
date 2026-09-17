// directwindow_probe -- a standalone research tool, NOT part of hrecord itself.
//
// What this is for: hrecord's --logfps diagnostics confirmed that on real
// Haiku hardware, BScreen::GetBitmap()/ReadBitmap() -- the IPC round-trip to
// app_server that every hrecord capture mode ultimately goes through -- is
// the dominant per-frame cost (hundreds of ms), while the same call inside a
// Haiku VM guest costs single-digit ms. The working theory: BDirectWindow
// gives an ordinary app a direct, shared-memory pointer into the actual
// frame buffer app_server itself draws into (the same `frame_buffer_config`
// memory), bypassing the normal client/app_server message-passing draw
// pipeline that GetBitmap()/ReadBitmap() goes through -- so reading through
// it should be close to a raw memcpy, independent of whatever's making the
// IPC path slow on this hardware.
//
// This is NOT a validated assumption yet -- hence a standalone probe instead
// of touching hrecord's actual capture code:
//
//   1. BDirectWindow is documented and designed to give a window access to
//      ITS OWN backing-store region -- not arbitrary desktop content drawn
//      by other windows. Whether the pointer you get actually sits inside
//      one single, whole-desktop-sized frame buffer (so reading past your
//      own window's bounds reaches real neighboring desktop content) is an
//      internal implementation detail of Haiku's app_server + the specific
//      video driver/accelerant in use -- true for a classic non-compositing
//      single-frame-buffer design (which is what hrecord's own comments,
//      elsewhere in this project, already describe app_server as being),
//      but not guaranteed by the DirectWindow API contract itself, and not
//      something to assume works identically across every driver -- notably
//      including the generic/VESA-style drivers a lot of real hardware
//      falls back to, which is exactly the hardware this matters for.
//   2. Driver support for *windowed* direct connection (as opposed to
//      SetFullScreen(true), which takes over the whole display -- not
//      viable for a background recorder that's supposed to capture the
//      user's other apps while they keep working) varies. SupportsWindowMode()
//      is checked below and its result printed -- if it comes back false on
//      real hardware, windowed BDirectWindow is a dead end there regardless
//      of anything else this probe finds.
//
// This probe answers both questions empirically, on whatever machine it's
// actually run on, without risking hrecord's own capture path:
//   - prints SupportsWindowMode()'s result and the direct_buffer_info Haiku
//     hands back on connect (state flags, bits pointer, bytes_per_row,
//     bits_per_pixel, pixel_format, window/clip bounds, clip list)
//   - times a raw memcpy read through the direct pointer, over the window's
//     own (spec-legal) clipped region, against an equivalent-sized
//     BScreen::ReadBitmap() call -- a rough, hrecord-independent version of
//     the same capture-timing comparison --logfps already does
//   - optionally (--desktop-read-test, off by default -- see the giant
//     warning where it's used below), tests whether memory *outside* the
//     window's own clip region is (a) safely readable at all and (b) matches
//     real on-screen content read via BScreen::ReadBitmap() at the same
//     screen coordinates -- the actual test of the "one shared whole-screen
//     buffer" hypothesis this whole investigation hinges on
//
// Build (from this directory): make
// (BDirectWindow's own symbols live in Haiku's separate Game Kit lib, not
// libbe itself -- confirmed by a real link failure; the Makefile already
// links -lgame.)
//
// Run: ./directwindow_probe
//      ./directwindow_probe --desktop-read-test   (see warning below)
//
// Please paste back the full output (and, for --desktop-read-test, whether
// it ran cleanly, printed a MISMATCH, or hit the SIGSEGV/SIGBUS handler)
// from both a real-hardware run and a VM-guest run, same as the --logfps
// diagnostics -- that's what turns this from a theory into a decision about
// whether integrating this into hrecord is even possible.

#include <DirectWindow.h>
#include <Application.h>
#include <Screen.h>
#include <Bitmap.h>
#include <Locker.h>
#include <OS.h>

#include <csignal>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static bool g_desktopReadTest = false;

// ----------------------------------------------------------------------
// A minimal safety net for --desktop-read-test: reading memory outside
// the window's own clip_list is outside what the DirectWindow API
// contract promises is valid, so on some driver/hardware combination it
// could raise SIGSEGV/SIGBUS instead of just returning wrong bytes. A
// hard crash there would still be a useful, real answer ("this driver
// doesn't share one buffer, full stop") -- but only if we can report it
// instead of just dying silently. sigsetjmp/siglongjmp turns that crash
// into a message and a clean exit.
// ----------------------------------------------------------------------
static sigjmp_buf g_segvJmpBuf;
static volatile sig_atomic_t g_inRiskyRead = 0;

static void SegvHandler(int sig) {
    if (g_inRiskyRead) {
        siglongjmp(g_segvJmpBuf, sig);
    }
    // Not something we were expecting -- fall back to default handling
    // (abort/core) rather than silently swallowing a real crash elsewhere.
    signal(sig, SIG_DFL);
    raise(sig);
}

static const char* PixelFormatName(color_space format) {
    switch (format) {
        case B_RGB32:  return "B_RGB32";
        case B_RGBA32: return "B_RGBA32";
        case B_RGB24:  return "B_RGB24";
        case B_RGB16:  return "B_RGB16";
        case B_RGB15:  return "B_RGB15";
        case B_CMAP8:  return "B_CMAP8";
        default:       return "(other/unrecognized -- see numeric value)";
    }
}

class ProbeWindow : public BDirectWindow {
public:
    ProbeWindow(BRect frame)
        : BDirectWindow(frame, "hrecord DirectWindow Probe", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE),
          fConnected(false),
          fConnectSem(create_sem(0, "directwindow_probe_connect")) {
        memset(&fInfo, 0, sizeof(fInfo));
    }

    ~ProbeWindow() {
        delete_sem(fConnectSem);
    }

    // Called by app_server on its own dedicated thread, not this window's
    // usual message-handling thread -- must stay fast and must not touch
    // fInfo.bits after a B_DIRECT_STOP without re-checking buffer_state
    // first (the memory it pointed to may be gone/relocated by then).
    virtual void DirectConnected(direct_buffer_info* info) {
        fLock.Lock();
        fInfo = *info; // shallow copy is fine for the fixed-size fields we
                        // read below; we never touch info->clip_list past
                        // this call, only the fields already copied here.
        if (info->buffer_state & B_DIRECT_START) {
            fConnected = true;
            release_sem(fConnectSem);
        } else if (info->buffer_state & B_DIRECT_STOP) {
            fConnected = false;
        }
        fLock.Unlock();
    }

    // Blocks the calling (non-app_server) thread until the first
    // DirectConnected(..., B_DIRECT_START) arrives, or timeout.
    bool WaitForConnect(bigtime_t timeoutUs) {
        status_t err = acquire_sem_etc(fConnectSem, 1, B_RELATIVE_TIMEOUT, timeoutUs);
        return err == B_OK;
    }

    // Snapshots the current state under the lock -- called from the probe
    // thread, never from DirectConnected() itself.
    direct_buffer_info Snapshot() {
        fLock.Lock();
        direct_buffer_info copy = fInfo;
        fLock.Unlock();
        return copy;
    }

    bool IsConnected() {
        fLock.Lock();
        bool c = fConnected;
        fLock.Unlock();
        return c;
    }

private:
    BLocker fLock;
    bool fConnected;
    sem_id fConnectSem;
    direct_buffer_info fInfo;
};

static void PrintBufferState(uint32 state) {
    printf("    buffer_state flags:");
    if (state & B_DIRECT_START)          printf(" B_DIRECT_START");
    if (state & B_DIRECT_STOP)           printf(" B_DIRECT_STOP");
    if (state & B_DIRECT_MODIFY)         printf(" B_DIRECT_MODIFY");
    if (state & B_BUFFER_MOVED)          printf(" B_BUFFER_MOVED");
    if (state & B_CLIPPING_MODIFIED)     printf(" B_CLIPPING_MODIFIED");
    if (state & B_BUFFER_RESIZED)        printf(" B_BUFFER_RESIZED");
    if (state & B_BUFFER_RESET)          printf(" B_BUFFER_RESET");
    printf(" (0x%08x)\n", (unsigned)state);
}

// Times `iterations` full reads of `bytes` bytes starting at `src`, each
// copied into a scratch buffer -- the same shape of work hrecord's own
// capture-vs-encode split under --logfps measures for the capture side,
// just isolated here from everything else in hrecord's pipeline.
static double TimeMemcpyReadMs(const uint8_t* src, size_t bytes, int iterations) {
    uint8_t* scratch = (uint8_t*)malloc(bytes);
    if (!scratch) return -1.0;
    bigtime_t start = system_time();
    for (int i = 0; i < iterations; i++) {
        memcpy(scratch, src, bytes);
    }
    bigtime_t elapsed = system_time() - start;
    free(scratch);
    return (elapsed / 1000.0) / iterations;
}

// One BBitmap allocated up front and refilled in place every iteration --
// matches hrecord's own --raw-capture path exactly (see readme.md's
// "Screen recording quality profiles" section: allocating fresh per call,
// as BScreen::GetBitmap() does, was itself a confirmed, fixed bottleneck),
// so this is a fair baseline for the direct-pointer number above, not an
// artificially slow one.
static double TimeScreenReadBitmapMs(BScreen& screen, BRect rect, int iterations) {
    BBitmap bitmap(rect, B_RGB32);
    bigtime_t start = system_time();
    for (int i = 0; i < iterations; i++) {
        screen.ReadBitmap(&bitmap, false, &rect);
    }
    bigtime_t elapsed = system_time() - start;
    return (elapsed / 1000.0) / iterations;
}

static int32 ProbeThreadEntry(void* arg) {
    ProbeWindow* window = (ProbeWindow*)arg;

    printf("[*] SupportsWindowMode(): %s\n",
        window->SupportsWindowMode() ? "true" : "false");
    printf("    (if false, windowed BDirectWindow is not usable on this\n"
           "    driver/hardware at all -- SetFullScreen(true) full-screen\n"
           "    exclusive mode might still work, but that takes over the\n"
           "    whole display and isn't viable for a background recorder.)\n\n");

    window->Show();

    printf("[*] Waiting for DirectConnected() (up to 5s)...\n");
    if (!window->WaitForConnect(5000000)) {
        printf("[-] Never connected. Either this driver doesn't support\n"
               "    direct windowed access, or something else blocked the\n"
               "    handshake. Nothing more to test.\n");
        be_app->PostMessage(B_QUIT_REQUESTED);
        return 0;
    }

    direct_buffer_info info = window->Snapshot();
    printf("\n[+] Connected. direct_buffer_info snapshot:\n");
    PrintBufferState(info.buffer_state);
    printf("    driver_state: 0x%08x%s%s\n", (unsigned)info.driver_state,
        (info.driver_state & B_DRIVER_CHANGED) ? " B_DRIVER_CHANGED" : "",
        (info.driver_state & B_MODE_CHANGED) ? " B_MODE_CHANGED" : "");
    printf("    bits: %p\n", (void*)info.bits);
    printf("    bytes_per_row: %u\n", (unsigned)info.bytes_per_row);
    printf("    bits_per_pixel: %u\n", (unsigned)info.bits_per_pixel);
    printf("    pixel_format: %s (%d)\n", PixelFormatName(info.pixel_format),
        (int)info.pixel_format);
    printf("    window_bounds: (%d,%d)-(%d,%d)\n",
        (int)info.window_bounds.left, (int)info.window_bounds.top,
        (int)info.window_bounds.right, (int)info.window_bounds.bottom);
    printf("    clip_bounds: (%d,%d)-(%d,%d)\n",
        (int)info.clip_bounds.left, (int)info.clip_bounds.top,
        (int)info.clip_bounds.right, (int)info.clip_bounds.bottom);
    printf("    clip_list_count: %u\n", (unsigned)info.clip_list_count);

    if (info.bits == nullptr || info.bytes_per_row == 0) {
        printf("\n[-] No usable buffer pointer -- stopping here.\n");
        be_app->PostMessage(B_QUIT_REQUESTED);
        return 0;
    }

    // --- Safe test: time a read of the window's own clipped region ---
    // This is squarely inside what DirectConnected() promises is valid, on
    // any driver that got this far.
    int bytesPerPixel = info.bits_per_pixel / 8;
    if (bytesPerPixel <= 0) bytesPerPixel = 4;
    int clipWidth = info.clip_bounds.right - info.clip_bounds.left + 1;
    int clipHeight = info.clip_bounds.bottom - info.clip_bounds.top + 1;
    if (clipWidth > 0 && clipHeight > 0) {
        size_t ownRegionBytes = (size_t)clipHeight * info.bytes_per_row;
        const uint8_t* ownRegionStart = (const uint8_t*)info.bits
            + (size_t)info.clip_bounds.top * info.bytes_per_row;

        printf("\n[*] Timing 30 reads of the window's own clip region "
            "(%dx%d, spec-legal)...\n", clipWidth, clipHeight);
        double directMs = TimeMemcpyReadMs(ownRegionStart, ownRegionBytes, 30);
        printf("    direct-pointer memcpy: %.3fms/read\n", directMs);

        BScreen screen;
        BRect sameRect(info.window_bounds.left + info.clip_bounds.left,
            info.window_bounds.top + info.clip_bounds.top,
            info.window_bounds.left + info.clip_bounds.right,
            info.window_bounds.top + info.clip_bounds.bottom);
        double screenMs = TimeScreenReadBitmapMs(screen, sameRect, 30);
        printf("    BScreen::ReadBitmap() for the same-sized region: "
            "%.3fms/read\n", screenMs);
    } else {
        printf("\n[-] Window is fully clipped away (0x0 visible region) -- "
            "can't time a same-size read. Move it somewhere visible and "
            "rerun.\n");
    }

    // --- Risky test: is this really ONE shared, whole-desktop buffer? ---
    if (g_desktopReadTest) {
        printf("\n[!] --desktop-read-test: reading outside this window's "
            "own clip region.\n"
            "    This is NOT something the DirectWindow API contract "
            "promises is safe --\n"
            "    if it crashes, that itself is a real, useful answer "
            "(this driver does NOT\n"
            "    give windowed clients a pointer into one shared "
            "whole-screen buffer).\n"
            "    A SIGSEGV/SIGBUS here is caught and reported below rather "
            "than crashing\n"
            "    this whole probe, but save any other work before running "
            "this flag anyway.\n");

        BScreen screen;
        BRect screenFrame = screen.Frame();
        // Pick a screen-space probe point well clear of this window (top-left
        // corner of the screen), compute where that would land in the direct
        // buffer if it really is one contiguous whole-screen buffer whose
        // (0,0) corresponds to screen (0,0) minus this window's own origin,
        // and compare actual pixel bytes against a BScreen::ReadBitmap() of
        // that exact same screen coordinate taken moments apart.
        int probeScreenX = 4, probeScreenY = 4;
        // BWindow state must only be read while holding the window's own
        // BLooper lock when called from a thread other than the window's
        // own -- this probe thread isn't it.
        window->Lock();
        BRect windowFrame = window->Frame();
        window->Unlock();
        int windowScreenX = (int)windowFrame.left;
        int windowScreenY = (int)windowFrame.top;
        long offsetBytes = (long)(probeScreenY - windowScreenY) * info.bytes_per_row
            + (long)(probeScreenX - windowScreenX) * bytesPerPixel;
        const uint8_t* probePtr = (const uint8_t*)info.bits + offsetBytes;

        printf("    Screen frame: %dx%d. Probing screen point (%d,%d) -- "
            "window origin is (%d,%d), so that's buffer offset %ld from "
            "info.bits.\n", (int)screenFrame.Width() + 1,
            (int)screenFrame.Height() + 1, probeScreenX, probeScreenY,
            windowScreenX, windowScreenY, offsetBytes);

        struct sigaction sa, oldSegv, oldBus;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SegvHandler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &oldSegv);
        sigaction(SIGBUS, &sa, &oldBus);

        int caughtSignal = sigsetjmp(g_segvJmpBuf, 1);
        if (caughtSignal == 0) {
            g_inRiskyRead = 1;
            uint8_t directBytes[4] = {0, 0, 0, 0};
            memcpy(directBytes, probePtr, bytesPerPixel > 4 ? 4 : bytesPerPixel);
            g_inRiskyRead = 0;

            BRect probeRect(probeScreenX, probeScreenY, probeScreenX, probeScreenY);
            BBitmap refBitmap(probeRect, B_RGB32);
            screen.ReadBitmap(&refBitmap, false, &probeRect);
            uint8_t* refBytes = (uint8_t*)refBitmap.Bits();

            printf("    direct-pointer bytes at that offset: "
                "%02x %02x %02x %02x\n",
                directBytes[0], directBytes[1], directBytes[2], directBytes[3]);
            printf("    BScreen::ReadBitmap() bytes at the same screen "
                "point: %02x %02x %02x %02x\n",
                refBytes[0], refBytes[1], refBytes[2], refBytes[3]);
            bool match = memcmp(directBytes, refBytes,
                bytesPerPixel > 4 ? 4 : bytesPerPixel) == 0;
            printf("    -> %s\n", match
                ? "MATCH. Strong evidence this driver hands windowed "
                  "DirectWindow clients a pointer into one real, whole-"
                  "screen-sized shared buffer."
                : "MISMATCH. Either this isn't one shared whole-screen "
                  "buffer on this driver, the two reads just landed a "
                  "frame apart on changing content, or the offset math "
                  "above is off for this driver's layout -- try probing a "
                  "static area (e.g. bare desktop background) to rule out "
                  "the timing explanation.");

            // Also time a full-desktop-sized raw read from (computed)
            // buffer offset 0, IF the single-pixel probe above didn't
            // segfault -- this is the number that actually matters for
            // hrecord: how fast would a whole-screen capture be through
            // this pointer, if the mismatch check above says it's real.
            size_t fullBytes = (size_t)(screenFrame.Height() + 1) * info.bytes_per_row;
            const uint8_t* fullStart = (const uint8_t*)info.bits
                - (size_t)windowScreenY * info.bytes_per_row
                - (size_t)windowScreenX * bytesPerPixel;
            g_inRiskyRead = 1;
            double fullDesktopMs = TimeMemcpyReadMs(fullStart, fullBytes, 10);
            g_inRiskyRead = 0;
            printf("    Full-desktop-sized (%zu bytes) raw memcpy from the "
                "computed buffer origin: %.3fms/read (10-read average) -- "
                "compare this against hrecord --logfps's own reported "
                "capture time on the same machine.\n", fullBytes, fullDesktopMs);
        } else {
            g_inRiskyRead = 0;
            printf("    -> CRASHED (signal %d, %s) reading outside this "
                "window's own clip region. This driver does NOT safely "
                "expose a shared whole-screen buffer to windowed "
                "DirectWindow clients -- this approach is a dead end on "
                "this hardware.\n", caughtSignal,
                caughtSignal == SIGSEGV ? "SIGSEGV" : "SIGBUS");
        }

        sigaction(SIGSEGV, &oldSegv, nullptr);
        sigaction(SIGBUS, &oldBus, nullptr);
    } else {
        printf("\n[i] Skipping the whole-desktop-buffer test (pass "
            "--desktop-read-test to run it -- read the warning in this "
            "file's own top comment first).\n");
    }

    printf("\n[*] Done. Closing.\n");
    be_app->PostMessage(B_QUIT_REQUESTED);
    return 0;
}

int main(int argc, char** argv) {
    // Force line buffering regardless of whether stdout is a real TTY --
    // ProbeThreadEntry's prints happen on a spawned thread, and if this
    // process exits abnormally (crash, or app_server force-killing it for
    // being slow to answer DirectConnected()) with stdout fully buffered
    // instead of line buffered, everything since the last flush is lost
    // silently: the shell just gets its prompt back with no error and no
    // indication anything after the last flushed line ever ran.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--desktop-read-test") == 0) {
            g_desktopReadTest = true;
        }
    }

    BApplication app("application/x-vnd.hrecord-directwindow-probe");

    BRect frame(200, 150, 200 + 319, 150 + 239);
    ProbeWindow* window = new ProbeWindow(frame);

    thread_id probeThread = spawn_thread(ProbeThreadEntry, "probe_thread",
        B_NORMAL_PRIORITY, window);
    resume_thread(probeThread);

    app.Run();

    status_t result;
    wait_for_thread(probeThread, &result);
    return 0;
}
