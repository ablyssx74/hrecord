// hrecord_mixer's own download window for the H.264 add-on (the hrecord_stream package): it asks GitHub what hrecord's release
// holds, downloads the package for this system with its progress shown, compares its SHA-256 with the one GitHub gives for it,
// and installs it with pkgman when the user says so.
#ifndef HRECORD_ADDON_WINDOW_H
#define HRECORD_ADDON_WINDOW_H

#include <Directory.h>
#include <FindDirectory.h>
#include <Json.h>		// private (libshared): the release list is JSON
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <String.h>
#include <Window.h>

#include <curl/curl.h>

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "mixer_widgets.h"


static const uint32 kMsgAddonInstalled = 'adin';	// to the mixer: the add-on has been installed

#ifndef HRECORD_VERSION
#define HRECORD_VERSION "v0.0.0"
#endif


// How far the download is.
class ProgressBar : public BView {
public:
	ProgressBar()
		:
		BView("progress", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fFraction(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetExplicitMinSize(BSize(420, 18));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 18));
	}

	void Set(float fraction)
	{
		fFraction = std::max(0.0f, std::min(1.0f, fraction));
		Invalidate();
	}

	void Draw(BRect)
	{
		BRect bounds = Bounds();
		const float at = floorf(bounds.Width() * fFraction);
		SetHighColor(theme::kAccent);
		if (at > 0)
			FillRect(BRect(0, 0, at, bounds.bottom));
		SetHighColor(theme::kInset);
		if (at < bounds.Width())
			FillRect(BRect(at > 0 ? at + 1 : 0, 0, bounds.right, bounds.bottom));
		char text[16];
		snprintf(text, sizeof(text), "%d %%", (int)(fFraction * 100));
		DrawText(this, text, bounds, theme::kText, fFraction > 0.5f ? theme::kAccent : theme::kInset, true);
	}

private:
	float	fFraction;
};


class AddonWindow : public BWindow {
	enum {
		kMsgStatus		= 'dlst',
		kMsgFile		= 'dlfi',
		kMsgProgress	= 'dlpr',
		kMsgVerified	= 'dlok',
		kMsgMismatch	= 'dlbd',
		kMsgFailed		= 'dlfl',
		kMsgInstalled	= 'dldn',
		kMsgInstall		= 'dlIn',
		kMsgRetry		= 'dlre'
	};

public:
	AddonWindow(const BMessenger& mixer, BRect over)
		:
		BWindow(BRect(0, 0, 100, 100), "hRecord H.264 Add-on", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS),
		fMixer(mixer)
	{
		Panel* background = new Panel("addon", theme::kWindow);
		fStatus = new Label("status", "", theme::kWindow, theme::kText, false, B_SIZE_UNLIMITED);
		fFile = new Label("file", "", theme::kWindow, theme::kDimText, false, B_SIZE_UNLIMITED);
		fBar = new ProgressBar();
		fAmount = new Label("amount", "", theme::kWindow, theme::kDimText, false, B_SIZE_UNLIMITED);
		Label* title = new Label("title", "SHA-256 checksum", theme::kWindow, theme::kText, false, B_SIZE_UNLIMITED);
		fExpected = new Label("expected", "", theme::kWindow, theme::kDimText, false, B_SIZE_UNLIMITED);
		fActual = new Label("actual", "", theme::kWindow, theme::kDimText, false, B_SIZE_UNLIMITED);
		fGood = new Label("good", "The checksum matches: the file is the one in the release.", theme::kWindow,
			make_color(90, 200, 110), false, B_SIZE_UNLIMITED);
		fBad = new Label("bad", "The checksum does NOT match. The file was thrown away.", theme::kWindow,
			make_color(235, 90, 75), false, B_SIZE_UNLIMITED);
		BFont fixed(be_fixed_font);
		fixed.SetSize(be_plain_font->Size() * 0.9f);
		fExpected->SetFont(&fixed);
		fActual->SetFont(&fixed);
		const float hashWidth = fixed.StringWidth("Downloaded: ") + fixed.StringWidth("0") * 64 + 8;
		fExpected->SetExplicitMinSize(BSize(hashWidth, B_SIZE_UNSET));
		fInstall = new FlatButton("install", "Install", new BMessage(kMsgInstall), theme::kWindow, theme::kAccent);
		fRetry = new FlatButton("retry", "Try again", new BMessage(kMsgRetry), theme::kWindow);
		fClose = new FlatButton("close", "Cancel", new BMessage(B_QUIT_REQUESTED), theme::kWindow);

		BLayoutBuilder::Group<>(background, B_VERTICAL, 6)
			.SetInsets(14, 12, 14, 12)
			.Add(fStatus)
			.Add(fFile)
			.Add(fBar)
			.Add(fAmount)
			.AddStrut(4)
			.Add(title)
			.Add(fExpected)
			.Add(fActual)
			.Add(fGood)
			.Add(fBad)
			.AddStrut(6)
			.AddGroup(B_HORIZONTAL, 8)
				.AddGlue()
				.Add(fRetry)
				.Add(fInstall)
				.Add(fClose)
			.End();
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(background);
		fGood->Hide();
		fBad->Hide();
		fRetry->Hide();

		Layout(true);
		const BSize size = background->PreferredSize();
		ResizeTo(std::max(hashWidth + 28, 520.0f), size.height);
		CenterIn(over);
		MoveOnScreen();
		fInstall->SetTarget(this);
		fRetry->SetTarget(this);
		fClose->SetTarget(this);
		StartDownload();
	}

