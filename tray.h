// What hrecord_mixer and its item in the Deskbar's tray (tray.cpp) say to each other. (The 32-bit item is built with the old
// compiler the Deskbar is built with: nothing newer than that may be used here.)
#ifndef HRECORD_TRAY_H
#define HRECORD_TRAY_H

#define HRECORD_MIXER_SIGNATURE	"application/x-vnd.hrecord-mixer"
#define HRECORD_TRAY_SIGNATURE	"application/x-vnd.hrecord-mixer-tray"
#define HRECORD_TRAY_NAME		"hRecordMixerTray"		// the item's name in the Deskbar
#define HRECORD_TRAY_FILE		"hrecord_tray"

enum {
	// from the item to the mixer
	kTrayAsk		= 'trAs',		// how things stand: answered with kTrayState
	kTrayRecord		= 'trRc',
	kTrayStop		= 'trSt',
	kTrayShow		= 'trSh',
	kTrayHide		= 'trHd',
	kTrayQuit		= 'trQt',
	// the answer: "recording" and "hidden" (bool)
	kTrayState		= 'trSa'
};

#endif	// HRECORD_TRAY_H
