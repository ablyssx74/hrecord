/*
 * hRecord Mixer: a window for hrecord.
 *
 * It looks and is laid out much like OBS Studio: a preview of what will be recorded on top, and docks under it for the sources
 * (the whole desktop, one window, or sound alone), the audio mixer and the controls.
 *
 * It starts hrecord (the program that records) as its engine, with --allaudio --meter --control, reads what hrecord prints
 * (the sources it has tapped, and a line of levels a few times a second) and writes commands to its stdin (the gain, mute and solo
 * of a source, the gain of the mix and what the limiter does) while it records. hrecord on the command line is not changed by
 * this: it records, and stops, as it always did.
 */

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CardLayout.h>
#include <Deskbar.h>
#include <Entry.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <File.h>
#include <MenuItem.h>
#include <ListView.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <MediaRoster.h>
#include <Message.h>
#include <Url.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <String.h>
#include <ScrollView.h>
#include <Spinner.h>
#include <Slider.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mixer_widgets.h"
#include "preview.h"
#include "tray.h"
#include "eq.h"


static const char* kAppSignature = HRECORD_MIXER_SIGNATURE;

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
	kMsgLimiter		= 'lim!',
	kMsgMixHub		= 'mxh!',
	kMsgMixHubSelf	= 'mxs!',
	kMsgStreamToggle	= 'stg!',
	kMsgStreamConfigure	= 'scf!',
	kMsgStreamSaved		= 'ssv!',
	kMsgStreamService	= 'ssc!',
	kMsgStreamSave		= 'sav!',
	kMsgStartEngine		= 'eng!',
	kMsgShowLog			= 'slg!',
	kMsgShowNodes		= 'snd!',
	kMsgTestClick		= 'tcl!',
	kMsgTestAction		= 'tac!',
	kMsgSource			= 'src!',
	kMsgSourceList		= 'wls!',
	kMsgPreviewToggle	= 'pvt!',
	kMsgDebug			= 'dbg!',
	kMsgShowMixer		= 'smx!',
	kMsgSourceByName	= 'sbn!',
	kMsgEqOpen			= 'eqo!',		// the EQ button
	kMsgEqChanged		= 'eqc!',		// the EQ window to the mixer: "on" (bool) and 15 "band" (float)
	kMsgEqEnable		= 'eqe!',		// inside the EQ window
	kMsgEqBand			= 'eqb!',
	kMsgEqPreset		= 'eqp!'
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
		SetExplicitMinSize(vertical ? BSize(7, 80) : BSize(80, 10));
		SetExplicitPreferredSize(vertical ? BSize(8, 180) : BSize(120, 10));
		SetExplicitMaxSize(vertical ? BSize(8, B_SIZE_UNLIMITED) : BSize(B_SIZE_UNLIMITED, 10));
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
		SetHighColor(theme::kInset);
		FillRect(bounds);
		SetHighColor(theme::kHeader);
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


// A left and a right level meter side by side.
class StereoMeter : public BView {
public:
	StereoMeter(const char* name, float low, float high)
		:
		BView(name, 0),
		fLeft(new LevelMeter("left", true, low, high)),
		fRight(new LevelMeter("right", true, low, high))
	{
		SetViewColor(theme::kDock);
		SetExplicitMaxSize(BSize(17, B_SIZE_UNLIMITED));
		BLayoutBuilder::Group<>(this, B_HORIZONTAL, 1)
			.Add(fLeft)
			.Add(fRight);
	}

	void SetLevels(float left, float right)
	{
		fLeft->SetLevel(left);
		fRight->SetLevel(right);
	}

private:
	LevelMeter*	fLeft;
	LevelMeter*	fRight;
};


// ---------------------------------------------------------------------------------------------------------------
// A fader in dB (values are half decibels) with the text that says where it is, and a balance control.
// ---------------------------------------------------------------------------------------------------------------

static void
FaderText(Fader* fader, char* text, size_t size)
{
	snprintf(text, size, "%+.1f dB", fader->Value() / 2.0f);
}


static void
PanText(Fader* fader, char* text, size_t size)
{
	const int v = fader->Value();
	if (v == 0)
		snprintf(text, size, "Center");
	else
		snprintf(text, size, v < 0 ? "%d%% L" : "%d%% R", v < 0 ? -v : v);
}


// One source: its name, fader, balance, mute and solo, and its level.
class SourceStrip : public BView {
public:
	SourceStrip(int index, const char* name)
		:
		BView("strip", B_WILL_DRAW),
		fIndex(index)
	{
		SetViewColor(theme::kDock);
		BString title;
		title << "#" << (index + 1) << " " << name;
		Label* nameLabel = new Label("name", title.String(), theme::kDock, theme::kText, true, 104);
		nameLabel->SetToolTip(name);

		BMessage* gain = new BMessage(kMsgGain);
		gain->AddInt32("source", index);
		fFader = new Fader("fader", gain, (int32)(kFaderMin * 2), (int32)(kFaderMax * 2), 0, 2, true, theme::kDock);
		fMeter = new StereoMeter("meter", kMeterMin, 0.0f);
		fDbText = new Label("db", "+0.0 dB", theme::kDock, theme::kDimText, true, 104);

		BMessage* pan = new BMessage(kMsgPan);
		pan->AddInt32("source", index);
		fPan = new Fader("pan", pan, -100, 100, 0, 5, false, theme::kDock);
		fPan->SetExplicitMaxSize(BSize(104, 16));
		fPan->SetToolTip("Balance, left to right. A right click centers it.");

		BMessage* mute = new BMessage(kMsgMute);
		mute->AddInt32("source", index);
		fMute = new FlatCheck("mute", "Mute", mute, theme::kDock, true, theme::kRecord);
		BMessage* solo = new BMessage(kMsgSolo);
		solo->AddInt32("source", index);
		fSolo = new FlatCheck("solo", "Solo", solo, theme::kDock, true, theme::kSolo);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 4)
			.SetInsets(6, 4, 6, 6)
			.Add(nameLabel)
			.Add(fDbText)
			.AddGroup(B_HORIZONTAL, 6)
				.AddGlue()
				.Add(fMeter)
				.Add(fFader)
				.AddGlue()
			.End()
			.Add(fPan)
			.AddGroup(B_HORIZONTAL, 4)
				.Add(fMute)
				.Add(fSolo)
			.End();
		SetExplicitMaxSize(BSize(116, B_SIZE_UNLIMITED));
	}

	void AttachedToWindow()
	{
		BView::AttachedToWindow();
		fFader->SetTarget(Window());
		fPan->SetTarget(Window());
		fMute->SetTarget(Window());
		fSolo->SetTarget(Window());
	}

	void ShowGain()
	{
		char text[32];
		FaderText(fFader, text, sizeof(text));
		fDbText->SetText(text);
		PanText(fPan, text, sizeof(text));
		fPan->SetToolTip(text);
	}

	void SetLevels(float left, float right) { fMeter->SetLevels(left, right); }
	int Index() const { return fIndex; }
	float Pan() const { return fPan->Value() / 100.0f; }
	float Gain() const { return fFader->Value() / 2.0f; }
	bool Muted() const { return fMute->Value() == B_CONTROL_ON; }
	bool Soloed() const { return fSolo->Value() == B_CONTROL_ON; }

private:
	int				fIndex;
	Label*			fDbText;
	Fader*			fFader;
	Fader*			fPan;
	StereoMeter*	fMeter;
	FlatCheck*		fMute;
	FlatCheck*		fSolo;
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
// Live streaming: where to, and how. Kept in a settings file; the stream key in it is private to the user's account.
// ---------------------------------------------------------------------------------------------------------------

struct StreamService {
	const char*	name;
	const char*	base;		// the server's address up to the stream key; empty: the user enters it
};

static const StreamService kStreamServices[] = {
	{ "YouTube (RTMPS, recommended)",	"rtmps://a.rtmps.youtube.com:443/live2" },
	{ "YouTube (RTMP)",					"rtmp://a.rtmp.youtube.com/live2" },
	{ "Twitch",							"rtmp://live.twitch.tv/app" },
	{ "Facebook Live",					"rtmps://live-api-s.facebook.com:443/rtmp" },
	{ "Other RTMP / RTMPS server",		"" },
	{ "RTSP server",					"" }
};
static const int kStreamServiceCount = sizeof(kStreamServices) / sizeof(kStreamServices[0]);
static_assert(sizeof(kStreamServices) / sizeof(kStreamServices[0]) == 6, "StreamConfig keeps 6 services");
static const int kServiceCustomRtmp = 4;
static const int kServiceRtsp = 5;

// where the add-on is downloaded from (the hrecord_stream package, in the release list)
static const char* kAddonDownloadUrl = "https://github.com/ablyssx74/hrecord/releases/latest";

static const char* kStreamPresets[] = { "ultrafast", "superfast", "veryfast", "faster", "fast", "medium" };
static const int kStreamPresetCount = sizeof(kStreamPresets) / sizeof(kStreamPresets[0]);

struct StreamConfig {
	bool		enabled = false;		// the "Stream live" box
	int			service = 0;
	std::string	servers[6];				// by service: the address of those that have no fixed one (kStreamServiceCount)
	std::string	keys[6];				// by service: each server has a stream key of its own
	int			videoKbps = 2500;
	int			audioKbps = 128;
	int			preset = 2;				// veryfast
	int			keyintSeconds = 2;
	bool		showLog = true;			// the log under the window is shown
	bool		hubOn = true;			// hrecord shares the audio input with programs (its input hub)
	bool		mixHubOn = true;		// hrecord offers the mix (what is heard) to programs that record (its mix input)
	bool		mixHubLeaveOut = true;	// ... without what the recording program plays itself
	bool		debug = false;			// the three above can be changed; without it they are hidden, and on
	bool		audioOnly = false;		// the source that was chosen: sound alone (or else the screen)
	bool		preview = true;			// the preview is shown
	bool		eqOn = false;			// the master equalizer
	float		eqBands[kEqBands] = {};	// dB
	std::map<std::string, int>	hubModes;	// by program name: what it gets from the hub, 0 both channels, 1 left, 2 right

	// what hrecord is told
	bool HubWanted() const { return !debug || hubOn; }
	bool MixHubWanted() const { return !debug || mixHubOn; }
	bool LeaveOutWanted() const { return !debug || mixHubLeaveOut; }

	std::string BaseAddress() const
	{
		std::string base = service < kServiceCustomRtmp ? kStreamServices[service].base : servers[service];
		while (!base.empty() && (base[base.size() - 1] == '/' || base[base.size() - 1] == ' '))
			base.resize(base.size() - 1);
		return base;
	}

	// The whole address hrecord sends to (with the key).
	std::string Url() const
	{
		std::string base = BaseAddress();
		if (service == kServiceRtsp || keys[service].empty())
			return base;
		return base + "/" + keys[service];
	}

	// What is missing, or empty when it is fine.
	std::string Problem() const
	{
		const std::string base = BaseAddress();
		if (service == kServiceRtsp) {
			if (base.compare(0, 7, "rtsp://") != 0)
				return "Stream settings: the RTSP address must start with rtsp://";
			return "";
		}
		if (service == kServiceCustomRtmp && base.compare(0, 7, "rtmp://") != 0 && base.compare(0, 8, "rtmps://") != 0)
			return "Stream settings: the server address must start with rtmp:// or rtmps://";
		if (keys[service].empty())
			return "Stream settings: enter the stream key.";
		return "";
	}

	static std::string FilePath()
	{
		BPath path;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
			return "";
		path.Append("hrecord_mixer_settings");
		return path.Path();
	}

	void Load()
	{
		BFile file(FilePath().c_str(), B_READ_ONLY);
		BMessage message;
		if (file.InitCheck() != B_OK || message.Unflatten(&file) != B_OK)
			return;
		int32 number;
		const char* text;
		bool flag;
		if (message.FindBool("stream", &flag) == B_OK) enabled = flag;
		if (message.FindBool("showlog", &flag) == B_OK) showLog = flag;
		// (a file without "debug" is from before the three were on by default: what it says about them is not kept)
		if (message.FindBool("debug", &flag) == B_OK) {
			debug = flag;
			if (message.FindBool("hubon", &flag) == B_OK) hubOn = flag;
			if (message.FindBool("mixhubon", &flag) == B_OK) mixHubOn = flag;
			if (message.FindBool("mixhubleaveout", &flag) == B_OK) mixHubLeaveOut = flag;
		}
		if (message.FindBool("audioonly", &flag) == B_OK) audioOnly = flag;
		if (message.FindBool("preview", &flag) == B_OK) preview = flag;
		if (message.FindBool("eqon", &flag) == B_OK) eqOn = flag;
		float gain;
		for (int i = 0; i < kEqBands; i++) {
			if (message.FindFloat("eqband", i, &gain) == B_OK)
				eqBands[i] = gain;
		}
		for (int32 i = 0;; i++) {
			char* name = NULL;
			type_code type;
			int32 count;
			if (message.GetInfo(B_INT32_TYPE, i, &name, &type, &count) != B_OK)
				break;
			int32 mode;
			if (strncmp(name, "hubmode:", 8) == 0 && message.FindInt32(name, &mode) == B_OK)
				hubModes[name + 8] = (int)mode;
		}
		if (message.FindInt32("service", &number) == B_OK && number >= 0 && number < kStreamServiceCount) service = number;
		// (an older file has one key and one address, for the service that was chosen then)
		if (message.FindString("server", &text) == B_OK) servers[service] = text;
		if (message.FindString("key", &text) == B_OK) keys[service] = text;
		for (int i = 0; i < kStreamServiceCount; i++) {
			char name[16];
			snprintf(name, sizeof(name), "server%d", i);
			if (message.FindString(name, &text) == B_OK) servers[i] = text;
			snprintf(name, sizeof(name), "key%d", i);
			if (message.FindString(name, &text) == B_OK) keys[i] = text;
		}
		if (message.FindInt32("video", &number) == B_OK) videoKbps = number;
		if (message.FindInt32("audio", &number) == B_OK) audioKbps = number;
		if (message.FindInt32("preset", &number) == B_OK && number >= 0 && number < kStreamPresetCount) preset = number;
		if (message.FindInt32("keyint", &number) == B_OK) keyintSeconds = number;
	}

