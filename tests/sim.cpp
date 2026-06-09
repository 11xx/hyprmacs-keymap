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
    std::string reg(const std::string& seq, const std::string& label, bool strict = false) {
        auto p = parseSequence(seq);
        if (!p.ok) {
            std::cerr << "  parse error for \"" << seq << "\": " << p.error << "\n";
            ++g_failures;
            return label;
        }
        const ActionId id = static_cast<ActionId>(labels.size());
        labels.push_back(label);
        tree.insert(p.chords, id, strict);
        return label;
    }

    void rebuildEngine() { sm = ChordStateMachine(&tree); }

    std::vector<std::string> committed; // accumulated across feed()
    bool                     lastSuppress = false;

    StepResult feed(const StepInput& in) {
        auto r       = sm.step(in);
        lastSuppress = r.suppress;
        for (ActionId a : r.commits)
            committed.push_back(labels[a]);
        return r;
    }

    // event helpers
    void modDown(Mods m) { feed({0, m, true}); }
    void modUp(Mods m) { feed({0, m, false}); }
    StepResult keyDown(const std::string& name) { return feed({resolveKeyName(name), 0, true}); }
    void       keyUp(const std::string& name) { feed({resolveKeyName(name), 0, false}); }
    void       tap(const std::string& name) {
        keyDown(name);
        keyUp(name);
    }
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

    // === Requirement 3: overlapping modifier chords commit correctly =======
    for (const bool superFirst : {true, false}) {
        Sim s;
        s.reg("M-print", "M-print");
        s.reg("s-M-print", "s-M-print");
        s.rebuildEngine();
        s.modDown(MOD_SUPER);
        s.modDown(MOD_ALT);
        s.tap("print"); // recognised as s-M-print
        // keep holding, then release mods in the chosen order
        if (superFirst) {
            s.modUp(MOD_SUPER);
            expectSeq(std::string("req3 no commit/downgrade after super-up (") + (superFirst ? "super-first" : "alt-first") + ")", s.committed, {});
            s.modUp(MOD_ALT);
        } else {
            s.modUp(MOD_ALT);
            expectSeq("req3 no commit/downgrade after alt-up (alt-first)", s.committed, {});
            s.modUp(MOD_SUPER);
        }
        expectSeq(std::string("req3 commit s-M-print (") + (superFirst ? "super-first" : "alt-first") + ")", s.committed, {"s-M-print"});
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
        expectBool("insert s-x leaf ok", t.insert(a.chords, 0, false) == InsertStatus::OK, true);
        expectBool("prefix-over-final rejected", t.insert(b.chords, 1, false) == InsertStatus::ErrPrefixOverFinal, true);
    }
    {
        PrefixTree t;
        auto       a = parseSequence("s-x f");
        auto       b = parseSequence("s-x");
        expectBool("insert s-x f ok", t.insert(a.chords, 0, false) == InsertStatus::OK, true);
        expectBool("final-over-prefix rejected", t.insert(b.chords, 1, false) == InsertStatus::ErrFinalOverPrefix, true);
    }
    {
        PrefixTree t;
        auto       a = parseSequence("a");
        expectBool("first insert ok", t.insert(a.chords, 0, false) == InsertStatus::OK, true);
        expectBool("dup allowed by default", t.insert(a.chords, 1, false) == InsertStatus::AddedDuplicate, true);
        expectBool("dup rejected when strict", t.insert(a.chords, 2, true) == InsertStatus::ErrStrictDuplicate, true);
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

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
