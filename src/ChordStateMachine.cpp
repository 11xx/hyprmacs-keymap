#include "ChordStateMachine.hpp"

namespace hyprmacs {

void ChordStateMachine::gotoRoot() {
    m_current = m_tree->root();
    ++m_gen;
}

void ChordStateMachine::reset() {
    gotoRoot();
}

void ChordStateMachine::timeoutReset(uint64_t armedGeneration) {
    if (m_gen != armedGeneration)
        return; // chord activity happened after the timer was armed; ignore
    gotoRoot();
}

void ChordStateMachine::trackModifier(const StepInput& in) {
    const KeyId key{in.device, in.code};
    if (in.pressed && in.modBit)
        m_heldModKeys[key] = in.modBit;
    else
        m_heldModKeys.erase(key);

    m_held = 0;
    for (const auto& [_, bit] : m_heldModKeys)
        m_held |= bit;
}

StepResult ChordStateMachine::stepPassive(const StepInput& in) {
    StepResult r;
    if (in.isModifier) {
        trackModifier(in);
        return r;
    }
    const KeyId key{in.device, in.code};
    if (!in.pressed) {
        r.suppress = (m_consumedDown.erase(key) > 0);
        return r;
    }
    m_consumedDown.erase(key); // a fresh press: any earlier release was lost
    if (awaitingNextChord())
        gotoRoot();
    return r;
}

StepResult ChordStateMachine::step(const StepInput& in) {
    StepResult r;

    // ---- modifier key -----------------------------------------------------
    // Track it from the event stream (race-free) and let it through. Modifiers
    // never fire or block a chord. Chord-neutral modifiers (modBit == 0, e.g.
    // AltGr) update nothing but still pass through without aborting a prefix.
    if (in.isModifier) {
        trackModifier(in);
        return r;
    }

    // ---- non-modifier release --------------------------------------------
    // Suppress the release iff we suppressed the matching press, keeping
    // press/release symmetric for clients and for Hyprland's own bind state.
    const KeyId key{in.device, in.code};
    if (!in.pressed) {
        r.suppress = (m_consumedDown.erase(key) > 0);
        return r;
    }

    // A fresh press of a key still marked as swallowed means its release was
    // lost; forget it so this press's release is routed by this press alone.
    m_consumedDown.erase(key);

    // ---- non-modifier press ----------------------------------------------
    // The chord is (modifiers held right now) + this key, matched by keysym
    // first, then by raw keycode (Hyprland "code:NN" style).
    const Node* child = in.sym ? m_current->child(Chord{m_held, in.sym, 0}) : nullptr;
    if (!child && in.code)
        child = m_current->child(Chord{m_held, 0, in.code});

    if (!child) {
        const bool atRoot = (m_current == m_tree->root());
        gotoRoot();
        if (atRoot) {
            r.suppress = false; // unknown key at idle: let normal binds/typing run
        } else {
            // Unknown key inside a prefix: abort the sequence and eat the key.
            r.suppress = true;
            m_consumedDown.insert(key);
        }
        return r;
    }

    r.suppress = true;
    m_consumedDown.insert(key);

    if (child->isPrefix()) {
        // More keys can follow — advance and wait for the next chord.
        m_current = child;
        ++m_gen;
        return r;
    }

    // Final binding — eager commit on key-down, even with modifiers still held.
    // A node is never both a prefix and a final, so this is unambiguous. All
    // actions registered on this binding run in order.
    for (const Action& a : child->actions)
        r.commits.push_back(a);
    gotoRoot();
    return r;
}

} // namespace hyprmacs
