/*
 * hRecord Mixer: a window for hrecord.
 *
 * It starts hrecord (the program that records) as its engine, with --allaudio --meter --control, reads what hrecord prints
 * (the sources it has tapped, and a line of levels a few times a second) and writes commands to its stdin (the gain, mute and solo
 * of a source, the gain of the mix and what the limiter does) while it records. hrecord on the command line is not changed by
 * this: it records, and stops, as it always did.
 */

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <String.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringView.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>


static const char* kAppSignature = "application/x-vnd.hrecord-mixer";

enum {
	kMsgRecord		= 'rec!',
	kMsgStop		= 'stp!',
	kMsgLine		= 'line',
	kMsgEngineExit	= 'xit!',
	kMsgTick		= 'tck!',
	kMsgGain		= 'gai!',
	kMsgMute		= 'mut!',
	kMsgPan			= 'pan!',
	kMsgSolo		= 'sol!',
	kMsgMixGain		= 'mxg!',
	kMsgCeiling		= 'cei!',
	kMsgRelease		= 'rls!',
	kMsgLimiter		= 'lim!'
};

static const float kFaderMin = -60.0f, kFaderMax = 12.0f;		// dB, on a fader
static const float kMeterMin = -60.0f;							// dB, the bottom of a meter


// ---------------------------------------------------------------------------------------------------------------
// A level meter: a bar for a peak level in dB, with a hold mark that falls slowly.
// ---------------------------------------------------------------------------------------------------------------

class LevelMeter : public BView {
public:
	LevelMeter(const char* name, bool vertical, float low, float high, bool reduction = false)
		:
		BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fVertical(vertical),
		fLow(low),
		fHigh(high),
		fReduction(reduction),
		fLevel(low),
		fHold(low),
		fHoldTime(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetExplicitMinSize(vertical ? BSize(14, 80) : BSize(80, 12));
		SetExplicitPreferredSize(vertical ? BSize(16, 200) : BSize(160, 14));
	}

	void SetLevel(float db)
	{
		if (db < fLow)
			db = fLow;
		if (db > fHigh)
			db = fHigh;
		fLevel = db;
		const bigtime_t now = system_time();
		if (db >= fHold) {
			fHold = db;
			fHoldTime = now;
		} else if (now - fHoldTime > 1200000) {
			// the hold mark falls once it has been held a while
			fHold = std::max(db, fHold - 0.05f * (float)(now - fHoldTime - 1200000) / 1000000.0f * 20.0f);
			if (fHold < db)
				fHold = db;
		}
		Invalidate();
	}

	void Draw(BRect)
	{
		BRect bounds = Bounds();
		SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		FillRect(bounds);
		SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
		StrokeRect(bounds);
		bounds.InsetBy(1, 1);

		const float span = bounds.Width() * (fVertical ? 0 : 1) + bounds.Height() * (fVertical ? 1 : 0);
		const float filled = Fraction(fLevel) * span;

		// segments: green up to -12 dB, yellow up to -3 dB, red above (or red all through for a reduction meter)
		const rgb_color green = make_color(60, 190, 80), yellow = make_color(230, 200, 40), red = make_color(220, 60, 50);
		DrawBar(bounds, 0, filled, span, green, yellow, red);

		// the hold mark
		const float holdAt = Fraction(fHold) * span;
		if (holdAt > 1) {
			SetHighColor(make_color(250, 250, 250));
			if (fVertical)
				StrokeLine(BPoint(bounds.left, bounds.bottom - holdAt), BPoint(bounds.right, bounds.bottom - holdAt));
			else
				StrokeLine(BPoint(bounds.left + holdAt, bounds.top), BPoint(bounds.left + holdAt, bounds.bottom));
		}
	}

private:
	float Fraction(float db) const
	{
		return (db - fLow) / (fHigh - fLow);
	}

	rgb_color ZoneColor(float db, rgb_color green, rgb_color yellow, rgb_color red) const
	{
		if (fReduction)
			return red;
		return db > -3.0f ? red : (db > -12.0f ? yellow : green);
	}

