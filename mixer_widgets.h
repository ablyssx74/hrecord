// The look of hrecord_mixer: dark docks in the manner of OBS Studio, and the controls that go in them. They are drawn here, not by
// the system's control look, so that they read well on the dark background with every look.
#ifndef HRECORD_MIXER_WIDGETS_H
#define HRECORD_MIXER_WIDGETS_H

#include <Bitmap.h>
#include <Control.h>
#include <LayoutBuilder.h>
#include <LayoutUtils.h>
#include <ListItem.h>
#include <ListView.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string>


namespace theme {
	static const rgb_color kWindow		= { 29, 31, 38, 255 };
	static const rgb_color kDock		= { 39, 42, 51, 255 };
	static const rgb_color kHeader		= { 49, 53, 65, 255 };
	static const rgb_color kInset		= { 22, 23, 29, 255 };
	static const rgb_color kButton		= { 60, 64, 77, 255 };
	static const rgb_color kButtonHover	= { 74, 79, 95, 255 };
	static const rgb_color kAccent		= { 56, 92, 200, 255 };
	static const rgb_color kText		= { 232, 233, 237, 255 };
	static const rgb_color kDimText		= { 140, 144, 156, 255 };
	static const rgb_color kRecord		= { 190, 57, 45, 255 };
	static const rgb_color kSolo		= { 190, 150, 30, 255 };
}


static inline rgb_color
Lighter(rgb_color color, int by)
{
	color.red = std::min(255, std::max(0, color.red + by));
	color.green = std::min(255, std::max(0, color.green + by));
	color.blue = std::min(255, std::max(0, color.blue + by));
	return color;
}


// Draws a line of text in a rectangle: centered or from the left, cut short with an ellipsis when it does not fit.
static inline void
DrawText(BView* view, const char* text, BRect rect, rgb_color color, rgb_color background, bool centered)
{
	BString shown(text);
	view->TruncateString(&shown, B_TRUNCATE_END, rect.Width());
	font_height height;
	view->GetFontHeight(&height);
	float x = rect.left;
	if (centered)
		x += floorf((rect.Width() - view->StringWidth(shown.String())) / 2);
	const float y = rect.top + floorf((rect.Height() + 1 - ceilf(height.ascent) - ceilf(height.descent)) / 2)
		+ ceilf(height.ascent);
	view->SetHighColor(color);
	view->SetLowColor(background);
	view->SetDrawingMode(B_OP_OVER);
	view->DrawString(shown.String(), BPoint(x, y));
	view->SetDrawingMode(B_OP_COPY);
}


static inline float
LineHeight(const BFont* font)
{
	font_height height;
	font->GetHeight(&height);
	return ceilf(height.ascent) + ceilf(height.descent) + ceilf(height.leading);
}


// A plain box of one color.
class Panel : public BView {
public:
	Panel(const char* name, rgb_color color)
		:
		BView(name, B_WILL_DRAW)
	{
		SetViewColor(color);
		SetLowColor(color);
	}
};


// A line of text. fixedWidth > 0: that wide at most (the text is cut short); 0: as wide as its text.
class Label : public BView {
public:
	Label(const char* name, const char* text, rgb_color background, rgb_color color = theme::kText, bool centered = false,
		float fixedWidth = 0)
		:
		BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fText(text), fBackground(background), fColor(color), fCentered(centered), fFixedWidth(fixedWidth)
	{
		SetViewColor(background);
	}

	void SetText(const char* text)
	{
		if (fText == text)
			return;
		const bool resize = fFixedWidth == 0;
		fText = text;
		if (resize)
			InvalidateLayout();
		Invalidate();
	}

	const char* Text() const { return fText.String(); }

	BSize MinSize() { return BSize(fFixedWidth != 0 ? std::min(fFixedWidth, 40.0f) : 20, Height()); }
	BSize PreferredSize() { return BSize(Width(), Height()); }
	BSize MaxSize()
	{
		return BSize(fFixedWidth > 0 ? fFixedWidth : B_SIZE_UNLIMITED, Height());
	}

