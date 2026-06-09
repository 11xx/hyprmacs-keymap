// Hyprmacs — native Hyprland plugin glue.
//
// Responsibilities (everything Hyprland-specific lives here):
//   * export the plugin ABI entry points;
//   * hook CKeybindManager::onKeyEvent so the chord engine sees every key;
//   * expose hl.plugin.hyprmacs_keymap.{register,configure,clear} to the Lua config;
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
#include <src/managers/input/InputManager.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/version.h>

#include <xkbcommon/xkbcommon.h>

#include <lua.hpp>

#include <sys/stat.h>

#include <chrono>
#include <cstdarg>
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
    bool debug            = false; // log every key event the engine sees
} g_cfg;

SP<CEventLoopTimer> g_timer;
SP<CEventLoopTimer> g_debugWarnTimer;

// Hyprland's stderr is usually the console (e.g. /dev/tty1), invisible from the
// session and absent from hyprland.log/rollinglog. So mirror our messages to a
// file we (and the user) can actually read.
constexpr const char* LOGFILE = "/tmp/hyprmacs-keymap.log";

void logfile(const std::string& s) {
    if (FILE* f = std::fopen(LOGFILE, "a")) {
        std::fputs(s.c_str(), f);
        std::fputc('\n', f);
        std::fclose(f);
        ::chmod(LOGFILE, S_IRUSR | S_IWUSR); // owner-only; /tmp is world-readable
    }
}

void logmsg(const std::string& s) {
    std::fprintf(stderr, "[hyprmacs-keymap] %s\n", s.c_str());
    logfile("[hyprmacs-keymap] " + s);
}

// printf-style; only emits when debug is enabled.
void dbg(const char* fmt, ...) {
    if (!g_cfg.debug)
        return;
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logfile(buf);
}