	void DrawBar(BRect bounds, float from, float to, float span, rgb_color green, rgb_color yellow, rgb_color red)
	{
		// drawn in small steps so that the color follows the level
		const float step = 2.0f;
		for (float at = from; at < to; at += step) {
			const float end = std::min(to, at + step);
			const float db = fLow + (at / span) * (fHigh - fLow);
			SetHighColor(ZoneColor(db, green, yellow, red));
			if (fVertical)
				FillRect(BRect(bounds.left, bounds.bottom - end + 1, bounds.right, bounds.bottom - at));
			else
				FillRect(BRect(bounds.left + at, bounds.top, bounds.left + end - 1, bounds.bottom));
		}
	}

	bool		fVertical;
	float		fLow, fHigh;
	bool		fReduction;
	float		fLevel, fHold;
	bigtime_t	fHoldTime;
};


// ---------------------------------------------------------------------------------------------------------------
// A fader in dB (steps of half a dB), shown with its value; a right click puts it back to 0 dB.
// ---------------------------------------------------------------------------------------------------------------

class DbSlider : public BSlider {
public:
	DbSlider(const char* name, BMessage* message, float minDb, float maxDb, orientation direction, float resetDb = 0.0f)
		:
		BSlider(name, NULL, message, (int32)(minDb * 2), (int32)(maxDb * 2), direction),
		fResetDb(resetDb)
	{
		SetValue((int32)(resetDb * 2));
		SetModificationMessage(new BMessage(*message));
		BString low, high;
		low << (int)minDb;
		high << (int)maxDb;
		SetLimitLabels(low.String(), high.String());
	}

	float Db() const { return Value() / 2.0f; }

	const char* UpdateText() const
	{
		snprintf(fText, sizeof(fText), "%+.1f dB", Db());
		return fText;
	}

	void MouseDown(BPoint where)
	{
		int32 buttons = 0;
		if (Window() != NULL && Window()->CurrentMessage() != NULL)
			Window()->CurrentMessage()->FindInt32("buttons", &buttons);
		if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
			SetValue((int32)(fResetDb * 2));
			Invoke();
			return;
		}
		BSlider::MouseDown(where);
	}

private:
	float			fResetDb;
	mutable char	fText[32];
};


// A balance control: -100 (left) .. +100 (right); a right click centers it.
class PanSlider : public BSlider {
public:
	PanSlider(BMessage* message)
		:
		BSlider("pan", NULL, message, -100, 100, B_HORIZONTAL)
	{
		SetValue(0);
		SetModificationMessage(new BMessage(*message));
		SetLimitLabels("L", "R");
	}

	float Pan() const { return Value() / 100.0f; }

	const char* UpdateText() const
	{
		const int v = Value();
		if (v == 0)
			snprintf(fText, sizeof(fText), "Center");
		else
			snprintf(fText, sizeof(fText), v < 0 ? "%d%% L" : "%d%% R", v < 0 ? -v : v);
		return fText;
	}

	void MouseDown(BPoint where)
	{
		int32 buttons = 0;
		if (Window() != NULL && Window()->CurrentMessage() != NULL)
			Window()->CurrentMessage()->FindInt32("buttons", &buttons);
		if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
			SetValue(0);
			Invoke();
			return;
		}
		BSlider::MouseDown(where);
	}

private:
	mutable char	fText[32];
};


// ---------------------------------------------------------------------------------------------------------------
// One source: its name, fader, mute and solo, and its level.
// ---------------------------------------------------------------------------------------------------------------

