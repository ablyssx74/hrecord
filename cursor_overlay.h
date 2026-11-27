// The mouse pointer in a recording (hrecord) and in the preview of it (hrecord_mixer). Reading the screen or a window gives
// pictures without the pointer, so it is drawn into them: app_server's cursor image, where the pointer is at that moment.
#ifndef HRECORD_CURSOR_OVERLAY_H
#define HRECORD_CURSOR_OVERLAY_H

#include <Bitmap.h>
#include <InterfaceDefs.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>


class CursorOverlay {
public:
	// Draws the pointer into 32-bit pixels that show the screen from (originX, originY) on, "scale" pixels of them to one of
	// the screen. What was under it is kept until Restore(), for pictures that are used again (only what changes is read anew).
	// On some video drivers app_server draws the pointer into the screen's own memory, and every reading of the screen has it:
	// a picture that has it already is left alone, as is every picture when "draw" is false (it is only looked at then).
	void Draw(uint8* bits, int32 bytesPerRow, int32 width, int32 height, float originX, float originY, float scale = 1.0f,
		bool draw = true)
	{
		fBits = NULL;
		if (bits == NULL || scale <= 0)
			return;
		BPoint where;
		uint32 buttons;
		if (get_mouse(&where, &buttons) != B_OK)
			return;
		BBitmap* cursor = NULL;
		BPoint hotSpot;
		if (get_mouse_bitmap(&cursor, &hotSpot) != B_OK || cursor == NULL)
			return;
		if (cursor->ColorSpace() != B_RGBA32) {
			delete cursor;
			return;
		}
		const int32 cursorWidth = cursor->Bounds().IntegerWidth() + 1;
		const int32 cursorHeight = cursor->Bounds().IntegerHeight() + 1;
		// where the cursor image lies in the picture, and the part of that which is inside it
		const float left = (where.x - hotSpot.x - originX) * scale;
		const float top = (where.y - hotSpot.y - originY) * scale;
		const int32 x0 = std::max((int32)0, (int32)floorf(left));
		const int32 y0 = std::max((int32)0, (int32)floorf(top));
		const int32 x1 = std::min(width, (int32)ceilf(left + cursorWidth * scale));
		const int32 y1 = std::min(height, (int32)ceilf(top + cursorHeight * scale));
		// (a pointer outside the picture, in a recording of one window, is not in it)
		const float pointerX = (where.x - originX) * scale, pointerY = (where.y - originY) * scale;
		if (pointerX < 0 || pointerY < 0 || pointerX >= width || pointerY >= height || x0 >= x1 || y0 >= y1) {
			delete cursor;
			return;
		}

		if (scale == 1.0f && _IsThere(cursor, bits, bytesPerRow, width, height, (int32)floorf(left), (int32)floorf(top))) {
			if (fSeen < kSure)
				fSeen++;
			delete cursor;
			return;
		}
		if (!draw || ScreenHasPointer()) {
			delete cursor;
			return;
		}

		fBits = bits;
		fBytesPerRow = bytesPerRow;
		fLeft = x0;
		fTop = y0;
		fRowBytes = (x1 - x0) * 4;
		fRows = y1 - y0;
		fUnder.resize((size_t)fRowBytes * fRows);
		// in a smaller picture each of its pixels is the average of several of the cursor's
		const int32 samples = std::max((int32)1, std::min((int32)4, (int32)ceilf(1.0f / scale)));
		for (int32 y = y0; y < y1; y++) {
			uint8* pixel = bits + (size_t)y * bytesPerRow + (size_t)x0 * 4;
			memcpy(&fUnder[(size_t)(y - y0) * fRowBytes], pixel, fRowBytes);
			for (int32 x = x0; x < x1; x++, pixel += 4) {
				int32 sum[4] = { 0, 0, 0, 0 };
				for (int32 j = 0; j < samples; j++) {
					const int32 cy = (int32)floorf((y + (j + 0.5f) / samples - top) / scale);
					if (cy < 0 || cy >= cursorHeight)
						continue;
					const uint8* row = (const uint8*)cursor->Bits() + (size_t)cy * cursor->BytesPerRow();
					for (int32 i = 0; i < samples; i++) {
						const int32 cx = (int32)floorf((x + (i + 0.5f) / samples - left) / scale);
						if (cx < 0 || cx >= cursorWidth)
							continue;
						for (int32 c = 0; c < 4; c++)
							sum[c] += row[cx * 4 + c];
					}
				}
				const int32 count = samples * samples;
				const int32 keep = 255 - sum[3] / count;
				if (keep == 255)
					continue;
				// app_server's cursor images have their colors multiplied by their alpha already
				for (int32 c = 0; c < 3; c++)
					pixel[c] = (uint8)std::min((int32)255, ((pixel[c] * keep + 255) >> 8) + sum[c] / count);
			}
		}
		delete cursor;
	}

	// true once pictures have been found to come with the pointer in them
	bool ScreenHasPointer() const { return fSeen >= kSure; }

	void Restore()
	{
		if (fBits == NULL)
			return;
		for (int32 y = 0; y < fRows; y++) {
			memcpy(fBits + (size_t)(fTop + y) * fBytesPerRow + (size_t)fLeft * 4, &fUnder[(size_t)y * fRowBytes],
				fRowBytes);
		}
		fBits = NULL;
	}

private:
	static const int32 kSure = 3;

	// whether the solid pixels of the cursor image are in the picture already, at that place
	static bool _IsThere(const BBitmap* cursor, const uint8* bits, int32 bytesPerRow, int32 width, int32 height, int32 left,
		int32 top)
	{
		int32 solid = 0, same = 0;
		for (int32 y = 0; y <= cursor->Bounds().IntegerHeight(); y++) {
			const uint8* from = (const uint8*)cursor->Bits() + (size_t)y * cursor->BytesPerRow();
			for (int32 x = 0; x <= cursor->Bounds().IntegerWidth(); x++, from += 4) {
				if (from[3] < 255 || left + x < 0 || top + y < 0 || left + x >= width || top + y >= height)
					continue;
				const uint8* at = bits + (size_t)(top + y) * bytesPerRow + (size_t)(left + x) * 4;
				solid++;
				if (abs(at[0] - from[0]) <= 3 && abs(at[1] - from[1]) <= 3 && abs(at[2] - from[2]) <= 3)
					same++;
			}
		}
		return solid >= 12 && same * 20 >= solid * 19;
	}

	int32				fSeen = 0;
	uint8*				fBits = NULL;
	int32				fBytesPerRow = 0, fLeft = 0, fTop = 0, fRowBytes = 0, fRows = 0;
	std::vector<uint8>	fUnder;
};


#endif	// HRECORD_CURSOR_OVERLAY_H
