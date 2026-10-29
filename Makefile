# PS5 HDMI Stream — payload build
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

ELF     := ps5-hdmi-stream.elf
SRCDIR  := src
GENDIR  := $(SRCDIR)/gen
SRCS    := $(SRCDIR)/main.c $(SRCDIR)/flv.c $(SRCDIR)/avcap.c \
           $(SRCDIR)/rtmp.c $(SRCDIR)/http.c $(SRCDIR)/scanin.c \
           $(SRCDIR)/audio.c $(SRCDIR)/status.c
GEN     := $(GENDIR)/web_index.h $(GENDIR)/web_flv.h $(GENDIR)/web_flv_license.h

CFLAGS  := -Wall -Wextra -Werror -O2 -g -rdynamic -pthread -I$(SRCDIR)
LDFLAGS := -pthread

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CFLAGS  += -DPS5HS_VERSION=\"$(VERSION)\"
ifneq ($(HTTP_PORT),)
CFLAGS += -DDEF_HTTP_PORT=$(HTTP_PORT)
endif
ifneq ($(RTMP_PORT),)
CFLAGS += -DDEF_RTMP_PORT=$(RTMP_PORT)
endif

.PHONY: all clean deploy test host-test probe-audio probe-video FORCE

all: $(ELF)

# Embed the preview page as a C byte array so the payload is a single ELF.
$(GENDIR)/web_index.h: web/index.html
	@mkdir -p $(GENDIR)
	@printf 'static const unsigned char web_index_html[] = {\n' > $@
	@xxd -i < web/index.html >> $@
	@printf '};\nstatic const unsigned int web_index_html_len = sizeof(web_index_html);\n' >> $@

$(GENDIR)/web_flv.h: web/vendor/flv.min.js
	@mkdir -p $(GENDIR)
	@printf 'static const unsigned char web_flv_js[] = {\n' > $@
	@xxd -i < $< >> $@
	@printf '};\nstatic const unsigned int web_flv_js_len = sizeof(web_flv_js);\n' >> $@

$(GENDIR)/web_flv_license.h: web/vendor/flv.LICENSE
	@mkdir -p $(GENDIR)
	@printf 'static const unsigned char web_flv_license[] = {\n' > $@
	@xxd -i < $< >> $@
	@printf '};\nstatic const unsigned int web_flv_license_len = sizeof(web_flv_license);\n' >> $@

$(ELF): $(GEN) $(SRCS) $(wildcard $(SRCDIR)/*.h) FORCE
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(ELF) $(GEN)

# Send to a running ELF loader (elfldr, port 9021).
deploy: $(ELF)
	python3 tools/deploy.py $(PS5_HOST) $(PS5_PORT) $(ELF)

test: host-test

# Finite hardware probes: building does not deploy them.
probe-audio:
	@mkdir -p build
	$(CC) $(CFLAGS) tests/audio_probe.c src/audio.c $(LDFLAGS) -o build/audio-probe.elf

probe-video:
	@mkdir -p build
	$(CC) $(CFLAGS) tests/video_probe.c src/avcap.c src/scanin.c $(LDFLAGS) -o build/video-probe.elf

host-test: $(GEN)
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) tests/test_avcap.c -o build/test_avcap
	./build/test_avcap
	$(HOST_CC) $(HOST_CFLAGS) tests/test_audio.c -o build/test_audio
	./build/test_audio
	$(HOST_CC) $(HOST_CFLAGS) tests/test_capture_loop.c src/flv.c -o build/test_capture_loop
	./build/test_capture_loop
	$(HOST_CC) $(HOST_CFLAGS) tests/test_transport.c src/flv.c -o build/test_transport
	./build/test_transport
	$(HOST_CC) $(HOST_CFLAGS) tests/http_fixture.c src/http.c src/flv.c src/status.c -o build/http_fixture
	python3 tests/test_http.py build/http_fixture
	node tests/test_preview.cjs
	$(HOST_CC) $(HOST_CFLAGS) -DTEST_HTTP tests/transport_fixture_server.c src/flv.c src/rtmp.c src/http.c src/status.c -o build/transport_fixture
	python3 tests/test_streaming.py build/transport_fixture