class SourceStrip : public BView {
public:
	SourceStrip(int index, const char* name)
		:
		BView("strip", B_WILL_DRAW),
		fIndex(index)
	{
		SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		BString title;
		title << "#" << (index + 1);
		fTitle = new BStringView("title", title.String());
		fName = new BStringView("name", name);
		fName->SetToolTip(name);
		fName->SetExplicitMaxSize(BSize(110, B_SIZE_UNSET));
		fName->SetTruncation(B_TRUNCATE_END);

		BMessage* gain = new BMessage(kMsgGain);
		gain->AddInt32("source", index);
		fFader = new DbSlider("fader", gain, kFaderMin, kFaderMax, B_VERTICAL);
		fFader->SetExplicitMinSize(BSize(60, 160));
		fMeter = new LevelMeter("meter", true, kMeterMin, 0.0f);

		BMessage* pan = new BMessage(kMsgPan);
		pan->AddInt32("source", index);
		fPan = new PanSlider(pan);
		fPan->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));

		BMessage* mute = new BMessage(kMsgMute);
		mute->AddInt32("source", index);
		fMute = new BCheckBox("mute", "Mute", mute);
		BMessage* solo = new BMessage(kMsgSolo);
		solo->AddInt32("source", index);
		fSolo = new BCheckBox("solo", "Solo", solo);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 2)
			.SetInsets(6, 4, 6, 4)
			.Add(fTitle)
			.Add(fName)
			.AddGroup(B_HORIZONTAL, 4)
				.Add(fFader)
				.Add(fMeter)
			.End()
			.Add(fPan)
			.Add(fMute)
			.Add(fSolo);
	}

	void AttachedToWindow()
	{
		BView::AttachedToWindow();
		fFader->SetTarget(Window());
		fPan->SetTarget(Window());
		fMute->SetTarget(Window());
		fSolo->SetTarget(Window());
	}

	void SetLevel(float db) { fMeter->SetLevel(db); }
	int Index() const { return fIndex; }
	float Pan() const { return fPan->Pan(); }
	float Gain() const { return fFader->Db(); }

private:
	int				fIndex;
	BStringView*	fTitle;
	BStringView*	fName;
	DbSlider*		fFader;
	PanSlider*		fPan;
	LevelMeter*		fMeter;
	BCheckBox*		fMute;
	BCheckBox*		fSolo;
};


// ---------------------------------------------------------------------------------------------------------------
// The engine: hrecord as a child process, with a pipe each way.
// ---------------------------------------------------------------------------------------------------------------

class Engine {
public:
	Engine() : fPid(-1), fToEngine(-1), fFromEngine(-1), fReader(-1) {}

	bool Running() const { return fPid > 0; }

	// Starts hrecord with the arguments; what it prints goes to `target` as kMsgLine messages.
	bool Start(const std::vector<std::string>& arguments, BMessenger target, std::string& error)
	{
		if (Running())
			return false;

		std::string program = ProgramPath();
		int in[2], out[2];
		if (pipe(in) != 0 || pipe(out) != 0) {
			error = "Could not make the pipes to hrecord.";
			return false;
		}

		std::vector<std::string> args;
		args.push_back(program);
		for (size_t i = 0; i < arguments.size(); i++)
			args.push_back(arguments[i]);
		std::vector<char*> argv;
		for (size_t i = 0; i < args.size(); i++)
			argv.push_back(const_cast<char*>(args[i].c_str()));
		argv.push_back(NULL);

		pid_t pid = fork();
		if (pid < 0) {
			error = "Could not start hrecord.";
			return false;
		}
		if (pid == 0) {
			dup2(in[0], 0);
			dup2(out[1], 1);
			dup2(out[1], 2);
			close(in[0]);
			close(in[1]);
			close(out[0]);
			close(out[1]);
			execv(program.c_str(), argv.data());
			_exit(127);
		}
		close(in[0]);
		close(out[1]);
		fPid = pid;
		fToEngine = in[1];
		fFromEngine = out[0];
		fTarget = target;
		fReader = spawn_thread(ReaderThread, "hrecord output", B_NORMAL_PRIORITY, this);
		resume_thread(fReader);
		return true;
	}

	void Send(const char* text)
	{
		if (fToEngine >= 0)
			write(fToEngine, text, strlen(text));
	}

	// Asks hrecord to stop (it finishes its file first).
	void Stop()
	{
		Send("quit\n");
	}

