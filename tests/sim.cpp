// Standalone deterministic simulation of the chord state machine.
// Builds with: make test   (links only xkbcommon, no Hyprland needed).
//
// Each scenario registers some chords, feeds a key-event script, and asserts
// the committed action sequence (and, where relevant, the suppress decision).
#include "ChordStateMachine.hpp"
#include "KeyParser.hpp"
#include "PrefixTree.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace hyprmacs;

namespace {

int g_failures = 0;
int g_checks   = 0;

// A small test rig: a tree + engine + a registry mapping ActionId -> label.
struct Sim {
    PrefixTree               tree;
    std::vector<std::string> labels; // index == ActionId
    ChordStateMachine        sm{&tree};

    // register a sequence; returns the action label assigned
    std::string reg(const std::string& seq, const std::string& label, bool strict = false, bool repeating = false) {
        auto p = parseSequence(seq);
        if (!p.ok) {
            std::cerr << "  parse error for \"" << seq << "\": " << p.error << "\n";
            ++g_failures;
            return label;
        }
        const ActionId id = static_cast<ActionId>(labels.size());
        labels.push_back(label);
        tree.insert(p.chords, Action{id, repeating}, strict);
        return label;
    }

    void rebuildEngine() { sm = ChordStateMachine(&tree); }

    std::vector<std::string> committed; // accumulated across feed()
    bool                     lastSuppress = false;

    StepResult feed(const StepInput& in) {
        auto r       = sm.step(in);
        lastSuppress = r.suppress;
        for (const Action& a : r.commits)
            committed.push_back(labels[a.id]);
        return r;
    }

    // event helpers. Modifier events are fed to the engine (isModifier) so it
    // tracks held state from the stream, exactly like the plugin does. Key
    // events carry a pseudo-keycode (the keysym value) so press/release
    // tracking by keycode works like in the plugin.
    void       modDown(Mods m) { feed({0, 0, m, true, true}); }
    void       modUp(Mods m) { feed({0, 0, m, true, false}); }
    StepResult keyDown(const std::string& name) {
        const Keysym s = resolveKeyName(name);
        return feed({s, s, 0, false, true});
    }
    void keyUp(const std::string& name) {
        const Keysym s = resolveKeyName(name);
        feed({s, s, 0, false, false});
    }
    void tap(const std::string& name) {
        keyDown(name);
        keyUp(name);
    }
    // a key event by raw keycode (sym unresolvable or unbound)
    StepResult codeDown(Keycode c, Keysym sym = 0) { return feed({sym, c, 0, false, true}); }
    void       codeUp(Keycode c, Keysym sym = 0) { feed({sym, c, 0, false, false}); }
};

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i)
            s += ",";
        s += v[i];
    }
    return s;
}

void expectSeq(const std::string& name, const std::vector<std::string>& got, const std::vector<std::string>& want) {
    ++g_checks;
    if (got != want) {
        std::cerr << "FAIL " << name << "\n      got:  [" << join(got) << "]\n      want: [" << join(want) << "]\n";
        ++g_failures;
    } else {
        std::cout << "ok   " << name << "  -> [" << join(got) << "]\n";
    }
}

void expectBool(const std::string& name, bool got, bool want) {
    ++g_checks;
    if (got != want) {
        std::cerr << "FAIL " << name << "  got=" << got << " want=" << want << "\n";
        ++g_failures;
    } else {
        std::cout << "ok   " << name << "\n";
    }
}

} // namespace

