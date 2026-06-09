# Hyprmacs — native Hyprland plugin (Hyprland 0.55.x)
#
#   make            build ./hyprmacs-keymap.so (default; this is what hyprpm runs)
#   make test       build & run the standalone chord simulation (no Hyprland)
#   make install    alias for `make all` — builds ./hyprmacs-keymap.so in the repo dir
#   make clean
#
# hyprpm builds with `pkg-config hyprland` on its managed PKG_CONFIG_PATH.
# For a manual build against a Hyprland checkout, pass HYPRLAND_SRC:
#   make HYPRLAND_SRC=/tmp/Hyprland

CXX ?= g++
HYPRLAND_SRC ?=

# Where `make install-helper` copies the Lua helper. Hyprland's config dir is
# already on Lua's package.path, so require("hyprmacs-keymap") finds it there.
LUA_HELPER_DIR ?= $(HOME)/.config/hypr

TARGET = hyprmacs-keymap.so
SRCS   = src/KeyParser.cpp src/PrefixTree.cpp src/ChordStateMachine.cpp src/Plugin.cpp

# Hyprland is built with C++26; plugins must match.
CXXFLAGS += -shared -fPIC -std=c++26 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

INCLUDES = $(shell pkg-config --cflags --keep-system-cflags hyprland 2>/dev/null)
ifneq ($(strip $(HYPRLAND_SRC)),)
INCLUDES += -I$(HYPRLAND_SRC) -I$(HYPRLAND_SRC)/protocols
endif

# Hyprland 0.55 embeds PUC Lua 5.5 (NOT LuaJIT). Build against the same Lua so
# the plugin shares Hyprland's lua_State and registry index.
LUA_CFLAGS := $(shell pkg-config --cflags lua 2>/dev/null || pkg-config --cflags lua5.5 2>/dev/null || pkg-config --cflags lua-5.5 2>/dev/null || pkg-config --cflags lua5.4 2>/dev/null)
LUA_LIBS   := $(shell pkg-config --libs   lua 2>/dev/null || pkg-config --libs   lua5.5 2>/dev/null || pkg-config --libs   lua-5.5 2>/dev/null || pkg-config --libs   lua5.4 2>/dev/null)

PKG_CFLAGS := $(INCLUDES) $(LUA_CFLAGS) $(shell pkg-config --cflags xkbcommon)
PKG_LIBS   := $(LUA_LIBS) $(shell pkg-config --libs xkbcommon)

.PHONY: all install install-helper test clean check-hyprland-src check-deps

all: check-hyprland-src check-deps $(TARGET)

install: all

# Copy the Lua helper onto Hyprland's Lua path. The .so is loaded separately
# (by hyprpm, or via hl.plugin.load); this only places the keymap_* API helper.
install-helper:
	mkdir -p "$(LUA_HELPER_DIR)"
	cp hyprmacs-keymap.lua "$(LUA_HELPER_DIR)/hyprmacs-keymap.lua"
	@echo "installed helper -> $(LUA_HELPER_DIR)/hyprmacs-keymap.lua  (add: require(\"hyprmacs-keymap\"))"

check-hyprland-src:
	@if test -n "$(HYPRLAND_SRC)"; then test -f "$(HYPRLAND_SRC)/src/plugins/PluginAPI.hpp" || \
		(printf 'HYPRLAND_SRC must point to a Hyprland source tree. Current: %s\n' "$(HYPRLAND_SRC)" >&2; exit 1); \
	fi

check-deps:
	@if test -z "$(HYPRLAND_SRC)"; then pkg-config --exists hyprland || \
		(printf 'Missing Hyprland headers. Run `hyprpm update`, or set PKG_CONFIG_PATH to a dir with hyprland.pc, or pass HYPRLAND_SRC.\n' >&2; exit 1); \
	fi

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) $(SRCS) -o $@ $(PKG_LIBS)
	@echo "built ./$(TARGET)"

# Standalone engine simulation — needs only xkbcommon, never Hyprland.
test:
	$(CXX) -std=c++26 -O2 -Wall -Wextra -Isrc \
		src/KeyParser.cpp src/PrefixTree.cpp src/ChordStateMachine.cpp tests/sim.cpp \
		$(shell pkg-config --cflags --libs xkbcommon) -o sim
	@echo "== running chord state-machine simulation ==" && ./sim

clean:
	rm -f $(TARGET) sim
