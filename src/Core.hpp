// -*- mode: c++; -*-
// Shared, Hyprland-free core types for Hyprmacs.
//
// Everything in the core (Core.hpp, KeyParser, PrefixTree, ChordStateMachine)
// deliberately avoids including any Hyprland header so the chord engine can be
// unit-tested standalone (see tests/sim.cpp). The Hyprland glue lives entirely
// in src/Plugin.cpp.
#pragma once

#include <cstdint>

namespace hyprmacs {

// Resolved, layout-aware, modifier-independent keysym (an xkb_keysym_t),
// canonicalised to lower case so chord matching is case-insensitive the same
// way Hyprland's own bind matching is.
using Keysym = uint32_t;

// Modifier bitmask. The bit positions are intentionally identical to
// Hyprland's eKeyboardModifiers (devices/IKeyboard.hpp) so the plugin can feed
// HL_MODIFIER_* masks straight in after masking to MOD_ALL.
using Mods = uint32_t;

enum : Mods {
    MOD_SHIFT = 1u << 0, // HL_MODIFIER_SHIFT
    MOD_CTRL  = 1u << 2, // HL_MODIFIER_CTRL
    MOD_ALT   = 1u << 3, // HL_MODIFIER_ALT  (Emacs "M-")
    MOD_SUPER = 1u << 6, // HL_MODIFIER_META (Emacs "s-")
    MOD_ALL   = MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER,
};

// Opaque action handle. In the plugin this IS the LuaJIT registry ref of the
// action closure; in the test harness it is just an arbitrary integer.
using ActionId = int;

// A single chord step: the modifiers held when the non-modifier key was pressed
// plus that key's keysym.
struct Chord {
    Mods   mods = 0;
    Keysym sym  = 0;

    bool operator==(const Chord&) const = default;
};

} // namespace hyprmacs
