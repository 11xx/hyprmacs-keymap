// Hyprmacs — native Hyprland plugin glue.
//
// Responsibilities (everything Hyprland-specific lives here):
//   * export the plugin ABI entry points;
//   * hook CKeybindManager::onKeyEvent so the chord engine sees every key;
//   * expose hl.plugin.hyprmacs.{register,configure,clear} to the Lua config;
//   * run committed actions by pcall'ing the stored Lua closures;
//   * drive the optional prefix timeout via the event loop.
//
// The chord logic itself is in the Hyprland-free core (see ChordStateMachine).
#include "ChordStateMachine.hpp"
#include "KeyParser.hpp"
#include "PrefixTree.hpp"

#include <src/plugins/PluginAPI.hpp>
#include <src/Compositor.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/managers/KeybindManager.hpp>
#include <src/managers/SessionLockManager.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/version.h>

#include <xkbcommon/xkbcommon.h>

#include <lua.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace hyprmacs;

// ===========================================================================
// plugin state
// ===========================================================================
namespace {

inline HANDLE     PHANDLE   = nullptr;
CFunctionHook*    g_keyHook = nullptr;

PrefixTree        g_tree;
ChordStateMachine g_sm{&g_tree};

// The config Lua state. Set on first register/clear; refreshed if it changes
// (i.e. the config was reloaded into a new state).
lua_State*        g_lua = nullptr;
// Every action closure we hold a LuaJIT registry ref for, so clear() can unref.
std::vector<int>  g_refs;

struct Config {
    int  timeoutMs        = 0;     // prefix timeout, 0 = off (matches the Org default)
    bool strictDuplicates = false; // reject duplicate final bindings
    bool reportDuplicates = false; // notify on (still-allowed) duplicate finals
} g_cfg;

SP<CEventLoopTimer> g_timer;

void logmsg(const std::string& s) {
    std::fprintf(stderr, "[hyprmacs] %s\n", s.c_str());
}

// ---------------------------------------------------------------------------
// action execution
// ---------------------------------------------------------------------------
void runAction(ActionId ref) {
    if (!g_lua)
        return;
    lua_rawgeti(g_lua, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(g_lua, 0, 0, 0) != 0) {
        const char* err = lua_tostring(g_lua, -1);
        logmsg(std::string("action error: ") + (err ? err : "?"));
        lua_pop(g_lua, 1);
    }
}

// ---------------------------------------------------------------------------
// optional prefix timeout
// ---------------------------------------------------------------------------
void cancelTimer() {
    if (g_timer && g_pEventLoopManager) {
        g_pEventLoopManager->removeTimer(g_timer);
        g_timer.reset();
    }
}

void manageTimeout() {
    if (g_cfg.timeoutMs <= 0 || !g_sm.awaitingNextChord()) {
        cancelTimer();
        return;
    }
    cancelTimer();
    const uint64_t gen = g_sm.generation();
    g_timer            = makeShared<CEventLoopTimer>(
        std::chrono::milliseconds(g_cfg.timeoutMs), [gen](SP<CEventLoopTimer> self, void*) { g_sm.timeoutReset(gen); }, nullptr);
    g_pEventLoopManager->addTimer(g_timer);
}

// ---------------------------------------------------------------------------
// the key-event hook
// ---------------------------------------------------------------------------
using PonKeyEvent = bool (*)(void*, std::any, SP<IKeyboard>);

bool hkOnKeyEvent(void* thisptr, std::any event, SP<IKeyboard> keyboard) {
    const auto passThrough = [&]() -> bool { return reinterpret_cast<PonKeyEvent>(g_keyHook->m_original)(thisptr, event, keyboard); };

    if (!keyboard)
        return passThrough();

    // Don't run chords while locked / inactive; keep the engine in sync.
    const bool locked = g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
    if (!g_pCompositor->m_sessionActive || g_pCompositor->m_unsafeState || locked) {
        g_sm.reset();
        cancelTimer();
        return passThrough();
    }

    IKeyboard::SKeyEvent e;
    try {
        e = std::any_cast<IKeyboard::SKeyEvent>(event);
    } catch (...) { return passThrough(); }

    const uint32_t KEYCODE = e.keycode + 8; // libinput -> xkb offset
    const Mods     modBit  = static_cast<Mods>(g_pKeybindManager->keycodeToModifier(KEYCODE)) & MOD_ALL;
    const bool     pressed = (e.state == WL_KEYBOARD_KEY_STATE_PRESSED);

    Keysym sym = 0;
    if (modBit == 0) {
        xkb_state* st = keyboard->m_xkbSymState ? keyboard->m_xkbSymState : keyboard->m_xkbStaticState;
        if (st)
            sym = canonicaliseSym(static_cast<Keysym>(xkb_state_key_get_one_sym(st, KEYCODE)));
    }

    const StepResult r = g_sm.step(StepInput{sym, modBit, pressed});

    for (ActionId a : r.commits)
        runAction(a);

    manageTimeout();

    if (r.suppress)
        return false; // consume: Hyprland binds & client never see this key
    return passThrough();
}

// ===========================================================================
// hl.plugin.hyprmacs.* lua bridge
// ===========================================================================

// Drop all registrations. If the lua state is unchanged, also unref closures.
void clearRegistrations(lua_State* L) {
    if (g_lua && g_lua == L) {
        for (int ref : g_refs)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    g_refs.clear();
    g_tree.clear();
    g_sm.reset();
    cancelTimer();
}

// If the config was reloaded into a fresh lua state, our refs belong to a gone
// state; drop bookkeeping (without unref) and adopt the new state.
void adoptState(lua_State* L) {
    if (g_lua && g_lua != L) {
        g_refs.clear();
        g_tree.clear();
        g_sm.reset();
        cancelTimer();
    }
    g_lua = L;
}

int hm_register(lua_State* L) {
    adoptState(L);

    if (lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "hyprmacs.register: argument 1 must be a key-sequence string");
    if (!lua_isfunction(L, 2))
        return luaL_error(L, "hyprmacs.register: argument 2 must be a function");

    const std::string seq = lua_tostring(L, 1);

    const ParseResult parsed = parseSequence(seq);
    if (!parsed.ok)
        return luaL_error(L, "hyprmacs.register: %s", parsed.error.c_str());

    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    const InsertStatus status = g_tree.insert(parsed.chords, ref, g_cfg.strictDuplicates);
    switch (status) {
        case InsertStatus::OK: g_refs.push_back(ref); break;
        case InsertStatus::AddedDuplicate:
            g_refs.push_back(ref);
            if (g_cfg.reportDuplicates)
                HyprlandAPI::addNotification(PHANDLE, "[hyprmacs] duplicate final binding: " + seq, CHyprColor(0.8f, 0.6f, 0.1f, 1.0f), 5000);
            break;
        case InsertStatus::ErrStrictDuplicate:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs.register: duplicate binding '%s'", seq.c_str());
        case InsertStatus::ErrPrefixOverFinal:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs.register: '%s' conflicts with an existing final binding", seq.c_str());
        case InsertStatus::ErrFinalOverPrefix:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs.register: '%s' conflicts with an existing prefix binding", seq.c_str());
        case InsertStatus::ErrEmpty:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs.register: empty key sequence");
    }
    return 0;
}

int hm_configure(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "hyprmacs.configure: argument 1 must be a table");

    lua_getfield(L, 1, "submap_timeout_ms");
    if (lua_isnumber(L, -1))
        g_cfg.timeoutMs = static_cast<int>(lua_tointeger(L, -1));
    else if (lua_isnil(L, -1) && lua_gettop(L) >= 1) { /* leave default */ }
    lua_pop(L, 1);

    lua_getfield(L, 1, "strict_duplicates");
    if (lua_isboolean(L, -1))
        g_cfg.strictDuplicates = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "report_duplicates");
    if (lua_isboolean(L, -1))
        g_cfg.reportDuplicates = lua_toboolean(L, -1);
    lua_pop(L, 1);