	void Save() const
	{
		BMessage message;
		message.AddBool("stream", enabled);
		message.AddBool("showlog", showLog);
		message.AddBool("hubon", hubOn);
		message.AddBool("mixhubon", mixHubOn);
		message.AddBool("mixhubleaveout", mixHubLeaveOut);
		message.AddBool("debug", debug);
		message.AddBool("audioonly", audioOnly);
		message.AddBool("preview", preview);
		message.AddBool("eqon", eqOn);
		for (int i = 0; i < kEqBands; i++)
			message.AddFloat("eqband", eqBands[i]);
		for (std::map<std::string, int>::const_iterator it = hubModes.begin(); it != hubModes.end(); ++it)
			message.AddInt32((std::string("hubmode:") + it->first).c_str(), it->second);
		message.AddInt32("service", service);
		for (int i = 0; i < kStreamServiceCount; i++) {
			char name[16];
			snprintf(name, sizeof(name), "server%d", i);
			message.AddString(name, servers[i].c_str());
			snprintf(name, sizeof(name), "key%d", i);
			message.AddString(name, keys[i].c_str());
		}
		message.AddInt32("video", videoKbps);
		message.AddInt32("audio", audioKbps);
		message.AddInt32("preset", preset);
		message.AddInt32("keyint", keyintSeconds);
		const std::string path = FilePath();
		if (path.empty())
			return;
		BFile file(path.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (file.InitCheck() != B_OK)
			return;
		message.Flatten(&file);
		chmod(path.c_str(), S_IRUSR | S_IWUSR);		// the stream key is for this user only
	}
};


class StreamSettingsWindow : public BWindow {
public:
	StreamSettingsWindow(BMessenger owner, const StreamConfig& config)
		:
		BWindow(BRect(200, 200, 600, 520), "Stream settings", B_TITLED_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
			B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
		fOwner(owner)
	{
		fServiceMenu = new BPopUpMenu("service");
		for (int i = 0; i < kStreamServiceCount; i++) {
			BMenuItem* item = new BMenuItem(kStreamServices[i].name, new BMessage(kMsgStreamService));
			fServiceMenu->AddItem(item);
		}
		fServiceMenu->ItemAt(config.service)->SetMarked(true);
		fService = new BMenuField("service", "Stream to:", fServiceMenu);

		fServer = new BTextControl("server", "Server address:", "", NULL);
		// HideTyping() deletes what the field holds, so the key is put in after it
		fKey = new BTextControl("key", "Stream key:", "", NULL);
		fKey->TextView()->HideTyping(true);


		fVideo = new BSpinner("video", "Video bitrate (kbps):", NULL);
		fVideo->SetRange(300, 20000);
		fVideo->SetValue(config.videoKbps);
		fAudio = new BSpinner("audio", "Audio bitrate (kbps):", NULL);
		fAudio->SetRange(32, 320);
		fAudio->SetValue(config.audioKbps);
		fKeyint = new BSpinner("keyint", "Key frame every (s):", NULL);
		fKeyint->SetRange(1, 10);
		fKeyint->SetValue(config.keyintSeconds);

		fPresetMenu = new BPopUpMenu("preset");
		for (int i = 0; i < kStreamPresetCount; i++)
			fPresetMenu->AddItem(new BMenuItem(kStreamPresets[i], NULL));
		fPresetMenu->ItemAt(config.preset)->SetMarked(true);
		fPreset = new BMenuField("preset", "Encoder speed:", fPresetMenu);

		BStringView* note1 = new BStringView("note1", "The size and frame rate follow the Screen quality menu (Low: 1280 wide, 15 fps;");
		BStringView* note2 = new BStringView("note2", "Medium: 1600, 24 fps; High: the whole screen, 30 fps). Faster encoder speeds use less CPU.");
		BString storedIn("Configuration settings stored in ");
		storedIn << StreamConfig::FilePath().c_str();
		BStringView* note3 = new BStringView("note3", storedIn.String());

		BButton* save = new BButton("save", "Save", new BMessage(kMsgStreamSave));
		save->MakeDefault(true);
		BButton* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_INSETS)
			.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
				.Add(fService->CreateLabelLayoutItem(), 0, 0)
				.Add(fService->CreateMenuBarLayoutItem(), 1, 0)
				.Add(fServer->CreateLabelLayoutItem(), 0, 1)
				.Add(fServer->CreateTextViewLayoutItem(), 1, 1)
				.Add(fKey->CreateLabelLayoutItem(), 0, 2)
				.Add(fKey->CreateTextViewLayoutItem(), 1, 2)
				.Add(fVideo->CreateLabelLayoutItem(), 0, 3)
				.Add(fVideo->CreateTextViewLayoutItem(), 1, 3)
				.Add(fAudio->CreateLabelLayoutItem(), 0, 4)
				.Add(fAudio->CreateTextViewLayoutItem(), 1, 4)
				.Add(fKeyint->CreateLabelLayoutItem(), 0, 5)
				.Add(fKeyint->CreateTextViewLayoutItem(), 1, 5)
				.Add(fPreset->CreateLabelLayoutItem(), 0, 6)
				.Add(fPreset->CreateMenuBarLayoutItem(), 1, 6)
			.End()
			.Add(note1)
			.Add(note2)
			.Add(note3)
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(cancel)
				.Add(save)
			.End();

		for (int i = 0; i < kStreamServiceCount; i++) {
			fKeys[i] = config.keys[i];
			fServers[i] = config.servers[i];
		}
		fShown = config.service;
		FillFields();
		CenterOnScreen();
	}

	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgStreamService:
				// what is in the fields belongs to the service that was shown; the new one brings its own
				RememberFields();
				fShown = Service();
				FillFields();
				break;
			case kMsgStreamSave:
			{
				BMessage saved(kMsgStreamSaved);
				RememberFields();
				saved.AddInt32("service", Service());
				for (int i = 0; i < kStreamServiceCount; i++) {
					char name[16];
					snprintf(name, sizeof(name), "server%d", i);
					saved.AddString(name, fServers[i].c_str());
					snprintf(name, sizeof(name), "key%d", i);
					saved.AddString(name, fKeys[i].c_str());
				}
				saved.AddInt32("video", fVideo->Value());
				saved.AddInt32("audio", fAudio->Value());
				BMenuItem* marked = fPresetMenu->FindMarked();
				saved.AddInt32("preset", marked != NULL ? fPresetMenu->IndexOf(marked) : 2);
				saved.AddInt32("keyint", fKeyint->Value());
				fOwner.SendMessage(&saved);
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	int Service() const
	{
		BMenuItem* marked = fServiceMenu->FindMarked();
		return marked != NULL ? fServiceMenu->IndexOf(marked) : 0;
	}

	// What the fields hold is kept for the service that is shown.
	void RememberFields()
	{
		fKeys[fShown] = fKey->Text();
		if (fShown >= kServiceCustomRtmp)
			fServers[fShown] = fServer->Text();
	}

	// The fields show the key (and the address) of the service that is shown. The fixed services show their address and cannot
	// change it; the others have a box to fill in.
	void FillFields()
	{
		fKey->SetText(fKeys[fShown].c_str());
		if (fShown < kServiceCustomRtmp) {
			fServer->SetText(kStreamServices[fShown].base);
			fServer->SetEnabled(false);
			fKey->SetEnabled(true);
		} else {
			fServer->SetText(fServers[fShown].c_str());
			fServer->SetEnabled(true);
			fKey->SetEnabled(fShown != kServiceRtsp);
			fServer->SetLabel(fShown == kServiceRtsp ? "RTSP address:" : "Server address:");
		}
	}

	BMessenger		fOwner;
	BPopUpMenu*		fServiceMenu;
	BMenuField*		fService;
	BTextControl*	fServer;
	BTextControl*	fKey;
	BSpinner*		fVideo;
	BSpinner*		fAudio;
	BSpinner*		fKeyint;
	BPopUpMenu*		fPresetMenu;
	BMenuField*		fPreset;
	std::string		fKeys[6];		// by service, as edited in this window
	std::string		fServers[6];
	int				fShown;			// the service that the fields show
};


// ---------------------------------------------------------------------------------------------------------------
// The Nodes tab: the programs and devices that make sound, hrecord, the Mixer and the sound hardware as boxes, and the
// connections between them as lines (what Cortex shows). A box can be selected for what can be done with it.
// ---------------------------------------------------------------------------------------------------------------

static const int32 kHrecordKey = -100;			// hrecord's taps and its playback, together
static const int32 kSinkOffset = 1000000;		// the output half of a device that is also an input (the sound hardware)

struct GraphNode {
	int32			key = 0;
	media_node_id	id = -1;					// the Media Kit node (-1 for hrecord)
	std::string		name;
	uint64			kind = 0;
	bool			hrecord = false;
	bool			mixer = false;
	bool			sink = false;
	bool			device = false;				// a source that is hardware: the microphone, the line in
	bool			app = false;				// a program that makes sound
	int				level = 0;
	BRect			frame;
};

struct FreeOutput {
	int32			key = 0;
	media_output	output;
};

struct GraphEdge {
	int32			from = 0;
	int32			to = 0;
	media_output	output;
	media_input		input;
	bool			throughHrecord = false;		// a program that hrecord has tapped
};

enum {
	kMsgNodeCommand		= 'ncm!',		// a command for hrecord, from the Nodes tab
	kMsgNodeRefresh		= 'nrf!',
	kMsgNodeRoute		= 'nrt!',
	kMsgNodeTakeOff		= 'nto!',
	kMsgNodePutBack		= 'npb!',
	kMsgNodeDisconnect	= 'ndc!',
	kMsgNodeReconnect	= 'nrc!',
	kMsgNodeSelectEdge	= 'nse!',
	kMsgHubToggle		= 'hbt!',
	kMsgHubMode			= 'hbm!'
};

class NodesTab;

class GraphCanvas : public BView {
public:
	GraphCanvas(NodesTab* tab);
	void Draw(BRect updateRect);
	void MouseDown(BPoint where);
private:
	NodesTab*	fTab;
};


class NodesTab : public BView {
public:
	NodesTab(BMessenger window, StreamConfig* config)
		:
		BView("Nodes", 0),
		fWindow(window),
		fConfig(config),
		fHubActive(false),
		fSelected(0),
		fSelectedEdge(-1),
		fEngineReady(false),
		fRunner(NULL)
	{
		SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		fCanvas = new GraphCanvas(this);
		fCanvas->SetExplicitMinSize(BSize(400, 200));
		fCanvas->SetExplicitPreferredSize(BSize(900, 280));

		fTitle = new Label("title", "Select a box to see what can be done with it.", theme::kWindow, theme::kText, false,
			B_SIZE_UNLIMITED);
		BMessage* routeMessage = new BMessage(kMsgNodeRoute);
		fStereo = new FlatCheck("stereo", "Stereo", new BMessage(*routeMessage), theme::kWindow);
		fLeft = new FlatCheck("left", "Left", new BMessage(*routeMessage), theme::kWindow);
		fRight = new FlatCheck("right", "Right", new BMessage(*routeMessage), theme::kWindow);
		delete routeMessage;
		fMixLabel = new Label("mixlabel", "In the mix as:", theme::kWindow);
		fTakeOff = new FlatButton("takeoff", "Take off the mix", new BMessage(kMsgNodeTakeOff), theme::kWindow);
		fPutBack = new FlatButton("putback", "Put on the mix again", new BMessage(kMsgNodePutBack), theme::kWindow);
		fConnections = new BListView("connections");
		fConnections->SetSelectionMessage(new BMessage(kMsgNodeSelectEdge));
		fConnections->SetViewColor(theme::kInset);
		fConnections->SetLowColor(theme::kInset);
		BScrollView* connectionsScroll = new BScrollView("connectionsscroll", fConnections, 0, false, true, B_NO_BORDER);
		connectionsScroll->SetViewColor(theme::kWindow);
		connectionsScroll->SetExplicitMinSize(BSize(200, 70));
		fDisconnect = new FlatButton("disconnect", "Disconnect", new BMessage(kMsgNodeDisconnect), theme::kWindow);
		fReconnectMenu = new BPopUpMenu("reconnect");
		fReconnectField = new FlatMenu("reconnectfield", "Connect it to:", fReconnectMenu, theme::kWindow);
		fReconnect = new FlatButton("reconnect", "Connect", new BMessage(kMsgNodeReconnect), theme::kWindow);
		fNote = new Label("note", "", theme::kWindow, theme::kDimText, false, B_SIZE_UNLIMITED);
		fHubShare = new FlatCheck("hubshare", "Share this input with programs (hrecord's input hub)",
			new BMessage(kMsgHubToggle), theme::kWindow);
		fHubShare->SetExplicitMaxSize(BSize(fHubShare->MinSize().width, B_SIZE_UNSET));
		fHubMenu = new BPopUpMenu("hubmode");
		const char* hubChoices[] = { "Stereo (both channels)", "Left channel", "Right channel" };
		for (int i = 0; i < 3; i++)
			fHubMenu->AddItem(new BMenuItem(hubChoices[i], new BMessage(kMsgHubMode)));
		fHubMenu->ItemAt(0)->SetMarked(true);
		fHubField = new FlatMenu("hubfield", "Input it gets from hrecord:", fHubMenu, theme::kWindow);
		// (in a row with room to spare, these stay as wide as their text)
		BControl* narrow[] = { fStereo, fLeft, fRight, fTakeOff, fPutBack };
		for (int i = 0; i < 5; i++)
			narrow[i]->SetExplicitMaxSize(BSize(narrow[i]->MinSize().width, B_SIZE_UNSET));
		SetViewColor(theme::kWindow);
		SetLowColor(theme::kWindow);
		if (!fConfig->debug)
			fHubShare->Hide();

		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(8, 8, 8, 8)
			.Add(fCanvas, 3.0f)
			.Add(fTitle)
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.Add(fHubShare)
				.Add(fHubField)
				.AddGlue()
			.End()
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.Add(fMixLabel)
				.Add(fStereo)
				.Add(fLeft)
				.Add(fRight)
				.Add(fTakeOff)
				.Add(fPutBack)
				.AddGlue()
			.End()
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.Add(connectionsScroll, 2.0f)
				.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
					.Add(fDisconnect)
					.Add(fReconnectField)
					.Add(fReconnect)
					.AddGlue()
				.End()
			.End()
			.Add(fNote);
		UpdatePanel();
	}

