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
#include <src/config/ConfigManager.hpp>
#include <src/config/lua/ConfigManager.hpp>
#include <src/event/EventBus.hpp>
#include <src/output/Monitor.hpp>
#include <src/state/MonitorState.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/managers/KeybindManager.hpp>
#include <src/managers/SessionLockManager.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/version.h>

#include <xkbcommon/xkbcommon.h>

#include <lua.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <ranges>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
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

// The config Lua state the registrations belong to. Set by register/clear;
// null from the start of every config reload (Hyprland closes the state then)
// until the new config registers again.
lua_State*        g_lua = nullptr;
// Every action closure we hold a Lua registry ref for, so clear() can unref.
std::vector<int>  g_refs;
// Bumped whenever the registrations are dropped, so a commit or repeat in
// flight stops running refs that no longer belong to the tree.
uint64_t          g_regEpoch = 0;

CHyprSignalListener g_preReloadListener;

struct Settings {
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

// /tmp is shared: create the file owner-only, never follow a symlink, and
// refuse a file someone else planted there.
void logfile(const std::string& s) {
    const int fd = ::open(LOGFILE, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return;
    struct stat st;
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid()) {
        ::fchmod(fd, S_IRUSR | S_IWUSR);
        const std::string line = s + '\n';
        [[maybe_unused]] const auto written = ::write(fd, line.data(), line.size()); // best effort
    }
    ::close(fd);
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
// Run through Hyprland's own keybind-callback path: the live config state,
// its watchdog (a runaway action is aborted instead of freezing the
// compositor) and its runtime-error notification.
void runAction(ActionId ref) {
    if (!g_lua || !Config::mgr() || Config::mgr()->type() != Config::CONFIG_LUA)
        return;
    static_cast<Config::Lua::CConfigManager*>(Config::mgr().get())->callLuaFn(ref);
}

// ---------------------------------------------------------------------------
// repeating binds (Hyprland's `repeating` flag)
// ---------------------------------------------------------------------------
// Mirrors CKeybindManager's native repeat: arm with the keyboard's repeat
// delay on commit, re-fire at its repeat rate, and cancel on ANY subsequent
// key event (native clears m_activeKeybinds at the top of every onKeyEvent).
SP<CEventLoopTimer>   g_repeatTimer;
std::vector<ActionId> g_repeatActions;
int                   g_repeatIntervalMs = 25;

void cancelRepeat() {
    if (g_repeatTimer && g_pEventLoopManager) {
        g_pEventLoopManager->removeTimer(g_repeatTimer);
        g_repeatTimer.reset();
    }
    g_repeatActions.clear();
}

void armRepeat(std::vector<ActionId> actions, SP<IKeyboard> keyboard) {
    if (actions.empty() || !g_pEventLoopManager)
        return;
    const int delay    = (keyboard && keyboard->m_repeatDelay > 0) ? keyboard->m_repeatDelay : 600;
    const int rate     = (keyboard && keyboard->m_repeatRate > 0) ? keyboard->m_repeatRate : 25;
    g_repeatActions    = std::move(actions);
    g_repeatIntervalMs = std::max(1, 1000 / rate);
    g_repeatTimer      = makeShared<CEventLoopTimer>(
        std::chrono::milliseconds(delay),
        [](SP<CEventLoopTimer> self, void*) {
            // An action can cancel this repeat (and its action list) itself.
            const auto actions = g_repeatActions;
            for (ActionId a : actions) {
                runAction(a);
                if (g_repeatTimer != self)
                    return;
            }
            self->updateTimeout(std::chrono::milliseconds(g_repeatIntervalMs));
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_repeatTimer);
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

    IKeyboard::SKeyEvent e;
    try {
        e = std::any_cast<IKeyboard::SKeyEvent>(event);
    } catch (...) { return passThrough(); }

    const uint32_t KEYCODE = e.keycode + 8; // libinput -> xkb offset
    const bool     pressed = (e.state == WL_KEYBOARD_KEY_STATE_PRESSED);

    // Resolve the unmodified keysym the way Hyprland's own binds do: from the
    // first layout, or from the active layout when the device sets
    // resolve_binds_by_sym.
    Keysym sym = 0;
    if (xkb_state* st = (keyboard->m_resolveBindsBySym && keyboard->m_xkbSymState) ? keyboard->m_xkbSymState : keyboard->m_xkbStaticState)
        sym = canonicaliseSym(static_cast<Keysym>(xkb_state_key_get_one_sym(st, KEYCODE)));

    // Classify as a modifier from the event itself (race-free). The keysym is
    // authoritative — it follows XKB remaps (caps:super, AltGr on intl layouts)
    // the same way Hyprland's own modmask does; fall back to the well-known
    // modifier keycodes only when the keysym is unresolvable.
    Mods modBit = 0;
    bool isMod  = modFromKeysym(sym, modBit);
    if (!isMod && sym == 0) {
        modBit = static_cast<Mods>(g_pKeybindManager->keycodeToModifier(KEYCODE)) & MOD_ALL;
        isMod  = modBit != 0;
    }
    if (isMod)
        sym = 0;

    const StepInput in{sym, KEYCODE, modBit, isMod, pressed, reinterpret_cast<uintptr_t>(keyboard.get())};

    // Any key event ends an in-progress bind repeat, matching native repeats.
    cancelRepeat();

    // No chords while locked, switched away, on the unsafe fallback monitor
    // (every output gone), or from a keyboard with binds disabled. The engine
    // still tracks modifiers and swallows releases of keys it swallowed.
    const bool locked = g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
    const bool unsafe = std::ranges::any_of(State::monitorState()->monitors(), [](const auto& m) { return m->m_isUnsafeFallback; });
    if (!g_pCompositor->m_sessionActive || unsafe || locked || !keyboard->m_allowBinds) {
        const StepResult r = g_sm.stepPassive(in);
        manageTimeout();
        return r.suppress ? false : passThrough();
    }

    const StepResult r = g_sm.step(in);

    // Log ONLY chord-relevant events: modifier keys and keys we capture or
    // commit. Every press inside a sequence is captured; pass-through keys
    // (normal typing, incl. shifted text, and the releases of typed keys) are
    // never logged, so debug can't keylog.
    if (g_cfg.debug && (isMod || r.suppress || !r.commits.empty())) {
        char name[64] = {0};
        if (sym)
            xkb_keysym_get_name(static_cast<xkb_keysym_t>(sym), name, sizeof(name));
        const Mods kbs = static_cast<Mods>(g_pInputManager->getModsFromAllKBs()) & MOD_ALL; // cross-check
        dbg("%s sym=%s(0x%x) code=%u modBit=0x%x held=0x%x kbs=0x%x suppress=%d commits=%zu", pressed ? "down" : "up  ", sym ? name : "-", static_cast<unsigned>(sym), KEYCODE,
            static_cast<unsigned>(modBit), static_cast<unsigned>(g_sm.heldMods()), static_cast<unsigned>(kbs), static_cast<int>(r.suppress), r.commits.size());
    }

    // An action may drop every registration (e.g. by calling clear()); stop
    // running this commit's remaining actions if one does.
    const uint64_t        epoch = g_regEpoch;
    std::vector<ActionId> repeats;
    for (const Action& a : r.commits) {
        if (g_regEpoch != epoch)
            break;
        runAction(a.id);
        if (a.repeating)
            repeats.push_back(a.id);
    }
    if (pressed && g_regEpoch == epoch)
        armRepeat(std::move(repeats), keyboard);

    manageTimeout();

    if (r.suppress)
        return false; // consume: Hyprland binds & client never see this key
    return passThrough();
}

// ===========================================================================
// hl.plugin.hyprmacs_keymap.* lua bridge
// ===========================================================================

// Drop every registration and its bookkeeping without touching Lua.
void dropRegistrations() {
    g_refs.clear();
    g_tree.clear();
    g_sm.reset();
    cancelTimer();
    cancelRepeat();
    ++g_regEpoch;
}

// Settings are declared fresh by each config evaluation, so e.g. removing
// `keymap_configure({ debug = true })` turns debug back off.
void resetSettings() {
    g_cfg = Settings{};
    stopDebugWarning(); // re-armed if the config enables debug again
}

// A config reload closes the Lua state that holds every action closure, so
// forget them (nothing to unref) along with the settings, whether or not the
// new config registers anything.
void onPreReload() {
    dropRegistrations();
    resetSettings();
    g_lua = nullptr;
}

// clear(): unref our closures in the live state they belong to.
void clearRegistrations(lua_State* L) {
    if (g_lua && g_lua == L) {
        for (int ref : g_refs)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    dropRegistrations();
    resetSettings();
}

// Registrations made in another Lua state are not ours to unref here; drop
// the bookkeeping and adopt the calling state.
void adoptState(lua_State* L) {
    if (g_lua && g_lua != L)
        dropRegistrations();
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

    // optional flags table (argument 3); only `repeating` is meaningful here.
    bool repeating = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "repeating");
        if (lua_isboolean(L, -1))
            repeating = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    const InsertStatus status = g_tree.insert(parsed.chords, Action{ref, repeating}, g_cfg.strictDuplicates);
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
            char b[56];
            std::snprintf(b, sizeof(b), "%s{mods=0x%x,sym=0x%x,code=%u}", chords.empty() ? "" : " ", static_cast<unsigned>(c.mods), static_cast<unsigned>(c.sym),
                          static_cast<unsigned>(c.code));
            chords += b;
        }
        dbg("register '%s'%s -> [%s] status=%d", seq.c_str(), repeating ? " (repeating)" : "", chords.c_str(), static_cast<int>(status));
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
    if (lua_isnumber(L, -1)) {
        const lua_Number ms = lua_tonumber(L, -1); // a float or negative must not wrap or vanish
        g_cfg.timeoutMs     = ms > 0 ? static_cast<int>(std::min<lua_Number>(ms, INT_MAX)) : 0;
    }
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

    // Other keys, such as modified_leaf_commit_delay_ms, are ignored: a final
    // binding always fires on key-down.
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

    // The hook reads Hyprland structs (keyboards, monitors) by layout, so a
    // build from other headers would misread them on every key press. Refuse
    // to load; Hyprland unloads a plugin whose init throws.
    const SVersionInfo ver = HyprlandAPI::getHyprlandVersion(handle);
    if (ver.hash != GIT_COMMIT_HASH) {
        HyprlandAPI::addNotification(handle, "[hyprmacs-keymap] built against a different Hyprland commit; rebuild it (hyprpm update). Not loaded.",
                                     CHyprColor(0.9f, 0.2f, 0.2f, 1.0f), 10000);
        throw std::runtime_error("[hyprmacs-keymap] Hyprland version mismatch");
    }

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
        return {"hyprmacs-keymap", "Emacs-like keymap helper (FAILED to hook)", "11xx", "2026.6.9"};
    }

    g_keyHook = HyprlandAPI::createFunctionHook(handle, addr, rc<void*>(&hkOnKeyEvent));
    if (!g_keyHook || !g_keyHook->hook()) {
        HyprlandAPI::addNotification(handle, "[hyprmacs-keymap] failed to install onKeyEvent hook; chords disabled", CHyprColor(0.9f, 0.2f, 0.2f, 1.0f), 10000);
        return {"hyprmacs-keymap", "Emacs-like keymap helper (FAILED to hook)", "11xx", "2026.6.9"};
    }

    g_preReloadListener = Event::bus()->m_events.config.preReload.listen([] { onPreReload(); });

    HyprlandAPI::addLuaFunction(handle, "hyprmacs_keymap", "register", &hm_register);
    HyprlandAPI::addLuaFunction(handle, "hyprmacs_keymap", "configure", &hm_configure);
    HyprlandAPI::addLuaFunction(handle, "hyprmacs_keymap", "clear", &hm_clear);

    logmsg("loaded; hl.plugin.hyprmacs_keymap.{register,configure,clear} available");
    return {"hyprmacs-keymap", "Emacs-like keymap helper Hyprland plugin", "11xx", "2026.6.9"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_preReloadListener.reset();
    cancelTimer();
    cancelRepeat();
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