int main() {
    // === Requirement 1: exact modifier-aware chords ========================
    {
        Sim s;
        s.reg("s-a", "s-a");
        s.reg("s-b", "s-b");
        s.rebuildEngine();
        // hold Super, press A then B  -> s-a then s-b
        s.modDown(MOD_SUPER);
        s.tap("a");
        s.tap("b");
        s.modUp(MOD_SUPER);
        expectSeq("req1 hold-super a then b", s.committed, {"s-a", "s-b"});
    }
    {
        Sim s;
        s.reg("s-a", "s-a");
        s.reg("b", "b");
        s.rebuildEngine();
        // press s-a, release Super, press b -> s-a then b
        s.modDown(MOD_SUPER);
        s.tap("a");
        s.modUp(MOD_SUPER);
        s.tap("b");
        expectSeq("req1 s-a release-super then plain b", s.committed, {"s-a", "b"});
    }

    // === Requirement 2: native prefix chords ===============================
    {
        Sim s;
        s.reg("s-print d", "shotdrag");
        s.reg("s-print w", "window-shot");
        s.rebuildEngine();
        // Super held, press print, RELEASE super, press d  -> s-print d
        s.modDown(MOD_SUPER);
        s.tap("print");
        s.modUp(MOD_SUPER);
        s.tap("d");
        expectSeq("req2 s-print then plain d", s.committed, {"shotdrag"});
    }
    {
        Sim s;
        s.reg("s-print d", "shotdrag");
        s.rebuildEngine();
        // Super STILL held for d -> must NOT match s-print d
        s.modDown(MOD_SUPER);
        s.tap("print");
        s.tap("d"); // super still held
        s.modUp(MOD_SUPER);
        expectSeq("req2 s-print then s-d does not match s-print d", s.committed, {});
    }
    {
        Sim s;
        s.reg("s-print d", "shotdrag");
        s.reg("s-print s-d", "super-d");
        s.rebuildEngine();
        // Super held through d -> matches s-print s-d
        s.modDown(MOD_SUPER);
        s.tap("print");
        s.tap("d");
        s.modUp(MOD_SUPER);
        expectSeq("req2 s-print then s-d matches s-print s-d", s.committed, {"super-d"});
    }

    // === Overlapping modifier chords: resolved by the modifiers held at the
    //     key press, and fired eagerly on that press. =========================
    {
        Sim s;
        s.reg("M-print", "M-print");
        s.reg("s-M-print", "s-M-print");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        s.modDown(MOD_ALT);
        auto r = s.keyDown("print"); // held = {s,M}
        expectSeq("s-M-print fires on press while mods held", s.committed, {"s-M-print"});
        expectBool("print captured", r.suppress, true);
        s.keyUp("print");
        s.modUp(MOD_SUPER);
        s.modUp(MOD_ALT);
        expectSeq("nothing extra fires on modifier release", s.committed, {"s-M-print"});
    }
    {
        Sim s;
        s.reg("M-print", "M-print");
        s.reg("s-M-print", "s-M-print");
        s.rebuildEngine();
        s.modDown(MOD_ALT); // only Alt held
        s.tap("print");
        expectSeq("M-print fires with only Alt held (no downgrade confusion)", s.committed, {"M-print"});
        s.modUp(MOD_ALT);
    }

    // === Requirement 4: multi-chord after a modified prefix while held ======
    {
        Sim s;
        s.reg("s-M-f s-a", "smf-sa");
        s.rebuildEngine();
        // hold s-M, press f; release M; press a; release s  -> s-M-f s-a
        s.modDown(MOD_SUPER);
        s.modDown(MOD_ALT);
        s.tap("f");
        s.modUp(MOD_ALT);
        s.tap("a"); // super still held -> s-a
        s.modUp(MOD_SUPER);
        expectSeq("req4 s-M-f then s-a", s.committed, {"smf-sa"});
    }

    // === Eager: a final sub-chord fires on key-down while modifiers stay held
    {
        Sim s;
        s.reg("s-x s-c", "close");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        s.tap("x");              // prefix s-x; Super stays held
        auto r = s.keyDown("c"); // s-c is final -> commit NOW, Super still held
        expectSeq("eager: s-x s-c fires on c-down with Super held", s.committed, {"close"});
        expectBool("eager: c captured", r.suppress, true);
        s.keyUp("c");
        s.modUp(MOD_SUPER);
        expectSeq("eager: nothing extra after Super release", s.committed, {"close"});
    }

    // === Extra: plain (unmodified) leaf commits immediately ================
    {
        Sim s;
        s.reg("print", "screenshot");
        s.rebuildEngine();
        s.keyDown("print");
        expectSeq("plain print commits on press", s.committed, {"screenshot"});
        s.keyUp("print");
    }

    // === Extra: pass-through vs capture suppression ========================
    {
        Sim s;
        s.reg("s-x f", "x-f");
        s.rebuildEngine();
        // unknown key at idle -> NOT suppressed (normal binds/typing run)
        auto r1 = s.keyDown("z");
        expectBool("idle unknown key passes through", r1.suppress, false);
        s.keyUp("z");
        // chord-start key -> suppressed
        s.modDown(MOD_SUPER);
        auto r2 = s.keyDown("x");
        expectBool("chord-start key is captured", r2.suppress, true);
        s.keyUp("x");
        // unknown key inside prefix -> suppressed (aborts), nothing committed
        s.modUp(MOD_SUPER);
        auto r3 = s.keyDown("q");
        expectBool("unknown key inside prefix is captured", r3.suppress, true);
        s.keyUp("q");
        expectSeq("aborted prefix commits nothing", s.committed, {});
    }

    // === Extra: duplicate finals run in order (default policy) =============
    {
        Sim s;
        s.reg("s-x f", "first");
        s.reg("s-x f", "second"); // allowed duplicate -> appended
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        s.tap("x");
        s.modUp(MOD_SUPER);
        s.tap("f");
        expectSeq("duplicate finals run in order", s.committed, {"first", "second"});
    }

    // === Extra: insert-time conflict/duplicate policy ======================
    {
        PrefixTree t;
        auto       a = parseSequence("s-x");
        auto       b = parseSequence("s-x f");
        expectBool("insert s-x leaf ok", t.insert(a.chords, Action{0, false}, false) == InsertStatus::OK, true);
        expectBool("prefix-over-final rejected", t.insert(b.chords, Action{1, false}, false) == InsertStatus::ErrPrefixOverFinal, true);
    }
    {
        PrefixTree t;
        auto       a = parseSequence("s-x f");
        auto       b = parseSequence("s-x");
        expectBool("insert s-x f ok", t.insert(a.chords, Action{0, false}, false) == InsertStatus::OK, true);
        expectBool("final-over-prefix rejected", t.insert(b.chords, Action{1, false}, false) == InsertStatus::ErrFinalOverPrefix, true);
    }
    {
        PrefixTree t;
        auto       a = parseSequence("a");
        expectBool("first insert ok", t.insert(a.chords, Action{0, false}, false) == InsertStatus::OK, true);
        expectBool("dup allowed by default", t.insert(a.chords, Action{1, false}, false) == InsertStatus::AddedDuplicate, true);
        expectBool("dup rejected when strict", t.insert(a.chords, Action{2, false}, true) == InsertStatus::ErrStrictDuplicate, true);
    }

    // === Extra: parser sanity =============================================
    {
        auto p = parseSequence("s-M-C-c");
        ++g_checks;
        if (p.ok && p.chords.size() == 1 && p.chords[0].mods == (MOD_SUPER | MOD_ALT | MOD_CTRL) && p.chords[0].sym == resolveKeyName("c"))
            std::cout << "ok   parser s-M-C-c -> SUPER|ALT|CTRL + c\n";
        else {
            std::cerr << "FAIL parser s-M-C-c\n";
            ++g_failures;
        }
    }
    {
        auto p = parseSequence("SUPER + F");
        ++g_checks;
        if (p.ok && p.chords.size() == 1 && p.chords[0].mods == MOD_SUPER && p.chords[0].sym == resolveKeyName("f"))
            std::cout << "ok   parser raw SUPER + F -> SUPER + f\n";
        else {
            std::cerr << "FAIL parser raw SUPER + F\n";
            ++g_failures;
        }
    }

    // === XF86 keysyms with modifiers (both syntaxes) ========================
    {
        Sim s;
        s.reg("s-XF86AudioLowerVolume", "ff-down");
        s.reg("s-M-XF86AudioRaiseVolume", "game-up");
        s.reg("SUPER + ALT + XF86AudioLowerVolume", "game-down");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        s.tap("XF86AudioLowerVolume");
        s.modDown(MOD_ALT);
        s.tap("XF86AudioRaiseVolume");
        s.tap("XF86AudioLowerVolume");
        s.modUp(MOD_ALT);
        s.modUp(MOD_SUPER);
        s.tap("XF86AudioLowerVolume"); // unmodified: passes through, no commit
        expectSeq("XF86 syms with mods (emacs + raw syntax)", s.committed, {"ff-down", "game-up", "game-down"});
        expectBool("bare XF86 key passes through", s.lastSuppress, false);
    }

    // === code:NN keycode chords =============================================
    {
        Sim s;
        s.reg("s-code:122", "code-chord");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        auto r = s.codeDown(122, resolveKeyName("XF86AudioLowerVolume")); // sym unbound -> code matches
        expectSeq("s-code:122 fires by keycode", s.committed, {"code-chord"});
        expectBool("code chord captured", r.suppress, true);
        s.codeUp(122, resolveKeyName("XF86AudioLowerVolume"));
        expectBool("code chord release also captured", s.lastSuppress, true);
        s.modUp(MOD_SUPER);
    }
    {
        // keysym binding wins over keycode binding for the same event
        Sim s;
        s.reg("s-XF86AudioLowerVolume", "by-sym");
        s.reg("s-code:122", "by-code");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        const Keysym vol = resolveKeyName("XF86AudioLowerVolume");
        s.feed({vol, 122, 0, false, true});
        s.feed({vol, 122, 0, false, false});
        s.modUp(MOD_SUPER);
        expectSeq("sym chord preferred over code chord", s.committed, {"by-sym"});
    }
    {
        auto p = parseSequence("SUPER + code:122");
        ++g_checks;
        if (p.ok && p.chords.size() == 1 && p.chords[0].mods == MOD_SUPER && p.chords[0].sym == 0 && p.chords[0].code == 122)
            std::cout << "ok   parser raw SUPER + code:122\n";
        else {
            std::cerr << "FAIL parser raw SUPER + code:122\n";
            ++g_failures;
        }
        auto q = parseSequence("s-code:122 code:30");
        ++g_checks;
        if (q.ok && q.chords.size() == 2 && q.chords[0].code == 122 && q.chords[1].mods == 0 && q.chords[1].code == 30)
            std::cout << "ok   parser emacs s-code:122 code:30\n";
        else {
            std::cerr << "FAIL parser emacs s-code:122 code:30\n";
            ++g_failures;
        }
        auto bad = parseSequence("s-code:abc");
        expectBool("parser rejects code:abc", bad.ok, false);
    }

    // === modifier classification by keysym ==================================
    {
        Mods bit = 0;
        expectBool("Super_L classifies as SUPER", modFromKeysym(resolveKeyName("Super_L"), bit) && bit == MOD_SUPER, true);
        expectBool("Meta_L classifies as ALT", modFromKeysym(resolveKeyName("Meta_L"), bit) && bit == MOD_ALT, true);
        expectBool("ISO_Level3_Shift is a chord-neutral modifier", modFromKeysym(resolveKeyName("ISO_Level3_Shift"), bit) && bit == 0, true);
        expectBool("Caps_Lock is a chord-neutral modifier", modFromKeysym(resolveKeyName("Caps_Lock"), bit) && bit == 0, true);
        expectBool("plain letter is not a modifier", modFromKeysym(resolveKeyName("a"), bit), false);
    }
    {
        // a chord-neutral modifier (AltGr) mid-sequence must not abort it
        Sim s;
        s.reg("s-x f", "x-f");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        s.tap("x");
        s.modUp(MOD_SUPER);
        s.feed({0, 0, 0, true, true});  // AltGr down: modifier, no chord bit
        s.feed({0, 0, 0, true, false}); // AltGr up
        s.tap("f");
        expectSeq("neutral modifier does not abort prefix", s.committed, {"x-f"});
    }

    // === repeating flag propagates to commits ================================
    {
        Sim s;
        s.reg("s-XF86AudioLowerVolume", "vol", /*strict=*/false, /*repeating=*/true);
        s.reg("s-a", "once");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        auto r1 = s.keyDown("XF86AudioLowerVolume");
        expectBool("repeating binding commits with repeating=true", r1.commits.size() == 1 && r1.commits[0].repeating, true);
        s.keyUp("XF86AudioLowerVolume");
        auto r2 = s.keyDown("a");
        expectBool("non-repeating binding commits with repeating=false", r2.commits.size() == 1 && !r2.commits[0].repeating, true);
        s.keyUp("a");
        s.modUp(MOD_SUPER);
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
