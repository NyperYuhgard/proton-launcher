# Proton Launcher - build rules
#
# Targets:
#   make            build build/proton-launcher
#   make check      build and run the core unit tests
#   make install    install to $(DESTDIR)$(PREFIX)/bin
#   make run        build and launch
#   make clean      remove build artifacts

PACKAGE   := proton-launcher
VERSION   := 2.0.0

PREFIX    ?= /usr/local
DESTDIR   ?=
BINDIR    := $(DESTDIR)$(PREFIX)/bin

CC        ?= cc
PKG_CONFIG?= pkg-config

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/$(PACKAGE)

SRCS      := $(wildcard src/*.c)
OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS      := $(OBJS:.o=.d)

PKGS      := gtk+-3.0 glib-2.0 gio-2.0

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS))

WARNINGS := -Wall -Wextra -Wno-unused-parameter \
            -Wmissing-prototypes -Wstrict-prototypes \
            -Wshadow -Wpointer-arith -Wwrite-strings

CFLAGS   ?= -O2
ALL_CFLAGS := -std=c11 $(WARNINGS) $(CFLAGS) $(PKG_CFLAGS) \
              -DPL_VERSION_STRING='"$(VERSION)"' -MMD -MP
ALL_LDFLAGS := $(LDFLAGS)
ALL_LIBS := $(PKG_LIBS) -lm

.PHONY: all clean install uninstall run check check-gui check-asan

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LIBS)
	@echo "built $@"

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

# The tests link the non-UI core only, so they run headless with no
# display or Proton installation required.
CORE_SRCS := src/pl_config.c src/pl_prefix.c src/pl_preflight.c \
             src/pl_proton.c src/pl_run.c
CORE_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
TEST_BIN  := $(BUILD_DIR)/test-core

$(TEST_BIN): $(BUILD_DIR)/test-core.o $(CORE_OBJS)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LIBS)

$(BUILD_DIR)/test-core.o: tests/test-core.c src/pl.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -Isrc -c -o $@ $<

check: $(TEST_BIN)
	$(TEST_BIN)

# The GUI smoke test needs a display, so it skips itself when there is none.
# It links the UI sources too, minus main.c.
UI_SRCS  := src/ui_prefs.c src/ui_window.c src/ui_wizard.c
UI_OBJS  := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(UI_SRCS))
GUI_BIN  := $(BUILD_DIR)/test-gui

$(GUI_BIN): $(BUILD_DIR)/test-gui.o $(UI_OBJS) $(CORE_OBJS)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LIBS)

$(BUILD_DIR)/test-gui.o: tests/test-gui.c src/pl.h src/ui.h src/ui_private.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -Isrc -c -o $@ $<

check-gui: $(GUI_BIN)
	$(GUI_BIN)

# Same test under ASan/UBSan/LSan, for when memory bugs are the target.
check-asan: $(GUI_BIN) $(TEST_BIN)
	ASAN_OPTIONS=detect_leaks=1 \
	LSAN_OPTIONS=suppressions=tests/lsan-suppressions.txt:print_suppressions=0 \
	$(TEST_BIN)
	ASAN_OPTIONS=detect_leaks=1 \
	LSAN_OPTIONS=suppressions=tests/lsan-suppressions.txt:print_suppressions=0 \
	$(GUI_BIN)

install: $(TARGET)
	install -Dm755 $(TARGET) $(BINDIR)/$(PACKAGE)

uninstall:
	rm -f $(BINDIR)/$(PACKAGE)

run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)