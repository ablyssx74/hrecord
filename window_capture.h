// Reading one window, for hrecord (recording a window) and hrecord_mixer (the list of windows and the preview).
#ifndef HRECORD_WINDOW_CAPTURE_H
#define HRECORD_WINDOW_CAPTURE_H

#include <AppServerLink.h>	// private: the request below
#include <Bitmap.h>
#include <InterfaceDefs.h>
#include <OS.h>
#include <Rect.h>
#include <Screen.h>
#include <WindowInfo.h>		// private: get_window_info()

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <vector>


// RenkuOS's is_composited_desktop() is looked up by name: Haiku's libbe does not have it.
static inline bool
IsCompositedDesktop()
{
	typedef bool (*Function)();
	static Function function = (Function)dlsym(RTLD_DEFAULT, "is_composited_desktop");
	return function != NULL && function();
}


// The part of the screen a window covers, its border and tab with it. (The tab is only as wide as its title, so what is beside it
// belongs to whatever is behind.)
static inline BRect
WindowOuterRect(const client_window_info& info)
{
	const float border = ceilf(info.border_size);
	const float tab = ceilf(info.tab_height);
	return BRect(info.window_left - border, info.window_top - border - tab, info.window_right + border,
		info.window_bottom + border);
}


// The picture of one window, straight from the compositor's buffer for it, also while other windows cover it. Only a composited
// desktop whose app_server knows the request has this (AS_READ_WINDOW_PIXELS, 'RWpx'); Available() turns false for good once
// app_server says that it does not.
class WindowPicture {
public:
	WindowPicture()
		:
		fArea(-1), fBits(NULL), fMaxWidth(0), fMaxHeight(0), fWidth(0), fHeight(0), fUnknown(false)
	{
	}

	~WindowPicture()
	{
		if (fArea >= 0)
			delete_area(fArea);
	}

	bool Available() const { return !fUnknown && IsCompositedDesktop(); }

