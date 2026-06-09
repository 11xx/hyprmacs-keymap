# Hyprmacs — native Hyprland plugin (Hyprland 0.55.x)
#
# Targets:
#   make            build the plugin -> build/hyprmacs.so
#   make test       build & run the standalone chord state-machine simulation
#   make install    copy build/hyprmacs.so to $(INSTALL_DIR)
#   make clean

CXX        ?= g++
CXXSTD     ?= -std=c++26

# Hyprland plugins must be built against the running Hyprland's headers and the
# same toolchain/ABI. The 'hyprland' pkg-config pulls in every transitive dep.
# Hyprland 0.55 embeds PUC Lua 5.5 (NOT LuaJIT); the plugin must use the very
# same Lua so it shares Hyprland's lua_State and registry index.
PLUGIN_PKGS = hyprland lua
PLUGIN_CFLAGS  = $(shell pkg-config --cflags $(PLUGIN_PKGS))

WARN       = -Wall -Wextra -Wno-unused-parameter
COMMON     = $(CXXSTD) $(WARN) -fPIC

INSTALL_DIR ?= $(HOME)/.config/hypr/plugins

BUILD      = build
CORE_SRC   = src/KeyParser.cpp src/PrefixTree.cpp src/ChordStateMachine.cpp
CORE_OBJ   = $(CORE_SRC:src/%.cpp=$(BUILD)/%.o)
PLUGIN_SRC = src/Plugin.cpp
PLUGIN_OBJ = $(PLUGIN_SRC:src/%.cpp=$(BUILD)/plugin_%.o)

PLUGIN_SO  = $(BUILD)/hyprmacs.so

.PHONY: all plugin test install clean

all: plugin

plugin: $(PLUGIN_SO)

# --- plugin objects (need Hyprland headers) -------------------------------
$(BUILD)/plugin_%.o: src/%.cpp | $(BUILD)
	$(CXX) $(COMMON) $(PLUGIN_CFLAGS) -c $< -o $@

# core objects are compiled twice: once for the plugin (with Hyprland include
# paths available, though they don't need them) and once for the test harness.
$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(COMMON) $(PLUGIN_CFLAGS) -c $< -o $@

$(PLUGIN_SO): $(CORE_OBJ) $(PLUGIN_OBJ)
	$(CXX) $(COMMON) -shared $^ -o $@ $(shell pkg-config --libs xkbcommon lua)
	@echo "built $(PLUGIN_SO)"

# --- standalone test harness (no Hyprland needed, only xkbcommon) ---------
test: $(BUILD)
	$(CXX) $(CXXSTD) $(WARN) -Isrc \
		src/KeyParser.cpp src/PrefixTree.cpp src/ChordStateMachine.cpp tests/sim.cpp \
		$(shell pkg-config --cflags --libs xkbcommon) \
		-o $(BUILD)/sim
	@echo "== running chord state-machine simulation ==" && $(BUILD)/sim

install: $(PLUGIN_SO)
	mkdir -p $(INSTALL_DIR)
	cp $(PLUGIN_SO) $(INSTALL_DIR)/hyprmacs.so
	@echo "installed to $(INSTALL_DIR)/hyprmacs.so"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