	bool QuitRequested()
	{
		// a download is given up; an installation is waited for
		fCancel = true;
		Wait();
		return true;
	}

	void MessageReceived(BMessage* message)
	{
		const char* text = "";
		message->FindString("text", &text);
		switch (message->what) {
			case B_SILENT_RELAUNCH:		// (asked for again)
				Activate();
				break;
			case kMsgStatus:
				fStatus->SetText(text);
				break;
			case kMsgFile:
				fFile->SetText(text);
				break;
			case kMsgProgress:
			{
				int64 done = 0, total = 0;
				message->FindInt64("done", &done);
				message->FindInt64("total", &total);
				if (total > 0)
					fBar->Set((float)((double)done / total));
				BString amount;
				amount.SetToFormat("%.1f of %.1f KiB", done / 1024.0, total / 1024.0);
				fAmount->SetText(amount.String());
				break;
			}
			case kMsgVerified:
			case kMsgMismatch:
			{
				const char* expected = "";
				const char* actual = "";
				message->FindString("expected", &expected);
				message->FindString("actual", &actual);
				fExpected->SetText((BString("Expected:   ") << expected).String());
				fActual->SetText((BString("Downloaded: ") << actual).String());
				if (message->what == kMsgVerified) {
					fPath = text;
					fHash = expected;
					fGood->Show();
					fStatus->SetText("Downloaded and checked. Install it now?");
					fInstall->SetEnabled(true);
				} else {
					fBad->Show();
					fStatus->SetText("The download was not accepted.");
					fRetry->Show();
				}
				fClose->SetLabel("Close");
				break;
			}
			case kMsgFailed:
				fStatus->SetText(text);
				fClose->SetLabel("Close");
				if (fPath.Length() > 0)
					fInstall->SetEnabled(true);		// the installation failed: it can be tried again
				else
					fRetry->Show();
				break;
			case kMsgInstalled:
				fStatus->SetText("Installed. Stream live is ready to use.");
				fInstall->SetEnabled(false);
				fClose->SetLabel("Close");
				fClose->SetEnabled(true);
				fMixer.SendMessage(kMsgAddonInstalled);
				break;
			case kMsgInstall:
				if (fPath.Length() == 0)
					break;
				Wait();
				fInstall->SetEnabled(false);
				fStatus->SetText("Installing with pkgman" B_UTF8_ELLIPSIS);
				Start(_Install);
				break;
			case kMsgRetry:
				Wait();
				StartDownload();
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void StartDownload()
	{
		fPath = "";
		fCancel = false;
		fInstall->SetEnabled(false);
		if (!fRetry->IsHidden(fRetry))
			fRetry->Hide();
		if (!fGood->IsHidden(fGood))
			fGood->Hide();
		if (!fBad->IsHidden(fBad))
			fBad->Hide();
		fClose->SetLabel("Cancel");
		fBar->Set(0);
		fStatus->SetText("Asking GitHub for the release" B_UTF8_ELLIPSIS);
		fFile->SetText("");
		fAmount->SetText("");
		fExpected->SetText("Expected:   -");
		fActual->SetText("Downloaded: -");
		Start(_Download);
	}

	void Start(thread_func function)
	{
		fThread = spawn_thread(function, "hrecord add-on", B_NORMAL_PRIORITY, this);
		if (fThread >= 0)
			resume_thread(fThread);
	}

	void Wait()
	{
		if (fThread >= 0) {
			status_t result;
			wait_for_thread(fThread, &result);
			fThread = -1;
		}
	}

	// ---- what the threads do (they speak to the window only through messages) ----

	void Say(uint32 what, const char* text)
	{
		BMessage message(what);
		message.AddString("text", text);
		BMessenger(this).SendMessage(&message, (BHandler*)NULL, 500000);
	}

	static size_t _ToString(char* data, size_t size, size_t count, void* into)
	{
		std::string* text = (std::string*)into;
		if (text->size() + size * count > 4 * 1024 * 1024)
			return 0;
		text->append(data, size * count);
		return size * count;
	}

	static size_t _ToFile(char* data, size_t size, size_t count, void* into)
	{
		return fwrite(data, size, count, (FILE*)into);
	}

	static int _Progress(void* data, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t)
	{
		AddonWindow* window = (AddonWindow*)data;
		const bigtime_t now = system_time();
		if (done > 0 && total > 0 && (now - window->fLastProgress > 100000 || done == total)) {
			window->fLastProgress = now;
			BMessage message(kMsgProgress);
			message.AddInt64("done", done);
			message.AddInt64("total", total);
			BMessenger(window).SendMessage(&message, (BHandler*)NULL, 100000);
		}
		return window->fCancel ? 1 : 0;
	}

	CURL* Request(const char* url)
	{
		CURL* curl = curl_easy_init();
		if (curl == NULL)
			return NULL;
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "hrecord-mixer");
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
		// (given up when next to nothing arrives for half a minute)
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _Progress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		return curl;
	}

	// The release's own account of itself. false: there is no answer, or no such release (code says which).
	bool Ask(const char* url, std::string& answer, long& code, std::string& problem)
	{
		answer.clear();
		code = 0;
		CURL* curl = Request(url);
		if (curl == NULL) {
			problem = "libcurl could not be started.";
			return false;
		}
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _ToString);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
		const CURLcode result = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_cleanup(curl);
		if (result != CURLE_OK) {
			problem = std::string("GitHub could not be reached: ") + curl_easy_strerror(result);
			return false;
		}
		if (code != 200) {
			char text[80];
			snprintf(text, sizeof(text), "GitHub answered with error %ld.", code);
			problem = text;
			return false;
		}
		return true;
	}