	void Draw(BRect) { DrawText(this, fText.String(), Bounds(), fColor, fBackground, fCentered); }

private:
	float Height()
	{
		BFont font;
		GetFont(&font);
		return LineHeight(&font) + 2;
	}
	float Width() { return fFixedWidth > 0 ? fFixedWidth : ceilf(StringWidth(fText.String())) + 2; }

	BString		fText;
	rgb_color	fBackground, fColor;
	bool		fCentered;
	float		fFixedWidth;
};


// A push button, flat, with a color of its own.
class FlatButton : public BControl {
public:
	FlatButton(const char* name, const char* label, BMessage* message, rgb_color background,
		rgb_color color = theme::kButton)
		:
		BControl(name, label, message, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE),
		fBackground(background), fColor(color), fInside(false), fPressed(false)
	{
		SetViewColor(background);
	}

	void SetColor(rgb_color color)
	{
		fColor = color;
		Invalidate();
	}

	void SetLabel(const char* label)
	{
		BControl::SetLabel(label);
		InvalidateLayout();
		Invalidate();
	}

	BSize MinSize()
	{
		BFont font;
		GetFont(&font);
		return BSize(ceilf(StringWidth(Label())) + 24, LineHeight(&font) + 12);
	}
	BSize PreferredSize() { return MinSize(); }
	BSize MaxSize() { return BLayoutUtils::ComposeSize(ExplicitMaxSize(), BSize(B_SIZE_UNLIMITED, MinSize().height)); }

	void Draw(BRect)
	{
		rgb_color color = fColor;
		if (!IsEnabled())
			color = Lighter(theme::kDock, 8);
		else if (fPressed && fInside)
			color = Lighter(fColor, -18);
		else if (fInside)
			color = Lighter(fColor, 16);
		SetHighColor(color);
		FillRoundRect(Bounds(), 4, 4);
		if (IsFocus() && Window() != NULL && Window()->IsActive()) {
			SetHighColor(theme::kDimText);
			StrokeRoundRect(Bounds(), 4, 4);
		}
		DrawText(this, Label(), Bounds().InsetByCopy(6, 0), IsEnabled() ? theme::kText : theme::kDimText, color, true);
	}

	void MouseDown(BPoint)
	{
		if (!IsEnabled())
			return;
		fPressed = fInside = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Invalidate();
	}

	void MouseMoved(BPoint where, uint32 transit, const BMessage*)
	{
		const bool inside = transit != B_EXITED_VIEW && transit != B_OUTSIDE_VIEW && Bounds().Contains(where);
		if (inside != fInside) {
			fInside = inside;
			Invalidate();
		}
	}

	void MouseUp(BPoint where)
	{
		if (!fPressed)
			return;
		fPressed = false;
		Invalidate();
		if (IsEnabled() && Bounds().Contains(where))
			Invoke();
	}

private:
	rgb_color	fBackground, fColor;
	bool		fInside, fPressed;
};


// An on/off control: a check box with its label, or (pill) a small button that lights up in its color while it is on.
class FlatCheck : public BControl {
public:
	FlatCheck(const char* name, const char* label, BMessage* message, rgb_color background, bool pill = false,
		rgb_color onColor = theme::kAccent)
		:
		BControl(name, label, message, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE),
		fBackground(background), fOnColor(onColor), fPill(pill)
	{
		SetViewColor(background);
	}

	void SetLabel(const char* label)
	{
		BControl::SetLabel(label);
		InvalidateLayout();
		Invalidate();
	}

	BSize MinSize()
	{
		BFont font;
		GetFont(&font);
		const float height = LineHeight(&font) + (fPill ? 6 : 3);
		return BSize(ceilf(StringWidth(Label())) + (fPill ? 16 : height + 8), height);
	}
	BSize PreferredSize() { return MinSize(); }
	BSize MaxSize() { return BLayoutUtils::ComposeSize(ExplicitMaxSize(), BSize(B_SIZE_UNLIMITED, MinSize().height)); }