	void AttachedToWindow()
	{
		BView::AttachedToWindow();
		fStereo->SetTarget(this);
		fLeft->SetTarget(this);
		fRight->SetTarget(this);
		fTakeOff->SetTarget(this);
		fPutBack->SetTarget(this);
		fConnections->SetTarget(this);
		fDisconnect->SetTarget(this);
		fReconnect->SetTarget(this);
		fHubShare->SetTarget(this);
		fHubMenu->SetTargetForItems(this);
		Rebuild();
		BMessage tick(kMsgNodeRefresh);
		fRunner = new BMessageRunner(this, &tick, 1000000);
	}

	void DetachedFromWindow()
	{
		delete fRunner;
		fRunner = NULL;
	}

	// Is hrecord up (and so able to take sources into the mix, or off it)?
	void SetEngineReady(bool ready)
	{
		fEngineReady = ready;
		if (ready && fConfig->HubWanted())
			Send("hub 1\n");
		if (!ready) {
			fModes.clear();
			fDetached.clear();
			fHubSent.clear();
		}
		UpdatePanel();
	}

	const std::vector<GraphNode>& Nodes() const { return fNodes; }
	const std::vector<GraphEdge>& Edges() const { return fEdges; }
	int32 Selected() const { return fSelected; }
	int Mode(media_node_id id) const
	{
		std::map<media_node_id, int>::const_iterator found = fModes.find(id);
		return found != fModes.end() ? found->second : 0;
	}

	// "Advanced Options" was ticked or unticked: the hub's box shows or goes, and hrecord is told what holds now.
	void ApplyDebug()
	{
		if (fConfig->debug == fHubShare->IsHidden(fHubShare)) {
			if (fConfig->debug)
				fHubShare->Show();
			else
				fHubShare->Hide();
		}
		if (fEngineReady) {
			Send("hub %d\n", fConfig->HubWanted() ? 1 : 0);
			if (!fConfig->HubWanted())
				fHubSent.clear();
		}
	}

	// For testing from the command line: the same as clicking the box with this name (and its L or R port).
	void ClickByName(const char* name, int port)
	{
		for (size_t i = 0; i < fNodes.size(); i++) {
			if (fNodes[i].name == name && !fNodes[i].sink) {
				Clicked(fNodes[i].key, port);
				return;
			}
		}
	}

	// A click on a box (or on the L or R port of an input device): what it does.
	void Clicked(int32 key, int port)
	{
		fSelected = key;
		fSelectedEdge = -1;
		const GraphNode* node = Find(key);
		if (node != NULL && node->device && !node->sink && port != 0 && fEngineReady) {
			// the L or R port of an input device: record that channel of it, or stop
			int mask = Mode(node->id) ^ (port == 1 ? 2 : 4);
			ApplyRoute(*node, mask);
		}
		UpdatePanel();
		fCanvas->Invalidate();
	}

	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgNodeRefresh:
				if (IsHidden())
					break;
				Rebuild();
				break;
			case kMsgNodeRoute:
			{
				const GraphNode* node = Find(fSelected);
				if (node != NULL) {
					int mask = (fStereo->Value() == B_CONTROL_ON ? 1 : 0) | (fLeft->Value() == B_CONTROL_ON ? 2 : 0)
						| (fRight->Value() == B_CONTROL_ON ? 4 : 0);
					if (mask == 0 && !node->device) {
						// a program with nothing ticked would leave the mix: that is what the button is for, not an unticked box
						fNote->SetText("A program in the mix needs one of these ticked. Use \"Take off the mix\" to remove it.");
						UpdatePanel();
						break;
					}
					ApplyRoute(*node, mask);
				}
				break;
			}
			case kMsgNodeTakeOff:
			{
				const GraphNode* node = Find(fSelected);
				if (node != NULL)
					ApplyRoute(*node, 0);
				break;
			}
			case kMsgNodePutBack:
			{
				const GraphNode* node = Find(fSelected);
				if (node != NULL) {
					Send("attach %d\n", (int)node->id);
					fDetached.erase(node->id);
					fModes[node->id] = 1;
				}
				break;
			}
			case kMsgHubToggle:
			{
				const bool on = fHubShare->Value() == B_CONTROL_ON;
				fConfig->hubOn = on;
				fConfig->Save();
				if (fEngineReady)
					Send("hub %d\n", on ? 1 : 0);
				break;
			}
			case 'rcn!':
				fReconnectField->Invalidate();		// another node is marked in its menu
				break;
			case kMsgHubMode:
			{
				fHubField->Invalidate();		// another item is marked in its menu
				const GraphNode* node = Find(fSelected);
				BMenuItem* marked = fHubMenu->FindMarked();
				if (node != NULL && marked != NULL && fEngineReady) {
					const int mode = fHubMenu->IndexOf(marked);
					if (fHubSent.find(node->id) != fHubSent.end()) {
						Send("hubmap %d %d\n", (int)node->id, mode);
						fHubSent[node->id] = mode;
					}
					fConfig->hubModes[node->name] = mode;
					fConfig->Save();
				}
				break;
			}
			case kMsgTestAction:
			{
				// for testing from the command line: what a click on a button (or on a row of the list) would do
				const char* what = "";
				message->FindString("what", &what);
				if (strncmp(what, "edge", 4) == 0) {
					fSelectedEdge = atoi(what + 4);
					fConnections->Select(fSelectedEdge);
					UpdatePanel();
				} else if (strcmp(what, "takeoff") == 0)
					MessageReceived(new BMessage(kMsgNodeTakeOff));
				else if (strcmp(what, "putback") == 0)
					MessageReceived(new BMessage(kMsgNodePutBack));
				else if (strcmp(what, "disconnect") == 0)
					DisconnectSelected();
				else if (strcmp(what, "reconnect") == 0)
					ReconnectSelected();
				break;
			}
			case kMsgNodeSelectEdge:
				fSelectedEdge = fConnections->CurrentSelection();
				UpdatePanel();
				break;
			case kMsgNodeDisconnect:
				DisconnectSelected();
				break;
			case kMsgNodeReconnect:
				ReconnectSelected();
				break;
			default:
				BView::MessageReceived(message);
		}
	}

	// Looks at the Media Kit: which nodes there are, and what is connected to what.
	void Rebuild()
	{
		BMediaRoster* roster = BMediaRoster::Roster();
		if (roster == NULL)
			return;
		live_node_info infos[96];
		int32 count = 96;
		if (roster->GetLiveNodes(infos, &count) != B_OK)
			return;

		std::vector<GraphNode> nodes;
		std::vector<GraphEdge> edges;
		std::vector<FreeOutput> freeOutputs;
		std::set<media_node_id> hrecordIds;
		bool anyHrecord = false;
		bool hubActive = false;
		for (int32 i = 0; i < count; i++) {
			if (strncmp(infos[i].name, "hrecord ", 8) == 0) {
				hrecordIds.insert(infos[i].node.node);
				anyHrecord = true;
			}
			if (strcmp(infos[i].name, "hrecord Input") == 0)
				hubActive = true;
		}
		if (anyHrecord) {
			GraphNode node;
			node.key = kHrecordKey;
			node.name = "hrecord";
			node.hrecord = true;
			nodes.push_back(node);
		}
		for (int32 i = 0; i < count; i++) {
			const media_node& media = infos[i].node;
			if (hrecordIds.count(media.node) != 0 || (media.kind & (B_BUFFER_PRODUCER | B_BUFFER_CONSUMER)) == 0)
				continue;
			const bool producer = (media.kind & B_BUFFER_PRODUCER) != 0;
			const bool consumer = (media.kind & B_BUFFER_CONSUMER) != 0;
			const bool physicalIn = (media.kind & B_PHYSICAL_INPUT) != 0;
			const bool physicalOut = (media.kind & B_PHYSICAL_OUTPUT) != 0;
			const bool mixer = (media.kind & B_SYSTEM_MIXER) != 0;
			if (physicalIn) {
				GraphNode node;
				node.key = media.node;
				node.id = media.node;
				node.name = infos[i].name;
				node.kind = media.kind;
				node.device = true;
				nodes.push_back(node);
			}
			if (physicalOut) {
				GraphNode node;
				node.key = media.node + kSinkOffset;
				node.id = media.node;
				node.name = infos[i].name;
				node.kind = media.kind;
				node.sink = true;
				node.device = true;
				nodes.push_back(node);
			} else if (!physicalIn) {
				GraphNode node;
				node.key = media.node;
				node.id = media.node;
				node.name = infos[i].name;
				node.kind = media.kind;
				node.mixer = mixer;
				node.app = producer && !consumer && !mixer;
				nodes.push_back(node);
			}
			if (!producer)
				continue;
			media_output outputs[16];
			int32 outputCount = 0;
			if (strncmp(infos[i].name, "hrecord ", 8) != 0 && !mixer) {
				int32 freeCount = 0;
				if (roster->GetFreeOutputsFor(media, outputs, 16, &freeCount, B_MEDIA_RAW_AUDIO) == B_OK) {
					for (int32 j = 0; j < freeCount; j++) {
						FreeOutput freeOutput;
						freeOutput.key = physicalOut && !physicalIn ? media.node + kSinkOffset : (int32)media.node;
						freeOutput.output = outputs[j];
						freeOutputs.push_back(freeOutput);
					}
				}
			}
			if (roster->GetConnectedOutputsFor(media, outputs, 16, &outputCount) != B_OK)
				continue;
			const bool fromHrecord = strncmp(infos[i].name, "hrecord ", 8) == 0;
			for (int32 j = 0; j < outputCount; j++) {
				media_node_id destination = roster->NodeIDFor(outputs[j].destination.port);
				if (destination < 0)
					continue;
				GraphEdge edge;
				edge.output = outputs[j];
				edge.from = fromHrecord ? kHrecordKey : (int32)media.node;
				edge.to = hrecordIds.count(destination) != 0 ? kHrecordKey : (int32)destination;
				edge.throughHrecord = edge.to == kHrecordKey;
				edges.push_back(edge);
			}
		}
		// (the producers named "hrecord ..." are skipped above as boxes, but their connections are edges: redo them)
		for (int32 i = 0; i < count; i++) {
			if (strncmp(infos[i].name, "hrecord ", 8) != 0 || (infos[i].node.kind & B_BUFFER_PRODUCER) == 0)
				continue;
			media_output outputs[16];
			int32 outputCount = 0;
			if (roster->GetConnectedOutputsFor(infos[i].node, outputs, 16, &outputCount) != B_OK)
				continue;
			for (int32 j = 0; j < outputCount; j++) {
				media_node_id destination = roster->NodeIDFor(outputs[j].destination.port);
				if (destination < 0 || hrecordIds.count(destination) != 0)
					continue;
				GraphEdge edge;
				edge.output = outputs[j];
				edge.from = kHrecordKey;
				edge.to = (int32)destination;
				edges.push_back(edge);
			}
		}
		// the output of the Mixer goes to the output half of the hardware
		for (size_t e = 0; e < edges.size(); e++) {
			for (size_t n = 0; n < nodes.size(); n++) {
				if (nodes[n].sink && nodes[n].id == edges[e].to && edges[e].to < kSinkOffset)
					edges[e].to = nodes[n].key;
			}
		}
		fHubActive = hubActive;
		Layout(nodes, edges);
		// nothing to redraw (and the selection of a connection to keep) while it all is as it was
		std::string signature;
		for (size_t i = 0; i < nodes.size(); i++)
			signature += nodes[i].name + "#" + std::to_string(nodes[i].key) + ";";
		for (size_t i = 0; i < edges.size(); i++)
			signature += std::to_string(edges[i].from) + ">" + std::to_string(edges[i].to) + ":" + std::to_string(edges[i].output.destination.id) + ";";
		for (size_t i = 0; i < freeOutputs.size(); i++)
			signature += "free" + std::to_string(freeOutputs[i].key) + ";";
		if (signature == fSignature)
			return;
		fSignature = signature;
		fSelectedEdge = -1;
		fNodes = nodes;
		fEdges = edges;
		fFree = freeOutputs;
		// programs that are fed by the input hub get the channel they were given the last time (by name)
		for (size_t e = 0; e < fEdges.size(); e++) {
			if (!IsHubEdge(fEdges[e]))
				continue;
			const GraphNode* node = Find(fEdges[e].to);
			if (node == NULL || !fEngineReady || fHubSent.find(node->id) != fHubSent.end())
				continue;
			std::map<std::string, int>::const_iterator saved = fConfig->hubModes.find(node->name);
			const int mode = saved != fConfig->hubModes.end() ? saved->second : 0;
			if (mode != 0)
				Send("hubmap %d %d\n", (int)node->id, mode);
			fHubSent[node->id] = mode;
		}
		// programs that hrecord has tapped are in the mix as a stereo signal until told otherwise
		for (size_t e = 0; e < fEdges.size(); e++) {
			if (fEdges[e].throughHrecord && fEdges[e].from != kHrecordKey) {
				const GraphNode* node = Find(fEdges[e].from);
				if (node != NULL && node->app && fModes.find(node->id) == fModes.end())
					fModes[node->id] = 1;
			}
		}
		if (Find(fSelected) == NULL)
			fSelected = 0;
		UpdatePanel();
		fCanvas->Invalidate();
	}

private:
	const GraphNode* Find(int32 key) const
	{
		for (size_t i = 0; i < fNodes.size(); i++) {
			if (fNodes[i].key == key)
				return &fNodes[i];
		}
		return NULL;
	}

	void Send(const char* format, ...)
	{
		char text[160];
		va_list list;
		va_start(list, format);
		vsnprintf(text, sizeof(text), format, list);
		va_end(list);
		BMessage message(kMsgNodeCommand);
		message.AddString("text", text);
		fWindow.SendMessage(&message);
	}