	// The add-on's package among the files of a release: its name, address and checksum.
	static bool FindPackage(const std::string& answer, std::string& name, std::string& url, std::string& hash)
	{
#if defined(__i386__)
		const BString ending("-x86_gcc2.hpkg");
#elif defined(__x86_64__)
		const BString ending("-x86_64.hpkg");
#else
		const BString ending("-unknown.hpkg");
#endif
		BMessage release, assets;
		if (BPrivate::BJson::Parse(answer.c_str(), release) != B_OK || release.FindMessage("assets", &assets) != B_OK)
			return false;
		BMessage asset;
		for (int32 i = 0;; i++) {
			BString key;
			key << i;
			if (assets.FindMessage(key.String(), &asset) != B_OK)
				return false;
			BString file, address, digest;
			if (asset.FindString("name", &file) != B_OK || asset.FindString("browser_download_url", &address) != B_OK)
				continue;
			if (!file.StartsWith("hrecord_stream-") || !file.EndsWith(ending) || file.FindFirst('/') >= 0)
				continue;
			// only from hrecord's own releases
			if (!address.StartsWith("https://github.com/ablyssx74/hrecord/releases/download/"))
				continue;
			asset.FindString("digest", &digest);
			if (digest.StartsWith("sha256:"))
				digest.Remove(0, 7);
			else
				digest = "";
			name = file.String();
			url = address.String();
			hash = digest.ToLower().String();
			return true;
		}
	}

	static std::string Sha256(const std::string& path)
	{
		// (the path is made here, from a name that holds no quote)
		const std::string command = "sha256sum '" + path + "' 2>/dev/null";
		char line[256] = "";
		FILE* pipe = popen(command.c_str(), "r");
		if (pipe == NULL)
			return "";
		if (fgets(line, sizeof(line), pipe) == NULL)
			line[0] = '\0';
		pclose(pipe);
		const std::string text(line);
		return text.size() > 64 && text[64] == ' ' ? text.substr(0, 64) : "";
	}

	static int32 _Download(void* data)
	{
		((AddonWindow*)data)->Download();
		return 0;
	}