	void Draw(BRect)
	{
		const bool on = Value() == B_CONTROL_ON;
		const rgb_color text = IsEnabled() ? theme::kText : theme::kDimText;
		if (fPill) {
			rgb_color color = on ? fOnColor : theme::kButton;
			if (!IsEnabled())
				color = Lighter(theme::kDock, 8);
			SetHighColor(color);
			FillRoundRect(Bounds(), 4, 4);
			DrawText(this, Label(), Bounds().InsetByCopy(3, 0), text, color, true);
			return;
		}
		SetHighColor(fBackground);
		FillRect(Bounds());
		const float side = floorf(Bounds().Height()) - 4;
		BRect box(1, floorf((Bounds().Height() - side) / 2), 1 + side, floorf((Bounds().Height() - side) / 2) + side);
		const rgb_color fill = on ? (IsEnabled() ? fOnColor : theme::kButton) : theme::kInset;
		SetHighColor(fill);
		FillRoundRect(box, 3, 3);
		SetHighColor(IsFocus() && Window() != NULL && Window()->IsActive() ? theme::kText : theme::kButtonHover);
		StrokeRoundRect(box, 3, 3);
		if (on) {
			SetHighColor(text);
			SetPenSize(2);
			StrokeLine(BPoint(box.left + side * 0.22f, box.top + side * 0.52f),
				BPoint(box.left + side * 0.43f, box.top + side * 0.72f));
			StrokeLine(BPoint(box.left + side * 0.43f, box.top + side * 0.72f),
				BPoint(box.left + side * 0.80f, box.top + side * 0.28f));
			SetPenSize(1);
		}
		BRect label = Bounds();
		label.left = box.right + 7;
		DrawText(this, Label(), label, text, fBackground, false);
	}

	void MouseDown(BPoint)
	{
		if (!IsEnabled())
			return;
		SetValue(Value() == B_CONTROL_ON ? B_CONTROL_OFF : B_CONTROL_ON);
		Invoke();
	}

	void KeyDown(const char* bytes, int32 count)
	{
		if (IsEnabled() && (bytes[0] == B_SPACE || bytes[0] == B_ENTER))
			MouseDown(B_ORIGIN);
		else
			BControl::KeyDown(bytes, count);
	}

private:
	rgb_color	fBackground, fOnColor;
	bool		fPill;
};


