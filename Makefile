# PS5 HDMI Stream — payload build
#
# Requires the ps5-payload-dev SDK:
#   export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
# On macOS also point the toolchain at a modern LLVM, e.g.:
#   export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config

PS5_HOST ?= 192.168.0.190
PS5_PORT ?= 9021

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK is undefined; export it to point at the SDK root)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF     := ps5-hdmi-stream.elf
SRCDIR  := src
GENDIR  := $(SRCDIR)/gen
SRCS    := $(SRCDIR)/main.c $(SRCDIR)/flv.c $(SRCDIR)/avcap.c \
           $(SRCDIR)/rtmp.c $(SRCDIR)/http.c $(SRCDIR)/scanin.c $(SRCDIR)/share.c
GEN     := $(GENDIR)/web_index.h

CFLAGS  := -Wall -Wextra -Werror -O2 -g -rdynamic -pthread -I$(SRCDIR)
LDFLAGS := -pthread

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CFLAGS  += -DPS5HS_VERSION=\"$(VERSION)\"

.PHONY: all clean deploy test

all: $(ELF)

# Embed the preview page as a C byte array so the payload is a single ELF.
$(GEN): web/index.html
	@mkdir -p $(GENDIR)
	@printf 'static const unsigned char web_index_html[] = {\n' > $@
	@xxd -i < web/index.html >> $@
	@printf '};\nstatic const unsigned int web_index_html_len = sizeof(web_index_html);\n' >> $@

$(ELF): $(GEN) $(SRCS) $(wildcard $(SRCDIR)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(ELF) $(GEN)

# Send to a running ELF loader (elfldr, port 9021).
deploy: $(ELF)
	nc -q0 $(PS5_HOST) $(PS5_PORT) < $(ELF)

test: deploy
