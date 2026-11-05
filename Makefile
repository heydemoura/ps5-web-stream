# PS5 Web Stream — payload build
#
# Requires the ps5-payload-dev SDK:
#   export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
# On macOS also point the toolchain at a modern LLVM, e.g.:
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config

PS5_HOST ?= 192.168.0.190
PS5_PORT ?= 9021
.DEFAULT_GOAL := all

HOST_CC ?= clang
HOST_CFLAGS := -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -pthread -Isrc

ifneq ($(filter-out clean test host-test,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK is undefined; export it to point at the SDK root)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

ELF     := ps5-web-stream.elf
SRCDIR  := src
GENDIR  := $(SRCDIR)/gen
SRCS    := $(SRCDIR)/main.c $(SRCDIR)/flv.c $(SRCDIR)/avcap.c \
           $(SRCDIR)/http.c \
           $(SRCDIR)/audio.c $(SRCDIR)/status.c $(SRCDIR)/settings.c \
           $(SRCDIR)/snapshot.c $(SRCDIR)/frame_jpeg.c
GEN     := $(GENDIR)/web_index.h $(GENDIR)/web_player.h $(GENDIR)/web_app.h \
           $(GENDIR)/web_flv.h $(GENDIR)/web_flv_license.h \
           $(GENDIR)/web_videojs.h $(GENDIR)/web_videojs_license.h $(GENDIR)/web_player_css.h

# FFmpeg (LGPL-2.1+) supplies the software H.264 decoder and JPEG encoder for
# /frame.jpeg. Only those codecs and swscale are built, statically, from a
# pinned and checksummed release; the build is cached under build/.
FFMPEG_VERSION := 7.1.5
FFMPEG_SHA256  := de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
FFMPEG_URL     := https://ffmpeg.org/releases/ffmpeg-$(FFMPEG_VERSION).tar.xz
FFMPEG_BUILD   := build/ffmpeg-$(FFMPEG_VERSION)-ps5
FFMPEG_PREFIX  := $(abspath $(FFMPEG_BUILD))/install
FFMPEG_LIBS    := $(FFMPEG_PREFIX)/lib/libavcodec.a $(FFMPEG_PREFIX)/lib/libswscale.a \
                  $(FFMPEG_PREFIX)/lib/libavutil.a
FFMPEG_STAMP   := $(FFMPEG_BUILD)/.installed
FFMPEG_CONFIG  := --enable-cross-compile --target-os=freebsd --arch=x86_64 \
  --cc=$(CC) --ar=$(AR) --ranlib=$(RANLIB) --nm=$(NM) --strip=$(STRIP) \
  --pkg-config=false --disable-autodetect --disable-everything --disable-programs \
  --disable-doc --disable-network --disable-debug --disable-avdevice \
  --disable-avformat --disable-avfilter --disable-swresample \
  --enable-static --disable-shared --enable-pic --enable-pthreads \
  --enable-decoder=h264 --enable-encoder=mjpeg --enable-swscale

CFLAGS  := -Wall -Wextra -Werror -O2 -g -rdynamic -pthread -I$(SRCDIR) -I$(FFMPEG_PREFIX)/include
LDFLAGS := -pthread
HOST_FFMPEG := $(shell pkg-config --cflags --libs libavcodec libswscale libavutil 2>/dev/null)

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CFLAGS  += -DPS5HS_VERSION=\"$(VERSION)\"
ifneq ($(HTTP_PORT),)
CFLAGS += -DDEF_HTTP_PORT=$(HTTP_PORT)
endif

.PHONY: all clean deploy test host-test probe-audio probe-video FORCE

all: $(ELF)

$(GENDIR)/web_index.h: web/index.html tools/embed.py
	python3 tools/embed.py $< $@ web_index_html

$(GENDIR)/web_player.h: web/player.html tools/embed.py
	python3 tools/embed.py $< $@ web_player_html

$(GENDIR)/web_app.h: web/app.js tools/embed.py
	python3 tools/embed.py $< $@ web_app_js

$(GENDIR)/web_flv.h: web/vendor/flv.min.js tools/embed.py
	python3 tools/embed.py $< $@ web_flv_js

$(GENDIR)/web_flv_license.h: web/vendor/flv.LICENSE tools/embed.py
	python3 tools/embed.py $< $@ web_flv_license

$(GENDIR)/web_videojs.h: web/vendor/videojs.min.js tools/embed.py
	python3 tools/embed.py $< $@ web_videojs_js

$(GENDIR)/web_videojs_license.h: web/vendor/videojs.LICENSE tools/embed.py
	python3 tools/embed.py $< $@ web_videojs_license

$(GENDIR)/web_player_css.h: web/player.css tools/embed.py
	python3 tools/embed.py $< $@ web_player_css

build/ffmpeg-$(FFMPEG_VERSION).tar.xz: tools/fetch.py
	@mkdir -p build
	python3 tools/fetch.py $(FFMPEG_URL) $(FFMPEG_SHA256) $@

$(FFMPEG_STAMP): build/ffmpeg-$(FFMPEG_VERSION).tar.xz
	rm -rf $(FFMPEG_BUILD) && mkdir -p $(FFMPEG_BUILD)/obj
	tar -xJf $< -C $(FFMPEG_BUILD)
	cd $(FFMPEG_BUILD)/obj && ../ffmpeg-$(FFMPEG_VERSION)/configure \
	  --prefix=$(FFMPEG_PREFIX) $(FFMPEG_CONFIG) > configure.log
	$(MAKE) -C $(FFMPEG_BUILD)/obj install > $(FFMPEG_BUILD)/build.log
	touch $@

$(ELF): $(GEN) $(SRCS) $(FFMPEG_STAMP) $(wildcard $(SRCDIR)/*.h) FORCE
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(FFMPEG_LIBS) $(LDFLAGS)

clean:
	rm -f $(ELF) $(GEN)

# Send to a running ELF loader (elfldr, port 9021).
deploy: $(ELF)
	python3 tools/deploy.py $(PS5_HOST) $(PS5_PORT) $(ELF)

test: host-test

# Finite hardware probes: building does not deploy them.
probe-audio:
	@mkdir -p build
	$(CC) $(CFLAGS) tests/audio_probe.c src/audio.c src/avcap.c $(LDFLAGS) -o build/audio-probe.elf

probe-video:
	@mkdir -p build
	$(CC) $(CFLAGS) tests/video_probe.c src/avcap.c $(LDFLAGS) -o build/video-probe.elf

host-test: $(GEN)
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) tests/test_avcap.c -o build/test_avcap
	./build/test_avcap
	$(HOST_CC) $(HOST_CFLAGS) tests/test_audio.c -o build/test_audio
	./build/test_audio
	$(HOST_CC) $(HOST_CFLAGS) tests/test_capture_loop.c src/flv.c src/settings.c src/snapshot.c -o build/test_capture_loop
	./build/test_capture_loop
	$(HOST_CC) $(HOST_CFLAGS) tests/test_transport.c src/flv.c -o build/test_transport
	./build/test_transport
	@test -n "$(HOST_FFMPEG)" || { echo "host tests need libavcodec, libswscale and libavutil development packages (pkg-config)"; exit 1; }
	$(HOST_CC) $(HOST_CFLAGS) tests/http_fixture.c src/http.c src/flv.c src/status.c src/settings.c src/snapshot.c src/frame_jpeg.c $(HOST_FFMPEG) -o build/http_fixture
	python3 tests/test_http.py build/http_fixture
	node tests/test_preview.cjs
	$(HOST_CC) $(HOST_CFLAGS) tests/transport_fixture_server.c src/flv.c src/http.c src/status.c src/settings.c src/snapshot.c src/frame_jpeg.c $(HOST_FFMPEG) -o build/transport_fixture
	python3 tests/test_streaming.py build/transport_fixture
