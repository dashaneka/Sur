CC := gcc
PKG := gtk4 libadwaita-1 gstreamer-1.0 gstreamer-tag-1.0
CFLAGS_PKG := $(shell pkg-config --cflags $(PKG))
LIBS_PKG := $(shell pkg-config --libs $(PKG)) -lm

WARN := -std=c17 -Wall -Wextra -Werror -Wformat=2 -Wshadow
OPT := -Oz -flto=auto -ffunction-sections -fdata-sections -fvisibility=hidden \
       -fno-asynchronous-unwind-tables -fno-unwind-tables -fomit-frame-pointer \
       -fno-ident -fmerge-all-constants
LDFLAGS_OPT := -flto=auto -Wl,--gc-sections -Wl,--as-needed -Wl,-O1 \
               -Wl,--build-id=none -Wl,--no-eh-frame-hdr -s

SRCS := src/core.c src/mpris.c src/app.c
TARGET := sur

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS) src/core.h src/mpris.h
	$(CC) $(WARN) $(OPT) $(CFLAGS_PKG) $(SRCS) -o $(TARGET) $(LDFLAGS_OPT) $(LIBS_PKG)
	strip --strip-all -R .comment -R .note.gnu.build-id -R .note.ABI-tag \
		-R .annobin.notes -R .gnu.build.attributes $(TARGET)
	@ls -lh $(TARGET) && stat -c '%s bytes  %n' $(TARGET)

test: src/core.c tests/test_core.c src/core.h
	$(CC) $(WARN) -O2 -g -D_FORTIFY_SOURCE=2 -fstack-protector-strong $(CFLAGS_PKG) \
		src/core.c tests/test_core.c -o test-core $(LIBS_PKG)
	./test-core
	rm -f test-core

clean:
	rm -f $(TARGET) sur-nano test-core