	void Download()
	{
		std::string answer, problem, name, url, hash;
		long code = 0;
		// the release of this version; when there is none, the newest one
		const std::string base = "https://api.github.com/repos/ablyssx74/hrecord/releases/";
		bool have = Ask((base + "tags/" HRECORD_VERSION).c_str(), answer, code, problem);
		if (!have && code == 404)
			have = Ask((base + "latest").c_str(), answer, code, problem);
		if (fCancel)
			return;
		if (!have) {
			Say(kMsgFailed, problem.c_str());
			return;
		}
		if (!FindPackage(answer, name, url, hash)) {
			Say(kMsgFailed, "The release holds no add-on package for this system.");
			return;
		}
		if (hash.size() != 64 || name.find('\'') != std::string::npos) {
			Say(kMsgFailed, "GitHub gives no checksum for the package: it was not downloaded.");
			return;
		}
		Say(kMsgFile, name.c_str());
		Say(kMsgStatus, "Downloading" B_UTF8_ELLIPSIS);

		BPath folder;
		if (find_directory(B_USER_CACHE_DIRECTORY, &folder) != B_OK || folder.Append("hrecord") != B_OK
			|| create_directory(folder.Path(), 0755) != B_OK || strchr(folder.Path(), '\'') != NULL) {
			Say(kMsgFailed, "There is no folder to download into.");
			return;
		}
		const std::string path = std::string(folder.Path()) + "/" + name;
		const std::string part = path + ".part";
		FILE* file = fopen(part.c_str(), "wb");
		if (file == NULL) {
			Say(kMsgFailed, "The file could not be made.");
			return;
		}
		CURL* curl = Request(url.c_str());
		CURLcode result = CURLE_FAILED_INIT;
		code = 0;
		if (curl != NULL) {
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _ToFile);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
			curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 64L * 1024 * 1024);
			result = curl_easy_perform(curl);
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
			curl_easy_cleanup(curl);
		}
		const bool written = fclose(file) == 0;
		if (fCancel || result != CURLE_OK || code != 200 || !written) {
			unlink(part.c_str());
			if (!fCancel) {
				const std::string text = std::string("The download failed: ")
					+ (result != CURLE_OK ? curl_easy_strerror(result) : "the file could not be fetched or written");
				Say(kMsgFailed, text.c_str());
			}
			return;
		}

		Say(kMsgStatus, "Checking the checksum" B_UTF8_ELLIPSIS);
		const std::string actual = Sha256(part);
		const bool good = actual == hash && rename(part.c_str(), path.c_str()) == 0;
		if (!good)
			unlink(part.c_str());
		BMessage done(good ? kMsgVerified : kMsgMismatch);
		done.AddString("text", path.c_str());
		done.AddString("expected", hash.c_str());
		done.AddString("actual", actual.empty() ? "(could not be worked out)" : actual.c_str());
		BMessenger(this).SendMessage(&done, (BHandler*)NULL, 500000);
	}

	static int32 _Install(void* data)
	{
		((AddonWindow*)data)->Install();
		return 0;
	}

	void Install()
	{
		const std::string path = fPath.String();
		// (it may have lain there a while)
		if (Sha256(path) != fHash.String()) {
			Say(kMsgFailed, "The file has changed since it was checked: it was not installed.");
			return;
		}
		const std::string command = "pkgman install -y '" + path + "' 2>&1";
		FILE* pipe = popen(command.c_str(), "r");
		if (pipe == NULL) {
			Say(kMsgFailed, "pkgman could not be started.");
			return;
		}
		char line[512];
		std::string last;
		while (fgets(line, sizeof(line), pipe) != NULL) {
			BString text(line);
			text.RemoveSet("\r\n");
			text.Trim();
			if (text.Length() > 0) {
				last = text.String();
				Say(kMsgFile, text.String());
			}
		}
		const int status = pclose(pipe);
		if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
			Say(kMsgInstalled, "");
		else {
			const std::string text = "pkgman could not install it: " + last;
			Say(kMsgFailed, text.c_str());
		}
	}

	BMessenger			fMixer;
	Label*				fStatus;
	Label*				fFile;
	Label*				fAmount;
	Label*				fExpected;
	Label*				fActual;
	Label*				fGood;
	Label*				fBad;
	ProgressBar*		fBar;
	FlatButton*			fInstall;
	FlatButton*			fRetry;
	FlatButton*			fClose;
	BString				fPath, fHash;		// of the package that has been checked
	std::atomic<bool>	fCancel { false };
	bigtime_t			fLastProgress = 0;
	thread_id			fThread = -1;
};


#endif	// HRECORD_ADDON_WINDOW_H