	// Called when the reader has seen the end of what hrecord prints.
	void Finished()
	{
		if (fPid > 0) {
			int status = 0;
			waitpid(fPid, &status, 0);
		}
		if (fToEngine >= 0)
			close(fToEngine);
		if (fFromEngine >= 0)
			close(fFromEngine);
		fPid = -1;
		fToEngine = fFromEngine = -1;
	}

	// True once hrecord has ended (checked without waiting).
	bool HasEnded()
	{
		if (fPid <= 0)
			return true;
		int status = 0;
		if (waitpid(fPid, &status, WNOHANG) == fPid) {
			fPid = -1;
			return true;
		}
		return false;
	}

	void KillNow()
	{
		if (fPid > 0)
			kill(fPid, SIGKILL);
	}

	// hrecord next to this program, else the one on the path.
	static std::string ProgramPath();

private:
	static int32 ReaderThread(void* data)
	{
		Engine* self = (Engine*)data;
		std::string pending;
		char buffer[1024];
		for (;;) {
			ssize_t got = read(self->fFromEngine, buffer, sizeof(buffer));
			if (got <= 0)
				break;
			pending.append(buffer, got);
			size_t newline;
			while ((newline = pending.find('\n')) != std::string::npos) {
				std::string line = pending.substr(0, newline);
				pending.erase(0, newline + 1);
				BMessage message(kMsgLine);
				message.AddString("line", line.c_str());
				self->fTarget.SendMessage(&message);
			}
		}
		BMessage done(kMsgEngineExit);
		self->fTarget.SendMessage(&done);
		return 0;
	}

	static std::string ProgramPathImpl()
	{
		app_info info;
		if (be_app->GetAppInfo(&info) == B_OK) {
			BEntry entry(&info.ref);
			BPath path;
			if (entry.GetPath(&path) == B_OK && path.GetParent(&path) == B_OK) {
				path.Append("hrecord");
				if (access(path.Path(), X_OK) == 0)
					return path.Path();
			}
		}
		return "/boot/system/bin/hrecord";
	}

	pid_t		fPid;
	int			fToEngine;
	int			fFromEngine;
	thread_id	fReader;
	BMessenger	fTarget;
};


std::string
Engine::ProgramPath()
{
	return ProgramPathImpl();
}


// ---------------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------------