// A choice from a menu: its title, then a button with the marked item, which opens the menu. The menu stays the caller's to
// fill and to aim; the control owns it.
class FlatMenu : public BControl {
public:
	FlatMenu(const char* name, const char* title, BPopUpMenu* menu, rgb_color background)
		:
		BControl(name, title, NULL, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fMenu(menu), fBackground(background)
	{
		SetViewColor(background);
	}

	~FlatMenu() { delete fMenu; }

	BSize MinSize()
	{
		BFont font;
		GetFont(&font);
		return BSize(TitleWidth() + 170, LineHeight(&font) + 10);
	}
	BSize PreferredSize() { return BSize(TitleWidth() + 230, MinSize().height); }
	BSize MaxSize() { return BSize(TitleWidth() + 320, MinSize().height); }

	void Draw(BRect)
	{
		SetHighColor(fBackground);
		FillRect(Bounds());
		const rgb_color text = IsEnabled() ? theme::kText : theme::kDimText;
		BRect title = Bounds();
		title.right = TitleWidth();
		DrawText(this, Label(), title, text, fBackground, false);
		const BRect box = Box();
		const rgb_color color = IsEnabled() ? theme::kButton : Lighter(theme::kDock, 8);
		SetHighColor(color);
		FillRoundRect(box, 4, 4);
		BMenuItem* marked = fMenu->FindMarked();
		BRect inside = box.InsetByCopy(8, 0);
		inside.right -= 14;
		DrawText(this, marked != NULL ? marked->Label() : "", inside, text, color, false);
		// the arrow that says there is a menu
		const float x = box.right - 14, y = floorf((box.top + box.bottom) / 2) - 2;
		SetHighColor(text);
		FillTriangle(BPoint(x, y), BPoint(x + 8, y), BPoint(x + 4, y + 5));
	}

	void MouseDown(BPoint)
	{
		if (!IsEnabled() || fMenu->CountItems() == 0)
			return;
		const BRect box = ConvertToScreen(Box());
		fMenu->Go(box.LeftBottom() + BPoint(0, 1), true, true, box, true);
	}

private:
	float TitleWidth() { return ceilf(StringWidth(Label())) + 8; }
	BRect Box()
	{
		BRect box = Bounds();
		box.left = TitleWidth();
		return box;
	}

	BPopUpMenu*	fMenu;
	rgb_color	fBackground;
};


// A row of choices of which one is taken.
class Segments : public BControl {
public:
	Segments(const char* name, const char* const* choices, int count, int value, rgb_color background)
		:
		BControl(name, NULL, NULL, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fChoices(choices), fCount(count)
	{
		SetViewColor(background);
		SetValue(value);
	}

	BSize MinSize()
	{
		BFont font;
		GetFont(&font);
		float widest = 0;
		for (int i = 0; i < fCount; i++)
			widest = std::max(widest, StringWidth(fChoices[i]));
		return BSize((ceilf(widest) + 14) * fCount, LineHeight(&font) + 8);
	}
	BSize PreferredSize() { return MinSize(); }
	BSize MaxSize() { return BSize(B_SIZE_UNLIMITED, MinSize().height); }

	void Draw(BRect)
	{
		const float width = (Bounds().Width() + 1) / fCount;
		for (int i = 0; i < fCount; i++) {
			BRect part(floorf(i * width), 0, floorf((i + 1) * width) - 2, Bounds().bottom);
			rgb_color color = Value() == i ? theme::kAccent : theme::kButton;
			if (!IsEnabled())
				color = Value() == i ? theme::kButtonHover : Lighter(theme::kDock, 8);
			SetHighColor(color);
			FillRoundRect(part, 3, 3);
			DrawText(this, fChoices[i], part, IsEnabled() ? theme::kText : theme::kDimText, color, true);
		}
	}

	void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		const int index = std::min(fCount - 1, std::max(0, (int)(where.x / ((Bounds().Width() + 1) / fCount))));
		if (index != Value()) {
			SetValue(index);
			Invoke();
		}
	}

private:
	const char* const*	fChoices;
	int					fCount;
};


// A fader: drag it, or turn the mouse wheel over it; a right click puts it back where it started. It sends its message with every
// change. Values are whole numbers: half decibels, percent of balance, or milliseconds.
class Fader : public BControl {
public:
	Fader(const char* name, BMessage* message, int32 low, int32 high, int32 reset, int32 wheelStep, bool vertical,
		rgb_color background)
		:
		BControl(name, NULL, message, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fLow(low), fHigh(high), fReset(reset), fWheelStep(wheelStep), fVertical(vertical), fBackground(background),
		fDragging(false)
	{
		SetViewColor(background);
		BControl::SetValue(reset);
		SetExplicitMinSize(vertical ? BSize(24, 110) : BSize(90, 16));
		SetExplicitPreferredSize(vertical ? BSize(24, 180) : BSize(120, 16));
		SetExplicitMaxSize(vertical ? BSize(24, B_SIZE_UNLIMITED) : BSize(B_SIZE_UNLIMITED, 16));
	}

	void SetValue(int32 value)
	{
		BControl::SetValue(std::min(fHigh, std::max(fLow, value)));
	}

	void Draw(BRect)
	{
		SetHighColor(fBackground);
		FillRect(Bounds());
		const BRect bounds = Bounds();
		const float fraction = (float)(Value() - fLow) / (fHigh - fLow);
		const rgb_color filled = IsEnabled() ? theme::kAccent : theme::kButton;
		const rgb_color knob = IsEnabled() ? theme::kText : theme::kDimText;
		if (fVertical) {
			const float middle = floorf(bounds.Width() / 2);
			const float top = kKnob / 2, bottom = bounds.bottom - kKnob / 2;
			const float at = floorf(bottom - fraction * (bottom - top));
			SetHighColor(theme::kInset);
			FillRoundRect(BRect(middle - 3, top, middle + 3, bottom), 3, 3);
			SetHighColor(filled);
			FillRoundRect(BRect(middle - 3, at, middle + 3, bottom), 3, 3);
			SetHighColor(knob);
			FillRoundRect(BRect(middle - 10, at - kKnob / 2, middle + 10, at + kKnob / 2), 3, 3);
			SetHighColor(theme::kButton);
			StrokeLine(BPoint(middle - 7, at), BPoint(middle + 7, at));
		} else {
			const float middle = floorf(bounds.Height() / 2);
			const float left = kKnob / 2, right = bounds.right - kKnob / 2;
			const float at = floorf(left + fraction * (right - left));
			SetHighColor(theme::kInset);
			FillRoundRect(BRect(left, middle - 2, right, middle + 2), 2, 2);
			// a balance control is filled from its middle
			const float from = fLow < 0 && fReset == 0 && fHigh == -fLow ? floorf((left + right) / 2) : left;
			SetHighColor(filled);
			FillRect(BRect(std::min(from, at), middle - 2, std::max(from, at), middle + 2));
			SetHighColor(knob);
			FillRoundRect(BRect(at - kKnob / 2, middle - 7, at + kKnob / 2, middle + 7), 3, 3);
		}
	}

	void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		int32 buttons = 0;
		if (Window() != NULL && Window()->CurrentMessage() != NULL)
			Window()->CurrentMessage()->FindInt32("buttons", &buttons);
		if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
			Change(fReset);
			return;
		}
		fDragging = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Follow(where);
	}