// Current kernel.sysrq value (0 = magic-SysRq disabled). When non-zero, the
// kernel intercepts Alt+SysRq (= Alt+PrtSc) before it ever reaches Hyprland.
int readSysrq() {
    int v = 0;
    if (FILE* f = std::fopen("/proc/sys/kernel/sysrq", "r")) {
        if (std::fscanf(f, "%d", &v) != 1)
            v = 0;
        std::fclose(f);
    }
    return v;
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
// loud, recurring reminder that debug key-logging is on
// ---------------------------------------------------------------------------
void stopDebugWarning() {
    if (g_debugWarnTimer && g_pEventLoopManager) {
        g_pEventLoopManager->removeTimer(g_debugWarnTimer);
        g_debugWarnTimer.reset();
    }
}

void showDebugWarning() {
    HyprlandAPI::addNotification(PHANDLE,
                                 "[hyprmacs-keymap] DEBUG KEY LOGGING IS ON — chord keys are written to " + std::string(LOGFILE) + ". Set debug=false (or remove it) and reload.",
                                 CHyprColor(0.9f, 0.1f, 0.1f, 1.0f), 10000);
}

void armDebugWarning() {
    stopDebugWarning();
    if (!g_pEventLoopManager)
        return;
    g_debugWarnTimer = makeShared<CEventLoopTimer>(
        std::chrono::seconds(30),
        [](SP<CEventLoopTimer>, void*) {
            if (!g_cfg.debug) // disabled meanwhile
                return;
            showDebugWarning();
            armDebugWarning(); // keep nagging until debug is turned off
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_debugWarnTimer);
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
    const bool     pressed = (e.state == WL_KEYBOARD_KEY_STATE_PRESSED);

    // Classify as a modifier from the event itself (race-free) and feed the
    // engine the whole key stream so it tracks held modifiers deterministically.
    const Mods modBit = static_cast<Mods>(g_pKeybindManager->keycodeToModifier(KEYCODE)) & MOD_ALL;

    Keysym sym = 0;
    if (modBit == 0) {
        xkb_state* st = keyboard->m_xkbSymState ? keyboard->m_xkbSymState : keyboard->m_xkbStaticState;
        if (st)
            sym = canonicaliseSym(static_cast<Keysym>(xkb_state_key_get_one_sym(st, KEYCODE)));
    }

    const StepResult r = g_sm.step(StepInput{sym, modBit, pressed});

    // Log ONLY chord-relevant events: modifier keys, keys we capture/commit, or
    // keys while a prefix is in progress. Plain pass-through keystrokes (normal
    // typing, incl. shifted text) are never logged, so debug can't keylog.
    if (g_cfg.debug && (modBit != 0 || r.suppress || !r.commits.empty() || g_sm.awaitingNextChord())) {
        char name[64] = {0};
        if (sym)
            xkb_keysym_get_name(static_cast<xkb_keysym_t>(sym), name, sizeof(name));
        const Mods kbs = static_cast<Mods>(g_pInputManager->getModsFromAllKBs()) & MOD_ALL; // cross-check
        dbg("%s sym=%s(0x%x) modBit=0x%x held=0x%x kbs=0x%x suppress=%d commits=%zu", pressed ? "down" : "up  ", sym ? name : "-", static_cast<unsigned>(sym),
            static_cast<unsigned>(modBit), static_cast<unsigned>(g_sm.heldMods()), static_cast<unsigned>(kbs), static_cast<int>(r.suppress), r.commits.size());
    }

    for (ActionId a : r.commits)
        runAction(a);

    manageTimeout();

    if (r.suppress)
        return false; // consume: Hyprland binds & client never see this key
    return passThrough();
}

// ===========================================================================
// hl.plugin.hyprmacs_keymap.* lua bridge
// ===========================================================================

// Drop all registrations. If the lua state is unchanged, also unref closures.
// Also reset config to defaults: settings are declared fresh each config
// evaluation (the shim calls clear() at the top of every load), so e.g.
// removing `keymap_configure({ debug = true })` actually turns debug back off —
// otherwise the flag would persist in the still-loaded plugin across reloads.
void clearRegistrations(lua_State* L) {
    if (g_lua && g_lua == L) {
        for (int ref : g_refs)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    g_refs.clear();
    g_tree.clear();
    g_sm.reset();
    cancelTimer();
    g_cfg = Config{};
    stopDebugWarning(); // debug just reset to false; re-armed if config re-enables it
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
        return luaL_error(L, "hyprmacs-keymap.register: argument 1 must be a key-sequence string");
    if (!lua_isfunction(L, 2))
        return luaL_error(L, "hyprmacs-keymap.register: argument 2 must be a function");

    const std::string seq = lua_tostring(L, 1);

    const ParseResult parsed = parseSequence(seq);
    if (!parsed.ok)
        return luaL_error(L, "hyprmacs-keymap.register: %s", parsed.error.c_str());

    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    const InsertStatus status = g_tree.insert(parsed.chords, ref, g_cfg.strictDuplicates);
    switch (status) {
        case InsertStatus::OK: g_refs.push_back(ref); break;
        case InsertStatus::AddedDuplicate:
            g_refs.push_back(ref);
            if (g_cfg.reportDuplicates)
                HyprlandAPI::addNotification(PHANDLE, "[hyprmacs-keymap] duplicate final binding: " + seq, CHyprColor(0.8f, 0.6f, 0.1f, 1.0f), 5000);
            break;
        case InsertStatus::ErrStrictDuplicate:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs-keymap.register: duplicate binding '%s'", seq.c_str());
        case InsertStatus::ErrPrefixOverFinal:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs-keymap.register: '%s' conflicts with an existing final binding", seq.c_str());
        case InsertStatus::ErrFinalOverPrefix:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs-keymap.register: '%s' conflicts with an existing prefix binding", seq.c_str());
        case InsertStatus::ErrEmpty:
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return luaL_error(L, "hyprmacs-keymap.register: empty key sequence");
    }

    if (g_cfg.debug) {
        std::string chords;
        for (const auto& c : parsed.chords) {
            char b[40];
            std::snprintf(b, sizeof(b), "%s{mods=0x%x,sym=0x%x}", chords.empty() ? "" : " ", static_cast<unsigned>(c.mods), static_cast<unsigned>(c.sym));
            chords += b;
        }
        dbg("register '%s' -> [%s] status=%d", seq.c_str(), chords.c_str(), static_cast<int>(status));
    }

    // Heads-up: Alt+Print is the magic-SysRq combo. When kernel.sysrq != 0 the
    // kernel buffers it until Alt is released, so the chord fires on Alt-release
    // (and can mis-resolve, e.g. s-M-print -> M-print) instead of on press. This
    // is unfixable in the plugin (the press-time state never reaches us), so just
    // warn loudly. Dormant when sysrq is disabled.
    bool altPrint = false;
    for (const auto& c : parsed.chords)
        if (c.sym == canonicaliseSym(static_cast<Keysym>(XKB_KEY_Print)) && (c.mods & MOD_ALT))
            altPrint = true;
    if (altPrint) {
        if (const int sysrq = readSysrq(); sysrq != 0) {
            logmsg("WARNING: '" + seq + "' uses Alt+Print = magic-SysRq; the kernel (kernel.sysrq=" + std::to_string(sysrq) +
                   ") intercepts it, so it fires on Alt-release, not on press. Fix: set kernel.sysrq=0, or remap PrtSc off KEY_SYSRQ.");
            static bool notified = false;
            if (!notified) {
                notified = true;
                HyprlandAPI::addNotification(PHANDLE,
                                             "[hyprmacs-keymap] An Alt+Print binding is intercepted by magic-SysRq (kernel.sysrq=" + std::to_string(sysrq) +
                                                 "); it fires on Alt-release. Set kernel.sysrq=0. See /tmp/hyprmacs-keymap.log",
                                             CHyprColor(0.9f, 0.5f, 0.1f, 1.0f), 9000);
            }
        }
    }
    return 0;
}

int hm_configure(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "hyprmacs-keymap.configure: argument 1 must be a table");

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

    lua_getfield(L, 1, "debug");
    if (lua_isboolean(L, -1))
        g_cfg.debug = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (g_cfg.debug) {
        dbg("configure: debug on (timeout=%d strict=%d report=%d)", g_cfg.timeoutMs, static_cast<int>(g_cfg.strictDuplicates), static_cast<int>(g_cfg.reportDuplicates));
        showDebugWarning(); // loud, immediate
        armDebugWarning();  // ...and keep reminding every 30s until it's off
    } else
        stopDebugWarning();

    // modified_leaf_commit_delay_ms is accepted for backward compatibility but
    // ignored: matching is eager now (a final binding fires on key-down).
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
        HyprlandAPI::addNotification(handle, "[hyprmacs-keymap] built against a different Hyprland commit; rebuild if chords misbehave", CHyprColor(0.9f, 0.5f, 0.1f, 1.0f), 7000);

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
        HyprlandAPI::addNotification(handle, "[hyprmacs-keymap] could not find onKeyEvent to hook; chords disabled", CHyprColor(0.9f, 0.2f, 0.2f, 1.0f), 10000);
        return {"hyprmacs-keymap", "Emacs-like key chords (FAILED to hook)", "11xx", "1.6"};
    }

    g_keyHook = HyprlandAPI::createFunctionHook(handle, addr, rc<void*>(&hkOnKeyEvent));
    if (!g_keyHook || !g_keyHook->hook()) {
        HyprlandAPI::addNotification(handle, "[hyprmacs-keymap] failed to install onKeyEvent hook; chords disabled", CHyprColor(0.9f, 0.2f, 0.2f, 1.0f), 10000);
        return {"hyprmacs-keymap", "Emacs-like key chords (FAILED to hook)", "11xx", "1.6"};
    }

    HyprlandAPI::addLuaFunction(handle, "hyprmacs_keymap", "register", &hm_register);
    HyprlandAPI::addLuaFunction(handle, "hyprmacs_keymap", "configure", &hm_configure);
    HyprlandAPI::addLuaFunction(handle, "hyprmacs_keymap", "clear", &hm_clear);

    logmsg("loaded; hl.plugin.hyprmacs_keymap.{register,configure,clear} available");
    return {"hyprmacs-keymap", "Emacs-like, modifier-aware key chords for Hyprland", "11xx", "1.6"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cancelTimer();
    stopDebugWarning();
    if (g_keyHook)
        g_keyHook->unhook();
    if (g_lua) {
        for (int ref : g_refs)
            luaL_unref(g_lua, LUA_REGISTRYINDEX, ref);
    }
    g_refs.clear();
    g_tree.clear();
}
