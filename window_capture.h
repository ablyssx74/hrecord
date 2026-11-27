// Reading one window, for hrecord (recording a window) and hrecord_mixer (the list of windows and the preview).
#ifndef HRECORD_WINDOW_CAPTURE_H
#define HRECORD_WINDOW_CAPTURE_H

#include <AppServerLink.h>	// private: the request below
#include <InterfaceDefs.h>
#include <OS.h>
#include <Rect.h>
#include <WindowInfo.h>		// private: get_window_info()

#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>


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


#endif	// HRECORD_WINDOW_CAPTURE_H