class MixerWindow : public BWindow {
public:
	MixerWindow()
		:
		BWindow(BRect(80, 80, 900, 640), "hRecord Mixer", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
		fRecording(false),
		fStopping(false),
		fSeconds(0),
		fTick(NULL)
	{
		fRecord = new BButton("record", "Record", new BMessage(kMsgRecord));
		fStop = new BButton("stop", "Stop", new BMessage(kMsgStop));
		fStop->SetEnabled(false);
		fScreen = new BCheckBox("screen", "Record the screen too", NULL);
		fRealtime = new BCheckBox("realtime", "Real-time (low latency)", NULL);
		fQualityMenu = new BPopUpMenu("quality");
		fQualityMenu->AddItem(new BMenuItem("Low", NULL));
		fQualityMenu->AddItem(new BMenuItem("Medium", NULL));
		fQualityMenu->AddItem(new BMenuItem("High", NULL));
		fQualityMenu->ItemAt(1)->SetMarked(true);
		fQuality = new BMenuField("quality", "Screen quality:", fQualityMenu);
		fTime = new BStringView("time", "00:00");
		BFont big;
		fTime->GetFont(&big);
		big.SetSize(big.Size() * 1.6f);
		fTime->SetFont(&big);

		fStripsGroup = new BView("strips", B_WILL_DRAW);
		fStripsGroup->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		BGroupLayout* stripsLayout = new BGroupLayout(B_HORIZONTAL, 6);
		fStripsGroup->SetLayout(stripsLayout);
		fHint = new BStringView("hint", "Start playing sound in the programs to record, then press Record. Each one gets a strip.");
		stripsLayout->AddView(fHint);
		stripsLayout->AddItem(BSpaceLayoutItem::CreateGlue());

		// the master section: the whole mix and the limiter
		fMixGain = new DbSlider("mixgain", new BMessage(kMsgMixGain), -12.0f, 12.0f, B_VERTICAL);
		fMixGain->SetExplicitMinSize(BSize(60, 120));
		fPreMeter = new LevelMeter("pre", true, kMeterMin, 6.0f);
		fPostMeter = new LevelMeter("post", true, kMeterMin, 6.0f);
		fLimiter = new BCheckBox("limiter", "Limiter", new BMessage(kMsgLimiter));
		fLimiter->SetValue(B_CONTROL_ON);
		fCeiling = new DbSlider("ceiling", new BMessage(kMsgCeiling), -12.0f, 0.0f, B_HORIZONTAL, -1.0f);
		fRelease = new BSlider("release", "Release", new BMessage(kMsgRelease), 10, 500, B_HORIZONTAL);
		fRelease->SetValue(80);
		fRelease->SetModificationMessage(new BMessage(kMsgRelease));
		fRelease->SetLimitLabels("10 ms", "500 ms");
		fReduction = new LevelMeter("reduction", false, 0.0f, 12.0f, true);
		fReductionText = new BStringView("reductiontext", "Gain reduction: 0.0 dB");

		BStringView* ceilingTitle = new BStringView("ceilingtitle", "Ceiling");
		BStringView* mixTitle = new BStringView("mixtitle", "Mix");
		BStringView* preTitle = new BStringView("pretitle", "Before");
		BStringView* postTitle = new BStringView("posttitle", "After");

		BView* master = new BView("master", B_WILL_DRAW);
		master->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		BLayoutBuilder::Group<>(master, B_VERTICAL, 4)
			.SetInsets(8, 6, 8, 6)
			.Add(mixTitle)
			.AddGroup(B_HORIZONTAL, 6)
				.Add(fMixGain)
				.AddGroup(B_VERTICAL, 2)
					.Add(preTitle)
					.Add(fPreMeter)
				.End()
				.AddGroup(B_VERTICAL, 2)
					.Add(postTitle)
					.Add(fPostMeter)
				.End()
			.End()
			.AddStrut(6)
			.Add(fLimiter)
			.Add(ceilingTitle)
			.Add(fCeiling)
			.Add(fRelease)
			.Add(fReductionText)
			.Add(fReduction);

		fStatus = new BStringView("status", "Ready.");
		fLog = new BTextView("log");
		fLog->MakeEditable(false);
		fLog->SetWordWrap(true);
		BScrollView* logScroll = new BScrollView("logscroll", fLog, 0, false, true);
		logScroll->SetExplicitMinSize(BSize(200, 70));
		logScroll->SetExplicitPreferredSize(BSize(400, 90));

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_INSETS)
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.Add(fRecord)
				.Add(fStop)
				.Add(fScreen)
				.Add(fQuality)
				.Add(fRealtime)
				.AddGlue()
				.Add(fTime)
			.End()
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.Add(fStripsGroup, 3.0f)
				.Add(master, 1.0f)
			.End()
			.Add(fStatus)
			.Add(logScroll);

		RefreshRealtime();
	}

