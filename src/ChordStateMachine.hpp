// -*- mode: c++; -*-
// The deterministic Emacs-chord engine. Consumes non-modifier key events
// (each carrying the modifier mask held at that moment) and produces, per
// event, a decision to suppress the key and/or commit one or more actions.
//
// Matching is eager, like Emacs: a binding fires the instant the key sequence
// is complete (a final binding), even if modifiers are still held. The engine
// only waits while the current sequence is a prefix of a longer one. Because a
// sequence can never be both a prefix and a final binding, there is never
// ambiguity about whether to fire now or wait.
//
// The host supplies the modifier mask with each key press (it knows the
// authoritative state), so the engine itself never tracks modifiers — that
// keeps it correct regardless of remappers/virtual keyboards. Modifier keys
// are not fed to the engine at all.
#pragma once

#include "Core.hpp"
#include "PrefixTree.hpp"

#include <set>
#include <vector>

namespace hyprmacs {

// One non-modifier key event fed to the engine.
struct StepInput {
    Keysym sym     = 0;     // resolved + canonicalised keysym
    Mods   mods    = 0;     // modifiers held at this event (masked to MOD_ALL)
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

    // Drop all transient state (position, consumed keys). Used on reload.
    void reset();

    bool     awaitingNextChord() const { return m_current != m_tree->root(); }
    uint64_t generation() const { return m_gen; }

  private:
    void gotoRoot(); // move to idle, bump generation

    const PrefixTree* m_tree;
    const Node*       m_current;
    std::set<Keysym>  m_consumedDown; // non-mod presses we suppressed
    uint64_t          m_gen = 0;
};

} // namespace hyprmacs