	// Columns by how far a box is from a source; the boxes of a column one under the other.
	static void Layout(std::vector<GraphNode>& nodes, const std::vector<GraphEdge>& edges)
	{
		for (size_t i = 0; i < nodes.size(); i++)
			nodes[i].level = 0;
		for (size_t pass = 0; pass < nodes.size(); pass++) {
			bool changed = false;
			for (size_t e = 0; e < edges.size(); e++) {
				int from = -1, to = -1;
				for (size_t n = 0; n < nodes.size(); n++) {
					if (nodes[n].key == edges[e].from) from = (int)n;
					if (nodes[n].key == edges[e].to) to = (int)n;
				}
				if (from >= 0 && to >= 0 && nodes[to].level <= nodes[from].level && nodes[to].level < 6) {
					nodes[to].level = nodes[from].level + 1;
					changed = true;
				}
			}
			if (!changed)
				break;
		}
		std::map<int, int> rows;
		const float width = 160, height = 46, gapX = 70, gapY = 14;
		for (size_t i = 0; i < nodes.size(); i++) {
			const int row = rows[nodes[i].level]++;
			const float x = 16 + nodes[i].level * (width + gapX);
			const float y = 14 + row * (height + gapY);
			nodes[i].frame = BRect(x, y, x + width, y + height);
		}
	}

	void ApplyRoute(const GraphNode& node, int mask)
	{
		if (!fEngineReady || node.id < 0)
			return;
		if (node.device) {
			Send("hwin %d 0 %d %d %d\n", (int)node.id, (mask & 1) ? 1 : 0, (mask & 2) ? 1 : 0, (mask & 4) ? 1 : 0);
		} else if (mask == 0) {
			Send("detach %d\n", (int)node.id);
			fDetached.insert(node.id);
		} else {
			Send("route %d %d %d %d\n", (int)node.id, (mask & 1) ? 1 : 0, (mask & 2) ? 1 : 0, (mask & 4) ? 1 : 0);
		}
		fModes[node.id] = mask;
		UpdatePanel();
	}

	// What the box under the list is about: the controls for the selected box.
	void UpdatePanel()
	{
		const GraphNode* node = Find(fSelected);
		fConnections->SetSelectionMessage(NULL);		// (it is the list being rebuilt, not a click)
		fConnections->MakeEmpty();
		fRows.clear();
		fReconnectMenu->RemoveItems(0, fReconnectMenu->CountItems(), true);
		fNote->SetText(fEngineReady ? "" : "hrecord is not connected to the sources now: the mix cannot be changed here.");
		bool route = false, takeOff = false, putBack = false, hubShare = false, hubInput = false;
		if (node == NULL) {
			fTitle->SetText("Select a box to see what can be done with it.");
		} else {
			BString title(node->name.c_str());
			if (node->hrecord) title << " (hrecord's mix: the sources it has taken in, and its playback to the Mixer)";
			else if (node->mixer) title << " (the System Mixer)";
			else if (node->sink) title << " (output)";
			else if (node->device) title << " (input: the L and R boxes on its right side switch a channel into the mix)";
			else if (node->app) title << " (program)";
			fTitle->SetText(title.String());
			const bool tapped = IsTapped(node->key);
			if (node->device && !node->sink) {
				route = true;
			} else if (node->app) {
				if (tapped) {
					route = true;
					takeOff = true;
				} else if (fDetached.count(node->id) != 0)
					putBack = true;
			}
			if (node->device && !node->sink)
				hubShare = true;
			for (size_t e = 0; e < fEdges.size(); e++) {
				if (IsHubEdge(fEdges[e]) && fEdges[e].to == node->key)
					hubInput = true;
			}
			{
				std::map<std::string, int>::const_iterator saved = fConfig->hubModes.find(node->name);
				fHubMenu->ItemAt(saved != fConfig->hubModes.end() ? saved->second : 0)->SetMarked(true);
			}
			const int mask = Mode(node->id);
			fStereo->SetValue((mask & 1) ? B_CONTROL_ON : B_CONTROL_OFF);
			fLeft->SetValue((mask & 2) ? B_CONTROL_ON : B_CONTROL_OFF);
			fRight->SetValue((mask & 4) ? B_CONTROL_ON : B_CONTROL_OFF);
			for (size_t e = 0; e < fEdges.size(); e++) {
				if (fEdges[e].from != node->key && fEdges[e].to != node->key)
					continue;
				const GraphNode* other = Find(fEdges[e].from == node->key ? fEdges[e].to : fEdges[e].from);
				BString text(fEdges[e].from == node->key ? "to " : "from ");
				text << (other != NULL ? other->name.c_str() : "?");
				if (fEdges[e].output.format.type == B_MEDIA_RAW_AUDIO) {
					text << "  (" << (int)fEdges[e].output.format.u.raw_audio.channel_count << " ch, "
						<< (int)fEdges[e].output.format.u.raw_audio.frame_rate << " Hz)";
				}
				fConnections->AddItem(new SourceItem(0, text.String()));
				fRows.push_back(std::make_pair(false, (int)e));
			}
			for (size_t f = 0; f < fFree.size(); f++) {
				if (fFree[f].key != node->key)
					continue;
				fConnections->AddItem(new SourceItem(0, "output: not connected"));
				fRows.push_back(std::make_pair(true, (int)f));
			}
			// where the selected connection can go instead: a node with a free input
			if (fSelectedEdge >= 0 && fSelectedEdge < (int)fRows.size()) {
				fConnections->Select(fSelectedEdge);
				FillReconnectMenu();
			}
		}
		fConnections->SetSelectionMessage(new BMessage(kMsgNodeSelectEdge));
		if (route) fMixLabel->Show(); else fMixLabel->Hide();
		fHubShare->SetEnabled(fEngineReady);
		fHubShare->SetValue(fHubActive ? B_CONTROL_ON : B_CONTROL_OFF);
		fHubField->SetEnabled(fEngineReady && fHubActive && node != NULL && node->app);
		fStereo->SetEnabled(route && fEngineReady);
		fLeft->SetEnabled(route && fEngineReady);
		fRight->SetEnabled(route && fEngineReady);
		fTakeOff->SetEnabled(takeOff && fEngineReady);
		fPutBack->SetEnabled(putBack && fEngineReady);
		const bool haveEdge = fSelectedEdge >= 0 && fSelectedEdge < (int)fRows.size();
		bool edgeOfHrecord = false, isFree = false;
		if (haveEdge) {
			isFree = fRows[fSelectedEdge].first;
			if (!isFree)
				edgeOfHrecord = fEdges[fRows[fSelectedEdge].second].from == kHrecordKey;
		}
		fDisconnect->SetEnabled(haveEdge && !edgeOfHrecord && !isFree);
		fReconnectField->SetEnabled(haveEdge && !edgeOfHrecord && fReconnectMenu->CountItems() > 0);
		fReconnect->SetEnabled(haveEdge && !edgeOfHrecord && fReconnectMenu->CountItems() > 0);
		fHubField->Invalidate();		// (their menus' marked items may have changed)
		fReconnectField->Invalidate();
	}

	static bool IsHubEdge(const GraphEdge& edge)
	{
		return edge.from == kHrecordKey && strncmp(edge.output.name, "input", 5) == 0;
	}

	bool IsTapped(int32 key) const
	{
		for (size_t e = 0; e < fEdges.size(); e++) {
			if (fEdges[e].from == key && fEdges[e].to == kHrecordKey)
				return true;
		}
		return false;
	}

	void FillReconnectMenu()
	{
		BMediaRoster* roster = BMediaRoster::Roster();
		if (roster == NULL)
			return;
		live_node_info infos[96];
		int32 count = 96;
		if (roster->GetLiveNodes(infos, &count, NULL, NULL, NULL, B_BUFFER_CONSUMER) != B_OK)
			return;
		for (int32 i = 0; i < count; i++) {
			if (strncmp(infos[i].name, "hrecord ", 8) == 0)
				continue;
			media_input inputs[8];
			int32 inputCount = 0;
			if (roster->GetFreeInputsFor(infos[i].node, inputs, 8, &inputCount, B_MEDIA_RAW_AUDIO) != B_OK || inputCount < 1)
				continue;
			BMessage* message = new BMessage('rcn!');
			message->AddInt32("node", infos[i].node.node);
			BMenuItem* item = new BMenuItem(infos[i].name, message);
			fReconnectMenu->AddItem(item);
		}
		fReconnectMenu->SetTargetForItems(this);
		if (fReconnectMenu->CountItems() > 0)
			fReconnectMenu->ItemAt(0)->SetMarked(true);
	}

	void DisconnectSelected()
	{
		if (fSelectedEdge < 0 || fSelectedEdge >= (int)fRows.size() || fRows[fSelectedEdge].first)
			return;
		const GraphEdge edge = fEdges[fRows[fSelectedEdge].second];
		BMediaRoster* roster = BMediaRoster::Roster();
		if (roster == NULL)
			return;
		const media_node_id destination = roster->NodeIDFor(edge.output.destination.port);
		status_t error = roster->Disconnect(edge.output.node.node, edge.output.source, destination, edge.output.destination);
		fNote->SetText(error == B_OK ? "Disconnected. Its output is in the list: connect it to another node there."
			: "That connection could not be disconnected.");
		fSignature = "";
		Rebuild();
	}

	// Connects the selected output (a connection is disconnected first) to the node chosen in the menu.
	void ReconnectSelected()
	{
		if (fSelectedEdge < 0 || fSelectedEdge >= (int)fRows.size())
			return;
		BMenuItem* marked = fReconnectMenu->FindMarked();
		if (marked == NULL || marked->Message() == NULL)
			return;
		int32 target = 0;
		if (marked->Message()->FindInt32("node", &target) != B_OK)
			return;
		const bool wasFree = fRows[fSelectedEdge].first;
		media_output output = wasFree ? fFree[fRows[fSelectedEdge].second].output : fEdges[fRows[fSelectedEdge].second].output;
		BMediaRoster* roster = BMediaRoster::Roster();
		if (roster == NULL)
			return;
		media_node targetNode;
		if (roster->GetNodeFor(target, &targetNode) != B_OK)
			return;
		media_input inputs[8];
		int32 inputCount = 0;
		if (roster->GetFreeInputsFor(targetNode, inputs, 8, &inputCount, B_MEDIA_RAW_AUDIO) != B_OK || inputCount < 1) {
			roster->ReleaseNode(targetNode);
			fNote->SetText("That node has no free input.");
			return;
		}
		const media_destination oldDestination = output.destination;
		media_output oldOutput = output;
		if (!wasFree) {
			const media_node_id oldNode = roster->NodeIDFor(output.destination.port);
			roster->Disconnect(output.node.node, output.source, oldNode, output.destination);
			snooze(100000);
		}
		media_format format = output.format;
		media_output newOutput;
		media_input newInput;
		status_t error = roster->Connect(output.source, inputs[0].destination, &format, &newOutput, &newInput);
		if (error != B_OK && !wasFree) {
			// back where it was
			format = oldOutput.format;
			roster->Connect(oldOutput.source, oldDestination, &format, &newOutput, &newInput);
			fNote->SetText("It could not be connected there; it is back where it was.");
		} else if (error != B_OK)
			fNote->SetText("It could not be connected there.");
		else
			fNote->SetText("Connected.");
		roster->ReleaseNode(targetNode);
		fSignature = "";
		Rebuild();
	}

	BMessenger			fWindow;
	StreamConfig*		fConfig;
	bool				fHubActive;
	std::map<media_node_id, int>	fHubSent;	// what each program was told to get from the hub
	FlatCheck*			fHubShare;
	BPopUpMenu*			fHubMenu;
	FlatMenu*			fHubField;
	GraphCanvas*		fCanvas;
	std::vector<GraphNode>	fNodes;
	std::vector<GraphEdge>	fEdges;
	std::vector<FreeOutput>	fFree;			// outputs of producers that are not connected to anything
	std::vector<std::pair<bool, int> >	fRows;	// the rows of the list: (free output?, its index in fEdges or fFree)
	std::string			fSignature;
	std::map<media_node_id, int>	fModes;		// what is asked of the mix for a source: 1 stereo, 2 left, 4 right
	std::set<media_node_id>	fDetached;
	int32				fSelected;
	int					fSelectedEdge;
	bool				fEngineReady;
	BMessageRunner*		fRunner;
	Label*				fTitle;
	Label*				fMixLabel;
	FlatCheck*			fStereo;
	FlatCheck*			fLeft;
	FlatCheck*			fRight;
	FlatButton*			fTakeOff;
	FlatButton*			fPutBack;
	BListView*			fConnections;
	FlatButton*			fDisconnect;
	BPopUpMenu*			fReconnectMenu;
	FlatMenu*			fReconnectField;
	FlatButton*			fReconnect;
	Label*				fNote;
};


