name			hrecord_stream
version			$(VERSION)-1
architecture	$(ARCH)
summary 		"H.264 add-on for hrecord's live streaming"
description 	"The H.264 video encoder (built on x264) that hrecord uses to stream the screen and the sound live to YouTube, Twitch, other RTMP / RTMPS servers and RTSP servers. Install it next to hrecord; the hRecord Mixer's Stream live option and hrecord's --stream option need it. Haiku's ffmpeg has no H.264 encoder, which is why it is a separate package."
packager		"ablyss <hrecord@epluribusunix.net>"
vendor			"epluribusunix.net Project"
licenses {
	"GNU GPL v2"
}
copyrights {
	"$(YEAR) ablyss"
	"x264 project"
}
provides {
	hrecord_stream = $(VERSION)-1
}
requires {
	haiku
	x264$(DEP_SUFFIX)
}
urls {
	"https://github.com/ablyssx74/hrecord"
}
