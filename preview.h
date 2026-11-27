// hrecord_mixer's preview: a thread of its own that reads what a recording would show, a few times a second, and hands the
// window small pictures of it.
#ifndef HRECORD_PREVIEW_H
#define HRECORD_PREVIEW_H

#include <Bitmap.h>
#include <Message.h>
#include <Messenger.h>
#include <Screen.h>

#include <algorithm>
#include <atomic>

#include "window_capture.h"


// what is recorded: the app_server token of a window, or one of these
static const int32 kSourceDesktop = -1;
static const int32 kSourceAudioOnly = -2;

static const uint32 kMsgPreview = 'prv!';


// The screen of a composited desktop, followed with app_server's screen observer (as hrecord's own capture does): only what has
// changed is copied.
class ScreenFollower {
public:
	~ScreenFollower() { Stop(); }

	const uint8* Read(int32 width, int32 height)
	{
		if (fFailed || !IsCompositedDesktop())
			return NULL;
		if (fArea >= 0 && (width != fWidth || height != fHeight))
			Stop();
		if (fArea < 0) {
			const size_t size = ((size_t)width * 4 * height + B_PAGE_SIZE - 1) & ~((size_t)B_PAGE_SIZE - 1);
			fArea = create_area("preview screen observer", (void**)&fBits, B_ANY_ADDRESS, size, B_NO_LOCK,
				B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
			if (fArea < 0)
				return NULL;
			fWidth = width;
			fHeight = height;
			BPrivate::AppServerLink link;
			link.StartMessage('RSos');
			int32 code = B_ERROR;
			if (link.FlushWithReply(code) != B_OK || code != B_OK) {
				fFailed = true;
				delete_area(fArea);
				fArea = -1;
				return NULL;
			}
		}
		bool good = false;
		{
			BPrivate::AppServerLink link;
			link.StartMessage('RSor');
			link.Attach<area_id>(fArea);
			link.Attach<int32>(fWidth * 4);
			int32 code = B_ERROR;
			if (link.FlushWithReply(code) == B_OK && code == B_OK) {
				int32 screenWidth = 0;
				link.Read<int32>(&screenWidth);
				good = screenWidth == fWidth;
			}
		}
		if (!good) {
			Stop();
			return NULL;
		}
		return fBits;
	}

	void Stop()
	{
		if (fArea < 0)
			return;
		{
			BPrivate::AppServerLink link;
			link.StartMessage('RSoe');
			int32 code;
			link.FlushWithReply(code);
		}
		delete_area(fArea);
		fArea = -1;
	}

private:
	area_id	fArea = -1;
	uint8*	fBits = NULL;
	int32	fWidth = 0, fHeight = 0;
	bool	fFailed = false;
};


class Preview {
public:
	// set by the window
	std::atomic<int32>	source { kSourceDesktop };
	std::atomic<bool>	enabled { true };
	std::atomic<bool>	recording { false };
	std::atomic<bool>	visible { true };
	std::atomic<int32>	width { 640 };		// the room there is for the picture
	std::atomic<int32>	height { 360 };
	std::atomic<bool>	pending { false };	// a picture is on its way to the window

	void Start(const BMessenger& target)
	{
		fTarget = target;
		fThread = spawn_thread(_Entry, "preview", B_LOW_PRIORITY, this);
		if (fThread >= 0)
			resume_thread(fThread);
	}

	void Stop()
	{
		fQuit = true;
		if (fThread >= 0) {
			status_t result;
			wait_for_thread(fThread, &result);
			fThread = -1;
		}
	}

private:
	static int32 _Entry(void* data)
	{
		((Preview*)data)->_Run();
		return 0;
	}

	// Waits, but not past a quit or a change of source.
	void _Wait(bigtime_t time, int32 shown)
	{
		const bigtime_t until = system_time() + time;
		while (!fQuit && system_time() < until && source.load() == shown)
			snooze(50000);
	}

