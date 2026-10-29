# Snowhush for Hyprland
#
#   make            build ./snowhush
#   make install    install to ~/.local/bin
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc -Ibuild
LDLIBS  += -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lm

PNG     := $(shell pkg-config --exists libpng && echo yes)
ifeq ($(PNG),yes)
CFLAGS  += $(shell pkg-config --cflags libpng)
LDLIBS  += $(shell pkg-config --libs libpng)
else
CFLAGS  += -DSNOW_NO_PNG
endif

# The tray icon is a StatusNotifierItem served over the session bus.
SYSTEMD := $(shell pkg-config --exists libsystemd && echo yes)
ifeq ($(SYSTEMD),yes)
CFLAGS  += $(shell pkg-config --cflags libsystemd)
LDLIBS  += $(shell pkg-config --libs libsystemd)
else
CFLAGS  += -DSNOW_NO_TRAY
endif

BUILD      := build
LAYER_PROTO := protocol/wlr-layer-shell-unstable-v1.xml
XDG_PROTO  := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
SOURCES    := src/main.c src/sim.c src/gl.c src/wl.c src/hypr.c src/json.c src/image.c src/tray.c
OBJECTS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SOURCES))
GENERATED  := $(BUILD)/layer-shell-protocol.c $(BUILD)/layer-shell-client-protocol.h \
              $(BUILD)/xdg-shell-protocol.c $(BUILD)/xdg-shell-client-protocol.h
PROTO_OBJ  := $(BUILD)/layer-shell.o $(BUILD)/xdg-shell.o

all: snowhush

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/layer-shell-protocol.c: $(LAYER_PROTO) | $(BUILD)
	wayland-scanner private-code $(LAYER_PROTO) $@

$(BUILD)/layer-shell-client-protocol.h: $(LAYER_PROTO) | $(BUILD)
	wayland-scanner client-header $(LAYER_PROTO) $@

$(BUILD)/xdg-shell-protocol.c: $(XDG_PROTO) | $(BUILD)
	wayland-scanner private-code $(XDG_PROTO) $@

$(BUILD)/xdg-shell-client-protocol.h: $(XDG_PROTO) | $(BUILD)
	wayland-scanner client-header $(XDG_PROTO) $@

$(BUILD)/layer-shell.o: $(BUILD)/layer-shell-protocol.c $(BUILD)/layer-shell-client-protocol.h
	$(CC) $(CFLAGS) -c $(BUILD)/layer-shell-protocol.c -o $@

$(BUILD)/xdg-shell.o: $(BUILD)/xdg-shell-protocol.c $(BUILD)/xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c $(BUILD)/xdg-shell-protocol.c -o $@

$(BUILD)/%.o: src/%.c $(GENERATED) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

snowhush: $(OBJECTS) $(PROTO_OBJ)
	$(CC) $(OBJECTS) $(PROTO_OBJ) -o $@ $(LDLIBS)

install: snowhush
	install -Dm755 snowhush $(HOME)/.local/bin/snowhush

clean:
	rm -rf $(BUILD) snowhush

.PHONY: all install clean
