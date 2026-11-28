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
#include <atomic>
#include <mutex>
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


// Takes the pointer out of pictures of the screen, for a recording without it on a video driver that draws it into the
// screen's own memory. app_server hands out no picture of what lies under it, so those pixels are filled in: with what was
// there before while their surroundings stay as they were, else from the pixels beside them. Pixels are only touched where
// the cursor image is actually found, so a screen without the pointer in it stays as it is. A thread of it asks app_server
// where the pointer is: Stop() ends it, which must happen while the program's BApplication is still there.
class PointerEraser {
public:
	~PointerEraser() { Stop(); }

	// Reads a part of the screen (left, top up to but not right, bottom) with "read" into "bits", a picture of the whole
	// screen, and takes the pointer out of what was read.
	template<typename Read>
	void Around(uint8* bits, int32 bytesPerRow, int32 width, int32 height, int32 left, int32 top, int32 right,
		int32 bottom, const Read& read)
	{
		_Start();
		// (a part the pointer has not been anywhere near is only read: most parts, and asking where it is takes time)
		if (!_PointerWasNear(left, top, right, bottom)) {
			read();
			return;
		}
		const int32 rowBytes = (right - left) * 4;
		fBefore.resize((size_t)rowBytes * (bottom - top));
		for (int32 y = top; y < bottom; y++) {
			memcpy(&fBefore[(size_t)(y - top) * rowBytes], bits + (size_t)y * bytesPerRow + (size_t)left * 4, rowBytes);
		}
		const bigtime_t began = system_time();
		read();
		_Erase(bits, bytesPerRow, width, height, left, top, right, bottom, began, &fBefore[0], 0, 0);
	}

	// The same for a picture that has just been read, which shows the screen from "origin" on, with nothing known about
	// what was there before.
	void Erase(uint8* bits, int32 bytesPerRow, int32 width, int32 height, BPoint origin)
	{
		_Start();
		_Erase(bits, bytesPerRow, width, height, 0, 0, width, height, system_time(), NULL, (int32)origin.x,
			(int32)origin.y);
	}

	// from the next read on, what was there before is not to be trusted (it may have the pointer in it)
	void Forget() { fTrustAfter = system_time() + 500000; }

	bool Found() const { return fFound; }

	void Stop()
	{
		if (fThread < 0)
			return;
		fQuit = true;
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
		fQuit = false;
		for (size_t i = 0; i < fShapes.size(); i++)
			delete fShapes[i].image;
		fShapes.clear();
		fSamples.clear();
	}

private:
	struct Shape {
		BBitmap*	image;
		BPoint		hotSpot;
	};
	struct Sample {
		bigtime_t	when;
		int32		x, y;
	};
	struct Placed {
		int32		left, top, shape;
	};

	// where the pointer has been lately is noted all the time: it moves while the screen is read
	void _Start()
	{
		if (fThread >= 0)
			return;
		_Look(true);
		Forget();
		fThread = spawn_thread(_Watch, "pointer watcher", B_NORMAL_PRIORITY, this);
		if (fThread >= 0)
			resume_thread(fThread);
	}

	static status_t _Watch(void* data)
	{
		PointerEraser* self = (PointerEraser*)data;
		for (int32 turn = 1; !self->fQuit; turn++) {
			self->_Look(turn % 12 == 0);
			snooze(4000);
		}
		return B_OK;
	}