	// A copy of 32-bit pixels, made smaller to fit the room there is (never larger), four samples to a pixel.
	BBitmap* _Shrink(const uint8* bits, int32 bytesPerRow, int32 sourceWidth, int32 sourceHeight)
	{
		const double scale = std::min(1.0, std::min((double)width.load() / sourceWidth,
			(double)height.load() / sourceHeight));
		const int32 w = std::max((int32)1, (int32)(sourceWidth * scale));
		const int32 h = std::max((int32)1, (int32)(sourceHeight * scale));
		BBitmap* bitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32);
		if (bitmap->InitCheck() != B_OK) {
			delete bitmap;
			return NULL;
		}
		const int64 stepX = ((int64)sourceWidth << 16) / w, stepY = ((int64)sourceHeight << 16) / h;
		for (int32 y = 0; y < h; y++) {
			uint32* to = (uint32*)((uint8*)bitmap->Bits() + (size_t)y * bitmap->BytesPerRow());
			const uint8* first = bits + (size_t)((y * stepY + stepY / 4) >> 16) * bytesPerRow;
			const uint8* second = bits + (size_t)((y * stepY + stepY * 3 / 4) >> 16) * bytesPerRow;
			for (int32 x = 0; x < w; x++) {
				const int32 a = (int32)((x * stepX + stepX / 4) >> 16), b = (int32)((x * stepX + stepX * 3 / 4) >> 16);
				// the average of four pixels, two at a time and byte by byte
				const uint32 p = ((const uint32*)first)[a], q = ((const uint32*)first)[b];
				const uint32 r = ((const uint32*)second)[a], s = ((const uint32*)second)[b];
				const uint32 top = (p & q) + (((p ^ q) >> 1) & 0x7f7f7f7f);
				const uint32 bottom = (r & s) + (((r ^ s) >> 1) & 0x7f7f7f7f);
				to[x] = ((top & bottom) + (((top ^ bottom) >> 1) & 0x7f7f7f7f)) | 0xff000000;
			}
		}
		return bitmap;
	}

	void _Send(BBitmap* bitmap, const char* note)
	{
		BMessage message(kMsgPreview);
		message.AddPointer("bitmap", bitmap);
		message.AddString("note", note);
		message.AddInt32("source", source.load());
		pending = true;
		if (fTarget.SendMessage(&message, (BHandler*)NULL, 200000) != B_OK) {
			pending = false;
			delete bitmap;
		}
	}

	void _Run()
	{
		WindowPicture window;
		ScreenFollower follower;
		while (!fQuit) {
			const int32 shown = source.load();
			if (!enabled || !visible || shown == kSourceAudioOnly || pending) {
				if (!enabled || !visible || shown == kSourceAudioOnly)
					follower.Stop();
				_Wait(100000, shown);
				continue;
			}
			BScreen screen;
			const BRect frame = screen.Frame();
			const int32 screenWidth = frame.IntegerWidth() + 1, screenHeight = frame.IntegerHeight() + 1;
			const bool composited = IsCompositedDesktop();
			bigtime_t rest = recording ? 200000 : 100000;
			BRect part = frame;

			if (shown >= 0) {
				follower.Stop();
				client_window_info* info = get_window_info(shown);
				if (info == NULL) {
					_Send(NULL, "The window has been closed.");
					_Wait(500000, shown);
					continue;
				}
				const bool hidden = info->is_mini || info->show_hide_level > 0
					|| (info->workspaces & (1u << current_workspace())) == 0;
				part = WindowOuterRect(*info) & frame;
				free(info);
				if (hidden) {
					const char* note = "The window is minimized or on another workspace. A recording keeps its last picture.";
					// app_server keeps a small picture of such a window
					if (window.Available() && window.Read(shown, width.load(), height.load()) == B_OK)
						_Send(_Shrink(window.Bits(), window.BytesPerRow(), window.Width(), window.Height()), note);
					else
						_Send(NULL, note);
					_Wait(1000000, shown);
					continue;
				}
				if (window.Available() && window.Read(shown, width.load(), height.load()) == B_OK) {
					_Send(_Shrink(window.Bits(), window.BytesPerRow(), window.Width(), window.Height()), "");
					_Wait(rest, shown);
					continue;
				}
				if (!part.IsValid()) {
					_Send(NULL, "The window is off the screen.");
					_Wait(500000, shown);
					continue;
				}
			} else if (composited) {
				const uint8* bits = follower.Read(screenWidth, screenHeight);
				if (bits != NULL) {
					_Send(_Shrink(bits, screenWidth * 4, screenWidth, screenHeight), "");
					_Wait(rest, shown);
					continue;
				}
			}

			// Reading the screen the ordinary way is slow on some drivers, and a recording needs the time.
			if (recording) {
				_Send(NULL, "The preview rests while recording.");
				_Wait(1000000, shown);
				continue;
			}
			BBitmap* whole = NULL;
			if (screen.GetBitmap(&whole, false, &part) == B_OK && whole != NULL
				&& (whole->ColorSpace() == B_RGB32 || whole->ColorSpace() == B_RGBA32)) {
				const int32 w = std::min(whole->Bounds().IntegerWidth() + 1, part.IntegerWidth() + 1);
				const int32 h = std::min(whole->Bounds().IntegerHeight() + 1, part.IntegerHeight() + 1);
				_Send(_Shrink((const uint8*)whole->Bits(), whole->BytesPerRow(), w, h),
					shown >= 0 ? "Seen from the screen: what covers the window is recorded too." : "");
			} else
				_Send(NULL, "No preview of this screen.");
			delete whole;
			_Wait(1000000, shown);
		}
	}

	BMessenger			fTarget;
	thread_id			fThread = -1;
	std::atomic<bool>	fQuit { false };
};


#endif	// HRECORD_PREVIEW_H