	void MouseMoved(BPoint where, uint32, const BMessage*)
	{
		if (fDragging)
			Follow(where);
	}

	void MouseUp(BPoint) { fDragging = false; }

	void MessageReceived(BMessage* message)
	{
		if (message->what != B_MOUSE_WHEEL_CHANGED) {
			BControl::MessageReceived(message);
			return;
		}
		float deltaX = 0.0f, deltaY = 0.0f;
		message->FindFloat("be:wheel_delta_x", &deltaX);
		message->FindFloat("be:wheel_delta_y", &deltaY);
		const float delta = deltaY != 0.0f ? -deltaY : deltaX;		// wheel down is a positive deltaY
		if (delta == 0.0f || !IsEnabled())
			return;
		const int32 notches = std::max((int32)1, (int32)(fabsf(delta) + 0.5f));
		Change(Value() + (delta > 0.0f ? notches : -notches) * fWheelStep);
	}

private:
	static const int kKnob = 10;

	void Follow(BPoint where)
	{
		float fraction;
		if (fVertical)
			fraction = (Bounds().bottom - kKnob / 2 - where.y) / (Bounds().Height() - kKnob);
		else
			fraction = (where.x - kKnob / 2) / (Bounds().Width() - kKnob);
		Change(fLow + (int32)lroundf(fraction * (fHigh - fLow)));
	}

	void Change(int32 value)
	{
		value = std::min(fHigh, std::max(fLow, value));
		if (value == Value())
			return;
		SetValue(value);
		if (fText)
			fText(this);
		Invoke();
	}

public:
	// called when the user changes the value (to keep a text beside it)
	void (*fText)(Fader*) = NULL;

private:
	int32		fLow, fHigh, fReset, fWheelStep;
	bool		fVertical;
	rgb_color	fBackground;
	bool		fDragging;
};


