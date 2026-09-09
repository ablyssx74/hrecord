name			$(GUI_TARGET)
version			$(VERSION)-1
architecture	$(ARCH)
summary 		"hrecord"
description 	"Haiku Recorder - Screen, Desktop Audio & Real-Time Mixing"
packager		"ablyss <hrecord@epluribusunix.net>"
vendor			"epluribusunix.net Project"
licenses {
	"MIT"
}
copyrights {
	"$(YEAR) ablyss"
}
provides {
	$(GUI_TARGET) = $(VERSION)-1	
}
requires {
	haiku
	ffmpeg8
	curl
}	
urls {
	"https://github.com/ablyssx74/hrecord"
}
