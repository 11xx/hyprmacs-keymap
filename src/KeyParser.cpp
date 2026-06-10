#include "KeyParser.hpp"

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cctype>
#include <optional>

namespace hyprmacs {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::string toUpper(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::vector<std::string> split(const std::string& s, char delim, bool keepEmpty) {
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (c == delim) {
            if (keepEmpty || !cur.empty())
                out.push_back(cur);
            cur.clear();
        } else
            cur.push_back(c);
    }
    if (keepEmpty || !cur.empty())
        out.push_back(cur);
    return out;
}

// Remove whitespace immediately around '+' so raw Hyprland chords written with
// spaces ("SUPER + F") survive the chord-level whitespace split as one token,
// while space-separated Emacs chords ("s-x f") are still split normally.
std::string collapsePlusSpaces(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
                out.pop_back();
            out.push_back('+');
            size_t j = i + 1;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
                ++j;
            i = j - 1;
        } else
            out.push_back(s[i]);
    }
    return out;
}

std::vector<std::string> splitWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Emacs-style modifier token (case-sensitive single letter), per the Org spec.
std::optional<Mods> emacsMod(const std::string& tok) {
    if (tok == "s")
        return MOD_SUPER;
    if (tok == "C")
        return MOD_CTRL;
    if (tok == "M")
        return MOD_ALT;
    if (tok == "S")
        return MOD_SHIFT;
    return std::nullopt;
}

// Raw Hyprland modifier token (used inside "SUPER + F" syntax). Mirrors the
// names CKeybindManager::stringToModMask accepts for the four chord modifiers.
std::optional<Mods> rawMod(const std::string& tokRaw) {
    const std::string t = toUpper(trim(tokRaw));
    if (t == "SUPER" || t == "WIN" || t == "LOGO" || t == "MOD4" || t == "META")
        return MOD_SUPER;
    if (t == "CTRL" || t == "CONTROL")
        return MOD_CTRL;
    if (t == "ALT" || t == "MOD1")
        return MOD_ALT;
    if (t == "SHIFT")
        return MOD_SHIFT;
    return std::nullopt;
}

// Apply the final-key aliases from the Org spec. Case handling beyond this is
// done by canonicalising the resolved keysym to lower case.
std::string normalizeKeyName(const std::string& key) {
    if (key == " ")
        return "space";
    const std::string up = toUpper(key);
    if (up == "SPC" || up == "SPACE")
        return "space";
    if (up == "RET" || up == "RETURN")
        return "return";
    if (up == "ESC" || up == "ESCAPE")
        return "escape";
    return key;
}

} // namespace

Keysym canonicaliseSym(Keysym sym) {
    return static_cast<Keysym>(xkb_keysym_to_lower(static_cast<xkb_keysym_t>(sym)));
}

bool modFromKeysym(Keysym sym, Mods& bit) {
    bit = 0;
    switch (sym) {
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Hyper_L:
        case XKB_KEY_Hyper_R: bit = MOD_SUPER; return true;
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R: bit = MOD_ALT; return true;
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R: bit = MOD_CTRL; return true;
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R: bit = MOD_SHIFT; return true;
        // Modifiers that exist at the XKB level but are not chord modifiers
        // (AltGr, locks, group switch). They must still be treated as modifier
        // events — pressing AltGr mid-sequence must not abort the chord — they
        // just contribute no chord bit.
        case XKB_KEY_ISO_Level3_Shift:
        case XKB_KEY_ISO_Level5_Shift:
        case XKB_KEY_Mode_switch:
        case XKB_KEY_Caps_Lock:
        case XKB_KEY_Shift_Lock:
        case XKB_KEY_Num_Lock: return true;
        default: return false;
    }
}

Keysym resolveKeyName(const std::string& name) {
    xkb_keysym_t s = xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_NO_FLAGS);
    if (s == XKB_KEY_NoSymbol)
        s = xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (s == XKB_KEY_NoSymbol)
        return 0;
    return canonicaliseSym(static_cast<Keysym>(s));
}

namespace {

// Resolve a final-key token into `out` (mods already set): either a
// Hyprland-style raw keycode ("code:NN", matched against the xkb keycode,
// i.e. libinput code + 8) or a keysym name.
bool resolveFinalKey(const std::string& token, Chord& out, std::string& err) {
    if (token.starts_with("code:")) {
        const std::string num = token.substr(5);
        if (num.empty() || !std::ranges::all_of(num, [](unsigned char c) { return std::isdigit(c); })) {
            err = "invalid keycode '" + token + "'";
            return false;
        }
        out.code = static_cast<Keycode>(std::stoul(num));
        out.sym  = 0;
        return true;
    }
    const std::string keyName = normalizeKeyName(token);
    out.sym                   = resolveKeyName(keyName);
    out.code                  = 0;
    if (out.sym == 0) {
        err = "unknown key '" + token + "'";
        return false;
    }
    return true;
}

// Parse one chord written in raw Hyprland syntax: "SUPER + SHIFT + F".
bool parseRawChord(const std::string& chord, Chord& out, std::string& err) {
    auto parts = split(chord, '+', /*keepEmpty=*/true);
    for (auto& p : parts)
        p = trim(p);
    // drop empty leading/trailing tokens produced by surrounding spaces
    std::erase_if(parts, [](const std::string& p) { return p.empty(); });
    if (parts.empty()) {
        err = "empty chord";
        return false;
    }

    Mods mods = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        auto m = rawMod(parts[i]);
        if (!m) {
            err = "unknown modifier '" + parts[i] + "' in raw chord '" + chord + "'";
            return false;
        }
        mods |= *m;
    }

    out = Chord{mods, 0, 0};
    if (!resolveFinalKey(parts.back(), out, err)) {
        err += " in chord '" + chord + "'";
        return false;
    }
    return true;
}

// Parse one chord written in Emacs syntax: "s-M-c".
bool parseEmacsChord(const std::string& chord, Chord& out, std::string& err) {
    auto parts = split(chord, '-', /*keepEmpty=*/false);
    if (parts.empty()) {
        err = "empty chord";
        return false;
    }

    auto asSingleKey = [&](const std::string& name) -> bool {
        out = Chord{0, 0, 0};
        return resolveFinalKey(name, out, err);
    };

    if (parts.size() == 1)
        return asSingleKey(parts[0]);

    Mods mods = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        auto m = emacsMod(parts[i]);
        if (!m) {
            // Not an Emacs chord after all (e.g. a bare key name that happens to
            // contain '-'); treat the whole token as a single key name, matching
            // the Org parser's `return chord` fallback.
            return asSingleKey(chord);
        }
        mods |= *m;
    }

    out = Chord{mods, 0, 0};
    if (!resolveFinalKey(parts.back(), out, err)) {
        err += " in chord '" + chord + "'";
        return false;
    }
    return true;
}

} // namespace

ParseResult parseSequence(const std::string& sequence) {
    ParseResult res;
    const auto  chords = splitWhitespace(collapsePlusSpaces(sequence));
    if (chords.empty()) {
        res.error = "empty key sequence";
        return res;
    }

    for (const auto& chordStr : chords) {
        Chord       chord;
        std::string err;
        const bool  ok = (chordStr.find('+') != std::string::npos) ? parseRawChord(chordStr, chord, err) : parseEmacsChord(chordStr, chord, err);
        if (!ok) {
            res.error = err;
            return res;
        }
        res.chords.push_back(chord);
    }

    res.ok = true;
    return res;
}

} // namespace hyprmacs
