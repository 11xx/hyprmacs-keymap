// -*- mode: c++; -*-
// The deterministic Emacs-chord engine. Consumes key events (modifier and
// non-modifier) and produces, per event, a decision to suppress the key and/or
// commit one or more actions.
//
// Matching is eager, like Emacs: a binding fires the instant the key sequence
// is complete (a final binding), even if modifiers are still held. The engine
// only waits while the current sequence is a prefix of a longer one. Because a
// sequence can never be both a prefix and a final binding, there is never
// ambiguity about whether to fire now or wait.
//
// The engine tracks held modifiers from the key-event stream itself (it sees
// each modifier press/release in order), which is race-free — unlike the
// compositor's async aggregate modifier mask, which can lag a just-pressed
// modifier. This keeps it fully deterministic and testable without Hyprland.
#pragma once

#include "Core.hpp"
#include "PrefixTree.hpp"

#include <set>
#include <vector>

namespace hyprmacs {

// One key event fed to the engine. isModifier marks events for keys that ARE
// modifiers; they only update held state and always pass through. modBit is
// the chord bit of such a key, masked to MOD_ALL — it may be 0 for modifiers
// with no chord bit (AltGr, locks), which update nothing but must not abort a
// sequence in progress.
struct StepInput {
    Keysym  sym        = 0; // resolved + canonicalised keysym (0 if unresolvable)
    Keycode code       = 0; // xkb keycode, for "code:NN" chords & release tracking
    Mods    modBit     = 0;
    bool    isModifier = false;
    bool    pressed    = false;
};

struct StepResult {
    bool                suppress = false; // true => consume the key event
    std::vector<Action> commits;          // actions to run now, in order
};

class ChordStateMachine {
  public:
    explicit ChordStateMachine(const PrefixTree* tree) : m_tree(tree), m_current(tree->root()) {}

    StepResult step(const StepInput& in);

    // Fired by the host's prefix timeout. Resets to idle only if no chord
    // activity has happened since the timer was armed (generation guard).
    void timeoutReset(uint64_t armedGeneration);

    // Drop all transient state (held mods, position, consumed keys). On reload.
    void reset();

    Mods     heldMods() const { return m_held; }
    bool     awaitingNextChord() const { return m_current != m_tree->root(); }
    uint64_t generation() const { return m_gen; }

  private:
    void gotoRoot(); // move to idle, bump generation

    const PrefixTree* m_tree;
    const Node*       m_current;
    Mods              m_held = 0;
    std::set<Keycode> m_consumedDown; // keycodes of non-mod presses we suppressed
    uint64_t          m_gen = 0;
};

} // namespace hyprmacs