    // modified_leaf_commit_delay_ms is accepted for backward compatibility but
    // ignored: commit timing is now deterministic (modifier-release driven).
    return 0;
}

int hm_clear(lua_State* L) {
    clearRegistrations(L);
    g_lua = L;
    return 0;
}

} // namespace

// ===========================================================================
// plugin ABI entry points
// ===========================================================================
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const SVersionInfo ver = HyprlandAPI::getHyprlandVersion(handle);
    if (ver.hash != GIT_COMMIT_HASH)
        HyprlandAPI::addNotification(handle, "[hyprmacs] built against a different Hyprland commit; rebuild if chords misbehave", CHyprColor(0.9f, 0.5f, 0.1f, 1.0f), 7000);

    // Locate and hook CKeybindManager::onKeyEvent.
    void*      addr = nullptr;
    const auto fns  = HyprlandAPI::findFunctionsByName(handle, "onKeyEvent");
    for (const auto& f : fns) {
        if (f.demangled.find("CKeybindManager::onKeyEvent") != std::string::npos) {
            addr = f.address;
            break;
        }
    }
    if (!addr) {
        HyprlandAPI::addNotification(handle, "[hyprmacs] could not find onKeyEvent to hook; chords disabled", CHyprColor(0.9f, 0.2f, 0.2f, 1.0f), 10000);
        return {"hyprmacs", "Emacs-like key chords (FAILED to hook)", "11xx", "1.0"};
    }

    g_keyHook = HyprlandAPI::createFunctionHook(handle, addr, rc<void*>(&hkOnKeyEvent));
    if (!g_keyHook || !g_keyHook->hook()) {
        HyprlandAPI::addNotification(handle, "[hyprmacs] failed to install onKeyEvent hook; chords disabled", CHyprColor(0.9f, 0.2f, 0.2f, 1.0f), 10000);
        return {"hyprmacs", "Emacs-like key chords (FAILED to hook)", "11xx", "1.0"};
    }

    HyprlandAPI::addLuaFunction(handle, "hyprmacs", "register", &hm_register);
    HyprlandAPI::addLuaFunction(handle, "hyprmacs", "configure", &hm_configure);
    HyprlandAPI::addLuaFunction(handle, "hyprmacs", "clear", &hm_clear);

    logmsg("loaded; hl.plugin.hyprmacs.{register,configure,clear} available");
    return {"hyprmacs", "Emacs-like, modifier-aware key chords for Hyprland", "11xx", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cancelTimer();
    if (g_keyHook)
        g_keyHook->unhook();
    if (g_lua) {
        for (int ref : g_refs)
            luaL_unref(g_lua, LUA_REGISTRYINDEX, ref);
    }
    g_refs.clear();
    g_tree.clear();
}