GraphCanvas::GraphCanvas(NodesTab* tab)
	:
	BView("graph", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fTab(tab)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
GraphCanvas::Draw(BRect)
{
	SetHighColor(theme::kInset);
	FillRect(Bounds());
	SetHighColor(theme::kHeader);
	StrokeRect(Bounds());

	const std::vector<GraphNode>& nodes = fTab->Nodes();
	const std::vector<GraphEdge>& edges = fTab->Edges();
	SetPenSize(2.0f);
	for (size_t e = 0; e < edges.size(); e++) {
		const GraphNode* from = NULL;
		const GraphNode* to = NULL;
		for (size_t n = 0; n < nodes.size(); n++) {
			if (nodes[n].key == edges[e].from) from = &nodes[n];
			if (nodes[n].key == edges[e].to) to = &nodes[n];
		}
		if (from == NULL || to == NULL)
			continue;
		SetHighColor(255, 255, 255);
		BPoint start(from->frame.right, (from->frame.top + from->frame.bottom) / 2);
		BPoint end(to->frame.left, (to->frame.top + to->frame.bottom) / 2);
		StrokeLine(start, end);
		FillEllipse(start, 3, 3);
		FillEllipse(end, 3, 3);
	}
	SetPenSize(1.0f);

	font_height fh;
	be_plain_font->GetHeight(&fh);
	for (size_t n = 0; n < nodes.size(); n++) {
		const GraphNode& node = nodes[n];
		rgb_color fill = make_color(225, 225, 235);
		if (node.hrecord) fill = make_color(190, 230, 195);
		else if (node.mixer) fill = make_color(245, 215, 170);
		else if (node.sink) fill = make_color(215, 215, 215);
		else if (node.device) fill = make_color(225, 205, 240);
		else if (node.app) fill = make_color(200, 220, 245);
		SetHighColor(fill);
		FillRoundRect(node.frame, 6, 6);
		SetLowColor(fill);		// what the text on it is smoothed against
		SetHighColor(node.key == fTab->Selected() ? make_color(220, 60, 50) : make_color(70, 70, 90));
		SetPenSize(node.key == fTab->Selected() ? 3.0f : 1.0f);
		StrokeRoundRect(node.frame, 6, 6);
		SetPenSize(1.0f);
		SetHighColor(0, 0, 0);
		BString label(node.name.c_str());
		TruncateString(&label, B_TRUNCATE_END, node.frame.Width() - 12 - (node.device && !node.sink ? 22 : 0));
		DrawString(label.String(), BPoint(node.frame.left + 6, node.frame.top + fh.ascent + 5));
		if (node.device && !node.sink) {
			// its left and right channel, as ports on its right side: filled when that channel is in the mix
			const int mask = fTab->Mode(node.id);
			for (int side = 0; side < 2; side++) {
				BRect port(node.frame.right - 20, node.frame.top + 4 + side * 18, node.frame.right - 4, node.frame.top + 18 + side * 18);
				SetHighColor(((side == 0 && (mask & 2)) || (side == 1 && (mask & 4))) ? make_color(60, 160, 80) : make_color(250, 250, 250));
				FillRect(port);
				SetHighColor(70, 70, 90);
				StrokeRect(port);
				SetHighColor(0, 0, 0);
				DrawString(side == 0 ? "L" : "R", BPoint(port.left + 4, port.bottom - 3));
			}
		}
		if (node.hrecord || node.app) {
			SetHighColor(90, 90, 110);
			const char* sub = node.hrecord ? "the mix" : "program";
			DrawString(sub, BPoint(node.frame.left + 6, node.frame.bottom - 6));
		}
	}
	if (nodes.empty()) {
		SetHighColor(theme::kText);
		SetLowColor(theme::kInset);
		DrawString("No nodes to show.", BPoint(16, 24));
	}
}


void
GraphCanvas::MouseDown(BPoint where)
{
	const std::vector<GraphNode>& nodes = fTab->Nodes();
	for (size_t n = 0; n < nodes.size(); n++) {
		if (!nodes[n].frame.Contains(where))
			continue;
		int port = 0;
		if (nodes[n].device && !nodes[n].sink && where.x >= nodes[n].frame.right - 22) {
			const float mid = nodes[n].frame.top + 4 + 15;
			port = where.y < mid ? 1 : 2;
		}
		fTab->Clicked(nodes[n].key, port);
		return;
	}
	fTab->Clicked(0, 0);
}


// ---------------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------------
// The master equalizer's window. It tells the mixer what is set; the mixer keeps it and tells hrecord.
// ---------------------------------------------------------------------------------------------------------------

class EqWindow : public BWindow {
public:
	EqWindow(const BMessenger& mixer, const StreamConfig& config, BRect over)
		:
		BWindow(BRect(0, 0, 100, 100), "hRecord Master EQ", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS),
		fMixer(mixer)
	{
		Panel* background = new Panel("eq", theme::kWindow);
		fEnable = new FlatCheck("enable", "Enable Equalizer", new BMessage(kMsgEqEnable), theme::kWindow);
		fEnable->SetValue(config.eqOn ? B_CONTROL_ON : B_CONTROL_OFF);
		fEnable->SetExplicitMaxSize(BSize(fEnable->MinSize().width, B_SIZE_UNSET));

		fPresets = new BPopUpMenu("Custom");
		for (int i = 0; i < kEqPresets; i++) {
			BMessage* message = new BMessage(kMsgEqPreset);
			message->AddInt32("preset", i);
			fPresets->AddItem(new BMenuItem(kEqPresetNames[i], message));
		}
		fPresetField = new FlatMenu("preset", "Preset:", fPresets, theme::kWindow);

		BGroupView* bands = new BGroupView(B_HORIZONTAL, 4);
		bands->SetViewColor(theme::kDock);
		bands->GroupLayout()->SetInsets(8, 8, 8, 8);
		for (int i = 0; i < kEqBands; i++) {
			BMessage* message = new BMessage(kMsgEqBand);
			message->AddInt32("band", i);
			// (half decibels)
			fFaders[i] = new Fader("band", message, (int32)(-kEqRange * 2), (int32)(kEqRange * 2), 0, 2, true, theme::kDock);
			fFaders[i]->SetValue((int32)lroundf(config.eqBands[i] * 2));
			fValues[i] = new Label("value", "", theme::kDock, theme::kDimText, true, 36);
			BGroupView* column = new BGroupView(B_VERTICAL, 4);
			column->SetViewColor(theme::kDock);
			BLayoutBuilder::Group<>(column, B_VERTICAL, 4)
				.Add(fValues[i])
				.AddGroup(B_HORIZONTAL, 0)
					.AddGlue()
					.Add(fFaders[i])
					.AddGlue()
				.End()
				.Add(new Label("frequency", kEqFrequencyLabels[i], theme::kDock, theme::kText, true, 36));
			bands->AddChild(column);
		}

		BLayoutBuilder::Group<>(background, B_VERTICAL, 8)
			.SetInsets(10, 10, 10, 10)
			.AddGroup(B_HORIZONTAL, 16)
				.Add(fEnable)
				.Add(fPresetField)
				.AddGlue()
			.End()
			.Add(bands)
			.Add(new Label("note", "It shapes the whole mix, ahead of the limiter. A right click on a band puts it back to 0 dB.",
				theme::kWindow, theme::kDimText));
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(background);
		ShowValues();

		// over the mixer's middle
		Layout(true);
		const BSize size = background->PreferredSize();
		ResizeTo(size.width, size.height);
		CenterIn(over);
		MoveOnScreen();
		// the controls send to this window
		fEnable->SetTarget(this);
		fPresets->SetTargetForItems(this);
		for (int i = 0; i < kEqBands; i++)
			fFaders[i]->SetTarget(this);
	}

	// (a program in the background has nowhere to bring a minimized window back from)
	void Minimize(bool minimize)
	{
		if (minimize)
			PostMessage(B_QUIT_REQUESTED);
	}

	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kTrayShow:		// (the EQ button again)
				Activate();
				break;
			case kMsgEqEnable:
				Tell();
				break;
			case kMsgEqBand:
				ShowValues();
				Tell();
				break;
			case kMsgEqPreset:
			{
				int32 preset;
				if (message->FindInt32("preset", &preset) != B_OK || preset < 0 || preset >= kEqPresets)
					break;
				for (int i = 0; i < kEqBands; i++)
					fFaders[i]->SetValue((int32)lroundf(kEqPresetValues[preset][i] * 2));
				fEnable->SetValue(B_CONTROL_ON);		// picking a preset means it is wanted
				ShowValues();
				Tell();
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void ShowValues()
	{
		for (int i = 0; i < kEqBands; i++) {
			char text[16];
			const float gain = fFaders[i]->Value() / 2.0f;
			if (gain == 0)
				snprintf(text, sizeof(text), "0");
			else
				snprintf(text, sizeof(text), "%+.1f", gain);
			fValues[i]->SetText(text);
		}
		// the menu shows the preset the bands are, if they are one
		for (int preset = 0; preset < kEqPresets; preset++) {
			bool same = true;
			for (int i = 0; i < kEqBands; i++)
				same &= fFaders[i]->Value() == (int32)lroundf(kEqPresetValues[preset][i] * 2);
			fPresets->ItemAt(preset)->SetMarked(same);
		}
		fPresetField->Invalidate();
	}

	void Tell()
	{
		BMessage message(kMsgEqChanged);
		message.AddBool("on", fEnable->Value() == B_CONTROL_ON);
		for (int i = 0; i < kEqBands; i++)
			message.AddFloat("band", fFaders[i]->Value() / 2.0f);
		fMixer.SendMessage(&message);
	}

	BMessenger		fMixer;
	FlatCheck*		fEnable;
	BPopUpMenu*		fPresets;
	FlatMenu*		fPresetField;
	Fader*			fFaders[kEqBands];
	Label*			fValues[kEqBands];
};


class MixerWindow : public BWindow {
public:
	MixerWindow()
		:
		BWindow(BRect(80, 60, 1060, 880), "hRecord Mixer", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
		fRecording(false),
		fStopping(false),
		fSeconds(0),
		fTick(NULL)
	{
		fStreamConfig.Load();

		// the controls
		fRecord = new FlatButton("record", "Start Recording", new BMessage(kMsgRecord), theme::kDock, theme::kAccent);
		fStream = new FlatCheck("stream", "Stream live", new BMessage(kMsgStreamToggle), theme::kDock);
		fStreamSettings = new FlatButton("streamsettings", "Stream settings" B_UTF8_ELLIPSIS,
			new BMessage(kMsgStreamConfigure), theme::kDock);
		static const char* const kQualities[] = { "Low", "Medium", "High" };
		fQuality = new Segments("quality", kQualities, 3, 2, theme::kDock);		// High: the default
		fRealtime = new FlatCheck("realtime", "Real-time (low latency)", NULL, theme::kDock);
		fShowPreview = new FlatCheck("showpreview", "Show preview", new BMessage(kMsgPreviewToggle), theme::kDock);
		fShowPreview->SetValue(fStreamConfig.preview ? B_CONTROL_ON : B_CONTROL_OFF);
		fShowPreview->SetToolTip("The preview takes some processor time.");
		fShowLog = new FlatCheck("showlog", "Show log", new BMessage(kMsgShowLog), theme::kDock);
		fShowLog->SetValue(fStreamConfig.showLog ? B_CONTROL_ON : B_CONTROL_OFF);
		fDebug = new FlatCheck("debug", "Advanced Options", new BMessage(kMsgDebug), theme::kDock);
		fDebug->SetValue(fStreamConfig.debug ? B_CONTROL_ON : B_CONTROL_OFF);
		fDebug->SetToolTip("Shows the Nodes page, and the settings for offering the mix and sharing the input with other "
			"programs, which are on otherwise.");

		Panel* controls = new Panel("controls", theme::kDock);
		BLayoutBuilder::Group<>(controls, B_VERTICAL, 6)
			.SetInsets(8, 8, 8, 8)
			.Add(fRecord)
			.Add(fStream)
			.Add(fStreamSettings)
			.Add(new Label("qualitytitle", "Screen quality", theme::kDock, theme::kDimText))
			.Add(fQuality)
			.Add(fRealtime)
			.Add(fShowPreview)
			.Add(fShowLog)
			.Add(fDebug)
			.AddGlue();

		// the sources: what the video shows
		fSources = new BListView("sources", B_SINGLE_SELECTION_LIST);
		fSources->SetViewColor(theme::kInset);
		fSources->SetLowColor(theme::kInset);
		fSources->SetSelectionMessage(new BMessage(kMsgSource));
		fSourceScroll = new BScrollView("sourcescroll", fSources, 0, false, true, B_NO_BORDER);
		fSourceScroll->SetViewColor(theme::kDock);
		fSourceScroll->SetExplicitMinSize(BSize(210, 120));
		Panel* sources = new Panel("sourcespanel", theme::kDock);
		BLayoutBuilder::Group<>(sources, B_VERTICAL, 0)
			.SetInsets(6, 6, 6, 6)
			.Add(fSourceScroll);
		fSelectedSource = fStreamConfig.audioOnly ? kSourceAudioOnly : kSourceDesktop;
		RefreshSources();

		// the audio mixer: a strip for each program, and the whole mix
		fStripsGroup = new Panel("strips", theme::kDock);
		BGroupLayout* stripsLayout = new BGroupLayout(B_HORIZONTAL, 2);
		fStripsGroup->SetLayout(stripsLayout);
		fStripsGroup->SetExplicitMinSize(BSize(236, B_SIZE_UNSET));
		fHint = new Label("hint", "Nothing playing yet.", theme::kDock, theme::kDimText, true, 220);
		stripsLayout->AddView(fHint);
		stripsLayout->AddItem(BSpaceLayoutItem::CreateGlue());

		fMixGain = new Fader("mixgain", new BMessage(kMsgMixGain), -24, 24, 0, 2, true, theme::kHeader);
		fMixGainText = new Label("mixgaintext", "+0.0 dB", theme::kHeader, theme::kDimText, true, 84);
		fPostMeter = new StereoMeter("post", kMeterMin, 6.0f);
		fPostMeter->SetViewColor(theme::kHeader);
		fPostMeter->SetToolTip("The level of the mix, after the limiter.");
		fLimiter = new FlatCheck("limiter", "Limiter", new BMessage(kMsgLimiter), theme::kHeader);
		fLimiter->SetValue(B_CONTROL_ON);
		fCeiling = new Fader("ceiling", new BMessage(kMsgCeiling), -24, 0, -2, 1, false, theme::kHeader);
		fCeiling->SetExplicitMinSize(BSize(130, 16));
		fCeilingText = new Label("ceilingtext", "Ceiling: -1.0 dB", theme::kHeader, theme::kDimText);
		fRelease = new Fader("release", new BMessage(kMsgRelease), 1, 50, 8, 1, false, theme::kHeader);
		fReleaseText = new Label("releasetext", "Release: 80 ms", theme::kHeader, theme::kDimText);
		fReduction = new LevelMeter("reduction", false, 0.0f, 12.0f, true);
		fEqButton = new FlatButton("eqbutton", "EQ", new BMessage(kMsgEqOpen), theme::kHeader,
			fStreamConfig.eqOn ? theme::kAccent : theme::kButton);
		fEqButton->SetToolTip("The master equalizer of the mix.");
		fReductionText = new Label("reductiontext", "Gain reduction: 0.0 dB", theme::kHeader, theme::kDimText);
		fReductionText->SetExplicitMinSize(BSize(fReductionText->PreferredSize().width + 16, B_SIZE_UNSET));

		fMixHub = new FlatCheck("mixhub", "Offer the mix to programs that record", new BMessage(kMsgMixHub), theme::kHeader);
		fMixHub->SetToolTip("Programs that record (Ardour) can take what is heard, with the effects of the programs that play "
			"(rakarrack), as an input called \"hrecord Mix\".");
		fMixHub->SetValue(fStreamConfig.mixHubOn ? B_CONTROL_ON : B_CONTROL_OFF);
		fMixHubLeaveOut = new FlatCheck("mixhubleaveout", "Leave out the recording program's own sound",
			new BMessage(kMsgMixHubSelf), theme::kHeader);
		fMixHubLeaveOut->SetToolTip("A program that records the mix does not get what it plays itself (Ardour recording a "
			"second track does not record the first one again). What you hear is not changed.");
		fMixHubLeaveOut->SetValue(fStreamConfig.mixHubLeaveOut ? B_CONTROL_ON : B_CONTROL_OFF);
		fMixHubLeaveOut->SetEnabled(fStreamConfig.mixHubOn);
		fDebugBox = new Panel("debugbox", theme::kHeader);
		BLayoutBuilder::Group<>(fDebugBox, B_VERTICAL, 4)
			.Add(fMixHub)
			.Add(fMixHubLeaveOut);
		if (!fStreamConfig.debug)
			fDebugBox->Hide();

		Panel* master = new Panel("master", theme::kHeader);
		BLayoutBuilder::Group<>(master, B_VERTICAL, 4)
			.SetInsets(8, 4, 8, 6)
			.AddGroup(B_HORIZONTAL, 12)
				.AddGroup(B_VERTICAL, 4)
					.Add(new Label("mixtitle", "Master", theme::kHeader, theme::kText, true, 84))
					.Add(fMixGainText)
					.AddGroup(B_HORIZONTAL, 6)
						.AddGlue()
						.Add(fPostMeter)
						.Add(fMixGain)
						.AddGlue()
					.End()
				.End()
				.AddGroup(B_VERTICAL, 4)
					.Add(fLimiter)
					.Add(fCeilingText)
					.Add(fCeiling)
					.Add(fReleaseText)
					.Add(fRelease)
					.Add(fReductionText)
					.Add(fReduction)
					.AddStrut(4)
					.Add(fEqButton)
					.AddGlue()
				.End()
			.End()
			.Add(fDebugBox);

		Panel* mixer = new Panel("mixer", theme::kDock);
		BLayoutBuilder::Group<>(mixer, B_HORIZONTAL, 6)
			.SetInsets(6, 6, 6, 6)
			.Add(fStripsGroup, 1.0f)
			.Add(master, 0.0f);
		mixer->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
		fStripsGroup->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
		controls->SetExplicitMaxSize(BSize(260, B_SIZE_UNLIMITED));

		fPreviewView = new PreviewView();
		fTime = new Label("time", "REC 00:00", theme::kWindow, theme::kDimText, false, 90);
		fSourceName = new Label("sourcename", "", theme::kWindow, theme::kDimText);
		fStatus = new Label("status", "Ready.", theme::kWindow, theme::kText, false, B_SIZE_UNLIMITED);
		fLog = new BTextView("log");
		fLog->MakeEditable(false);
		fLog->SetWordWrap(true);
		fLog->SetViewColor(theme::kInset);
		fLog->SetLowColor(theme::kInset);
		fLog->SetFontAndColor(be_plain_font, B_FONT_ALL, &theme::kText);
		BScrollView* logScroll = new BScrollView("logscroll", fLog, 0, false, true, B_NO_BORDER);
		fLogScroll = logScroll;
		logScroll->SetViewColor(theme::kWindow);
		logScroll->SetExplicitMinSize(BSize(200, 70));
		logScroll->SetExplicitPreferredSize(BSize(400, 90));

		Panel* mainTab = new Panel("Main", theme::kWindow);
		BLayoutBuilder::Group<>(mainTab, B_VERTICAL, 6)
			.SetInsets(6, 6, 6, 6)
			.Add(fPreviewView, 10.0f)
			.Add(fSourceName)
			.AddGroup(B_HORIZONTAL, 6, 0.0f)
				.Add(MakeDock("Sources", sources), 1.0f)
				.Add(MakeDock("Audio Mixer", mixer), 2.0f)
				.Add(MakeDock("Controls", controls), 0.0f)
			.End()
			.AddGroup(B_HORIZONTAL, 6, 0.0f)
				.Add(fStatus)
				.Add(fTime)
			.End()
			.Add(logScroll, 0.0f);

		// two pages: the mixer, and the nodes that make sound and how they are connected
		fNodes = new NodesTab(BMessenger(this), &fStreamConfig);
		fMixerButton = new FlatButton("showmixer", "Mixer", new BMessage(kMsgShowMixer), theme::kWindow, theme::kAccent);
		fNodesButton = new FlatButton("shownodes", "Nodes", new BMessage(kMsgShowNodes), theme::kWindow);
		Panel* bar = new Panel("bar", theme::kWindow);
		BLayoutBuilder::Group<>(bar, B_HORIZONTAL, 4)
			.SetInsets(6, 6, 6, 0)
			.Add(fMixerButton)
			.Add(fNodesButton)
			.AddGlue();
		fPageBar = bar;
		if (!fStreamConfig.debug)
			bar->Hide();		// there is only the mixer then
		BView* pages = new BView("pages", 0);
		fPages = new BCardLayout();
		pages->SetLayout(fPages);
		fPages->AddView(mainTab);
		fNodes->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
		mainTab->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
		pages->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
		fPages->AddView(fNodes);
		fPages->SetVisibleItem((int32)0);
		pages->SetViewColor(theme::kWindow);
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.SetInsets(0, 0, 0, 0)
			.Add(bar, 0.0f)
			.Add(pages);

		RefreshRealtime();
		RefreshStream();
		if (!fStreamConfig.showLog)
			fLogScroll->Hide();
		if (!fStreamConfig.preview)
			fPreviewView->SetPicture(NULL, "The preview is off.");
		PostMessage(kMsgStartEngine);

		// a roomy window to begin with, as far as the screen has room for it
		const BRect screen = BScreen(this).Frame();
		ResizeTo(std::min(1080.0f, screen.Width() - 100), std::min(860.0f, screen.Height() - 100));

		fPreview.source = fSelectedSource;
		fPreview.enabled = fStreamConfig.preview;
		fPreview.Start(BMessenger(this));
		BMessage refresh(kMsgSourceList);
		fSourceRunner = new BMessageRunner(this, &refresh, 1000000);
		ShowSourceName();
		// the item in the Deskbar's tray, made anew (one left by an earlier run may be of another version)
		BDeskbar deskbar;
		if (deskbar.HasItem(HRECORD_TRAY_NAME))
			deskbar.RemoveItem(HRECORD_TRAY_NAME);
		AddTrayItem();
	}

	// A program in the background has no place in the Deskbar to bring a minimized window back from: minimizing hides it to
	// the tray instead.
	void Minimize(bool minimize)
	{
		if (!minimize) {
			BWindow::Minimize(false);
			return;
		}
		if (!IsHidden()) {
			CloseEqWindow();
			Hide();
			UpdatePreviewState();
		}
	}

	bool QuitRequested()
	{
		if (fRecording) {
			BAlert* alert = new BAlert("quit", "A recording is running. Stop it and quit?", "Cancel", "Stop and quit", NULL,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() != 1)
				return false;
		}
		delete fSourceRunner;
		fSourceRunner = NULL;
		BDeskbar deskbar;
		if (deskbar.HasItem(HRECORD_TRAY_NAME))
			deskbar.RemoveItem(HRECORD_TRAY_NAME);
		fPreview.Stop();
		if (fEngine.Running()) {
			// hrecord ends the recording (if there is one) and puts every program back on the Mixer, which takes a moment
			fQuitting = true;
			fEngine.Stop();
			for (int i = 0; i < 200 && !fEngine.HasEnded(); i++)
				snooze(100000);
			fEngine.KillNow();
		}
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgShowLog:
			{
				// the window gives back or takes the room the log has
				const bool show = fShowLog->Value() == B_CONTROL_ON;
				fStreamConfig.showLog = show;
				fStreamConfig.Save();
				if (show != !fLogScroll->IsHidden()) {
					if (show) {
						fLogScroll->Show();
						ResizeBy(0, fLogHeight);
					} else {
						fLogHeight = fLogScroll->Frame().Height() + 10;
						fLogScroll->Hide();
						ResizeBy(0, -fLogHeight);
					}
				}
				break;
			}
			case kMsgShowNodes:
				if (fStreamConfig.debug)
					ShowPage(1);
				break;
			case kMsgShowMixer:
				ShowPage(0);
				break;
			case kMsgSource:
			{
				SourceItem* item = (SourceItem*)fSources->ItemAt(fSources->CurrentSelection());
				if (item == NULL) {
					SelectSource(fSelectedSource);		// something is always chosen
					break;
				}
				if (fRecording || fRefreshingSources)
					break;
				if (item->Source() == kSourceAudioOnly && StreamWanted()) {
					SetStatus("Streaming sends the screen: pick the desktop or a window.");
					SelectSource(fSelectedSource);
					break;
				}
				fSelectedSource = item->Source();
				if (fSelectedSource < 0) {
					fStreamConfig.audioOnly = fSelectedSource == kSourceAudioOnly;
					fStreamConfig.Save();
				}
				fPreview.source = fSelectedSource;
				if (fStreamConfig.preview)
					fPreviewView->SetPicture(NULL, "");		// the old picture was of something else
				ShowSourceName();
				break;
			}
			case kMsgSourceByName:
			{
				// the first source whose line holds this text
				const char* text;
				if (message->FindString("name", &text) != B_OK)
					break;
				for (int32 i = 0; i < fSources->CountItems(); i++) {
					if (((SourceItem*)fSources->ItemAt(i))->Text().IFindFirst(text) >= 0) {
						fSources->Select(i);
						break;
					}
				}
				break;
			}
			case kMsgSourceList:
				RefreshSources();
				UpdatePreviewState();
				if (++fTrayTicks >= 5) {
					// (the Deskbar may have been started again)
					fTrayTicks = 0;
					AddTrayItem();
				}
				break;
			case kTrayAsk:
			{
				BMessage state(kTrayState);
				state.AddBool("recording", fRecording);
				state.AddBool("hidden", IsHidden() || IsMinimized());
				message->SendReply(&state, (BHandler*)NULL, 100000);
				break;
			}
			case kTrayRecord:
				StartRecording();
				break;
			case kTrayStop:
				StopRecording();
				break;
			case kTrayShow:
				ShowFromTray();
				break;
			case kTrayHide:
				if (!IsHidden()) {
					CloseEqWindow();
					Hide();
					UpdatePreviewState();
				}
				break;
			case kMsgPreview:
			{
				BBitmap* bitmap = NULL;
				const char* note = "";
				int32 source = kSourceDesktop;
				message->FindPointer("bitmap", (void**)&bitmap);
				message->FindString("note", &note);
				message->FindInt32("source", &source);
				// (a note without a picture keeps the picture there is, unless the window it showed is gone)
				if (source != fSelectedSource || !fStreamConfig.preview || fSelectedSource == kSourceAudioOnly)
					delete bitmap;
				else if (bitmap != NULL)
					fPreviewView->SetPicture(bitmap, note);
				else
					fPreviewView->SetNote(note);
				fPreview.pending = false;
				UpdatePreviewState();
				break;
			}
			case kMsgPreviewToggle:
				fStreamConfig.preview = fShowPreview->Value() == B_CONTROL_ON;
				fStreamConfig.Save();
				fPreview.enabled = fStreamConfig.preview;
				ShowSourceName();
				break;
			case kMsgDebug:
				fStreamConfig.debug = fDebug->Value() == B_CONTROL_ON;
				fStreamConfig.Save();
				if (fStreamConfig.debug == fDebugBox->IsHidden(fDebugBox)) {
					if (fStreamConfig.debug)
						fDebugBox->Show();
					else
						fDebugBox->Hide();
				}
				if (fStreamConfig.debug == fPageBar->IsHidden(fPageBar)) {
					if (fStreamConfig.debug)
						fPageBar->Show();
					else {
						ShowPage(0);
						fPageBar->Hide();
					}
				}
				SendCommand("mixhubself %d\n", fStreamConfig.LeaveOutWanted() ? 0 : 1);
				SendCommand("mixhub %d\n", fStreamConfig.MixHubWanted() ? 1 : 0);
				fNodes->ApplyDebug();
				break;
			case kMsgTestAction:
			{
				const char* what = "";
				message->FindString("what", &what);
				if (strcmp(what, "debug") == 0) {
					fDebug->SetValue(fDebug->Value() == B_CONTROL_ON ? B_CONTROL_OFF : B_CONTROL_ON);
					PostMessage(kMsgDebug);
				} else if (strcmp(what, "main") == 0)
					ShowPage(0);
				else
					fNodes->MessageReceived(message);
				break;
			}
			case kMsgTestClick:
			{
				const char* name;
				int32 port = 0;
				message->FindInt32("port", &port);
				if (message->FindString("name", &name) == B_OK)
					fNodes->ClickByName(name, (int)port);
				break;
			}
			case kMsgNodeCommand:
			{
				const char* text;
				if (message->FindString("text", &text) == B_OK)
					SendCommand("%s", text);
				break;
			}
			case kMsgStartEngine:
				StartEngine();
				break;
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
				if (message->FindInt32("source", &source) == B_OK) {
					for (size_t i = 0; i < fStrips.size(); i++) {
						if (fStrips[i]->Index() == source) {
							fStrips[i]->ShowGain();
							SendCommand("gain %d %.1f\n", (int)source + 1, fStrips[i]->Gain());
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
						if (fStrips[i]->Index() == source) {
							fStrips[i]->ShowGain();
							SendCommand("pan %d %.2f\n", (int)source + 1, fStrips[i]->Pan());
						}
					}
				}
				break;
			}
			case kMsgMute:
			case kMsgSolo:
			{
				int32 source;
				if (message->FindInt32("source", &source) == B_OK) {
					for (size_t i = 0; i < fStrips.size(); i++) {
						if (fStrips[i]->Index() != source)
							continue;
						const bool on = message->what == kMsgMute ? fStrips[i]->Muted() : fStrips[i]->Soloed();
						SendCommand(message->what == kMsgMute ? "mute %d %d\n" : "solo %d %d\n", (int)source + 1, on ? 1 : 0);
					}
				}
				break;
			}
			case kMsgMixGain:
				ShowMasterValues();
				SendCommand("mixgain %.1f\n", fMixGain->Value() / 2.0f);
				break;
			case kMsgCeiling:
				ShowMasterValues();
				SendCommand("ceiling %.1f\n", fCeiling->Value() / 2.0f);
				break;
			case kMsgRelease:
				ShowMasterValues();
				SendCommand("release %d\n", (int)fRelease->Value() * 10);
				break;
			case kMsgStreamToggle:
				if (!fStreamAddon)
					RefreshStream();		// perhaps it was installed just now
				if (!fStreamAddon) {
					// streaming needs the H.264 add-on: take the user to where it is downloaded
					fStream->SetValue(B_CONTROL_OFF);
					SetStatus("Streaming needs the H.264 add-on (the hrecord_stream package). Opening its download page...");
					BString address(kAddonDownloadUrl);
					BUrl url;
					url.SetUrlString(address, true);
					url.OpenWithPreferredApplication();
					break;
				}
				fStreamConfig.enabled = fStream->Value() == B_CONTROL_ON;
				fStreamConfig.Save();
				ApplyStreamState();
				break;
			case kMsgStreamConfigure:
				(new StreamSettingsWindow(BMessenger(this), fStreamConfig))->Show();
				break;
			case kMsgStreamSaved:
			{
				int32 number;
				const char* text;
				if (message->FindInt32("service", &number) == B_OK) fStreamConfig.service = number;
				for (int i = 0; i < kStreamServiceCount; i++) {
					char name[16];
					snprintf(name, sizeof(name), "server%d", i);
					if (message->FindString(name, &text) == B_OK) fStreamConfig.servers[i] = text;
					snprintf(name, sizeof(name), "key%d", i);
					if (message->FindString(name, &text) == B_OK) fStreamConfig.keys[i] = text;
				}
				if (message->FindInt32("video", &number) == B_OK) fStreamConfig.videoKbps = number;
				if (message->FindInt32("audio", &number) == B_OK) fStreamConfig.audioKbps = number;
				if (message->FindInt32("preset", &number) == B_OK) fStreamConfig.preset = number;
				if (message->FindInt32("keyint", &number) == B_OK) fStreamConfig.keyintSeconds = number;
				fStreamConfig.Save();
				break;
			}
			case kMsgMixHub:
				fStreamConfig.mixHubOn = fMixHub->Value() == B_CONTROL_ON;
				fStreamConfig.Save();
				fMixHubLeaveOut->SetEnabled(fStreamConfig.mixHubOn);
				SendCommand("mixhub %d\n", fStreamConfig.mixHubOn ? 1 : 0);
				break;
			case kMsgMixHubSelf:
				fStreamConfig.mixHubLeaveOut = fMixHubLeaveOut->Value() == B_CONTROL_ON;
				fStreamConfig.Save();
				SendCommand("mixhubself %d\n", fStreamConfig.mixHubLeaveOut ? 0 : 1);
				break;
			case kMsgEqOpen:
				if (fEqWindow.IsValid())
					fEqWindow.SendMessage(kTrayShow);
				else {
					EqWindow* window = new EqWindow(BMessenger(this), fStreamConfig, Frame());
					fEqWindow = BMessenger(window);
					window->Show();
				}
				break;
			case kMsgEqChanged:
			{
				bool on = false;
				message->FindBool("on", &on);
				fStreamConfig.eqOn = on;
				for (int i = 0; i < kEqBands; i++)
					message->FindFloat("band", i, &fStreamConfig.eqBands[i]);
				fStreamConfig.Save();
				fEqButton->SetColor(on ? theme::kAccent : theme::kButton);
				SendEq();
				break;
			}
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

	// Is the H.264 add-on there? The streaming box can be used only when it is.
	void RefreshStream()
	{
		std::string command = "\"" + Engine::ProgramPath() + "\" --detect-stream 2>/dev/null";
		char answer[128] = "";
		FILE* pipe = popen(command.c_str(), "r");
		if (pipe != NULL) {
			if (fgets(answer, sizeof(answer), pipe) == NULL)
				answer[0] = '\0';
			pclose(pipe);
		}
		fStreamAddon = strncmp(answer, "stream on", 9) == 0;
		ApplyStreamState();
	}

	void ApplyStreamState()
	{
		const bool idle = !fRecording;
		fStream->SetEnabled(idle);		// without the add-on a click opens its download page
		fStreamSettings->SetEnabled(fStreamAddon && idle);
		if (fStreamAddon) {
			fStream->SetValue(fStreamConfig.enabled ? B_CONTROL_ON : B_CONTROL_OFF);
			fStream->SetToolTip((const char*)NULL);
		} else {
			fStream->SetValue(B_CONTROL_OFF);
			fStream->SetToolTip("Streaming needs the H.264 add-on (the hrecord_stream package), which is not installed. "
				"Click to open its download page.");
			fStreamSettings->SetToolTip("Available when the H.264 add-on (the hrecord_stream package) is installed.");
		}
		// streaming sends the screen, so the screen is always recorded with it
		if (idle && StreamWanted() && fSelectedSource == kSourceAudioOnly) {
			fSelectedSource = kSourceDesktop;
			fPreview.source = fSelectedSource;
			SelectSource(fSelectedSource);
			ShowSourceName();
		}
		EnableSources(idle);
	}

	bool StreamWanted() const { return fStreamAddon && fStreamConfig.enabled; }

	// hrecord runs as long as this window is open: it connects to every audio source at once, so that the levels show and a
	// recording starts at once, and it puts the programs back on the Mixer when it ends.
	void StartEngine()
	{
		if (fEngine.Running())
			return;
		RefreshRealtime();
		RefreshStream();
		std::vector<std::string> arguments;
		arguments.push_back("start");
		arguments.push_back("--allaudio");
		arguments.push_back("--standby");
		arguments.push_back("--meter");
		arguments.push_back("--meter-ms");
		arguments.push_back("70");
		arguments.push_back("--control");
		if (fRealtime->IsEnabled() && fRealtime->Value() == B_CONTROL_ON)
			arguments.push_back("--realtime");
		std::string error;
		if (!fEngine.Start(arguments, BMessenger(this), error)) {
			SetStatus(error.c_str());
			return;
		}
		fEngineReady = false;
		SetStatus("Connecting to the audio sources...");
		// what is set in the window goes to hrecord as soon as it runs
		SendCommand("mixgain %.1f\n", fMixGain->Value() / 2.0f);
		SendCommand("ceiling %.1f\n", fCeiling->Value() / 2.0f);
		SendCommand("release %d\n", (int)fRelease->Value() * 10);
		SendCommand("limiter %d\n", fLimiter->Value() == B_CONTROL_ON ? 1 : 0);
		SendEq();
		SendCommand("mixhubself %d\n", fStreamConfig.LeaveOutWanted() ? 0 : 1);
		if (fStreamConfig.MixHubWanted())
			SendCommand("mixhub 1\n");
	}

	void StartRecording()
	{
		if (fRecording)
			return;
		if (!fEngine.Running() || !fEngineReady) {
			// hrecord is not up (yet): start it, and the recording starts as soon as it is
			fRecordAfterReady = true;
			StartEngine();
			return;
		}
		fRecordAfterReady = false;
		RefreshStream();
		const bool streaming = fStream->IsEnabled() && fStream->Value() == B_CONTROL_ON;
		if (streaming) {
			const std::string problem = fStreamConfig.Problem();
			if (!problem.empty()) {
				SetStatus(problem.c_str());
				(new StreamSettingsWindow(BMessenger(this), fStreamConfig))->Show();
				return;
			}
			SendCommand("stream %d %d %s %d %s\n", fStreamConfig.videoKbps, fStreamConfig.audioKbps,
				kStreamPresets[fStreamConfig.preset], fStreamConfig.keyintSeconds, fStreamConfig.Url().c_str());
		} else
			SendCommand("nostream\n");
		if (streaming && fSelectedSource == kSourceAudioOnly) {
			fSelectedSource = kSourceDesktop;
			fPreview.source = fSelectedSource;
			SelectSource(fSelectedSource);
		}
		SendCommand("capwindow %d\n", fSelectedSource >= 0 ? (int)fSelectedSource : -1);
		SendCommand("record %d %d\n", fSelectedSource != kSourceAudioOnly ? 1 : 0, (int)fQuality->Value());

		fStreamingNow = false;
		fRecording = true;
		fStopping = false;
		fSeconds = 0;
		ShowTime();
		BMessage tick(kMsgTick);
		delete fTick;
		fTick = new BMessageRunner(this, &tick, 1000000);
		ShowRecordButton();
		fPreview.recording = true;
		EnableSources(false);
		fRealtime->SetEnabled(false);
		fStream->SetEnabled(false);
		fStreamSettings->SetEnabled(false);
		fQuality->SetEnabled(false);
		SetStatus("Starting the recording...");
		fOutputFile = "";
		fLastError = "";
		Log("-- recording started --");
	}

	void StopRecording()
	{
		if (!fRecording || fStopping)
			return;
		fStopping = true;
		ShowRecordButton();
		SetStatus("Stopping: finishing the file...");
		SendCommand("stoprecord\n");
	}

	// The recording is over (hrecord goes on, connected to the sources).
	void RecordingEnded()
	{
		fRecording = false;
		fStopping = false;
		fStreamingNow = false;
		delete fTick;
		fTick = NULL;
		ShowRecordButton();
		ShowTime();
		fPreview.recording = false;
		fQuality->SetEnabled(true);
		RefreshRealtime();
		RefreshStream();
		if (fOutputFile.length() > 0) {
			BString text("Saved: ");
			text << fOutputFile.c_str();
			SetStatus(text.String());
		} else if (fLastError.length() > 0) {
			// it ended without a file: say why (a stream that could not be reached, for one), not only "Stopped."
			SetStatus(fLastError.c_str());
			BString text("The recording could not start:\n\n");
			text << fLastError.c_str();
			(new BAlert("error", text.String(), "OK", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go(NULL);
		} else
			SetStatus("Stopped.");
		Log("-- recording ended --");
	}

	// hrecord itself has ended: at the end of this window, or unexpectedly.
	void EngineExited()
	{
		fEngine.Finished();
		fEngineReady = false;
		fNodes->SetEngineReady(false);
		const bool wasRecording = fRecording;
		if (fRecording)
			RecordingEnded();
		ClearStrips();
		fPostMeter->SetLevels(kMeterMin, kMeterMin);
		fReduction->SetLevel(0.0f);
		ShowRecordButton();
		RefreshRealtime();
		RefreshStream();
		if (!fQuitting) {
			SetStatus(wasRecording ? "hrecord stopped unexpectedly; the recording was ended."
				: "hrecord stopped. Press Record to connect again.");
			Log("-- hrecord ended --");
		}
	}

	// One line of what hrecord printed.
	void HandleLine(const char* line)
	{
		if (strncmp(line, "[meter]", 7) == 0 && strstr(line, "Summary") == NULL && strstr(line, "| mix before") != NULL) {
			UpdateMeters(line);		// (the line has no "#n" at all when no source is connected)
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
		// a source that has stopped (its program has quit) leaves the mix and loses its strip
		if (sscanf(line, "[-] Removed source #%d", &number) == 1) {
			RemoveStrip(number - 1);
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
		if (strstr(line, "[+] Streaming to ") != NULL)
			fStreamingNow = true;
		if (strstr(line, "[+] Standby:") != NULL) {
			fEngineReady = true;
			fNodes->SetEngineReady(true);
			SetStatus("Ready: every audio source is connected.");
			if (fRecordAfterReady) {
				Log(line);
				StartRecording();
				return;
			}
		}
		if (strstr(line, "Recording Started!") != NULL) {
			fSeconds = 0;
			ShowTime();
			SetStatus(fStreamingNow ? "Live: streaming and recording." : "Recording.");
		}
		if (strstr(line, "[+] Recording ended") != NULL) {
			Log(line);
			RecordingEnded();
			return;
		}
		if (strstr(line, "[meter]") != NULL || strstr(line, "    ") == line)
			; // the summary lines go to the log too
		Log(line);
		if (strncmp(line, "[-] Error", 9) == 0) {
			fLastError = line + 4;		// without the "[-] "
			SetStatus(line);
		}
	}

	// "[meter] #1 -0.9 -3.0 #2 -3.2 -3.2 | mix before 3.5 3.1 after -1.0 -1.0 dBFS | limiter -4.5 dB"
	void UpdateMeters(const char* line)
	{
		const char* cursor = line + 7;
		for (;;) {
			int number = 0;
			float left = 0, right = 0;
			int used = 0;
			if (sscanf(cursor, " #%d %f %f%n", &number, &left, &right, &used) < 3)
				break;
			cursor += used;
			for (size_t i = 0; i < fStrips.size(); i++) {
				if (fStrips[i]->Index() == number - 1)
					fStrips[i]->SetLevels(left, right);
			}
		}
		float beforeL, beforeR, afterL, afterR, limiter;
		const char* mix = strstr(line, "| mix before");
		if (mix != NULL && sscanf(mix, "| mix before %f %f after %f %f dBFS | limiter %f dB", &beforeL, &beforeR,
				&afterL, &afterR, &limiter) == 5) {
			fPostMeter->SetLevels(afterL, afterR);
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
		ShowHint(false);
		SourceStrip* strip = new SourceStrip(index, name);
		// before the glue at the end
		layout->AddView(layout->CountItems() - 1, strip);
		fStrips.push_back(strip);
	}

	// The text in the strips' place while there is no strip. (Hide and Show count, and IsHidden() is true as well while the whole
	// tab is out of sight, so what is shown is kept here.)
	void ShowHint(bool show)
	{
		if (show == fHintVisible)
			return;
		fHintVisible = show;
		if (show)
			fHint->Show();
		else
			fHint->Hide();
	}

	void RemoveStrip(int index)
	{
		for (size_t i = 0; i < fStrips.size(); i++) {
			if (fStrips[i]->Index() != index)
				continue;
			fStrips[i]->RemoveSelf();
			delete fStrips[i];
			fStrips.erase(fStrips.begin() + i);
			break;
		}
		if (fStrips.empty() && fEngineReady) {
			fHint->SetText("Nothing playing now.");
			ShowHint(true);
		}
	}

	void ClearStrips()
	{
		for (size_t i = 0; i < fStrips.size(); i++) {
			fStrips[i]->RemoveSelf();
			delete fStrips[i];
		}
		fStrips.clear();
		fHint->SetText("Nothing playing yet.");
		ShowHint(true);
	}

	void ShowTime()
	{
		char text[24];
		snprintf(text, sizeof(text), "REC %02d:%02d", (int)(fSeconds / 60), (int)(fSeconds % 60));
		fTime->SetText(text);
	}

	void ShowPage(int32 page)
	{
		fPages->SetVisibleItem(page);
		fMixerButton->SetColor(page == 0 ? theme::kAccent : theme::kButton);
		fNodesButton->SetColor(page == 1 ? theme::kAccent : theme::kButton);
		UpdatePreviewState();
	}

	void ShowFromTray()
	{
		SetWorkspaces(B_CURRENT_WORKSPACE);
		while (IsHidden())
			Show();
		if (IsMinimized())
			Minimize(false);
		Activate();
		UpdatePreviewState();
	}

	// hrecord_tray: beside this program (a build), or in the add-ons of an installed package.
	static bool FindTrayFile(entry_ref& ref)
	{
		std::vector<std::string> places;
		app_info info;
		if (be_app->GetAppInfo(&info) == B_OK) {
			BPath path(&info.ref);
			if (path.InitCheck() == B_OK && path.GetParent(&path) == B_OK)
				places.push_back(std::string(path.Path()) + "/" HRECORD_TRAY_FILE);
		}
		// (the packages' own add-ons folder: B_..._ADDONS_DIRECTORY is another one, add-ons/x86, for a 32-bit program of the
		// newer compiler, as this one is)
		const directory_which directories[] = { B_USER_NONPACKAGED_DIRECTORY, B_USER_CONFIG_DIRECTORY,
			B_SYSTEM_NONPACKAGED_DIRECTORY, B_SYSTEM_DIRECTORY };
		for (int i = 0; i < 4; i++) {
			BPath path;
			if (find_directory(directories[i], &path) == B_OK)
				places.push_back(std::string(path.Path()) + "/add-ons/hrecord/" HRECORD_TRAY_FILE);
		}
		for (size_t i = 0; i < places.size(); i++) {
			BEntry entry(places[i].c_str(), true);
			if (entry.Exists() && entry.GetRef(&ref) == B_OK)
				return true;
		}
		return false;
	}

	void AddTrayItem()
	{
		BDeskbar deskbar;
		entry_ref ref;
		if (!deskbar.IsRunning() || deskbar.HasItem(HRECORD_TRAY_NAME) || !FindTrayFile(ref))
			return;
		const status_t status = deskbar.AddItem(&ref);
		if (status != B_OK) {
			BString text("The tray icon could not be added: ");
			text << strerror(status);
			Log(text.String());
		}
	}

	void SendEq()
	{
		for (int i = 0; i < kEqBands; i++)
			SendCommand("eqband %d %.1f\n", i + 1, fStreamConfig.eqBands[i]);
		SendCommand("eq %d\n", fStreamConfig.eqOn ? 1 : 0);
	}

	void CloseEqWindow()
	{
		if (fEqWindow.IsValid())
			fEqWindow.SendMessage(B_QUIT_REQUESTED);
	}

	void ShowRecordButton()
	{
		fRecord->SetLabel(fRecording ? "Stop Recording" : "Start Recording");
		fRecord->SetMessage(new BMessage(fRecording ? kMsgStop : kMsgRecord));
		fRecord->SetColor(fRecording ? theme::kRecord : theme::kAccent);
		fRecord->SetEnabled(!fStopping);
	}

	void ShowMasterValues()
	{
		char text[48];
		FaderText(fMixGain, text, sizeof(text));
		fMixGainText->SetText(text);
		snprintf(text, sizeof(text), "Ceiling: %.1f dB", fCeiling->Value() / 2.0f);
		fCeilingText->SetText(text);
		snprintf(text, sizeof(text), "Release: %d ms", (int)fRelease->Value() * 10);
		fReleaseText->SetText(text);
	}

	// The preview is read only while it can be seen, and in the size there is room for.
	void UpdatePreviewState()
	{
		fPreview.visible = !IsMinimized() && !IsHidden() && fPages->VisibleIndex() == 0;
		const BRect bounds = fPreviewView->Bounds();
		if (bounds.IsValid()) {
			fPreview.width = std::max((int32)64, bounds.IntegerWidth() + 1);
			fPreview.height = std::max((int32)36, bounds.IntegerHeight() + 1);
		}
	}

	void ShowSourceName()
	{
		BString text;
		SourceItem* item = (SourceItem*)fSources->ItemAt(fSources->CurrentSelection());
		if (item != NULL)
			text << "Recording shows: " << item->Text();
		fSourceName->SetText(text.String());
		if (fSelectedSource == kSourceAudioOnly)
			fPreviewView->SetPicture(NULL, "Sound only: there is no picture.");
		else if (!fStreamConfig.preview)
			fPreviewView->SetPicture(NULL, "The preview is off.");
	}

	void EnableSources(bool enable)
	{
		fSourcesEnabled = enable;
		for (int32 i = 0; i < fSources->CountItems(); i++)
			fSources->ItemAt(i)->SetEnabled(enable || ((SourceItem*)fSources->ItemAt(i))->Source() == fSelectedSource);
		fSources->Invalidate();
	}

	void SelectSource(int32 source)
	{
		const bool refreshing = fRefreshingSources;
		fRefreshingSources = true;
		for (int32 i = 0; i < fSources->CountItems(); i++) {
			if (((SourceItem*)fSources->ItemAt(i))->Source() != source)
				continue;
			fSources->ItemAt(i)->SetEnabled(true);
			if (fSources->CurrentSelection() != i)
				fSources->Select(i);
		}
		fRefreshingSources = refreshing;
	}

	// The windows that are open, for the list of sources: the ordinary ones of every program but this one and the desktop's own
	// (the Deskbar, a dock).
	static void OpenWindows(std::vector<std::pair<int32, std::string> >& windows)
	{
		int32 count = 0;
		int32* tokens = get_token_list(-1, &count);
		if (tokens == NULL)
			return;
		team_info own;
		get_team_info(B_CURRENT_TEAM, &own);
		for (int32 i = 0; i < count; i++) {
			client_window_info* info = get_window_info(tokens[i]);
			if (info == NULL)
				continue;
			if (info->team != own.team && info->feel == B_NORMAL_WINDOW_FEEL && info->name[0] != '\0'
				&& (info->is_mini || info->show_hide_level <= 0)
				&& info->window_right > info->window_left && info->window_bottom > info->window_top
				&& info->window_right > -1000) {
				app_info app;
				std::string text;
				if (be_roster->GetRunningAppInfo(info->team, &app) == B_OK) {
					if (strcasecmp(app.signature, "application/x-vnd.Be-TSKB") != 0
						&& strcasestr(app.signature, "hdesktop") == NULL)
						text = app.ref.name;
					else
						text = "";
				} else
					text = "?";
				if (!text.empty()) {
					if (text != info->name) {
						text += ": ";
						text += info->name;
					}
					if (info->is_mini)
						text += " (minimized)";
					const int32 token = info->server_token;	// (the struct is packed)
					windows.push_back(std::make_pair(token, text));
				}
			}
			free(info);
		}
		free(tokens);
		std::sort(windows.begin(), windows.end());		// by token: the order they were opened in, which does not jump about
	}

	void RefreshSources()
	{
		if (fRecording)
			return;
		std::vector<std::pair<int32, std::string> > entries;
		entries.push_back(std::make_pair(kSourceDesktop, std::string("Entire desktop")));
		entries.push_back(std::make_pair(kSourceAudioOnly, std::string("Sound only (no screen)")));
		std::vector<std::pair<int32, std::string> > windows;
		OpenWindows(windows);
		for (size_t i = 0; i < windows.size(); i++)
			entries.push_back(std::make_pair(windows[i].first, "Window - " + windows[i].second));

		bool same = (int32)entries.size() == fSources->CountItems();
		for (size_t i = 0; same && i < entries.size(); i++) {
			SourceItem* item = (SourceItem*)fSources->ItemAt(i);
			same = item->Source() == entries[i].first && entries[i].second == item->Text().String();
		}
		if (same)
			return;

		bool present = false;
		for (size_t i = 0; i < entries.size(); i++)
			present |= entries[i].first == fSelectedSource;
		const bool lost = !present;
		if (lost) {
			fSelectedSource = kSourceDesktop;
			fPreview.source = fSelectedSource;
		}

		fRefreshingSources = true;
		const BPoint scrolled = fSources->Bounds().LeftTop();
		while (fSources->CountItems() > 0)
			delete fSources->RemoveItem((int32)0);
		for (size_t i = 0; i < entries.size(); i++) {
			SourceItem* item = new SourceItem(entries[i].first, entries[i].second.c_str());
			fSources->AddItem(item);
		}
		SelectSource(fSelectedSource);
		fSources->ScrollTo(scrolled);
		fRefreshingSources = false;
		if (fSourceName != NULL) {
			ShowSourceName();
			if (lost)
				SetStatus("The chosen window was closed: the entire desktop is chosen now.");
		}
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
	bool				fHintVisible = true;
	BCardLayout*		fPages;
	FlatButton*			fMixerButton;
	FlatButton*			fNodesButton;
	FlatButton*			fEqButton;
	BMessenger			fEqWindow;
	int					fTrayTicks = 0;
	BView*				fPageBar;
	BListView*			fSources;
	BScrollView*		fSourceScroll;
	int32				fSelectedSource = kSourceDesktop;
	bool				fRefreshingSources = false;
	bool				fSourcesEnabled = true;
	BMessageRunner*		fSourceRunner = NULL;
	Preview				fPreview;
	PreviewView*		fPreviewView = NULL;
	Label*				fSourceName = NULL;
	Panel*				fDebugBox;
	Label*				fMixGainText;
	Label*				fCeilingText;
	Label*				fReleaseText;
	FlatCheck*			fShowPreview;
	FlatCheck*			fDebug;
	NodesTab*			fNodes;
	FlatCheck*			fShowLog;
	BScrollView*		fLogScroll;
	float				fLogHeight = 100;
	bool				fEngineReady = false;		// hrecord is connected to the sources
	bool				fRecordAfterReady = false;	// Record was pressed before it was
	bool				fQuitting = false;
	std::string			fLastError;				// the last error hrecord reported since the recording began
	bool				fStopping;
	int					fSeconds;
	BMessageRunner*		fTick;

	FlatButton*			fRecord;
	FlatCheck*			fRealtime;
	FlatCheck*			fStream;
	FlatButton*			fStreamSettings;
	StreamConfig		fStreamConfig;
	bool				fStreamAddon = false;
	bool				fStreamingNow = false;
	Segments*			fQuality;
	Label*				fTime;
	BView*				fStripsGroup;
	Label*				fHint;
	std::vector<SourceStrip*> fStrips;
	Fader*				fMixGain;
	StereoMeter*		fPostMeter;
	FlatCheck*			fLimiter;
	FlatCheck*			fMixHub;
	FlatCheck*			fMixHubLeaveOut;
	Fader*				fCeiling;
	Fader*				fRelease;
	LevelMeter*			fReduction;
	Label*				fReductionText;
	Label*				fStatus;
	BTextView*			fLog;
	std::string			fOutputFile;
};


class MixerApp : public BApplication {
public:
	MixerApp() : BApplication(kAppSignature), fAutoRecord(false), fShowStream(false), fToggleStream(false), fShowNodes(false) {}

	// "hrecord_mixer --record" starts recording as soon as the window is open.
	void ArgvReceived(int32 argc, char** argv)
	{
		for (int32 i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--record") == 0)
				fAutoRecord = true;
			else if (strcmp(argv[i], "--stream-settings") == 0)
				fShowStream = true;
			else if (strcmp(argv[i], "--stream-toggle") == 0)
				fToggleStream = true;
			else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
				fSource = argv[++i];
			else if (strcmp(argv[i], "--stop-after") == 0 && i + 1 < argc)
				fStopAfter = atoi(argv[++i]);
			else if (strcmp(argv[i], "--nodes") == 0)
				fShowNodes = true;
			else if (strcmp(argv[i], "--click") == 0 && i + 1 < argc)
				fClicks.push_back(argv[++i]);
			else if (strcmp(argv[i], "--do") == 0 && i + 1 < argc)
				fClicks.push_back(std::string("do:") + argv[++i]);
		}
	}

	// the tray item's messages go to the window; starting the mixer again shows a hidden window
	void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kTrayAsk:
			case kTrayRecord:
			case kTrayStop:
			case kTrayShow:
			case kTrayHide:
				if (fWindow.IsValid())
					fWindow.SendMessage(message, message->ReturnAddress());
				break;
			case kTrayQuit:
				if (fWindow.IsValid())
					fWindow.SendMessage(B_QUIT_REQUESTED);
				break;
			case B_SILENT_RELAUNCH:
				if (fWindow.IsValid())
					fWindow.SendMessage(kTrayShow);
				break;
			default:
				BApplication::MessageReceived(message);
		}
	}

	void ReadyToRun()
	{
		MixerWindow* window = new MixerWindow();
		fWindow = BMessenger(window);
		window->Show();
		if (!fSource.empty()) {
			BMessage choose(kMsgSourceByName);
			choose.AddString("name", fSource.c_str());
			window->PostMessage(&choose);
		}
		if (fAutoRecord)
			window->PostMessage(kMsgRecord);
		if (fStopAfter > 0) {
			BMessage stop(kMsgStop);
			new BMessageRunner(BMessenger(window), &stop, (bigtime_t)fStopAfter * 1000000, 1);
		}
		if (fShowStream)
			window->PostMessage(kMsgStreamConfigure);
		if (fToggleStream)
			window->PostMessage(kMsgStreamToggle);
		if (fShowNodes)
			window->PostMessage(kMsgShowNodes);
		// (for testing: "--click Name:port" clicks a box of the Nodes tab, a little later each)
		for (size_t i = 0; i < fClicks.size(); i++) {
			BMessage click(kMsgTestClick);
			if (fClicks[i].compare(0, 3, "do:") == 0) {
				click.what = kMsgTestAction;
				click.AddString("what", fClicks[i].c_str() + 3);
			} else {
				const size_t colon = fClicks[i].rfind(':');
				click.AddString("name", fClicks[i].substr(0, colon).c_str());
				click.AddInt32("port", colon == std::string::npos ? 0 : atoi(fClicks[i].substr(colon + 1).c_str()));
			}
			new BMessageRunner(BMessenger(window), &click, 1000000 + (bigtime_t)i * 1500000, 1);
		}
	}

private:
	bool	fAutoRecord;
	bool	fShowStream;
	bool	fToggleStream;
	bool	fShowNodes;
	std::vector<std::string>	fClicks;
	std::string					fSource;
	BMessenger					fWindow;
	int							fStopAfter = 0;
};


int
main()
{
	MixerApp app;
	app.Run();
	return 0;
}
