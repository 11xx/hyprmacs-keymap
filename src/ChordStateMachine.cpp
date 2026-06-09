#include "ChordStateMachine.hpp"

namespace hyprmacs {

void ChordStateMachine::gotoRoot() {
    m_current = m_tree->root();
    ++m_gen;
}

void ChordStateMachine::reset() {
    m_current = m_tree->root();
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

    // ---- key release ------------------------------------------------------
    // Suppress the release iff we suppressed the matching press, keeping
    // press/release symmetric for clients.
    if (!in.pressed) {
        r.suppress = (m_consumedDown.erase(in.sym) > 0);
        return r;
    }

    // ---- key press --------------------------------------------------------
    // The chord is (modifiers held right now) + this key.
    const Node* child = m_current->child(Chord{in.mods, in.sym});

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

    r.suppress = true;
    m_consumedDown.insert(in.sym);

    if (child->isPrefix()) {
        // More keys can follow — advance and wait for the next chord.
        m_current = child;
        ++m_gen;
        return r;
    }

    // Final binding — eager commit on key-down, even with modifiers still held.
    // A node is never both a prefix and a final, so this is unambiguous. All
    // actions registered on this binding run in order.
    for (ActionId a : child->actions)
        r.commits.push_back(a);
    gotoRoot();
    return r;
}

} // namespace hyprmacs
