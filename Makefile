PKG = libavformat libavcodec libavutil alsa libdrm xcb xcb-randr xcb-screensaver xcb-keysyms
PREFIX ?= /usr/local

CFLAGS ?= -O2

all: wmt-play

# Keep the player's flags out of the ffmpeg build's environment
wmt-play.o: private CFLAGS += -std=gnu11 -Wall -Wextra $(shell $(pcenv) pkg-config --cflags $(PKG))
wmt-play: private LDLIBS += $(shell $(pcenv) pkg-config --libs $(PKG))

wmt-play: wmt-play.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

wmt-play.o: wmt-play.h

# Build static AVI-only libavformat from an ffmpeg source tree
ifdef FFMPEG
ffinst := $(abspath $(FFMPEG))/install
pcenv := PKG_CONFIG_PATH=$(ffinst)/lib/pkgconfig

$(ffinst)/lib/pkgconfig/libavformat.pc:
	cd $(FFMPEG) && ./configure --prefix=$(ffinst) --disable-everything \
		--disable-autodetect --disable-programs --disable-doc --disable-network \
		--disable-avdevice --disable-swscale --disable-swresample --disable-avfilter \
		--enable-demuxer=avi --enable-protocol=file --disable-debug && $(MAKE) install

wmt-play.o: $(ffinst)/lib/pkgconfig/libavformat.pc
endif

install: wmt-play
	install -D -m 755 wmt-play $(DESTDIR)$(PREFIX)/bin/wmt-play
	install -D -m 644 wmt-play.1 $(DESTDIR)$(PREFIX)/share/man/man1/wmt-play.1

clean:
	rm -f wmt-play wmt-play.o

.PHONY: all install clean
