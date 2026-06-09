// -*- mode: c++; -*-
// Parses Hyprmacs key-sequence strings into a list of Chord steps.
//
// Syntax (matches reference/hyprmacs-keymap.org):
//   * space-separated chords:           "s-x e e"
//   * Emacs modifier prefixes (case-sensitive):
//       s- = Super, C- = Ctrl, M- = Alt/Meta, S- = Shift
//   * raw Hyprland syntax when a chord contains '+':  "SUPER + F"
//   * final-key aliases: SPC/SPACE->space, RET/RETURN->return,
//     ESC/ESCAPE->escape; single ASCII letters are matched case-insensitively.
#pragma once

#include "Core.hpp"

#include <string>
#include <vector>

namespace hyprmacs {

struct ParseResult {
    bool               ok = false;
    std::string        error;       // populated when !ok
    std::vector<Chord> chords;
};

// Parse a whole sequence ("s-x e c") into chord steps.
ParseResult parseSequence(const std::string& sequence);

// Resolve a single key name (already alias-normalised or raw) to a canonical
// lower-cased keysym. Returns 0 (XKB_KEY_NoSymbol) if unknown. Exposed for the
// plugin so it can canonicalise the live pressed keysym the same way.
Keysym resolveKeyName(const std::string& name);

// Canonicalise an already-resolved keysym (xkb_keysym_to_lower). The plugin
// applies this to the pressed keysym so it matches the parsed, canonical tree.
Keysym canonicaliseSym(Keysym sym);

} // namespace hyprmacs
