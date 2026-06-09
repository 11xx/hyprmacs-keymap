// -*- mode: c++; -*-
// The deterministic Emacs-chord engine. Consumes key events (press/release of
// modifier and non-modifier keys) and produces, per event, a decision to
// suppress the key and/or commit one or more actions.
//
// See reference/hyprmacs-keymap.org for the behaviour being preserved and
// docs in README.md for the commit rules. The engine tracks held modifiers
// itself, so it is fully deterministic and testable without Hyprland.
#pragma once

#include "Core.hpp"
#include "PrefixTree.hpp"

#include <optional>
#include <set>
#include <vector>

namespace hyprmacs {

// One key event fed to the engine.
struct StepInput {
    Keysym sym     = 0; // resolved+canonicalised keysym (ignored when modBit!=0)
    Mods   modBit  = 0; // non-zero iff this key IS a modifier (its bit)
    bool   pressed = false;
};

struct StepResult {
    bool                  suppress = false; // true => consume the key event
    std::vector<ActionId> commits;          // actions to run now, in order
};

class ChordStateMachine {
  public:
    explicit ChordStateMachine(const PrefixTree* tree) : m_tree(tree), m_current(tree->root()) {}

    StepResult step(const StepInput& in);

    // Fired by the host's prefix timeout. Resets to idle only if no chord
    // activity has happened since the timer was armed (generation guard).
    void timeoutReset(uint64_t armedGeneration);

    // Drop all transient state (held mods, pending, position). Used on reload.
    void reset();

    Mods     heldMods() const { return m_held; }
    bool     awaitingNextChord() const { return m_current != m_tree->root(); }
    bool     hasPending() const { return m_pending.has_value(); }
    uint64_t generation() const { return m_gen; }

  private:
    struct Pending {
        std::vector<ActionId> actions;
        Mods                  chordMods = 0;
    };

    void gotoRoot(); // move to idle, bump generation (does NOT touch pending)

    const PrefixTree*      m_tree;
    const Node*            m_current;
    Mods                   m_held = 0;
    std::optional<Pending> m_pending;
    std::set<Keysym>       m_consumedDown; // non-mod presses we suppressed
    uint64_t               m_gen = 0;
};

} // namespace hyprmacs