	void _Look(bool shapeToo)
	{
		BBitmap* image = NULL;
		BPoint hotSpot;
		if (shapeToo && (get_mouse_bitmap(&image, &hotSpot) != B_OK || (image != NULL && image->ColorSpace() != B_RGBA32))) {
			delete image;
			image = NULL;
		}
		BPoint where;
		uint32 buttons;
		const bool placed = get_mouse(&where, &buttons) == B_OK;

		std::lock_guard<std::mutex> lock(fLock);
		if (image != NULL) {
			// the latest two shapes are kept: the picture may still have the one before
			if (!fShapes.empty() && fShapes.back().hotSpot == hotSpot && fShapes.back().image->BitsLength() == image->BitsLength()
				&& memcmp(fShapes.back().image->Bits(), image->Bits(), image->BitsLength()) == 0) {
				delete image;
			} else {
				fShapes.push_back({ image, hotSpot });
				if (fShapes.size() > 2) {
					delete fShapes.front().image;
					fShapes.erase(fShapes.begin());
				}
			}
		}
		if (placed) {
			const bigtime_t now = system_time();
			const int32 x = (int32)where.x, y = (int32)where.y;
			// (a pointer that rests is one entry, whose time is that of the last look)
			if (!fSamples.empty() && fSamples.back().x == x && fSamples.back().y == y)
				fSamples.back().when = now;
			else
				fSamples.push_back({ now, x, y });
			while (fSamples.size() > 1 && fSamples.front().when < now - 1000000)
				fSamples.erase(fSamples.begin());
		}
	}

	// "originX, originY": where on the screen the picture's own left top is
	void _Erase(uint8* bits, int32 bytesPerRow, int32 width, int32 height, int32 left, int32 top, int32 right,
		int32 bottom, bigtime_t began, const uint8* before, int32 originX, int32 originY)
	{
		BPoint where;
		uint32 buttons;
		const bool now = get_mouse(&where, &buttons) == B_OK;

		std::lock_guard<std::mutex> lock(fLock);
		// every place the pointer can have been at while this was read
		std::vector<Sample> path;
		for (size_t i = 0; i < fSamples.size(); i++) {
			if (fSamples[i].when >= began - 20000)
				path.push_back(fSamples[i]);
		}
		if (path.empty() && !fSamples.empty())
			path.push_back(fSamples.back());
		if (now)
			path.push_back({ system_time(), (int32)where.x, (int32)where.y });

		fMask.assign((size_t)(right - left) * (bottom - top), 0);
		bool any = false;
		std::vector<Placed> tried;
		for (size_t i = 0; i < path.size(); i++) {
			// (and between two looks, when it moved fast)
			const int32 dx = i > 0 ? path[i].x - path[i - 1].x : 0, dy = i > 0 ? path[i].y - path[i - 1].y : 0;
			const int32 steps = std::max((int32)1, std::min((int32)64, std::max(abs(dx), abs(dy)) / 2));
			for (int32 step = 1; step <= steps; step++) {
				const int32 x = path[i].x - dx + dx * step / steps, y = path[i].y - dy + dy * step / steps;
				for (size_t k = 0; k < fShapes.size(); k++) {
					const Placed place = { x - (int32)fShapes[k].hotSpot.x - originX,
						y - (int32)fShapes[k].hotSpot.y - originY, (int32)k };
					const BRect size = fShapes[k].image->Bounds();
					if (place.left >= right || place.top >= bottom || place.left + size.IntegerWidth() < left
						|| place.top + size.IntegerHeight() < top)
						continue;
					bool done = false;
					for (size_t t = 0; t < tried.size() && !done; t++) {
						done = tried[t].left == place.left && tried[t].top == place.top && tried[t].shape == place.shape;
					}
					if (done)
						continue;
					tried.push_back(place);
					any |= _Mark(fShapes[k].image, place.left, place.top, bits, bytesPerRow, left, top, right, bottom);
				}
			}
		}
		if (!any)
			return;

		const bool trust = before != NULL && system_time() >= fTrustAfter;
		const int32 w = right - left;
		for (int32 y = top; y < bottom; y++) {
			uint32* row = (uint32*)(bits + (size_t)y * bytesPerRow);
			const uint8* marked = &fMask[(size_t)(y - top) * w];
			for (int32 x = left; x < right; x++) {
				if (!marked[x - left])
					continue;
				int32 end = x + 1;
				while (end < right && marked[end - left])
					end++;
				// the pixels beside this run of them (at the screen's edge, the one on the other side)
				const bool hasLeft = x > 0, hasRight = end < width;
				const uint32 a = hasLeft ? row[x - 1] : hasRight ? row[end] : 0;
				const uint32 b = hasRight ? row[end] : a;
				const uint32* old = trust ? (const uint32*)(before + (size_t)(y - top) * w * 4) : NULL;
				// (beside the part that was read, the picture is as it was)
				const bool same = old != NULL && (x - 1 < left || _Near(old[x - 1 - left], a))
					&& (end >= right || _Near(old[end - left], b));
				for (int32 i = x; i < end; i++) {
					if (same) {
						row[i] = old[i - left];
						continue;
					}
					const int32 t = (i - x + 1) * 256 / (end - x + 1);
					uint32 value = 0;
					for (int32 shift = 0; shift < 32; shift += 8) {
						const int32 from = (a >> shift) & 255, to = (b >> shift) & 255;
						value |= (uint32)(from + (to - from) * t / 256) << shift;
					}
					row[i] = value;
				}
				x = end;
				x--;
			}
		}
	}