	// Reads the window, scaled down (never up) to fit maxWidth x maxHeight. When this fails, the last picture stays as it was.
	status_t Read(int32 token, int32 maxWidth, int32 maxHeight)
	{
		if (!Available() || maxWidth < 1 || maxHeight < 1 || maxWidth > 8192 || maxHeight > 8192)
			return B_NOT_SUPPORTED;
		if (fArea < 0 || maxWidth != fMaxWidth || maxHeight != fMaxHeight) {
			if (fArea >= 0)
				delete_area(fArea);
			fWidth = fHeight = 0;
			const size_t size = ((size_t)maxWidth * 4 * maxHeight + B_PAGE_SIZE - 1) & ~((size_t)B_PAGE_SIZE - 1);
			fArea = create_area("window picture", (void**)&fBits, B_ANY_ADDRESS, size, B_NO_LOCK,
				B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
			if (fArea < 0)
				return fArea;
			fMaxWidth = maxWidth;
			fMaxHeight = maxHeight;
		}

		BPrivate::AppServerLink link;
		link.StartMessage('RWpx');
		link.Attach<int32>(token);
		link.Attach<area_id>(fArea);
		link.Attach<int32>(maxWidth * 4);
		link.Attach<int32>(maxWidth);
		link.Attach<int32>(maxHeight);
		int32 status = B_ERROR;
		if (link.FlushWithReply(status) != B_OK)
			return B_ERROR;
		if (status == B_ERROR)
			fUnknown = true;		// the answer of an app_server that does not know the request
		if (status != B_OK)
			return status;
		int32 width = 0, height = 0;
		link.Read<int32>(&width);
		link.Read<int32>(&height);
		if (width < 1 || height < 1 || width > maxWidth || height > maxHeight)
			return B_BAD_VALUE;
		fWidth = width;
		fHeight = height;
		return B_OK;
	}

	const uint8* Bits() const { return fWidth > 0 ? fBits : NULL; }
	int32 BytesPerRow() const { return fMaxWidth * 4; }
	int32 Width() const { return fWidth; }
	int32 Height() const { return fHeight; }

private:
	area_id	fArea;
	uint8*	fBits;
	int32	fMaxWidth, fMaxHeight;
	int32	fWidth, fHeight;
	bool	fUnknown;
};


// Where on the screen the top left corner of such a picture lies, which is needed to put the mouse pointer into it. The
// compositor's picture has the decorator with it, with room around the window that differs from one decorator to the next and
// that app_server does not tell. The picture covers the window, so only a few places are possible: the one where it agrees
// with what the screen shows is taken (looked for again until enough of the window can be seen).
class PictureOrigin {
public:
	BPoint Get(const client_window_info& info, const uint8* bits, int32 bytesPerRow, int32 width, int32 height)
	{
		const int32 extraX = width - (info.window_right - info.window_left + 1);
		const int32 extraY = height - (info.window_bottom - info.window_top + 1);
		if (info.server_token != fToken || extraX != fExtraX || extraY != fExtraY) {
			fToken = info.server_token;
			fExtraX = extraX;
			fExtraY = extraY;
			fFound = false;
			fNextTry = 0;
			// until it is found: the same room left, right and below, the rest above
			fX = -extraX / 2;
			fY = -std::max((int32)0, extraY - extraX / 2);
		}
		if (!fFound && system_time() >= fNextTry) {
			fNextTry = system_time() + 1000000;
			_Find(info, bits, bytesPerRow, width, height);
		}
		return BPoint(info.window_left + fX, info.window_top + fY);
	}

private:
	void _Find(const client_window_info& info, const uint8* bits, int32 bytesPerRow, int32 width, int32 height)
	{
		// the places where the picture covers the whole window, relative to the window
		const int32 fromX = std::min((int32)0, -fExtraX), toX = std::max((int32)0, -fExtraX);
		const int32 fromY = std::min((int32)0, -fExtraY), toY = std::max((int32)0, -fExtraY);
		if (fromX == toX && fromY == toY) {
			fX = fromX;
			fY = fromY;
			fFound = true;
			return;
		}
		if (toX - fromX > 200 || toY - fromY > 200)
			return;

		// points of the picture with an edge in them, spread over it
		std::vector<BPoint> points;
		for (int32 y = 1; y < height - 1; y += 2) {
			const uint8* row = bits + (size_t)y * bytesPerRow;
			for (int32 x = 1; x < width - 1; x++) {
				if (abs(row[x * 4 + 1] - row[x * 4 - 3]) > 30)
					points.push_back(BPoint(x, y));
			}
		}
		const size_t step = points.size() / 1500 + 1;

		BScreen screen;
		BRect area(info.window_left + fromX, info.window_top + fromY, info.window_left + toX + width - 1,
			info.window_top + toY + height - 1);
		area = area & screen.Frame();
		BBitmap* shot = NULL;
		if (!area.IsValid() || screen.GetBitmap(&shot, false, &area) != B_OK || shot == NULL)
			return;
		const int32 shotWidth = std::min(shot->Bounds().IntegerWidth(), area.IntegerWidth()) + 1;
		const int32 shotHeight = std::min(shot->Bounds().IntegerHeight(), area.IntegerHeight()) + 1;
		int32 best = 0, tested = 0;
		for (int32 dy = fromY; dy <= toY; dy++) {
			for (int32 dx = fromX; dx <= toX; dx++) {
				const int32 offsetX = info.window_left + dx - (int32)area.left;
				const int32 offsetY = info.window_top + dy - (int32)area.top;
				int32 same = 0, seen = 0;
				for (size_t i = 0; i < points.size(); i += step) {
					const int32 x = (int32)points[i].x, y = (int32)points[i].y;
					const int32 sx = x + offsetX, sy = y + offsetY;
					if (sx < 0 || sy < 0 || sx >= shotWidth || sy >= shotHeight)
						continue;
					seen++;
					const uint8* a = bits + (size_t)y * bytesPerRow + x * 4;
					const uint8* b = (const uint8*)shot->Bits() + (size_t)sy * shot->BytesPerRow() + sx * 4;
					if (abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) < 36)
						same++;
				}
				if (same > best) {
					best = same;
					tested = seen;
					fCandidate = BPoint(dx, dy);
				}
			}
		}
		delete shot;
		// (a window that is mostly covered agrees nowhere)
		if (best >= 40 && best * 3 >= tested) {
			fX = (int32)fCandidate.x;
			fY = (int32)fCandidate.y;
			fFound = true;
		}
	}

	int32		fToken = -1;
	int32		fExtraX = 0, fExtraY = 0;
	int32		fX = 0, fY = 0;
	bool		fFound = false;
	bigtime_t	fNextTry = 0;
	BPoint		fCandidate;
};


#endif	// HRECORD_WINDOW_CAPTURE_H