// A dock: a titled box.
static inline BView*
MakeDock(const char* title, BView* content)
{
	Panel* dock = new Panel(title, theme::kDock);
	Panel* header = new Panel("header", theme::kHeader);
	BLayoutBuilder::Group<>(header, B_HORIZONTAL, 0)
		.SetInsets(8, 3, 8, 3)
		.Add(new Label("title", title, theme::kHeader));
	BFont font;
	header->GetFont(&font);
	header->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, LineHeight(&font) + 8));
	BLayoutBuilder::Group<>(dock, B_VERTICAL, 0)
		.Add(header, 0.0f)
		.Add(content, 1.0f);
	return dock;
}


// One line of the list of sources.
class SourceItem : public BListItem {
public:
	SourceItem(int32 source, const char* text)
		:
		fSource(source), fText(text)
	{
	}

	int32 Source() const { return fSource; }
	const BString& Text() const { return fText; }

	void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		SetHeight(LineHeight(font) + 8);
	}

	void DrawItem(BView* owner, BRect frame, bool)
	{
		const rgb_color background = IsSelected() ? theme::kAccent : theme::kInset;
		owner->SetHighColor(background);
		owner->FillRect(frame);
		DrawText(owner, fText.String(), frame.InsetByCopy(8, 0), IsEnabled() ? theme::kText : theme::kDimText, background,
			false);
		owner->SetLowColor(theme::kInset);
	}

private:
	int32	fSource;
	BString	fText;
};


// The picture of what a recording will show. It owns the bitmaps it is given.
class PreviewView : public BView {
public:
	PreviewView()
		:
		BView("preview", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fBitmap(NULL)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetExplicitMinSize(BSize(320, 150));
		SetExplicitPreferredSize(BSize(800, 420));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	}

	~PreviewView() { delete fBitmap; }

	// NULL: no picture. `note` is written over the picture (or in its place).
	void SetPicture(BBitmap* bitmap, const char* note)
	{
		delete fBitmap;
		fBitmap = bitmap;
		fNote = note;
		Invalidate();
	}

	void SetNote(const char* note)
	{
		if (fNote == note)
			return;
		fNote = note;
		Invalidate();
	}

	void Draw(BRect)
	{
		const BRect bounds = Bounds();
		BRect picture;
		if (fBitmap != NULL) {
			const float width = fBitmap->Bounds().Width() + 1, height = fBitmap->Bounds().Height() + 1;
			const float scale = std::min((bounds.Width() + 1) / width, (bounds.Height() + 1) / height);
			const float w = floorf(width * scale), h = floorf(height * scale);
			picture.left = floorf((bounds.Width() + 1 - w) / 2);
			picture.top = floorf((bounds.Height() + 1 - h) / 2);
			picture.right = picture.left + w - 1;
			picture.bottom = picture.top + h - 1;
		}
		SetHighColor(theme::kInset);
		if (!picture.IsValid())
			FillRect(bounds);
		else {
			// around the picture only, so that it does not flicker
			FillRect(BRect(0, 0, bounds.right, picture.top - 1));
			FillRect(BRect(0, picture.bottom + 1, bounds.right, bounds.bottom));
			FillRect(BRect(0, picture.top, picture.left - 1, picture.bottom));
			FillRect(BRect(picture.right + 1, picture.top, bounds.right, picture.bottom));
			DrawBitmap(fBitmap, fBitmap->Bounds(), picture, B_FILTER_BITMAP_BILINEAR);
		}
		if (fNote.Length() > 0) {
			BRect box = bounds;
			if (picture.IsValid()) {
				box.top = box.bottom - 26;
				SetHighColor(theme::kInset);
				FillRect(box);
			}
			DrawText(this, fNote.String(), box.InsetByCopy(8, 0), theme::kDimText, theme::kInset, true);
		}
	}

private:
	BBitmap*	fBitmap;
	BString		fNote;
};


#endif	// HRECORD_MIXER_WIDGETS_H