	bool QuitRequested()
	{
		if (fEngine.Running()) {
			BAlert* alert = new BAlert("quit", "A recording is running. Stop it and quit?", "Cancel", "Stop and quit", NULL,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() != 1)
				return false;
			fEngine.Stop();
			// give hrecord the time to finish its file
			for (int i = 0; i < 100 && !fEngine.HasEnded(); i++)
				snooze(100000);
			fEngine.KillNow();
		}
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgRecord:
				StartRecording();
				break;
			case kMsgStop:
				StopRecording();
				break;
			case kMsgLine:
			{
				const char* line;
				if (message->FindString("line", &line) == B_OK)
					HandleLine(line);
				break;
			}
			case kMsgEngineExit:
				EngineExited();
				break;
			case kMsgTick:
				fSeconds++;
				ShowTime();
				break;
			case kMsgGain:
			{
				int32 source;
				DbSlider* slider = NULL;
				if (message->FindInt32("source", &source) == B_OK && message->FindPointer("source", (void**)&slider) != B_OK) {
					// the value comes from the strip's fader
					for (size_t i = 0; i < fStrips.size(); i++) {
						if (fStrips[i]->Index() == source) {
							DbSlider* fader = (DbSlider*)fStrips[i]->FindView("fader");
							if (fader != NULL)
								SendCommand("gain %d %.1f\n", (int)source + 1, fader->Db());
						}
					}
				}
				break;
			}
			case kMsgPan:
			{
				int32 source;
				if (message->FindInt32("source", &source) == B_OK) {
					for (size_t i = 0; i < fStrips.size(); i++) {
						if (fStrips[i]->Index() == source)
							SendCommand("pan %d %.2f\n", (int)source + 1, fStrips[i]->Pan());
					}
				}
				break;
			}
			case kMsgMute:
			case kMsgSolo:
			{
				int32 source, on = 0;
				BControl* control = NULL;
				if (message->FindInt32("source", &source) == B_OK && message->FindPointer("source", (void**)&control) != B_OK) {
					// "be:source" holds the control that was clicked
					if (message->FindPointer("be:source", (void**)&control) == B_OK && control != NULL)
						on = control->Value() == B_CONTROL_ON ? 1 : 0;
					SendCommand(message->what == kMsgMute ? "mute %d %d\n" : "solo %d %d\n", (int)source + 1, (int)on);
				}
				break;
			}
			case kMsgMixGain:
				SendCommand("mixgain %.1f\n", fMixGain->Db());
				break;
			case kMsgCeiling:
				SendCommand("ceiling %.1f\n", fCeiling->Db());
				break;
			case kMsgRelease:
				SendCommand("release %d\n", (int)fRelease->Value());
				break;
			case kMsgLimiter:
				SendCommand("limiter %d\n", fLimiter->Value() == B_CONTROL_ON ? 1 : 0);
				fCeiling->SetEnabled(fLimiter->Value() == B_CONTROL_ON);
				fRelease->SetEnabled(fLimiter->Value() == B_CONTROL_ON);
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void SendCommand(const char* format, ...)
	{
		if (!fEngine.Running())
			return;
		char text[128];
		va_list list;
		va_start(list, format);
		vsnprintf(text, sizeof(text), format, list);
		va_end(list);
		fEngine.Send(text);
	}

	// Asks hrecord whether real-time audio is set on the sound driver and shows it: on or off and greyed out when
	// that is known, a box to choose only for a driver hrecord has no profile for.
	void RefreshRealtime()
	{
		std::string command = "\"" + Engine::ProgramPath() + "\" --detect-realtime 2>/dev/null";
		char answer[128] = "";
		FILE* pipe = popen(command.c_str(), "r");
		if (pipe != NULL) {
			if (fgets(answer, sizeof(answer), pipe) == NULL)
				answer[0] = '\0';
			pclose(pipe);
		}
		int frames = 0;
		if (sscanf(answer, "realtime on %d", &frames) == 1) {
			BString label;
			label << "Real-time audio: on (" << frames << " frames)";
			fRealtime->SetLabel(label.String());
			fRealtime->SetValue(B_CONTROL_ON);
			fRealtime->SetEnabled(false);
		} else if (strncmp(answer, "realtime off known", 18) == 0) {
			fRealtime->SetLabel("Real-time audio: off (set in the driver settings)");
			fRealtime->SetValue(B_CONTROL_OFF);
			fRealtime->SetEnabled(false);
		} else {
			fRealtime->SetLabel("Real-time (low latency)");
			fRealtime->SetEnabled(!fRecording);
		}
	}

	void StartRecording()
	{
		if (fEngine.Running())
			return;
		RefreshRealtime();
		ClearStrips();
		std::vector<std::string> arguments;
		arguments.push_back("start");
		if (fScreen->Value() != B_CONTROL_ON)
			arguments.push_back("--audioonly");
		arguments.push_back("--allaudio");
		arguments.push_back("--meter");
		arguments.push_back("--meter-ms");
		arguments.push_back("70");
		arguments.push_back("--control");
		if (fRealtime->IsEnabled() && fRealtime->Value() == B_CONTROL_ON)
			arguments.push_back("--realtime");
		if (fScreen->Value() == B_CONTROL_ON) {
			BMenuItem* marked = fQualityMenu->FindMarked();
			int32 index = marked != NULL ? fQualityMenu->IndexOf(marked) : 1;
			arguments.push_back(index == 0 ? "--low" : (index == 2 ? "--high" : "--medium"));
		}
		// what is set in the window now goes to the engine as soon as it runs
		fPendingMixGain = fMixGain->Db();
		fPendingCeiling = fCeiling->Db();
		fPendingRelease = (int)fRelease->Value();
		fPendingLimiter = fLimiter->Value() == B_CONTROL_ON;

		std::string error;
		if (!fEngine.Start(arguments, BMessenger(this), error)) {
			SetStatus(error.c_str());
			return;
		}
		fRecording = true;
		fStopping = false;
		fSeconds = 0;
		ShowTime();
		BMessage tick(kMsgTick);
		delete fTick;
		fTick = new BMessageRunner(this, &tick, 1000000);
		fRecord->SetEnabled(false);
		fStop->SetEnabled(true);
		fScreen->SetEnabled(false);
		fRealtime->SetEnabled(false);
		SetStatus("Starting hrecord...");
		fOutputFile = "";
		Log("-- recording started --");
		SendCommand("mixgain %.1f\n", fPendingMixGain);
		SendCommand("ceiling %.1f\n", fPendingCeiling);
		SendCommand("release %d\n", fPendingRelease);
		SendCommand("limiter %d\n", fPendingLimiter ? 1 : 0);
	}

	void StopRecording()
	{
		if (!fEngine.Running() || fStopping)
			return;
		fStopping = true;
		fStop->SetEnabled(false);
		SetStatus("Stopping: finishing the file...");
		fEngine.Stop();
	}

	void EngineExited()
	{
		fEngine.Finished();
		fRecording = false;
		fStopping = false;
		delete fTick;
		fTick = NULL;
		fRecord->SetEnabled(true);
		fStop->SetEnabled(false);
		fScreen->SetEnabled(true);
		RefreshRealtime();
		for (size_t i = 0; i < fStrips.size(); i++)
			fStrips[i]->SetLevel(kMeterMin);
		fPreMeter->SetLevel(kMeterMin);
		fPostMeter->SetLevel(kMeterMin);
		fReduction->SetLevel(0.0f);
		if (fOutputFile.length() > 0) {
			BString text("Saved: ");
			text << fOutputFile.c_str();
			SetStatus(text.String());
		} else
			SetStatus("Stopped.");
		Log("-- recording ended --");
	}

	// One line of what hrecord printed.
	void HandleLine(const char* line)
	{
		if (strncmp(line, "[meter] #", 9) == 0) {
			UpdateMeters(line);
			return;
		}
		int number = 0;
		if (sscanf(line, "[+] Tapped source #%d", &number) == 1) {
			const char* quote = strchr(line, '"');
			const char* last = quote != NULL ? strrchr(line, '"') : NULL;
			std::string name = (quote != NULL && last != NULL && last > quote) ? std::string(quote + 1, last - quote - 1) : "source";
			AddStrip(number - 1, name.c_str());
			Log(line);
			return;
		}
		const char* written = strstr(line, "Output written to '");
		if (written != NULL) {
			std::string path = written + strlen("Output written to '");
			size_t end = path.find('\'');
			if (end != std::string::npos)
				path.resize(end);
			fOutputFile = path;
		}
		if (strstr(line, "[+] Tapped ") != NULL && strstr(line, "currently-playing") != NULL)
			SetStatus("Recording.");
		if (strstr(line, "[meter]") != NULL || strstr(line, "    ") == line)
			; // the summary lines go to the log too
		Log(line);
		if (strncmp(line, "[-] Error", 9) == 0)
			SetStatus(line);
	}

	// "[meter] #1 -0.9 #2 -3.2 | mix before 3.5 after -1.0 dBFS | limiter -4.5 dB"
	void UpdateMeters(const char* line)
	{
		const char* cursor = line + 7;
		for (;;) {
			int number = 0;
			float db = 0;
			int used = 0;
			if (sscanf(cursor, " #%d %f%n", &number, &db, &used) < 2)
				break;
			cursor += used;
			for (size_t i = 0; i < fStrips.size(); i++) {
				if (fStrips[i]->Index() == number - 1)
					fStrips[i]->SetLevel(db);
			}
		}
		float before, after, limiter;
		const char* mix = strstr(line, "| mix before");
		if (mix != NULL && sscanf(mix, "| mix before %f after %f dBFS | limiter %f dB", &before, &after, &limiter) == 3) {
			fPreMeter->SetLevel(before);
			fPostMeter->SetLevel(after);
			float reduction = -limiter;
			if (reduction < 0.0f)
				reduction = 0.0f;
			fReduction->SetLevel(reduction);
			char text[64];
			snprintf(text, sizeof(text), "Gain reduction: %.1f dB", reduction);
			fReductionText->SetText(text);
		}
	}

	void AddStrip(int index, const char* name)
	{
		for (size_t i = 0; i < fStrips.size(); i++) {
			if (fStrips[i]->Index() == index)
				return;
		}
		BGroupLayout* layout = (BGroupLayout*)fStripsGroup->GetLayout();
		if (!fHint->IsHidden())
			fHint->Hide();
		SourceStrip* strip = new SourceStrip(index, name);
		// before the glue at the end
		layout->AddView(layout->CountItems() - 1, strip);
		fStrips.push_back(strip);
	}

	void ClearStrips()
	{
		for (size_t i = 0; i < fStrips.size(); i++) {
			fStrips[i]->RemoveSelf();
			delete fStrips[i];
		}
		fStrips.clear();
		if (fHint->IsHidden())
			fHint->Show();
	}

	void ShowTime()
	{
		char text[16];
		snprintf(text, sizeof(text), "%02d:%02d", (int)(fSeconds / 60), (int)(fSeconds % 60));
		fTime->SetText(text);
	}

	void SetStatus(const char* text) { fStatus->SetText(text); }

	void Log(const char* line)
	{
		BString text(line);
		text << "\n";
		fLog->Insert(fLog->TextLength(), text.String(), text.Length());
		fLog->ScrollToOffset(fLog->TextLength());
	}

	Engine				fEngine;
	bool				fRecording;
	bool				fStopping;
	int					fSeconds;
	BMessageRunner*		fTick;

	BButton*			fRecord;
	BButton*			fStop;
	BCheckBox*			fScreen;
	BCheckBox*			fRealtime;
	BPopUpMenu*			fQualityMenu;
	BMenuField*			fQuality;
	BStringView*		fTime;
	BView*				fStripsGroup;
	BStringView*		fHint;
	std::vector<SourceStrip*> fStrips;
	DbSlider*			fMixGain;
	LevelMeter*			fPreMeter;
	LevelMeter*			fPostMeter;
	BCheckBox*			fLimiter;
	DbSlider*			fCeiling;
	BSlider*			fRelease;
	LevelMeter*			fReduction;
	BStringView*		fReductionText;
	BStringView*		fStatus;
	BTextView*			fLog;
	std::string			fOutputFile;
	float				fPendingMixGain, fPendingCeiling;
	int					fPendingRelease;
	bool				fPendingLimiter;
};


class MixerApp : public BApplication {
public:
	MixerApp() : BApplication(kAppSignature) {}

	void ReadyToRun()
	{
		(new MixerWindow())->Show();
	}
};


int
main()
{
	MixerApp app;
	app.Run();
	return 0;
}