	// whether the pointer has been within reach of this part of the screen in the last moments
	bool _PointerWasNear(int32 left, int32 top, int32 right, int32 bottom)
	{
		// (more than a cursor's size, and the way a fast pointer goes between two looks)
		const int32 reach = 200;
		const bigtime_t since = system_time() - 100000;
		std::lock_guard<std::mutex> lock(fLock);
		for (size_t i = fSamples.size(); i-- > 0;) {
			const Sample& at = fSamples[i];
			if (at.x >= left - reach && at.x < right + reach && at.y >= top - reach && at.y < bottom + reach)
				return true;
			if (at.when < since)
				break;
		}
		return false;
	}

	static bool _Near(uint32 a, uint32 b)
	{
		for (int32 shift = 0; shift < 24; shift += 8) {
			if (abs((int32)((a >> shift) & 255) - (int32)((b >> shift) & 255)) > 6)
				return false;
		}
		return true;
	}

	// marks the cursor's pixels at this place, if the picture has them there
	bool _Mark(const BBitmap* cursor, int32 atX, int32 atY, const uint8* bits, int32 bytesPerRow, int32 left, int32 top,
		int32 right, int32 bottom)
	{
		const int32 cursorWidth = cursor->Bounds().IntegerWidth() + 1, cursorHeight = cursor->Bounds().IntegerHeight() + 1;
		const int32 x0 = std::max(left, atX), x1 = std::min(right, atX + cursorWidth);
		const int32 y0 = std::max(top, atY), y1 = std::min(bottom, atY + cursorHeight);
		int32 solid = 0, same = 0;
		for (int32 y = y0; y < y1; y++) {
			const uint8* from = (const uint8*)cursor->Bits() + (size_t)(y - atY) * cursor->BytesPerRow() + (x0 - atX) * 4;
			const uint8* at = bits + (size_t)y * bytesPerRow + (size_t)x0 * 4;
			for (int32 x = x0; x < x1; x++, from += 4, at += 4) {
				if (from[3] < 255)
					continue;
				solid++;
				if (abs(at[0] - from[0]) <= 3 && abs(at[1] - from[1]) <= 3 && abs(at[2] - from[2]) <= 3)
					same++;
			}
		}
		// (a part with hardly any solid pixels of the cursor, its edge, cannot be told: taken once the pointer has been seen)
		if (solid >= 6) {
			if (same * 10 < solid * 9)
				return false;
			if (solid >= 12)
				fFound = true;
		} else if (!fFound || same != solid)
			return false;
		bool any = false;
		for (int32 y = y0; y < y1; y++) {
			const uint8* from = (const uint8*)cursor->Bits() + (size_t)(y - atY) * cursor->BytesPerRow() + (x0 - atX) * 4;
			for (int32 x = x0; x < x1; x++, from += 4) {
				if (from[3] > 0) {
					fMask[(size_t)(y - top) * (right - left) + (x - left)] = 1;
					any = true;
				}
			}
		}
		return any;
	}

	thread_id			fThread = -1;
	std::atomic<bool>	fQuit { false };
	std::atomic<bool>	fFound { false };
	std::mutex			fLock;
	std::vector<Shape>	fShapes;
	std::vector<Sample>	fSamples;
	std::vector<uint8>	fMask, fBefore;
	bigtime_t			fTrustAfter = 0;
};


#endif	// HRECORD_CURSOR_OVERLAY_H
