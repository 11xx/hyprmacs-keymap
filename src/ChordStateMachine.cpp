#include "ChordStateMachine.hpp"

namespace hyprmacs {

void ChordStateMachine::gotoRoot() {
    m_current = m_tree->root();
    ++m_gen;
}

void ChordStateMachine::reset() {
    m_current = m_tree->root();
    m_held    = 0;
    m_pending.reset();
    m_consumedDown.clear();
    ++m_gen;
}

void ChordStateMachine::timeoutReset(uint64_t armedGeneration) {
    if (m_gen != armedGeneration)
        return; // chord activity happened after the timer was armed; ignore
    gotoRoot();
}

StepResult ChordStateMachine::step(const StepInput& in) {
    StepResult r;

    // ---- modifier key -----------------------------------------------------
    if (in.modBit != 0) {
        if (in.pressed) {
            m_held |= in.modBit;
        } else {
            m_held &= ~in.modBit;
            // Commit a deferred modified leaf once *all* of its modifiers are
            // released — order-independent, and never downgrades the chord.
            if (m_pending && (m_pending->chordMods & m_held) == 0) {
                r.commits = m_pending->actions;
                m_pending.reset();
            }
        }
        r.suppress = false; // modifiers always pass through
        return r;
    }

    // ---- non-modifier release --------------------------------------------
    if (!in.pressed) {
        // Suppress the release iff we suppressed the matching press, keeping
        // press/release symmetric for clients.
        r.suppress = (m_consumedDown.erase(in.sym) > 0);
        return r;
    }

    // ---- non-modifier press ----------------------------------------------
    // A new non-mod key starts the next chord, which flushes any pending
    // (already-recognised) modified leaf first.
    if (m_pending) {
        r.commits   = m_pending->actions;
        m_pending.reset();
        // current is already root whenever a pending exists
    }

    const Node* child = m_current->child(Chord{m_held, in.sym});

    if (!child) {
        const bool atRoot = (m_current == m_tree->root());
        gotoRoot();
        if (atRoot) {
            r.suppress = false; // unknown key at idle: let normal binds/typing run
        } else {
            // Unknown key inside a prefix: abort the sequence and eat the key.
            r.suppress = true;
            m_consumedDown.insert(in.sym);
        }
        return r;
    }

    if (child->isPrefix()) {
        m_current = child;
        ++m_gen;
        r.suppress = true;
        m_consumedDown.insert(in.sym);
        return r;
    }

    // Final binding.
    r.suppress = true;
    m_consumedDown.insert(in.sym);

    if (m_held != 0) {
        // Defer: recognised now, committed on modifier-release or next non-mod
        // key. gotoRoot so a fresh sequence can begin while mods stay held.
        m_pending = Pending{child->actions, m_held};
        gotoRoot();
    } else {
        // No modifiers to wait on: commit immediately.
        for (ActionId a : child->actions)
            r.commits.push_back(a);
        gotoRoot();
    }
    return r;
}

} // namespace hyprmacs
