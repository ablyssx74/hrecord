// hrecord_mixer's item in the Deskbar's tray: its icon (with a red dot while it records) and a menu with Record, Stop, Show,
// Hide and Quit. It runs inside the Deskbar. The mixer adds it when it starts and takes it away when it ends; an item whose
// mixer has gone takes itself away.

#include <Bitmap.h>
#include <Deskbar.h>
#include <IconUtils.h>
#include <MenuItem.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <File.h>
#include <Roster.h>
#include <View.h>
#include <Window.h>
#include <image.h>

#include <stdlib.h>

#include "tray.h"


static const uint32 kTrayTick = 'trTk';


static int32
RemoveItemThread(void*)
{
	BDeskbar deskbar;
	deskbar.RemoveItem(HRECORD_TRAY_NAME);
	return 0;
}


class MixerTrayView : public BView {
public:
	MixerTrayView(BRect frame)
		:
		BView(frame, HRECORD_TRAY_NAME, B_FOLLOW_NONE, B_WILL_DRAW),
		fIcon(NULL), fRunner(NULL), fRecording(false), fHidden(false)
	{
	}

	MixerTrayView(BMessage* archive)
		:
		BView(archive),
		fIcon(NULL), fRunner(NULL), fRecording(false), fHidden(false)
	{
	}

	virtual ~MixerTrayView()
	{
		delete fRunner;
		delete fIcon;
	}

	static BArchivable* Instantiate(BMessage* archive);

	virtual status_t Archive(BMessage* archive, bool deep = true) const
	{
		status_t status = BView::Archive(archive, deep);
		if (status != B_OK)
			return status;
		archive->AddString("add_on", HRECORD_TRAY_SIGNATURE);
		return archive->AddString("class", "MixerTrayView");
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		if (Parent() != NULL)
			SetViewColor(Parent()->ViewColor());
		SetLowColor(ViewColor());
		SetToolTip("hRecord Mixer");
		_LoadIcon();
		BMessage tick(kTrayTick);
		fRunner = new BMessageRunner(BMessenger(this), &tick, 1000000);
		_Ask();
		Invalidate();
	}

	virtual void DetachedFromWindow()
	{
		delete fRunner;
		fRunner = NULL;
		BView::DetachedFromWindow();
	}

	virtual void Draw(BRect)
	{
		if (fIcon != NULL) {
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			DrawBitmap(fIcon, B_ORIGIN);
			SetDrawingMode(B_OP_COPY);
		}
		if (fRecording) {
			const float side = Bounds().Height();
			const float radius = side * 0.24f;
			const BPoint middle(side - radius, side - radius);
			SetHighColor(255, 255, 255);
			FillEllipse(middle, radius + 1, radius + 1);
			SetHighColor(215, 35, 30);
			FillEllipse(middle, radius, radius);
		}
	}

	virtual void MouseDown(BPoint where)
	{
		BPopUpMenu* menu = new BPopUpMenu("hrecord", false, false);
		menu->SetFont(be_plain_font);
		_Add(menu, "Record", kTrayRecord, !fRecording);
		_Add(menu, "Stop", kTrayStop, fRecording);
		menu->AddSeparatorItem();
		_Add(menu, "Show", kTrayShow, true);
		_Add(menu, "Hide", kTrayHide, !fHidden);
		menu->AddSeparatorItem();
		_Add(menu, "Quit", kTrayQuit, true);
		menu->SetAsyncAutoDestruct(true);
		ConvertToScreen(&where);
		menu->Go(where, true, true, true);
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kTrayTick:
				if (!be_roster->IsRunning(HRECORD_MIXER_SIGNATURE)) {
					// (not from this thread: the Deskbar would wait for itself)
					delete fRunner;
					fRunner = NULL;
					thread_id thread = spawn_thread(RemoveItemThread, "remove hrecord tray item", B_NORMAL_PRIORITY, NULL);
					if (thread >= 0)
						resume_thread(thread);
					break;
				}
				_Ask();
				break;
			case kTrayState:
			{
				bool recording = false, hidden = false;
				message->FindBool("recording", &recording);
				message->FindBool("hidden", &hidden);
				fHidden = hidden;
				if (recording != fRecording) {
					fRecording = recording;
					SetToolTip(recording ? "hRecord Mixer: recording" : "hRecord Mixer");
					Invalidate();
				}
				break;
			}
			default:
				BView::MessageReceived(message);
		}
	}

private:
	void _Ask()
	{
		BMessenger mixer(HRECORD_MIXER_SIGNATURE);
		BMessage ask(kTrayAsk);
		if (mixer.IsValid())
			mixer.SendMessage(&ask, this, 0);		// (the Deskbar never waits)
	}

	void _Add(BPopUpMenu* menu, const char* label, uint32 what, bool enabled)
	{
		BMenuItem* item = new BMenuItem(label, new BMessage(what));
		item->SetTarget(BMessenger(HRECORD_MIXER_SIGNATURE));
		item->SetEnabled(enabled);
		menu->AddItem(item);
	}

	// The icon, from this add-on's own resources, in the size of the item.
	void _LoadIcon()
	{
		if (fIcon != NULL)
			return;
		image_info image;
		int32 cookie = 0;
		const addr_t here = (addr_t)&RemoveItemThread;
		while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
			if (here < (addr_t)image.text || here >= (addr_t)image.text + image.text_size)
				continue;
			BFile file(image.name, B_READ_ONLY);
			BResources resources(&file);
			size_t size = 0;
			const void* data = resources.LoadResource(B_VECTOR_ICON_TYPE, "BEOS:ICON", &size);
			if (data == NULL)
				break;
			BBitmap* icon = new BBitmap(BRect(0, 0, Bounds().Height(), Bounds().Height()), B_RGBA32);
			if (BIconUtils::GetVectorIcon((const uint8*)data, size, icon) == B_OK)
				fIcon = icon;
			else
				delete icon;
			break;
		}
	}

	BBitmap*		fIcon;
	BMessageRunner*	fRunner;
	bool			fRecording;
	bool			fHidden;
};


BArchivable*
MixerTrayView::Instantiate(BMessage* archive)
{
	return validate_instantiation(archive, "MixerTrayView") ? new MixerTrayView(archive) : NULL;
}


int
main()
{
	return 0;
}


extern "C" _EXPORT BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	return new MixerTrayView(BRect(0, 0, maxHeight - 1, maxHeight - 1));
}
