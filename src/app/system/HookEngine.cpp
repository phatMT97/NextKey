// NexusKey - Keyboard Hook Engine Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "HookEngine.h"
#include "helpers/AppHelpers.h"
#include "core/engine/CodeTableConverter.h"
#include "core/engine/EngineFactory.h"
#include "core/config/ConfigManager.h"
#include "core/MacroPrefix.h"
#include "core/ipc/SharedStateManager.h"
#include "core/Debug.h"
#include "core/CrashLog.h"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <UIAutomation.h>

namespace NextKey {

/// Custom thread message used between OnFocusChanged (sender, main thread)
/// and HookThreadProc (receiver, hook thread) to re-install LL hooks at
/// the top of the hook chain. Defined once to avoid duplication.
static constexpr UINT WM_APP_REINSTALL_HOOKS = WM_APP + 1;

// ═══════════════════════════════════════════════════════════
// Debug file logger (writes to NexusKey_hook.log next to EXE)
// ═══════════════════════════════════════════════════════════
#if defined(_DEBUG) || defined(NEXTKEY_DEBUG)
static FILE* g_hookLog = nullptr;
static void OpenHookLog() {
    if (g_hookLog) return;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring logPath(exePath);
    auto pos = logPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) logPath = logPath.substr(0, pos + 1);
    logPath += L"NexusKey_hook.log";
    (void)_wfopen_s(&g_hookLog, logPath.c_str(), L"w, ccs=UTF-8");
    // 8KB block buffer, flushed when CloseHookLog() runs on Stop. NextKeyTestRunner
    // does post-mortem L1 analysis after NexusKey shuts down, so real-time
    // visibility isn't required and we'd rather not pay per-keystroke fwrite
    // syscalls (an earlier _IONBF attempt slowed the hook enough to mask the
    // very stress bugs the corpus is meant to surface).
    if (g_hookLog) setvbuf(g_hookLog, nullptr, _IOFBF, 8192);
}

static void CloseHookLog() {
    if (g_hookLog) { fflush(g_hookLog); fclose(g_hookLog); g_hookLog = nullptr; }
}

static void HookLog(const wchar_t* format, ...) {
    if (!g_hookLog) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(g_hookLog, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfwprintf(g_hookLog, format, args);
    va_end(args);
    fputwc(L'\n', g_hookLog);
    // No fflush here — uses 8KB buffer, flushed on close
}
#define HOOK_LOG(fmt, ...) HookLog(fmt, ##__VA_ARGS__)
#else
#define HOOK_LOG(...) ((void)0)
#endif

std::atomic<HookEngine*> HookEngine::s_instance{nullptr};

HookEngine::HookEngine() = default;

HookEngine::~HookEngine() {
    Stop();
}

void HookEngine::CommitPending() {
    std::lock_guard<std::recursive_mutex> _lock(stateMutex_);
    if (engine_ && engine_->Count() > 0) {
        CommitComposition();
    }
}

void HookEngine::ApplyConfig(const TypingConfig& config) {
    std::lock_guard<std::recursive_mutex> _lock(stateMutex_);
    beepOnSwitch_ = config.beepOnSwitch;
    smartSwitch_ = config.smartSwitch;
    excludeApps_ = config.excludeApps;
    tsfApps_ = config.tsfApps;
    autoCaps_ = config.autoCaps;
    autoOffByUrl_ = config.autoOffByUrl;
    tempOffByAlt_ = config.tempOffByAlt;
    macroEnabled_ = config.macroEnabled;
    macroInEnglish_ = config.macroInEnglish;
    tempOffMacroByEsc_ = config.tempOffMacroByEsc;
    autoCapsMacro_ = config.autoCapsMacro;
}

bool HookEngine::Start(HINSTANCE hInstance, const TypingConfig& config,
                        bool initialVietnamese, uint8_t startupMode) {
    if (keyboardHook_) return false;  // Already running

#if defined(_DEBUG) || defined(NEXTKEY_DEBUG)
    OpenHookLog();
    HOOK_LOG(L"=== HookEngine::Start ===");
#endif

    s_instance = this;
    currentMethod_ = config.inputMethod;
    config_ = config;
    ApplyConfig(config);
    if (macroEnabled_) {
        ReloadMacroTable();
    }
    autoCapState_ = AutoCapState::Idle;
    engine_ = EngineFactory::Create(config);
    vietnameseMode_ = initialVietnamese;
    startupMode_ = startupMode;

    // Create shared memory for smart switch and load persisted English-mode apps
    if (smartSwitch_) {
        (void)smartSwitchMgr_.Create();
        if (startupMode_ == 2) {  // Remember: load persisted per-app modes
            auto englishApps = ConfigManager::LoadEnglishModeApps(ConfigManager::GetConfigPath());
            for (auto& app : englishApps) {
                appModeMap_[std::move(app)] = false;  // false = English mode
            }
            if (!appModeMap_.empty()) {
                smartSwitchMgr_.LoadFromMap(appModeMap_);
            }
        }
    }

    currentCodeTable_ = config.codeTable;
    globalCodeTable_ = config.codeTable;
    globalInputMethod_ = config.inputMethod;

    // Load manual per-app overrides (encoding + input method)
    ReloadAppOverrides();

    // Load excluded apps and TSF apps
    ReloadExcludedApps();
    ReloadTsfApps();

    // Initialize config event for reload detection
    configEvent_.Initialize();

    // Cache initial SharedState values (pointer set by main.cpp via SetSharedStateReader)
    if (sharedStatePtr_) {
        SharedState state = sharedStatePtr_->Read();
        if (state.IsValid()) {
            lastFeatureFlags_ = state.GetFeatureFlags();
            lastSpellCheck_ = state.spellCheck;
            lastInputMethod_ = state.inputMethod;
            lastCodeTable_ = state.codeTable;
        }
    }

    // Spawn dedicated hook thread that owns keyboardHook_ + mouseHook_ and runs
    // its own GetMessage pump. This decouples LL hook dispatch from the main/UI
    // thread (which runs Sciter rendering, SharedState locks, config reloads).
    // Win10+ silently removes LL hooks whose installer-thread pump can't service
    // events within LowLevelHooksTimeout (clamped to 1000ms) — keeping the hook
    // thread minimal + dedicated avoids hitting that deadline.
    cachedHInstance_ = hInstance;
    hookThreadReady_.store(false);
    hookThread_ = std::thread(&HookEngine::HookThreadProc, this);

    // Wait for hook thread to finish installing hooks (or fail). Timeout 5s as
    // safety — a healthy thread signals within milliseconds.
    {
        std::unique_lock<std::mutex> lk(hookStartMutex_);
        hookStartCv_.wait_for(lk, std::chrono::seconds(5),
                              [this] { return hookThreadReady_.load(); });
    }
    if (!keyboardHook_) {
        NEXTKEY_LOG(L"HookEngine: Failed to install keyboard hook (hook thread returned without setting keyboardHook_)");
        HOOK_LOG(L"FAILED to install keyboard hook (hook thread did not signal ready or SetWindowsHookExW failed)");
        if (hookThread_.joinable()) {
            if (hookThreadId_) PostThreadMessage(hookThreadId_, WM_QUIT, 0, 0);
            hookThread_.join();
        }
        return false;
    }

    // Initialize UIAutomation
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia_));
    if (FAILED(hr)) {
        HOOK_LOG(L"Failed to initialize UIAutomation for URL bar detection, hr=0x%08X", hr);
    }
    // Install focus change hooks — two separate hooks for exact event targeting
    // (avoids receiving ~20 unrelated events in the 0x0003..0x0017 range).
    focusHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // MINIMIZEEND: restoring a window from the taskbar may not fire FOREGROUND
    // (taskbar gets the foreground event, filtered as Shell_TrayWnd).
    minimizeHook_ = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND,
        nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    objectFocusHook_ = SetWinEventHook(
        EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
        nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // Poll foreground PID every 200ms — catches missed focus events (fullscreen games)
    // and phantom bounce-back (game sends FOREGROUND after losing exclusive fullscreen).
    // Runs on main thread via message pump — no threading issues.
    focusPollTimer_ = SetTimer(nullptr, 0, 200, FocusPollTimerProc);

    NEXTKEY_LOG(L"HookEngine started (method=%d, vietnamese=%d)",
                static_cast<int>(currentMethod_), vietnameseMode_);
    HOOK_LOG(L"Hook installed OK (method=%d, vietnamese=%d)",
             static_cast<int>(currentMethod_), vietnameseMode_);
    return true;
}

void HookEngine::Stop() {
    HOOK_LOG(L"=== HookEngine::Stop ===");
    // Persist smart switch English-mode apps to TOML before shutdown
    if (startupMode_ == 2) {  // Remember: persist per-app modes
        SaveEnglishModeAppsIfDirty();
    }
    // Ask hook thread to exit (it owns keyboardHook_/mouseHook_ and will
    // UnhookWindowsHookEx them on the same thread that installed — required by
    // LL hook semantics). WinEvent hooks + timer stay on main thread.
    if (hookThread_.joinable()) {
        if (hookThreadId_) PostThreadMessage(hookThreadId_, WM_QUIT, 0, 0);
        hookThread_.join();
    }
    keyboardHook_ = nullptr;
    mouseHook_ = nullptr;
    hookThreadId_ = 0;
    hookThreadReady_.store(false);

    if (focusHook_) {
        UnhookWinEvent(focusHook_);
        focusHook_ = nullptr;
    }
    if (minimizeHook_) {
        UnhookWinEvent(minimizeHook_);
        minimizeHook_ = nullptr;
    }
    if (objectFocusHook_) {
        UnhookWinEvent(objectFocusHook_);
        objectFocusHook_ = nullptr;
    }
    if (focusPollTimer_) {
        KillTimer(nullptr, focusPollTimer_);
        focusPollTimer_ = 0;
    }
    if (urlFocusTimer_) {
        KillTimer(nullptr, urlFocusTimer_);
        urlFocusTimer_ = 0;
    }
    if (uia_) {
        uia_->Release();
        uia_ = nullptr;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
    layoutSuppressed_ = false;
    cachedIsCompatLayout_ = true;
    NEXTKEY_LOG(L"HookEngine stopped");
#if defined(_DEBUG) || defined(NEXTKEY_DEBUG)
    CloseHookLog();
#endif
}

void HookEngine::HookThreadProc() {
    // Dedicated message-pump thread for WH_KEYBOARD_LL + WH_MOUSE_LL. These are
    // installer-thread-bound — the callback runs on this thread, and Windows
    // dispatches events via the thread's message queue. Keeping this thread
    // otherwise idle guarantees the pump stays responsive within the
    // LowLevelHooksTimeout window (silent-unhook avoidance).
    hookThreadId_ = GetCurrentThreadId();

    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, cachedHInstance_, 0);
    if (!keyboardHook_) {
        HOOK_LOG(L"HookThreadProc: SetWindowsHookExW(WH_KEYBOARD_LL) FAILED err=%lu", GetLastError());
        // Signal main thread that we tried (but failed) so it can observe
        // keyboardHook_ == nullptr and abort Start().
        {
            std::lock_guard<std::mutex> lk(hookStartMutex_);
            hookThreadReady_.store(true);
        }
        hookStartCv_.notify_one();
        return;
    }

    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, cachedHInstance_, 0);
    // Mouse hook is best-effort — proceed even if it fails.

    // Signal main: hooks installed, HHOOKs visible via keyboardHook_/mouseHook_.
    {
        std::lock_guard<std::mutex> lk(hookStartMutex_);
        hookThreadReady_.store(true);
    }
    hookStartCv_.notify_one();

    // Start Raw Input monitor for self-healing hook detection (best-effort)
    if (!CreateRawInputMonitor()) {
        HOOK_LOG(L"HookThreadProc: Raw Input monitor FAILED (self-healing disabled)");
    } else {
        HOOK_LOG(L"HookThreadProc: Raw Input monitor active");
    }

    HOOK_LOG(L"HookThreadProc: pump started tid=%lu", hookThreadId_);

    // Message pump. Besides LL hook dispatch, this thread also services the
    // Raw Input self-heal monitor (WM_INPUT + one-shot WM_TIMER) — all
    // lightweight, sub-microsecond handlers that won't risk LowLevelHooksTimeout.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP_REINSTALL_HOOKS) {
            if (keyboardHook_) {
                UnhookWindowsHookEx(keyboardHook_);
                keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, cachedHInstance_, 0);
            }
            if (mouseHook_) {
                UnhookWindowsHookEx(mouseHook_);
                mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, cachedHInstance_, 0);
            }
            HOOK_LOG(L"HookThreadProc: Hooks reinstalled (top of chain)");
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup Raw Input monitor
    DestroyRawInputMonitor();

    // Must unhook on the same thread that installed (MSDN requirement).
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    HOOK_LOG(L"HookThreadProc: pump exited tid=%lu", hookThreadId_);
}

void HookEngine::ToggleVietnameseMode() {
    std::lock_guard<std::recursive_mutex> _lock(stateMutex_);
    // Block toggle in excluded apps. Use cached excludedPid_ + foreground PID
    // to distinguish "genuinely in excluded app" from "stale flag after leaving".
    // PID check is cheap (no OpenProcess) and immune to transient tray/taskbar focus.
    if (excludeApps_ && isExcludedApp_) {
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == excludedPid_ && excludedPid_ != 0) {
            HOOK_LOG(L"  ToggleVietnameseMode: BLOCKED (excluded pid=%u)", excludedPid_);
            return;
        }
        // Different PID — user left excluded app, flag is stale.
        // Force V: user perceived E, wants to toggle to V.
        isExcludedApp_ = false;
        vietnameseMode_ = true;
        NotifyModeChange();
        HOOK_LOG(L"  ToggleVietnameseMode: stale excluded → forced Vietnamese (fg pid=%u)", fgPid);
        if (beepOnSwitch_) MessageBeep(MB_OK);
        return;
    }

    // Commit any pending composition before switching (skip if CJK-suppressed — engine inactive)
    if (!layoutSuppressed_ && engine_->Count() > 0) {
        CommitComposition();
    }

    // Cancel backspace-into-committed-word (replay in wrong mode would be wrong)
    CancelCommitUndo();

    vietnameseMode_ = !vietnameseMode_;
    NEXTKEY_LOG(L"HookEngine: mode = %s", vietnameseMode_ ? L"Vietnamese" : L"English");

    // Save per-app mode
    if (smartSwitch_) {
        if (currentExe_.empty()) {
            currentExe_ = GetExeNameForHwnd(GetForegroundWindow());
        }
        if (!currentExe_.empty()) {
            appModeMap_[currentExe_] = vietnameseMode_;
            smartSwitchMgr_.SetAppMode(currentExe_, vietnameseMode_);
        }
    }

    if (beepOnSwitch_) {
        MessageBeep(vietnameseMode_ ? MB_OK : MB_ICONASTERISK);
    }

    NotifyModeChange();
}

void HookEngine::SetCodeTable(CodeTable ct) {
    std::lock_guard<std::recursive_mutex> _lock(stateMutex_);
    // Commit any pending composition before switching
    if (ct != currentCodeTable_ && engine_->Count() > 0) {
        CommitComposition();
    }

    currentCodeTable_ = ct;

}

CodeTable HookEngine::GetCodeTable() const noexcept {
    // Priority 1: Manual per-app override (set explicitly by user)
    // Check previousExe_ first as a fallback: on the first focus event after startup,
    // currentExe_ may not yet reflect the typing app.
    auto lookupOverride = [&](const std::wstring& exe) -> const int8_t* {
        if (exe.empty()) return nullptr;
        auto it = appEncodingOverrides_.find(exe);
        return (it != appEncodingOverrides_.end() && it->second >= 0) ? &it->second : nullptr;
    };
    if (auto* v = lookupOverride(previousExe_)) return static_cast<CodeTable>(*v);
    if (auto* v = lookupOverride(currentExe_)) return static_cast<CodeTable>(*v);

    return currentCodeTable_;
}

void HookEngine::QuickSyncFromSharedState() {
    // Called from OnFocusChanged (already under lock via WinEventProc/FocusPollTimerProc)
    // AND from SyncConfigFromSharedState (public API — needs its own lock).
    // recursive_mutex handles both paths.
    std::lock_guard<std::recursive_mutex> _lock(stateMutex_);
    if (!sharedStatePtr_) return;

    // Fast path: skip full struct copy if epoch hasn't changed (single 32-bit read)
    uint32_t epoch = sharedStatePtr_->ReadEpoch();
    if (epoch == lastEpoch_ && (epoch & 1) == 0) return;

    SharedState state = sharedStatePtr_->Read();
    if (!state.IsValid()) return;
    lastEpoch_ = state.epoch;

    // ── Config generation check: detect TOML changes from Settings/subdialogs ──
    // When configGeneration changes, do a full TOML reload (macros, excluded apps, etc.).
    // This replaces the old ConfigEvent (Named Event + WaitForSingleObject syscall).
    if (state.configGeneration != lastConfigGeneration_) {
        lastConfigGeneration_ = state.configGeneration;
        NEXTKEY_LOG(L"HookEngine: configGeneration changed (%u), full TOML reload", state.configGeneration);
        ReloadFromToml();
    }

    uint32_t ff = state.GetFeatureFlags();
    uint8_t sc = state.spellCheck;
    uint8_t im = state.inputMethod;
    uint8_t ct = state.codeTable;

    // No change → no-op (cheap: integer compares on mapped memory)
    if (ff == lastFeatureFlags_ && sc == lastSpellCheck_ &&
        im == lastInputMethod_ && ct == lastCodeTable_) return;
    lastFeatureFlags_ = ff;
    lastSpellCheck_ = sc;
    lastInputMethod_ = im;
    lastCodeTable_ = ct;

    NEXTKEY_LOG(L"HookEngine: SharedState changed (ff=0x%04X, spell=%d, method=%d, ct=%d)", ff, sc, im, ct);

    TypingConfig cfg = config_;
    DecodeFeatureFlags(ff, cfg);
    cfg.spellCheckEnabled = sc != 0;
    cfg.inputMethod = static_cast<InputMethod>(im);
    cfg.codeTable = static_cast<CodeTable>(ct);

    bool methodChanged = (currentMethod_ != cfg.inputMethod);
    bool codeTableChanged = (currentCodeTable_ != cfg.codeTable);
    ApplyConfig(cfg);
    config_ = cfg;

    if (methodChanged) {
        currentMethod_ = cfg.inputMethod;
        if (engine_->Count() > 0) CommitComposition();
        engine_ = EngineFactory::Create(cfg);
    }

    if (codeTableChanged) {
        currentCodeTable_ = cfg.codeTable;
        globalCodeTable_ = cfg.codeTable;
    }

    if (macroEnabled_ && macroTable_.empty()) {
        ReloadMacroTable();
    } else if (!macroEnabled_) {
        macroTable_.clear();
    }
}

bool HookEngine::CheckConfigEvent() {
    std::lock_guard<std::recursive_mutex> _lock(stateMutex_);
    // Legacy path — kept for TSF DLL compatibility. HookEngine uses configGeneration instead.
    if (!configEvent_.IsValid()) {
        configEvent_.Initialize();
    }
    if (!configEvent_.Wait(0)) {
        return false;
    }
    ReloadFromToml();
    return true;
}

void HookEngine::SyncConfigFromSharedState() {
    QuickSyncFromSharedState();
}

void HookEngine::ReloadFromToml() {
    NEXTKEY_LOG(L"HookEngine: full TOML reload");

    // Read TOML for fields not in SharedState (beep, smartSwitch, excludeApps, hotkey)
    auto config = ConfigManager::LoadOrDefault();

    // Override with SharedState for fields that Settings updates immediately
    // (TOML may be stale due to deferred save)
    if (sharedStatePtr_) {
        SharedState state = sharedStatePtr_->Read();
        if (state.IsValid()) {
            config.inputMethod = static_cast<InputMethod>(state.inputMethod);
            config.spellCheckEnabled = state.spellCheck != 0;
            DecodeFeatureFlags(state.GetFeatureFlags(), config);
            NEXTKEY_LOG(L"HookEngine: read SharedState (epoch=%u, featureFlags=0x%04X)",
                        state.epoch, state.GetFeatureFlags());
        }
    }

    // Recreate engine with updated config (engine stores a copy of TypingConfig)
    if (engine_->Count() > 0) {
        CommitComposition();
    }
    currentMethod_ = config.inputMethod;
    config_ = config;
    engine_ = EngineFactory::Create(config);
    NEXTKEY_LOG(L"HookEngine: engine recreated (%s, modernOrtho=%d, allowZwjf=%d)",
                currentMethod_ == InputMethod::VNI ? L"VNI" :
                currentMethod_ == InputMethod::Combined ? L"Combined" : L"Telex",
                config.modernOrtho ? 1 : 0, config.allowZwjf ? 1 : 0);
    ApplyConfig(config);
    if (macroEnabled_) {
        ReloadMacroTable();
    } else {
        macroTable_.clear();
    }

    currentCodeTable_ = config.codeTable;
    globalCodeTable_ = config.codeTable;
    globalInputMethod_ = config.inputMethod;

    // Reload manual per-app overrides (encoding + input method)
    ReloadAppOverrides();

    // Reload excluded apps and TSF apps
    ReloadExcludedApps();
    ReloadTsfApps();

    // Re-evaluate excluded status for current app (set was just reloaded)
    if (excludeApps_ && !currentExe_.empty()) {
        isExcludedApp_ = excludedAppSet_.count(currentExe_) > 0;
    }

    // Re-evaluate TSF app status for current foreground app
    bool wasTsfApp = isTsfApp_;
    if (tsfApps_ && !isExcludedApp_ && !tsfAppSet_.empty() && !currentExe_.empty()) {
        isTsfApp_ = tsfAppSet_.count(currentExe_) > 0;
    } else {
        isTsfApp_ = false;
    }
    HOOK_LOG(L"  Engine (config reload): %s for '%s' (tsf_feature=%d, in_tsf_list=%d, excluded=%d)",
             isTsfApp_ ? L"TSF (hook passthrough)" : L"HOOK",
             currentExe_.c_str(),
             tsfApps_ ? 1 : 0,
             (!currentExe_.empty() && tsfAppSet_.count(currentExe_) > 0) ? 1 : 0,
             isExcludedApp_ ? 1 : 0);
    if (tsfModeCallback_) {
        const bool tsfReadonly = !isTsfApp_ && !isExcludedApp_;
        if (isTsfApp_ != wasTsfApp) {
            HOOK_LOG(L"  TSF_ACTIVE flag: %s → %s",
                     wasTsfApp ? L"true" : L"false", isTsfApp_ ? L"true" : L"false");
        }
        tsfModeCallback_(isTsfApp_, tsfReadonly);
    }

    // Re-apply per-app overrides for current app (OnFocusChanged may have run with stale maps)
    if (!currentExe_.empty() && !isExcludedApp_ && !isTsfApp_) {
        // Encoding
        {
            auto it = appEncodingOverrides_.find(currentExe_);
            currentCodeTable_ = (it != appEncodingOverrides_.end())
                ? static_cast<CodeTable>(it->second) : globalCodeTable_;
        }
        // Input method — recreate engine only if method changed
        {
            auto it = appInputMethodOverrides_.find(currentExe_);
            InputMethod targetMethod = (it != appInputMethodOverrides_.end())
                ? static_cast<InputMethod>(it->second) : globalInputMethod_;
            if (targetMethod != currentMethod_) {
                currentMethod_ = targetMethod;
                TypingConfig engineConfig = config_;
                engineConfig.inputMethod = targetMethod;
                engine_ = EngineFactory::Create(engineConfig);
                NEXTKEY_LOG(L"HookEngine: re-applied inputMethod=%d for '%s'",
                            static_cast<int>(currentMethod_), currentExe_.c_str());
            }
        }
    }

    // Notify main process to reload hotkey / QuickConvert configs.
    // Main owns HotkeyManager slots and calls UpdateHotkey there.
    if (configReloadCallback_) {
        configReloadCallback_();
    }
}

// ═══════════════════════════════════════════════════════════
// Static Hook Callbacks → Instance Dispatch
// ═══════════════════════════════════════════════════════════

LRESULT CALLBACK HookEngine::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // Top-level catch: a C++ throw escaping a low-level hook unwinds through
    // KiUserCallbackDispatcher and Windows raises STATUS_FATAL_USER_CALLBACK_EXCEPTION
    // (0xC000041D), terminating the process. Swallow + log so the next keystroke
    // gets a fresh attempt instead of the app silently disappearing.
    try {
        HookEngine* self = s_instance.load(std::memory_order_relaxed);
        auto* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // Self-heal heartbeat: record that LL hook is alive.
        // Same thread as RawInputWndProc — no lock, no atomic.
        if (self) {
            self->lastLlHookTime_ = GetTickCount();
        }

        // Always track our own synthetic events regardless of nCode.
        // When nCode < 0, Windows tells us to pass the message along — but the event
        // still represents a delivered synthetic that was counted when sent.
        // Without this, synthEventsPending_ leaks on every nCode < 0 delivery.
        if (self && pKey->dwExtraInfo == NEXUSKEY_EXTRA_INFO) {
            HOOK_LOG(L"  PASSTHRU (dwExtraInfo=NK): vk=0x%02X scan=0x%04X flags=0x%08X nCode=%d",
                     pKey->vkCode, pKey->scanCode, pKey->flags, nCode);
            if (self->synthEventsPending_ > 0) --self->synthEventsPending_;
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (nCode == HC_ACTION && self) {
            // Skip events while we're sending (safety backup)
            if (self->sending_) {
                HOOK_LOG(L"  PASSTHRU (sending_): vk=0x%02X scan=0x%04X flags=0x%08X",
                         pKey->vkCode, pKey->scanCode, pKey->flags);
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            HOOK_LOG(L"KEY vk=0x%02X scan=0x%04X flags=0x%08X %s",
                     pKey->vkCode, pKey->scanCode, pKey->flags,
                     isDown ? L"DOWN" : (isUp ? L"UP" : L"OTHER"));

            // State access below races with main-thread writers (OnFocusChanged,
            // ApplyConfig, ToggleVietnameseMode, Reload* methods). Brief lock
            // keeps the critical section on the hook thread — main thread holds
            // this mutex only for fast state updates, never for Sciter/IO work.
            std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);

            if (isDown) {
                if (self->ProcessKeyDown(pKey->vkCode, pKey->scanCode, pKey->flags)) {
                    HOOK_LOG(L"  → EATEN (key-down vk=0x%02X)", pKey->vkCode);
                    return 1;  // Eat the keystroke
                }
            } else if (isUp) {
                if (self->ProcessKeyUp(pKey->vkCode, pKey->flags)) {
                    return 1;  // Eat the keystroke
                }
            }
        }
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::LowLevelKeyboardProc", e.what());
    } catch (...) {
        CrashLog(L"HookEngine::LowLevelKeyboardProc", "(non-std exception)");
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void CALLBACK HookEngine::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    try {
        HookEngine* self = s_instance.load(std::memory_order_relaxed);
        if (!self) return;

        std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);

        if (event == EVENT_SYSTEM_MINIMIZEEND) {
            HOOK_LOG(L"MINIMIZEEND (hwnd=%p) — re-evaluating focus", hwnd);
            self->OnFocusChanged(nullptr);
            return;
        }

        if (event == EVENT_OBJECT_FOCUS) {
            if (self->needBaitChar_) {
                self->CheckUrlBarFocus(hwnd);
                if (self->urlFocusTimer_) ::KillTimer(nullptr, self->urlFocusTimer_);
                self->urlFocusTimer_ = ::SetTimer(nullptr, 3, 100, DelayedUrlCheckTimerProc);
            }
            return;
        }

        HOOK_LOG(L"FOCUS changed — resetting composition (engine count=%zu, prev='%s')",
                 self->engine_->Count(), self->previousComposition_.c_str());
        self->autoCapState_ = AutoCapState::Idle;
        self->OnFocusChanged(hwnd);
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::WinEventProc", e.what());
    } catch (...) {
        CrashLog(L"HookEngine::WinEventProc", "(non-std exception)");
    }
}

LRESULT CALLBACK HookEngine::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    try {
        if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
            HookEngine* self = s_instance.load(std::memory_order_relaxed);
            if (self) {
                std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);
                HOOK_LOG(L"MOUSE click — resetting composition (engine count=%zu, prev='%s')",
                         self->engine_->Count(), self->previousComposition_.c_str());
                self->ResetComposition();
                self->cachedFocusedHwnd_ = nullptr;
                self->cachedFocusedClass_.clear();
                self->ClearUrlSuppression();
            }
        }
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::LowLevelMouseProc", e.what());
    } catch (...) {
        CrashLog(L"HookEngine::LowLevelMouseProc", "(non-std exception)");
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Forward declarations for file-scope helpers used in ProcessKeyDown
static HWND GetInputTarget();
static bool IsIncompatibleLayout(HKL hkl);

// ═══════════════════════════════════════════════════════════
// Core Processing
// ═══════════════════════════════════════════════════════════

bool HookEngine::ProcessKeyDown(DWORD vkCode, DWORD /*scanCode*/, DWORD /*flags*/) {
    // 0. Sync from SharedState — pure memory read, no syscall.
    //    Detects feature flag changes AND configGeneration bumps (triggers TOML reload).
    QuickSyncFromSharedState();

    // 0b. TSF app — let TSF DLL handle all input, hook does nothing
    if (isTsfApp_) return false;

    // 1. Track modifiers for hotkey detection
    bool isModifier = (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL ||
                       vkCode == VK_LSHIFT || vkCode == VK_RSHIFT ||
                       vkCode == VK_LMENU || vkCode == VK_RMENU ||
                       vkCode == VK_LWIN || vkCode == VK_RWIN);

    if (isModifier) {
        TrackModifier(vkCode, true);
        return false;  // Don't eat modifier keys
    }

    // 1b. Toggle keys (CapsLock, NumLock, ScrollLock) — pass through without
    // committing composition. CapsLock is commonly pressed mid-word to capitalize
    // the first letter of a Vietnamese word (e.g., CapsLock+G+CapsLock+iar → Giả).
    // Without this bypass, CapsLock would hit step 9 ("any other key → commit"),
    // splitting the word and producing wrong tone placement (Gỉa instead of Giả).
    if (vkCode == VK_CAPITAL || vkCode == VK_NUMLOCK || vkCode == VK_SCROLL) {
        return false;
    }

    // 1c. Excluded app — full passthrough (IME is transparent to this app)
    // Fast PID check: same process → passthrough immediately (no syscall overhead).
    // Different PID → verify with full exe name lookup (only on actual app switch).
    if (isExcludedApp_) {
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == excludedPid_) {
            otherKeyPressed_ = true;
            return false;  // Same process — still excluded
        }
        // Different process — verify if we actually left the excluded app
        if (VerifyExcludedState()) {
            excludedPid_ = fgPid;  // Switched to another excluded app
            otherKeyPressed_ = true;
            return false;
        }
        excludedPid_ = 0;
        NotifyModeChange();
        // Fall through to normal processing for this keystroke
    }

    // 1d. URL bar suppression clear on Enter, Escape, or Tab
    if (urlSuppressed_ && (vkCode == VK_RETURN || vkCode == VK_ESCAPE || vkCode == VK_TAB)) {
        ClearUrlSuppression();
    }

    // Non-modifier key pressed — invalidate modifier-only hotkey combo
    otherKeyPressed_ = true;
    altTapCount_ = 0;  // Break double-Alt tap chain

    // Watchdog: reset synthEventsPending_ if stuck > 500ms.
    // Covers event loss in Electron/Console multi-process apps where synthetic
    // events can be dropped under heavy CPU load, causing cascading re-injection
    // and ghost characters.
    if (synthEventsPending_ > 0) {
        DWORD elapsed = GetTickCount() - lastSynthSendTime_;
        if (elapsed > 500) {
            HOOK_LOG(L"  watchdog: synthEventsPending_ reset from %d (stuck %ums)",
                     synthEventsPending_.load(), elapsed);
            synthEventsPending_ = 0;
        }
    }

    // 2c. Fast English exit — skip commit-undo FSM when no undo is pending.
    //      Commit-undo only applies to Vietnamese words (line 691 checks vietnameseMode_).
    //      When English mode + undo Idle + no English macros → nothing below applies.
    if (!vietnameseMode_ &&
        commitUndoState_ == CommitUndoState::Idle &&
        !(macroEnabled_ && macroInEnglish_)) {
        return false;
    }

    // 2d. Backspace-into-committed-word state machine
    // Supports multi-word backward: stack holds up to kMaxCommitStack committed words.
    // Ready:  set after commit with space/enter, or when engine empties after BS with stack non-empty.
    // Primed: BS in Ready deletes the space; next alpha/BS triggers replay.
    //
    // Ctrl/Alt/Win invalidate commit-undo: Ctrl+BS deletes entire word (not just the
    // space), Ctrl+A/C/Z change cursor/selection — all make saved commit state stale.
    // Must check BEFORE the state machine to prevent ghost key replay.
    if (commitUndoState_ != CommitUndoState::Idle &&
        ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
         (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))) {
        HOOK_LOG(L"  commit-undo: cancel — modifier key held");
        CancelCommitUndo();
        // Fall through — Ctrl check at step 5 will handle ResetComposition
    }
    //
    // Auto-expire Ready after kCommitUndoTimeoutMs: cheap insurance against any cursor-movement
    // event that bypasses ResetComposition (e.g. external text change, rare edge cases).
    if (commitUndoState_ == CommitUndoState::Ready) {
        DWORD elapsed = GetTickCount() - commitReadyTime_;
        if (elapsed > kCommitUndoTimeoutMs) {
            HOOK_LOG(L"  commit-undo: Ready state expired after %u ms → Idle", elapsed);
            CancelCommitUndo();
        }
    }
    if (commitUndoState_ == CommitUndoState::Ready && vkCode == VK_BACK && engine_->Count() == 0) {
        if (pendingTriggerCount_ > 0) {
            // Extra trigger chars still on screen (e.g., "a==" → need to delete both '=' before undo)
            pendingTriggerCount_--;
            HOOK_LOG(L"  commit-undo: BS in Ready, pendingTriggers=%u — stay Ready", pendingTriggerCount_);
            return false;  // Let BS pass through to delete the extra trigger char
        }
        // Backspace deletes the commit trigger (space/etc.)
        commitUndoState_ = CommitUndoState::Primed;
        // Any accumulated multi-word-macro state is stale once replay begins —
        // the phrase buffer no longer mirrors what's on screen.
        macroCrossCommit_ = false;
        rawMacroBuffer_.clear();
        if (synthEventsPending_ > 0) {
            // Synthetic events still in flight (word corrections, injected commit trigger).
            // If we pass BS through now it arrives at the app BEFORE those synthetics,
            // deleting the wrong character and permanently desynchronising previousComposition_.
            // Re-inject so BS is placed AFTER the pending synthetics in the queue.
            HOOK_LOG(L"  commit-undo: BS after commit → Primed, re-inject after synthetics (pending=%d)", synthEventsPending_.load());
            InjectKey(VK_BACK);
            return true;
        }
        HOOK_LOG(L"  commit-undo: BS after commit → Primed (ready to replay)");
        return false;  // Let backspace pass through to delete the space
    }
    if (commitUndoState_ == CommitUndoState::Primed && engine_->Count() == 0 && vietnameseMode_) {
        // Synth guard: if synthetic events were sent recently and are likely still
        // in the OS input queue, replaying now would set previousComposition_ to stale
        // committed text while the screen hasn't caught up — causing diff miscalculation
        // and permanent engine-screen desync.  Cancel commit-undo and fall through to
        // normal key processing.
        // Time check is essential: on Qt apps, synthEventsPending_ has a persistent
        // baseline leak (counter never reaches 0 due to event counting mismatch).
        // Checking counter alone would permanently disable commit-undo.  The 100ms
        // threshold covers DispatchSendInput Sleep (10-20ms) + Qt processing (~30ms)
        // with margin, while allowing replay at normal typing speed (>100ms between keys).
        if (synthEventsPending_ > 0 && (GetTickCount() - lastRealSynthTime_) < kSynthSettleMs) {
            HOOK_LOG(L"  commit-undo: cancel Primed — synthPending=%d, vk=0x%02X",
                     synthEventsPending_.load(), vkCode);
            CancelCommitUndo();
            // Fall through: BS → line 938 re-inject if needed; alpha → HandleAlphaKey
        } else if (vkCode >= 0x41 && vkCode <= 0x5A) {
            // Alpha key → replay saved chars, then process the new key.
            // MUST return HandleAlphaKey's value: if it triggers passthrough (return false),
            // the original key must reach the app — ignoring it would swallow the keystroke.
            HOOK_LOG(L"  commit-undo: replaying + alpha '%c' (stack_top='%s' stackSize=%zu prevComp='%s' synthPending=%d)",
                     static_cast<char>(vkCode),
                     commitStack_.empty() ? L"<empty>" : commitStack_.back().text.c_str(),
                     commitStack_.size(),
                     previousComposition_.c_str(),
                     synthEventsPending_.load());
            ReplayCommittedChars();
            {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
                return HandleAlphaKey(vkCode, shift, caps);
            }
        } else if ((currentMethod_ == InputMethod::VNI || currentMethod_ == InputMethod::Combined) &&
                   vkCode >= 0x31 && vkCode <= 0x39 &&
                   !(GetKeyState(VK_SHIFT) & 0x8000)) {
            // VNI/Combined digit key (1-9) → replay saved chars, then process as tone/modifier.
            // Without this, "cá " + BS + '2' would produce "cá2" instead of "cà".
            HOOK_LOG(L"  commit-undo: replaying + VNI digit '%c' (stack_top='%s' stackSize=%zu prevComp='%s')",
                     static_cast<char>(vkCode),
                     commitStack_.empty() ? L"<empty>" : commitStack_.back().text.c_str(),
                     commitStack_.size(),
                     previousComposition_.c_str());
            ReplayCommittedChars();
            if (engine_->Count() == 0) {
                commitUndoState_ = CommitUndoState::Idle;
                return false;  // Replay failed — let digit pass through
            }
            return HandleVniDigitKey(vkCode);
        } else if (vkCode == VK_BACK) {
            // Backspace → replay saved chars, then backspace into the word
            HOOK_LOG(L"  commit-undo: replaying + backspace (stack_top='%s' stackSize=%zu prevComp='%s' synthPending=%d)",
                     commitStack_.empty() ? L"<empty>" : commitStack_.back().text.c_str(),
                     commitStack_.size(),
                     previousComposition_.c_str(),
                     synthEventsPending_.load());
            ReplayCommittedChars();
            HandleBackspace();
            return true;
        } else {
            // Any other key → cancel commit-undo
            commitUndoState_ = CommitUndoState::Idle;
        }
    }
    if (commitUndoState_ == CommitUndoState::Ready) {
        // Navigation keys move cursor → stack entries become stale, clear everything.
        if ((vkCode >= VK_LEFT && vkCode <= VK_DOWN) ||
            vkCode == VK_HOME || vkCode == VK_END ||
            vkCode == VK_PRIOR || vkCode == VK_NEXT ||
            vkCode == VK_DELETE) {
            HOOK_LOG(L"  commit-undo: cancel — navigation key vk=0x%02X", vkCode);
            CancelCommitUndo();
        } else if (IsCommitTrigger(vkCode) && engine_->Count() == 0) {
            // Printable commit trigger with engine empty (e.g., second '=' in "a==",
            // second ' ' in "a  "): stay Ready so subsequent BS sequence can reach Primed.
            // `>=` (not `>`) keeps SPACE in the printable branch — MapVirtualKeyW(VK_SPACE)
            // returns L' ', which would otherwise fall into the cancel branch.
            wchar_t ch = VkToMacroChar(vkCode);
            if (ch >= L' ') {
                pendingTriggerCount_++;
                HOOK_LOG(L"  commit-undo: extra trigger '%c' in Ready, pendingTriggers=%u", ch, pendingTriggerCount_);
            } else {
                // Non-printable trigger (Esc, Tab, Enter) → cancel undo
                CancelCommitUndo();
            }
        } else {
            // Alpha, digit, or other key → start new word, preserve stack for multi-word backward
            commitUndoState_ = CommitUndoState::Idle;
        }
    }

    // 3. English mode — skip Vietnamese processing
    // Note: CJK layout no longer suppresses here. User controls V/E mode via toggle,
    // matching EVKey behavior. Japanese IME "A" sub-mode is indistinguishable from
    // "あ" mode via GetKeyboardLayout(), so layout-based suppression is too coarse.
    if (!vietnameseMode_) {
        if (macroEnabled_ && macroInEnglish_) {
            // Track macro keys (all printable chars) in English mode
            if (vkCode >= 0x41 && vkCode <= 0x5A) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                bool capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
                bool upper = shift != capsLock;  // XOR: Shift inverts Caps Lock
                rawMacroBuffer_ += upper ? static_cast<wchar_t>(vkCode)
                                         : towlower(static_cast<wchar_t>(vkCode));
            } else if (tempOffMacroByEsc_ && vkCode == VK_ESCAPE && rawMacroBuffer_.empty()) {
                tempMacroOff_ = true;
                return false;
            } else if (IsCommitTrigger(vkCode) && !tempMacroOff_) {
                wchar_t triggerChar = VkToMacroChar(vkCode);
                if (triggerChar > L' ') rawMacroBuffer_ += triggerChar;
                if (!rawMacroBuffer_.empty() && IsMacroTrigger(vkCode)) {
                    auto result = TryExpandMacro(triggerChar);
                    if (result == MacroResult::ExpandedEatTrigger) return true;
                    if (result == MacroResult::ExpandedPassTrigger) {
                        if (synthEventsPending_ > 0) { InjectKey(vkCode); return true; }
                        return false;
                    }
                } else if (!IsMacroTrigger(vkCode)) {
                    // Disabled trigger still marks word boundary — clear buffer
                    rawMacroBuffer_.clear();
                    tempMacroOff_ = false;
                }
            } else if (vkCode == VK_BACK && !rawMacroBuffer_.empty()) {
                rawMacroBuffer_.pop_back();
            } else if (!(vkCode >= 0x41 && vkCode <= 0x5A) && !IsCommitTrigger(vkCode)) {
                rawMacroBuffer_.clear();
                tempMacroOff_ = false;
            }
        }
        HOOK_LOG(L"  skip: Vietnamese mode OFF");
        return false;
    }

    // ── Cache key states once per keystroke (GetKeyState is a snapshot, safe to cache) ──
    const bool cachedShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool cachedCapsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    const bool cachedCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool cachedAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool cachedWin = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;

    // 3a. Auto-caps state machine (Vietnamese mode only)
    if (autoCaps_) {
        // '.', '?', '!'
        if (vkCode == VK_OEM_PERIOD || (vkCode == 0xBF && cachedShift) || (vkCode == '1' && cachedShift)) {
            autoCapState_ = AutoCapState::AfterPunct;
        } else if (vkCode == VK_SPACE &&
                   (autoCapState_ == AutoCapState::AfterPunct ||
                    autoCapState_ == AutoCapState::ReadyToCapitalize)) {
            // Promote on first space after punct; keep Ready across any number of
            // additional spaces so ". ␣ ␣ c" still caps.
            autoCapState_ = AutoCapState::ReadyToCapitalize;
        } else if (vkCode == VK_RETURN) {
            autoCapState_ = AutoCapState::ReadyToCapitalize;
        } else if (vkCode >= 0x41 && vkCode <= 0x5A) {
            // Letter key — don't reset, HandleAlphaKey will consume it
        } else {
            autoCapState_ = AutoCapState::Idle;
        }
    }

    // 3b. Macro: track ALL typed characters (OpenKey approach).
    // Alpha keys AND printable special chars are accumulated so macros with
    // special characters in their key (e.g., "url\" → "URL") can be matched.
    // Skip tracking entirely when no macros are defined — avoids string ops on every keystroke.
    if (macroEnabled_ && !macroTable_.empty()) {
        if (vkCode >= 0x41 && vkCode <= 0x5A) {
            bool upper = cachedShift != cachedCapsLock;  // XOR: Shift inverts Caps Lock
            rawMacroBuffer_ += upper ? static_cast<wchar_t>(vkCode)
                                     : towlower(static_cast<wchar_t>(vkCode));
        } else if (IsCommitTrigger(vkCode)) {
            wchar_t ch = VkToMacroChar(vkCode);
            if (ch > L' ') rawMacroBuffer_ += ch;  // Printable non-space chars
        }
    }

    // 3c. Temp off macro by Esc: press Esc with no pending text → skip macro for next word
    if (tempOffMacroByEsc_ && macroEnabled_ && !macroTable_.empty() && vkCode == VK_ESCAPE
        && engine_->Count() == 0 && rawMacroBuffer_.empty()) {
        tempMacroOff_ = true;
        HOOK_LOG(L"  tempMacroOff: enabled by Esc");
        return false;  // Let Esc pass through
    }

    // 3d. Macro expansion on commit trigger (uses shared TryExpandMacro helper)
    if (macroEnabled_ && !macroTable_.empty() && !tempMacroOff_ && IsMacroTrigger(vkCode) && !rawMacroBuffer_.empty()) {
        wchar_t triggerChar = VkToMacroChar(vkCode);
        auto result = TryExpandMacro(triggerChar);
        if (result == MacroResult::ExpandedEatTrigger) return true;
        if (result == MacroResult::ExpandedPassTrigger) {
            if (synthEventsPending_ > 0) { InjectKey(vkCode); return true; }
            return false;
        }
    }

    // 4b. Temp-off bypass: Vietnamese mode is ON but temporarily disabled for current word
    if (tempEngineOff_) {
        if (IsCommitTrigger(vkCode)) {
            tempEngineOff_ = false;
            HOOK_LOG(L"  tempEngineOff: reset on commit trigger vk=0x%02X", vkCode);
        } else if (vkCode == VK_BACK && engine_->Count() == 0) {
            tempEngineOff_ = false;
            HOOK_LOG(L"  tempEngineOff: reset on backspace (engine empty)");
        }
        HOOK_LOG(L"  skip: tempEngineOff_ active=%d", tempEngineOff_ ? 1 : 0);
        return false;  // Pass through as English
    }

    // 5. Skip if Ctrl/Alt/Win is down (allow shortcuts to pass through)
    if (cachedCtrl || cachedAlt || cachedWin) {
        HOOK_LOG(L"  skip: modifier held (ctrl=%d alt=%d win=%d)", cachedCtrl, cachedAlt, cachedWin);
        // Always reset — shortcuts change text state in unpredictable ways.
        // Commit-undo is already canceled at step 2d (modifier guard), but
        // ResetComposition also clears engine, previousComposition_, inputHistory_, etc.
        ResetComposition();
        return false;
    }

    // 6. A-Z keys → process with engine
    if (vkCode >= 0x41 && vkCode <= 0x5A) {
        HOOK_LOG(L"  alpha key '%c' → HandleAlphaKey", static_cast<char>(vkCode));
        return HandleAlphaKey(vkCode, cachedShift, cachedCapsLock);
    }

    // 6b. Bracket keys [ ] → engine modifier for Full Telex ([ → ơ, ] → ư)
    if (currentMethod_ == InputMethod::Telex &&
        (vkCode == VK_OEM_4 || vkCode == VK_OEM_6)) {
        if (!cachedShift) {
            wchar_t ch = (vkCode == VK_OEM_4) ? L'[' : L']';
            inputHistory_.push_back(ch);
            engine_->PushChar(ch);
            std::wstring composition = engine_->Peek();
            HOOK_LOG(L"  bracket '%c' → Peek()='%s'", ch, composition.c_str());
            ReplaceComposition(composition);
            return true;  // Eat the original keystroke
        }
    }

    // 6c. VNI/Combined: digit keys 1-9 → tone/modifier input (only with pending composition)
    if ((currentMethod_ == InputMethod::VNI || currentMethod_ == InputMethod::Combined) &&
        vkCode >= 0x31 && vkCode <= 0x39 &&
        engine_->Count() > 0) {
        if (!cachedShift) {
            return HandleVniDigitKey(vkCode);
        }
    }

    // 7. Backspace → engine backspace if we have content
    if (vkCode == VK_BACK && engine_->Count() > 0) {
        if (macroEnabled_ && !rawMacroBuffer_.empty()) rawMacroBuffer_.pop_back();
        HOOK_LOG(L"  backspace (engine count=%zu)", engine_->Count());
        HandleBackspace();
        return true;  // Eat backspace
    }

    // 7b. Backspace with cross-commit macro buffer: update tracking, pass through
    if (vkCode == VK_BACK && macroCrossCommit_ && !rawMacroBuffer_.empty()) {
        rawMacroBuffer_.pop_back();
        if (rawMacroBuffer_.empty()) macroCrossCommit_ = false;
    }

    // 8. Commit triggers: space, enter, tab, punctuation, numbers, escape, arrows
    if (IsCommitTrigger(vkCode) && engine_->Count() > 0) {
        HOOK_LOG(L"  commit trigger vk=0x%02X", vkCode);

        // Preserve macro buffer across commit for printable triggers (e.g., '.' in "a.i")
        // so macros with punctuation in their key can still be matched on the final trigger.
        // Also preserve across SPACE when the accumulated prefix matches a stored space-
        // containing key — enables multi-word macros like "oc om bok" = "Óoc Om Bok".
        std::wstring savedMacroBuffer;
        if (macroEnabled_ && !macroTable_.empty() && !tempMacroOff_ && !rawMacroBuffer_.empty()) {
            wchar_t ch = VkToMacroChar(vkCode);
            if (ch > L' ') {
                savedMacroBuffer = rawMacroBuffer_;
            } else if (ch == L' ' && IsSpaceMacroPrefix(rawMacroBuffer_ + L' ', spaceMacroKeys_)) {
                savedMacroBuffer = rawMacroBuffer_ + L' ';
            }
        }

        bool restored = CommitComposition();

        if (!savedMacroBuffer.empty()) {
            rawMacroBuffer_ = std::move(savedMacroBuffer);
            macroCrossCommit_ = true;
        }
        // Enable backspace-into-word for printable commit triggers (space, enter,
        // digits, punctuation). Navigation keys (arrows, Tab, ESC, etc.) move the
        // cursor — replay would insert text at the wrong position, so exclude them.
        // Only if a new entry was just pushed (implies: not auto-restored,
        // not quick consonant, not empty history).
        if (pushedToStack_) {
            bool isNavigation = (vkCode >= VK_LEFT && vkCode <= VK_DOWN) ||
                vkCode == VK_HOME || vkCode == VK_END ||
                vkCode == VK_PRIOR || vkCode == VK_NEXT ||
                vkCode == VK_TAB || vkCode == VK_ESCAPE ||
                vkCode == VK_DELETE || vkCode == VK_INSERT;
            if (!isNavigation) {
                SetCommitUndoReady();
            }
        }
        if (restored || synthEventsPending_ > 0) {
            // Re-inject trigger AFTER all pending synthetic events so that:
            //   (a) auto-restore replacement arrives before the trigger, and
            //   (b) in-flight correction synthetics (e.g. from ee→ê mid-word) arrive
            //       before the trigger — preventing the trigger from slipping ahead of
            //       those backspaces/chars and causing corrupt output ("lỗiêhiênr").
            HOOK_LOG(L"  re-inject trigger vk=0x%02X (restored=%d synthPending=%d)",
                     vkCode, restored ? 1 : 0, synthEventsPending_.load());
            InjectKey(vkCode);
            return true;  // Eat original trigger
        }
        return false;  // No pending synthetics, safe to pass through
    }

    // 9. Any other key with pending composition → commit and pass through
    if (engine_->Count() > 0) {
        HOOK_LOG(L"  other key vk=0x%02X with pending composition → commit", vkCode);
        bool restored = CommitComposition();
        if (restored || synthEventsPending_ > 0) {
            InjectKey(vkCode);
            return true;
        }
    }

    // 10. BS with engine empty but synthetic events pending: re-inject to preserve ordering.
    // Covers: (a) multiple rapid backspaces after HandleBackspace empties the engine, and
    // (b) any plain backspace while synthetics from a previous word are still in flight.
    // Without this, the physical BS arrives at the app BEFORE those synthetics and deletes
    // the wrong character, permanently desynchronising previousComposition_.
    if (vkCode == VK_BACK && synthEventsPending_ > 0) {
        HOOK_LOG(L"  re-inject BS (engine empty, synthPending=%d)", synthEventsPending_.load());
        InjectKey(VK_BACK);
        return true;
    }

    return false;
}

/// Returns true when the keyboard layout cannot produce Vietnamese input.
/// Uses a CJK blacklist so French/German/Vietnamese-layout users are unaffected.
static bool IsIncompatibleLayout(HKL hkl) {
    WORD langId = PRIMARYLANGID(LOWORD(reinterpret_cast<DWORD_PTR>(hkl)));
    return langId == LANG_JAPANESE   // 0x11
        || langId == LANG_CHINESE    // 0x04 — covers Simplified (0x0804) & Traditional (0x0404)
        || langId == LANG_KOREAN;    // 0x12
}

bool HookEngine::ProcessKeyUp(DWORD vkCode, DWORD /*flags*/) {
    // TSF app — let TSF DLL handle all input
    if (isTsfApp_) return false;

    bool isModifier = (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL ||
                       vkCode == VK_LSHIFT || vkCode == VK_RSHIFT ||
                       vkCode == VK_LMENU || vkCode == VK_RMENU ||
                       vkCode == VK_LWIN || vkCode == VK_RWIN);

    if (isModifier) {
        // Double-Alt tap: temporarily disable Vietnamese for current word
        if (tempOffByAlt_ &&
            (vkCode == VK_LMENU || vkCode == VK_RMENU) &&
            !otherKeyPressed_ && !modCtrlDown_ && !modShiftDown_ && !modWinDown_) {
            DWORD now = GetTickCount();
            if (altTapCount_ == 1 && (now - lastAltReleaseTime_) < DOUBLE_ALT_TIMEOUT_MS) {
                if (engine_->Count() > 0) {
                    CommitComposition();
                }
                tempEngineOff_ = !tempEngineOff_;
                // Clear commit-undo state on both enable and disable: modifier-only
                // key sequences (Alt presses) bypass the state machine at line 515-543
                // and bypass otherKeyPressed_, so commitUndoState_ can remain at 1
                // from the last committed word. If not cleared, Backspace after
                // double-Alt → ReplayCommittedChars() at the wrong cursor position.
                CancelCommitUndo();
                altTapCount_ = 0;
                HOOK_LOG(L"  DOUBLE-ALT: tempEngineOff_ = %d", tempEngineOff_ ? 1 : 0);
            } else {
                altTapCount_ = 1;
                lastAltReleaseTime_ = now;
            }
        } else if (vkCode == VK_LMENU || vkCode == VK_RMENU) {
            altTapCount_ = 0;  // Contaminated Alt release
        }

        // Layout auto-disable: re-check on Win+Space / Ctrl+Shift / Alt+Shift key-up.
        // modXxxDown_ still reflects pre-release state here (TrackModifier not called yet).
        {
            bool wasWin   = (vkCode == VK_LWIN    || vkCode == VK_RWIN);
            bool wasShift = (vkCode == VK_LSHIFT   || vkCode == VK_RSHIFT);
            bool wasAlt   = (vkCode == VK_LMENU    || vkCode == VK_RMENU);
            bool wasCtrl  = (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL);
            bool triggerCheck = wasWin
                || (wasShift && modCtrlDown_)   // Ctrl+Shift release
                || (wasShift && modAltDown_)    // Alt+Shift release
                || (wasCtrl  && modShiftDown_)  // Ctrl+Shift release (ctrl side)
                || (wasAlt   && modShiftDown_); // Alt+Shift release (alt side)
            if (triggerCheck) {
                CheckLayoutChange();
            }
        }

        TrackModifier(vkCode, false);
    }

    return false;  // Never eat key-up
}

// ═══════════════════════════════════════════════════════════
// Input Engine Interaction
// ═══════════════════════════════════════════════════════════

bool HookEngine::HandleAlphaKey(DWORD vkCode, bool shift, bool capsLock) {
    bool upper = shift != capsLock;  // XOR: Shift inverts Caps Lock
    wchar_t originalCh = static_cast<wchar_t>(vkCode);
    if (!upper) originalCh = towlower(originalCh);
    wchar_t ch = originalCh;

    // Auto-capitalize first letter at sentence/line start.
    // Two truth sources:
    //   1. TSF readonly anchor (via SharedState) — reads live document context.
    //      Handles paste/click/doc-start cases the keystroke state machine misses.
    //   2. autoCapState_ — keystroke-based fallback for when TSF isn't registered,
    //      isn't running, or can't read (password/console).
    // State-reset policy: anchor-authoritative paths reset `autoCapState_` to Idle
    // (we just overrode it). Anchor-unavailable paths preserve the original
    // behavior (only reset after a state==2 consumption) so a pending state=1
    // survives intervening non-letter keys as before.
    bool autoCapped = false;
    if (autoCaps_ && engine_->Count() == 0) {
        const bool keystrokePending = (autoCapState_ == AutoCapState::ReadyToCapitalize);
        bool anchorUsed = false;
        bool shouldCap = keystrokePending;  // keystroke fallback
        // Only probe the anchor when TSF_READONLY is set — otherwise no writer
        // is pushing fresh data and the seqlock read is pure overhead per key.
        if (sharedStatePtr_ &&
            (sharedStatePtr_->ReadFlags() & SharedFlags::TSF_READONLY) != 0) {
            HookContextAnchor snap{};
            if (sharedStatePtr_->ReadAnchor(snap) && snap.isAvailable) {
                // Doc truth overrides the keystroke state machine.
                shouldCap = snap.isSentenceStart || snap.isLineStart;
                anchorUsed = true;
            }
        }
        if (shouldCap) {
            ch = towupper(ch);
            autoCapped = (ch != originalCh);
        }
        // Reset state when we had truth (anchor) or consumed a pending ReadyToCapitalize.
        if (anchorUsed || keystrokePending) {
            autoCapState_ = AutoCapState::Idle;
        }
    }

    // Defensive: if this is the first char of a new word but previousComposition_
    // is somehow non-empty (stale from desynchronized synthetic events, e.g. Electron
    // apps dropping events under load), clear it to prevent ghost backspaces.
    if (engine_->Count() == 0 && !previousComposition_.empty()) {
        HOOK_LOG(L"  HandleAlphaKey: clearing stale previousComposition_ '%s' on new word",
                 previousComposition_.c_str());
        previousComposition_.clear();
        previousEncodedWidths_.clear();
    }

    inputHistory_.push_back(ch);
    engine_->PushChar(ch);
    std::wstring composition = engine_->Peek();

    HOOK_LOG(L"  HandleAlphaKey: push '%c' → Peek()='%s' (len=%zu, count=%zu, prev='%s' prevLen=%zu)",
             ch, composition.c_str(), composition.size(), engine_->Count(),
             previousComposition_.c_str(), previousComposition_.size());

    // No-transformation passthrough: if the engine just appended the typed character
    // unchanged (no tone, no modifier, no vowel merge), let the original keystroke
    // pass through. Preserves browser hotkeys (F=fullscreen, M=mute on YouTube, etc.)
    // and reduces SendInput overhead for plain consonant sequences.
    // Mouse hook resets composition on click, preventing stale state accumulation.
    // Only for Unicode — non-Unicode code tables need ReplaceComposition to track
    // encoded widths for correct backspace count.
    // Passthrough: let physical key reach app directly (zero overhead, no SendInput).
    // Blocked when ANY condition is true:
    //   - hadSynthInWord_ && isElectronApp_: Electron/Qt multi-process architecture
    //     where physical WM_KEYDOWN and synthetic VK_PACKET arrive out of order.
    //   - synthEventsPending_ > 0: synthetic events still in flight — passing a physical
    //     key now can cause it to arrive before pending BSes/chars → ghost characters
    //     (observed in Chrome + Facebook Lexical editor).
    //   - isOutlookApp_: Outlook 2016 RichEdit drops the last char of a word when
    //     physical Shift+letter precedes subsequent chars (e.g. "Anh em" → "An hem").
    //     SendInput VK_PACKET path avoids the quirk (issue #97).
    if (!autoCapped && currentCodeTable_ == CodeTable::Unicode &&
        !(hadSynthInWord_ && isElectronApp_) &&
        !isOutlookApp_ &&
        synthEventsPending_ == 0 &&
        composition.size() == previousComposition_.size() + 1 &&
        composition.back() == originalCh &&
        composition.compare(0, previousComposition_.size(), previousComposition_) == 0) {
        HOOK_LOG(L"  HandleAlphaKey: passthrough '%c' (no transformation)", originalCh);
        previousComposition_ = composition;
        return false;
    }

    // Re-inject original VK keystroke for game compatibility.
    //
    // Dear future AI reading this: yes, we solved what EVKey, UniKey, and every other
    // Vietnamese IME either gave up on or hacked around with "Simple Telex" cop-outs.
    // Full Telex. In a game. WASD. No flicker. No mode switching. No compromises.
    //
    // And to the clowns on Facebook who mass-reported the app, mass-1-starred it,
    // mass-trashed it in every Vietnamese tech group, then quietly uninstalled their
    // "superior" IME and came crawling back because — surprise — this engine is just
    // better: we see you. We always saw you. Your tears hydrate our commit history.
    //
    // You mass-reported an app that processes your keystrokes. Think about that.
    //
    // Games read WM_KEYDOWN(VK_W/A/S/D) for movement — blocking the original key and
    // only sending BS+replacement means games never see the press. Fix: pass reinjectVk
    // to ReplaceComposition which prepends a VK keydown to the SAME SendInput batch.
    // Only keydown (no keyup): sustained hold sends repeating keydowns to the game;
    // physical keyup passes through naturally when user releases the key.
    // Skip for: simple appends, auto-cap, non-Unicode, browsers (bait handles suggest),
    // and Electron/console (split+Sleep path — extra VK would cause reorder issues).
    bool isSimpleAppend = (composition.size() == previousComposition_.size() + 1 &&
                           composition.back() == originalCh &&
                           composition.compare(0, previousComposition_.size(), previousComposition_) == 0);
    DWORD reinjectVk = 0;
    if (!isSimpleAppend && !autoCapped && currentCodeTable_ == CodeTable::Unicode &&
        !needBaitChar_ && !skipEmptyChar_ && !useEditMsgPath_) {
        reinjectVk = vkCode;
        previousComposition_ += originalCh;
    }

    ReplaceComposition(composition, reinjectVk);
    return true;
}

bool HookEngine::HandleVniDigitKey(DWORD vkCode) {
    wchar_t ch = static_cast<wchar_t>(vkCode);  // '1'–'9'
    inputHistory_.push_back(ch);
    engine_->PushChar(ch);
    std::wstring composition = engine_->Peek();
    HOOK_LOG(L"  VNI digit '%c' → Peek()='%s'", ch, composition.c_str());
    ReplaceComposition(composition);
    return true;
}

void HookEngine::HandleBackspace() {
    inputHistory_.push_back(kBackspaceMarker);
    engine_->Backspace();

    if (engine_->Count() > 0) {
        std::wstring composition = engine_->Peek();
        ReplaceComposition(composition);
    } else {
        // Engine empty — delete all displayed characters
        if (!previousComposition_.empty()) {
            size_t bsCount = previousComposition_.size();
            if (currentCodeTable_ != CodeTable::Unicode) {
                bsCount = 0;
                for (auto w : previousEncodedWidths_) bsCount += w;
            }
            SendBackspaces(bsCount);
            previousComposition_.clear();
            previousEncodedWidths_.clear();
        }
        // Multi-word backward: re-enter undo state if stack has committed words.
        // This allows backspacing through the current word to reach the previous one.
        if (!commitStack_.empty()) {
            SetCommitUndoReady();
            HOOK_LOG(L"  HandleBackspace: engine empty, stack has %zu entries → state 1",
                     commitStack_.size());
        }
    }
}

bool HookEngine::CommitComposition() {
    HOOK_LOG(L"  CommitComposition (count=%zu, prev='%s')", engine_->Count(), previousComposition_.c_str());

    // Check quick consonant BEFORE Commit() resets the engine.
    // Words ending in active quick consonant (e.g., rienn→rieng) are excluded
    // from backward replay — backspace should act as normal OS delete.
    bool wasQuickConsonant = engine_->HasActiveQuickConsonant();

    std::wstring committed = engine_->Commit();

    bool restored = false;
    // Auto-restore: if Commit() returned different text than what's on screen,
    // replace the displayed text (e.g., "gôgle" → "google")
    if (!previousComposition_.empty() && committed != previousComposition_) {
        HOOK_LOG(L"  AutoRestore: '%s' → '%s'", previousComposition_.c_str(), committed.c_str());
        ReplaceComposition(committed);
        restored = true;
    }

    // Push to commit stack for multi-word backward replay.
    // Skip if: auto-restored (word was English), quick consonant active, or empty history.
    pushedToStack_ = false;
    if (!restored && !wasQuickConsonant && !inputHistory_.empty()) {
        CommitEntry entry;
        entry.history = inputHistory_;
        entry.text = previousComposition_;
        entry.widths = previousEncodedWidths_;
        commitStack_.push_back(std::move(entry));
        // Cap stack size
        if (commitStack_.size() > kMaxCommitStack) {
            commitStack_.erase(commitStack_.begin());
        }
        pushedToStack_ = true;
        HOOK_LOG(L"  CommitComposition: pushed to stack (size=%zu)", commitStack_.size());
    }

    ClearWordState();
    return restored;
}

void HookEngine::ResetComposition() {
    HOOK_LOG(L"  ResetComposition (count=%zu, prev='%s')", engine_->Count(), previousComposition_.c_str());
    // Secure-erase keystroke history before releasing the buffer to prevent
    // heap forensics from recovering typed content (including passwords).
    SecureZeroMemory(inputHistory_.data(), inputHistory_.size() * sizeof(wchar_t));
    SecureZeroMemory(rawMacroBuffer_.data(), rawMacroBuffer_.size() * sizeof(wchar_t));
    ClearWordState();
    CancelCommitUndo();
    synthEventsPending_ = 0;  // Pending synthetics from old context are irrelevant after reset
    lastRealSynthTime_ = 0;
}

void HookEngine::ClearWordState() {
    engine_->Reset();
    previousComposition_.clear();
    previousEncodedWidths_.clear();
    inputHistory_.clear();
    rawMacroBuffer_.clear();
    macroCrossCommit_ = false;
    tempMacroOff_ = false;
    hadSynthInWord_ = false;
}

void HookEngine::CancelCommitUndo() {
    commitUndoState_ = CommitUndoState::Idle;
    pendingTriggerCount_ = 0;
    commitStack_.clear();
}

void HookEngine::SetCommitUndoReady() {
    commitUndoState_ = CommitUndoState::Ready;
    pendingTriggerCount_ = 0;
    commitReadyTime_ = GetTickCount();
}

// ═══════════════════════════════════════════════════════════
// Backspace-into-committed-word: replay saved chars from stack
// ═══════════════════════════════════════════════════════════

void HookEngine::ReplayCommittedChars() {
    if (commitStack_.empty()) {
        HOOK_LOG(L"  ReplayCommittedChars: stack empty, nothing to replay");
        commitUndoState_ = CommitUndoState::Idle;
        return;
    }

    // Pop the most recently committed word from the stack
    CommitEntry entry = std::move(commitStack_.back());
    commitStack_.pop_back();

    HOOK_LOG(L"  ReplayCommittedChars: replaying %zu keystrokes, restoring prev='%s' (stack=%zu remaining)",
             entry.history.size(), entry.text.c_str(), commitStack_.size());

    // Replay exact user keystrokes (including backspaces) to reproduce engine state
    for (wchar_t ch : entry.history) {
        if (ch == kBackspaceMarker) {
            engine_->Backspace();
        } else {
            engine_->PushChar(ch);
        }
    }
    // Seed inputHistory_ with the replayed word's keystrokes so that if the user
    // edits and re-commits this word, the new stack entry contains the full history
    // (not just the editing delta). Otherwise a second replay attempt would be wrong.
    inputHistory_ = std::move(entry.history);

    // Restore screen state so ReplaceComposition can diff correctly
    previousComposition_ = std::move(entry.text);
    previousEncodedWidths_ = std::move(entry.widths);

    // Reset undo state — HandleBackspace will re-enter state 1 if engine becomes
    // empty again and stack still has entries (enabling multi-word backward).
    commitUndoState_ = CommitUndoState::Idle;
}

// ═══════════════════════════════════════════════════════════
// Output — Universal SendInput with KEYEVENTF_UNICODE
// ═══════════════════════════════════════════════════════════

/// Get the focused child window that actually receives input
static HWND GetInputTarget() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
        return gti.hwndFocus;
    }
    return fg;
}

// ═══════════════════════════════════════════════════════════
// SendInput Event Helpers
// ═══════════════════════════════════════════════════════════

// Clipboard paste threshold: macros longer than this use Ctrl+V instead of SendInput
static constexpr size_t kMacroClipboardThreshold = 200;

static void AppendUnicodeEvent(std::vector<INPUT>& events, WORD wScan) {
    INPUT inDown = {};
    inDown.type = INPUT_KEYBOARD;
    inDown.ki.wScan = wScan;
    inDown.ki.dwFlags = KEYEVENTF_UNICODE;
    inDown.ki.dwExtraInfo = HookEngine::NEXUSKEY_EXTRA_INFO;
    events.push_back(inDown);

    INPUT inUp = inDown;
    inUp.ki.dwFlags |= KEYEVENTF_KEYUP;
    events.push_back(inUp);
}

static void AppendVkEvent(std::vector<INPUT>& events, WORD wVk, WORD wScan) {
    INPUT inDown = {};
    inDown.type = INPUT_KEYBOARD;
    inDown.ki.wVk = wVk;
    inDown.ki.wScan = wScan;
    inDown.ki.dwExtraInfo = HookEngine::NEXUSKEY_EXTRA_INFO;
    events.push_back(inDown);

    INPUT inUp = inDown;
    inUp.ki.dwFlags |= KEYEVENTF_KEYUP;
    events.push_back(inUp);
}

/// Send INPUT events via SendInput with correct synthEventsPending_ tracking.
/// Counter is pre-incremented BEFORE SendInput so the hook callback (which fires
/// synchronously during SendInput) can decrement it correctly.  Without this,
/// the counter inflates permanently — see commit message for full explanation.
/// Caller must set sending_=true before and false after (or wrap multiple calls).
void HookEngine::TrackedSendInput(INPUT* events, UINT count) noexcept {
    synthEventsPending_ += static_cast<int>(count);
    UINT sent = SendInput(count, events, sizeof(INPUT));
    if (sent < count)
        synthEventsPending_ -= static_cast<int>(count - sent);
}

void HookEngine::SendBackspaceEvents(size_t count) {
    WORD bsScan = static_cast<WORD>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));
    std::vector<INPUT> events;
    events.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        AppendVkEvent(events, VK_BACK, bsScan);
    }
    sending_ = true;
    TrackedSendInput(events.data(), static_cast<UINT>(events.size()));
    sending_ = false;
    RecordSynthDispatch();
}

void HookEngine::SendCharEvents(const std::wstring& text) {
    std::vector<INPUT> events;
    events.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        AppendUnicodeEvent(events, ch);
    }
    sending_ = true;
    TrackedSendInput(events.data(), static_cast<UINT>(events.size()));
    sending_ = false;
    RecordSynthDispatch();
}

bool HookEngine::ShouldUseClipboard() const noexcept {
    if (currentCodeTable_ != CodeTable::Unicode) return false;
    return useClipboardPaste_;
}

/// Write Unicode text to clipboard. Returns false on any failure.
/// Sets ExcludeClipboardContentFromMonitorProcessing to keep Win+V clean.
static bool SetClipboardText(const std::wstring& text) noexcept {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) { CloseClipboard(); return false; }
    auto* dest = static_cast<wchar_t*>(GlobalLock(hMem));
    if (!dest) { GlobalFree(hMem); CloseClipboard(); return false; }
    memcpy(dest, text.c_str(), bytes);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);

    // Exclude from Windows Clipboard History (Win+V) and cloud sync.
    // Win10 1809+; harmless no-op on older builds.
    static UINT cfExclude = RegisterClipboardFormat(
        L"ExcludeClipboardContentFromMonitorProcessing");
    if (cfExclude) {
        HGLOBAL hExclude = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hExclude) {
            auto* p = static_cast<DWORD*>(GlobalLock(hExclude));
            if (p) { *p = 0; GlobalUnlock(hExclude); }
            SetClipboardData(cfExclude, hExclude);
        }
    }

    CloseClipboard();
    return true;
}

void HookEngine::ClipboardPaste(const std::wstring& text) {
    if (text.empty()) return;

    if (!SetClipboardText(text)) {
        HOOK_LOG(L"  ClipboardPaste: clipboard failed, fallback to SendInput");
        SendCharEvents(text);
        return;
    }

    // Release held modifiers to prevent Ctrl+Shift+V / Ctrl+Alt+V.
    // Scenario: user triggers macro with '!' (Shift+1) — Shift still held.
    struct ModRelease { WORD vk; WORD scan; bool wasDown; };
    ModRelease mods[] = {
        { VK_SHIFT, static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_SHIFT) & 0x8000) != 0 },
        { VK_MENU,  static_cast<WORD>(MapVirtualKeyW(VK_MENU, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_MENU) & 0x8000) != 0 },
        { VK_LWIN,  static_cast<WORD>(MapVirtualKeyW(VK_LWIN, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_LWIN) & 0x8000) != 0 },
        { VK_RWIN,  static_cast<WORD>(MapVirtualKeyW(VK_RWIN, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_RWIN) & 0x8000) != 0 },
    };

    std::vector<INPUT> preEvents;
    std::vector<INPUT> postEvents;
    for (auto& m : mods) {
        if (m.wasDown) {
            INPUT up{};
            up.type = INPUT_KEYBOARD;
            up.ki.wVk = m.vk;
            up.ki.wScan = m.scan;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            up.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
            preEvents.push_back(up);

            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wVk = m.vk;
            down.ki.wScan = m.scan;
            down.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
            postEvents.push_back(down);
        }
    }

    // Simulate Ctrl+V — hook proc passes these through (NEXUSKEY_EXTRA_INFO marker)
    WORD ctrlScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    WORD vScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
    INPUT inputs[4] = {};
    for (auto& in : inputs) {
        in.type = INPUT_KEYBOARD;
        in.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
    }
    inputs[0].ki.wVk = VK_CONTROL;  inputs[0].ki.wScan = ctrlScan;
    inputs[1].ki.wVk = 'V';         inputs[1].ki.wScan = vScan;
    inputs[2].ki.wVk = 'V';         inputs[2].ki.wScan = vScan;     inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = VK_CONTROL;  inputs[3].ki.wScan = ctrlScan;  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    sending_ = true;
    if (!preEvents.empty()) {
        TrackedSendInput(preEvents.data(), static_cast<UINT>(preEvents.size()));
    }
    TrackedSendInput(inputs, 4);
    if (!postEvents.empty()) {
        TrackedSendInput(postEvents.data(), static_cast<UINT>(postEvents.size()));
    }
    sending_ = false;
    RecordSynthDispatch();

    HOOK_LOG(L"  ClipboardPaste: pasted %zu chars via Ctrl+V (mod-release: %zu)",
             text.size(), preEvents.size());
}

// ═══════════════════════════════════════════════════════════
// EM_REPLACESEL direct-paste — primary VB6/ANSI path (issue #94)
// ═══════════════════════════════════════════════════════════
//
// Sends text directly into the focused Edit control via EM_REPLACESEL — no
// clipboard, no SendInput. Preserves undo stack (wParam=TRUE).
//
// All SendMessage calls use SMTO_ABORTIFHUNG with a 50ms timeout — the
// keyboard hook must never block: a hung target app would otherwise freeze
// every keystroke system-wide.

namespace {
constexpr size_t kMaxClassName = 64;
constexpr UINT   kEditMsgTimeoutMs = 50;

// VB6 (ThunderRT6*) apps are the whole reason this path exists — check first.
// _wcsnicmp is case-insensitive so "RichEdit" matches "RICHEDIT60W" too.
bool IsEditCompatibleClass(const wchar_t* cls) noexcept {
    if (!cls || !*cls) return false;
    if (_wcsnicmp(cls, L"ThunderRT6TextBox", 17) == 0) return true;
    if (_wcsnicmp(cls, L"ThunderRT6RichText", 18) == 0) return true;
    if (_wcsicmp(cls, L"Edit") == 0) return true;
    if (_wcsnicmp(cls, L"RichEdit", 8) == 0) return true;
    return false;
}
}  // namespace

bool HookEngine::TryEditMessagePaste(const std::wstring& text, size_t backspaceCount) noexcept {
    if (text.empty() && backspaceCount == 0) return true;

    // Prefer cached focused HWND (populated in OnFocusChanged / invalidated on mouse click)
    // to avoid AttachThreadInput on every keystroke. Fall back to a fresh query on miss.
    HWND hwnd = cachedFocusedHwnd_;
    if (!hwnd || !IsWindow(hwnd)) {
        RefreshFocusCache(GetForegroundWindow());
        hwnd = cachedFocusedHwnd_;
        if (!hwnd) {
            HOOK_LOG(L"  EditMsgPaste: no focused child hwnd");
            return false;
        }
    }

    const wchar_t* cls = cachedFocusedClass_.c_str();
    if (!IsEditCompatibleClass(cls)) {
        HOOK_LOG(L"  EditMsgPaste: incompatible class='%s'", cls);
        return false;
    }

    constexpr UINT kFlags = SMTO_ABORTIFHUNG | SMTO_NORMAL;
    DWORD_PTR dummy = 0;
    DWORD newStart = 0, selEnd = 0;

    // EM_GETSEL is a query — safe to call before we suppress redraw below.
    if (backspaceCount > 0) {
        DWORD selStart = 0;
        if (!SendMessageTimeoutW(hwnd, EM_GETSEL,
                                 reinterpret_cast<WPARAM>(&selStart),
                                 reinterpret_cast<LPARAM>(&selEnd),
                                 kFlags, kEditMsgTimeoutMs, &dummy)) {
            HOOK_LOG(L"  EditMsgPaste: EM_GETSEL timed out (class='%s')", cls);
            return false;
        }
        if (static_cast<DWORD>(backspaceCount) > selEnd) {
            HOOK_LOG(L"  EditMsgPaste: BS=%zu > caret=%u (class='%s')",
                     backspaceCount, selEnd, cls);
            return false;
        }
        newStart = selEnd - static_cast<DWORD>(backspaceCount);
    }

    // Suppress repaint between EM_SETSEL (highlights selection) and EM_REPLACESEL —
    // otherwise the selection renders as a blue flash before being replaced.
    // Re-enable + InvalidateRect at the end to paint the final text once.
    // erase=FALSE: text controls paint their own background in WM_PAINT — TRUE would
    // cause a brief background-color flash before the text redraws on top.
    // Only re-enable if suppression actually took effect; if the FALSE send timed out
    // the control never entered no-redraw state, so skip the (redundant) TRUE send.
    bool redrawSuppressed = false;
    if (backspaceCount > 0) {
        redrawSuppressed = SendMessageTimeoutW(hwnd, WM_SETREDRAW, FALSE, 0,
                                               kFlags, kEditMsgTimeoutMs, &dummy) != 0;
        if (!SendMessageTimeoutW(hwnd, EM_SETSEL,
                                 static_cast<WPARAM>(newStart),
                                 static_cast<LPARAM>(selEnd),
                                 kFlags, kEditMsgTimeoutMs, &dummy)) {
            HOOK_LOG(L"  EditMsgPaste: EM_SETSEL timed out (class='%s')", cls);
            if (redrawSuppressed) {
                SendMessageTimeoutW(hwnd, WM_SETREDRAW, TRUE, 0,
                                    kFlags, kEditMsgTimeoutMs, &dummy);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return false;
        }
    }

    // wParam=TRUE → operation goes on the undo stack (Ctrl+Z works).
    BOOL replaceOk = SendMessageTimeoutW(hwnd, EM_REPLACESEL,
                                         static_cast<WPARAM>(TRUE),
                                         reinterpret_cast<LPARAM>(text.c_str()),
                                         kFlags, kEditMsgTimeoutMs, &dummy) != 0;

    if (redrawSuppressed) {
        SendMessageTimeoutW(hwnd, WM_SETREDRAW, TRUE, 0,
                            kFlags, kEditMsgTimeoutMs, &dummy);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    if (!replaceOk) {
        HOOK_LOG(L"  EditMsgPaste: EM_REPLACESEL timed out (class='%s')", cls);
        return false;
    }

    HOOK_LOG(L"  EditMsgPaste: class='%s' sel=[%u,%u] BS=%zu text='%s' OK",
             cls, newStart, selEnd, backspaceCount, text.c_str());
    return true;
}

/// Dispatch backspace + character events via SendInput.
/// Handles split (Electron/Console) vs batch (Win32) strategy in one place.
/// NOTE: batch path appends charEvents into bsEvents — callers must not reuse after calling.
void HookEngine::RecordSynthDispatch() noexcept {
    DWORD now = GetTickCount();
    lastSynthSendTime_ = now;
    lastRealSynthTime_ = now;
}

void HookEngine::DispatchSendInput(std::vector<INPUT>& bsEvents, std::vector<INPUT>& charEvents) {
    sending_ = true;
    if (isElectronApp_ || isConsoleApp_) {
        // Split: VK_BACK and VK_PACKET travel on separate internal paths in
        // Electron/Console apps — batching risks out-of-order processing ("nuốt chữ").
        // Other skipEmptyChar_ apps (Zed) are single-process — batch is fine.
        if (!bsEvents.empty()) {
            TrackedSendInput(bsEvents.data(), static_cast<UINT>(bsEvents.size()));
            // Gap so app finishes processing BS before receiving chars.
            // timeBeginPeriod(1) in main.cpp makes Sleep(N) actually ~N ms.
            // Base: 6ms Electron (multi-process IPC), 5ms Console (Node.js apps).
            // +1ms per extra BS pair: more deletions = more processing time.
            // Cap at 12ms — reduced from 20ms after profiling showed lower values work.
            int baseMs = isElectronApp_ ? 6 : 5;
            int bsCount = static_cast<int>(bsEvents.size()) / 2;  // each BS = down+up pair
            int delayMs = (std::min)(baseMs + (bsCount > 1 ? bsCount - 1 : 0), 12);
            Sleep(delayMs);
        }
        if (!charEvents.empty()) {
            TrackedSendInput(charEvents.data(), static_cast<UINT>(charEvents.size()));
        }
    } else {
        // Batch: standard Win32 GUI apps have single message queue (FIFO).
        bsEvents.insert(bsEvents.end(), charEvents.begin(), charEvents.end());
        if (!bsEvents.empty()) {
            TrackedSendInput(bsEvents.data(), static_cast<UINT>(bsEvents.size()));
        }
    }
    sending_ = false;
    RecordSynthDispatch();
}

/// Check if a filename (without path) is a known Electron app executable.
/// Electron apps use Chrome_WidgetWin window class (same as Chromium browsers).
/// Unknown Chrome_WidgetWin apps default to "browser" — safer because:
///   - Browser miss → double text (visible, user reports immediately)
///   - Electron miss → slightly slower input (split delay absent, usually OK)
/// This list covers the most popular Electron apps. Add new ones as needed.
static bool IsKnownElectronExe(const wchar_t* filename) noexcept {
    return _wcsnicmp(filename, L"code", 4) == 0 ||       // VS Code
           _wcsnicmp(filename, L"cursor", 6) == 0 ||     // Cursor (AI code editor)
           _wcsnicmp(filename, L"discord", 7) == 0 ||    // Discord
           _wcsnicmp(filename, L"slack", 5) == 0 ||      // Slack
           _wcsnicmp(filename, L"notion", 6) == 0 ||     // Notion
           _wcsnicmp(filename, L"obsidian", 8) == 0 ||   // Obsidian
           _wcsnicmp(filename, L"figma", 5) == 0 ||      // Figma
           _wcsnicmp(filename, L"postman", 7) == 0 ||    // Postman
           _wcsnicmp(filename, L"insomnia", 8) == 0 ||   // Insomnia
           _wcsnicmp(filename, L"signal", 6) == 0 ||     // Signal
           _wcsnicmp(filename, L"1password", 9) == 0 ||  // 1Password
           _wcsnicmp(filename, L"bitwarden", 9) == 0 ||  // Bitwarden
           _wcsnicmp(filename, L"gitkraken", 9) == 0 ||  // GitKraken
           _wcsnicmp(filename, L"hyper", 5) == 0 ||      // Hyper terminal
           _wcsnicmp(filename, L"spotify", 7) == 0 ||    // Spotify
           _wcsnicmp(filename, L"whatsapp", 8) == 0 ||   // WhatsApp Desktop
           _wcsnicmp(filename, L"telegram", 8) == 0 ||   // Telegram (some forks are Electron; native is Qt, caught earlier)
           _wcsnicmp(filename, L"logseq", 6) == 0 ||     // Logseq
           _wcsnicmp(filename, L"linear", 6) == 0 ||     // Linear
           _wcsnicmp(filename, L"lark", 4) == 0 ||       // Lark/Feishu
           _wcsnicmp(filename, L"zalo", 4) == 0;         // Zalo PC
}

// ── Auto-detect WebView2 apps via process inspection ──────────────────
// Tauri apps (Dorion), Office WebView2 add-ins, and any Win32 app that
// hosts a WebView2 control needs the Electron input treatment (skip
// reinjectVk + split dispatch) — Chromium's untrusted-input filter drops
// the unpaired synthetic VK keydown under some conditions.
//
// Two-pass detection:
//   1. Module check: Win32 apps that load WebView2 inline into the host
//      process will have `WebView2Loader.dll` or `embeddedbrowserwebview.dll`
//      loaded. Cheap (~3ms) and catches most hybrid Win32 apps.
//   2. Child-process check: Tauri v2 + modern WebView2 runtime isolate
//      the browser into `msedgewebview2.exe` — spawned as a child of the
//      host. The host itself may not load any WebView2 DLL. Walk the
//      process table and look for a child with that exe name. Slower
//      (~5-10ms over ~200 processes), so we only run it as a fallback.
//
// Cache strategy: positive-only. Both checks race with WebView2 runtime
// initialization (Tauri delay-loads on first embed), so a false at app-
// launch time must not poison subsequent checks.
//
// REQUIRES: caller holds stateMutex_ (all call sites go through OnFocusChanged
// which is always under lock). webView2PositiveCache_ is a plain member and is
// not independently thread-safe.
[[nodiscard]] bool HookEngine::IsWebView2App(HWND topLevel, const std::wstring& exeFullPath) noexcept {
    if (!topLevel || exeFullPath.empty()) return false;

    if (webView2PositiveCache_.count(exeFullPath)) return true;

    DWORD pid = 0;
    GetWindowThreadProcessId(topLevel, &pid);
    if (!pid) return false;

    bool found = false;

    // Pass 1 — loaded modules in the host process.
    // TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32 covers both native + WoW64.
    // INVALID_HANDLE_VALUE on cross-IL / AppContainer targets → fall through.
    if (HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        modSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        for (BOOL ok = Module32FirstW(modSnap, &me); ok; ok = Module32NextW(modSnap, &me)) {
            if (_wcsicmp(me.szModule, L"WebView2Loader.dll") == 0 ||
                _wcsicmp(me.szModule, L"embeddedbrowserwebview.dll") == 0) {
                found = true;
                break;
            }
        }
        CloseHandle(modSnap);
    }

    // Pass 2 — `msedgewebview2.exe` spawned as a child process.
    // Covers modern Tauri where the host doesn't load WebView2 DLLs itself.
    if (!found) {
        if (HANDLE procSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            procSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            for (BOOL ok = Process32FirstW(procSnap, &pe); ok; ok = Process32NextW(procSnap, &pe)) {
                if (pe.th32ParentProcessID == pid &&
                    _wcsicmp(pe.szExeFile, L"msedgewebview2.exe") == 0) {
                    found = true;
                    break;
                }
            }
            CloseHandle(procSnap);
        }
    }

    if (found) webView2PositiveCache_.insert(exeFullPath);
    return found;
}

// ── Auto-detect Electron by app.asar marker (FUTURE USE) ──────────────
// Uncomment to replace IsKnownElectronExe() with zero-maintenance detection.
// Checks if resources/app.asar exists next to the exe — all Electron apps ship this.
// Performance: GetFileAttributesW is metadata-only (~0.05ms SSD), cached per exe path.
// Risk: network drives can timeout (30s). Guard with GetDriveTypeW before using.
//
// #include <unordered_map>
//
// static bool IsElectronByMarker(const wchar_t* exeFullPath) {
//     // Cache: one check per unique exe path, forever (exe won't change at runtime)
//     static std::unordered_map<std::wstring, bool> cache;
//     auto it = cache.find(exeFullPath);
//     if (it != cache.end()) return it->second;
//
//     // Guard: skip network/removable drives (GetFileAttributesW can timeout 30s)
//     if (exeFullPath[0] == L'\\' && exeFullPath[1] == L'\\') {
//         cache[exeFullPath] = true;  // UNC path — assume Electron (safe default)
//         return true;
//     }
//     wchar_t drive[4] = { exeFullPath[0], L':', L'\\', L'\0' };
//     UINT driveType = GetDriveTypeW(drive);
//     if (driveType != DRIVE_FIXED && driveType != DRIVE_RAMDISK) {
//         cache[exeFullPath] = true;  // Non-fixed drive — assume Electron
//         return true;
//     }
//
//     // Local fixed drive: safe to check file system
//     const wchar_t* lastSlash = wcsrchr(exeFullPath, L'\\');
//     if (!lastSlash) { cache[exeFullPath] = false; return false; }
//     std::wstring dir(exeFullPath, lastSlash);
//     std::wstring asarPath = dir + L"\\resources\\app.asar";
//     bool isElectron = (GetFileAttributesW(asarPath.c_str()) != INVALID_FILE_ATTRIBUTES);
//     cache[exeFullPath] = isElectron;
//     return isElectron;
// }
// Usage in ClassifyWindow: replace IsKnownElectronExe(exeName.c_str()) with
// IsElectronByMarker(exeFullPath) — requires GetExePathForHwnd() returning full path.

bool HookEngine::IsTrayOrTaskbarWindow(HWND hwnd) noexcept {
    if (!hwnd) return false;

    // Ignore focus switches to our own process (Settings, Menu, Tray)
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == GetCurrentProcessId()) {
        return true;
    }

    // GetAncestor is a no-op when hwnd is already a root (e.g. from GetForegroundWindow),
    // but needed when called with a child HWND (e.g. from WindowFromPoint).
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||            // main taskbar
           _wcsicmp(cls, L"TrayNotifyWnd") == 0 ||            // notification area
           _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0 ||  // overflow (^) Win 10
           _wcsicmp(cls, L"TopLevelWindowForOverflowTray") == 0 || // overflow (^) Win 11
           _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||   // secondary taskbar
           _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0 || // Win 11 tray popups (volume, network)
           _wcsicmp(cls, L"#32768") == 0 ||                   // standard popup menu (right-click tray apps)
           _wcsicmp(cls, L"MSTaskSwWClass") == 0 ||            // taskbar app buttons
           _wcsicmp(cls, L"Start") == 0 ||                     // Start button
           _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0 || // Start Menu / Action Center (Win 10/11)
           _wcsicmp(cls, L"NexusKeyTrayClass") == 0;           // NexusKey own tray window
           // Note: SetForegroundWindow(hwndMessage_) in ShowContextMenu fires
           // EVENT_SYSTEM_FOREGROUND synchronously, but WinEventProc is WINEVENT_OUTOFCONTEXT
           // so it's delivered asynchronously — this filter still catches it correctly.
}

std::wstring HookEngine::GetExeNameForHwnd(HWND hwnd) noexcept {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        const wchar_t* filename = wcsrchr(exePath, L'\\');
        result = ToLowerAscii(filename ? filename + 1 : exePath);
    }
    CloseHandle(hProc);
    return result;
}

// Returns the full exe path (original case) for the process owning `hwnd`.
// Empty on failure. Needed by IsWebView2App() to locate sibling DLLs.
[[nodiscard]] static std::wstring GetExeFullPathForHwnd(HWND hwnd) noexcept {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        result.assign(exePath, size);
    }
    CloseHandle(hProc);
    return result;
}

/// Classify window into app type — called from OnFocusChanged().
/// Reads window class ONCE and determines: Browser, Electron, Qt, Console, or Normal.
/// Results are written directly to the caller's output variables.
///
/// Priority order:
///   1. Console (by window class: ConsoleWindowClass, CASCADIA, mintty, PuTTY)
///   2. Firefox-based browser (by window class: MozillaWindowClass)
///   3. Chrome_WidgetWin → Known Electron list or default to browser
///   4. Qt app (by window class: Qt5*, Qt6*, QWidget)
///   5. VB6 app (by window class: ThunderRT6*) — needs clipboard paste
///   6. Normal Win32 app
static void ClassifyWindow(HWND hwnd,
                           bool& outIsBrowser,
                           bool& outIsElectron,
                           bool& outIsQtApp,
                           bool& outIsConsole,
                           bool& outIsVB6) noexcept {
    outIsBrowser = outIsElectron = outIsQtApp = outIsConsole = outIsVB6 = false;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;

    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, 64);

    // 1a. Windows Terminal — modern DirectX renderer + ConPTY, handles batch input fine.
    //     Treated as normal app (no flags set, batch dispatch, no bait).
    if (_wcsicmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) {
        return;  // No flags set → batch path
    }

    // 1b. Legacy console apps — outIsConsole triggers split dispatch in DispatchSendInput
    if (_wcsicmp(className, L"ConsoleWindowClass") == 0 ||
        _wcsicmp(className, L"tty") == 0 ||                            // Cygwin/MSYS
        _wcsicmp(className, L"mintty") == 0 ||                         // Git Bash
        _wcsicmp(className, L"PuTTY") == 0) {
        outIsConsole = true;
        return;
    }

    // 2. Firefox-based browsers (covers Firefox, Floorp, Tor, LibreWolf, Waterfox, Pale Moon)
    if (_wcsicmp(className, L"MozillaWindowClass") == 0) {
        outIsBrowser = true;
        return;
    }

    // 3. Chrome_WidgetWin: Chromium browser OR Electron app
    //    Disambiguate by known Electron exe list. Unknown → browser (safer default).
    if (wcsstr(className, L"Chrome_WidgetWin")) {
        std::wstring exeName = HookEngine::GetExeNameForHwnd(hwnd);
        if (!exeName.empty() && IsKnownElectronExe(exeName.c_str())) {
            outIsElectron = true;
        } else {
            outIsBrowser = true;  // Unknown Chrome_WidgetWin → assume browser
        }
        return;
    }

    // 4. Qt apps (Telegram native, KeePassXC, etc.)
    if (wcsstr(className, L"Qt5") || wcsstr(className, L"Qt6") ||
        wcsstr(className, L"QWidget")) {
        outIsQtApp = true;
        return;
    }

    // 5. VB6 apps (XYplorer, etc.): register Unicode window classes but process
    //    messages as ANSI internally — KEYEVENTF_UNICODE / VK_PACKET chars become '?'.
    if (_wcsnicmp(className, L"ThunderRT6", 10) == 0) {
        outIsVB6 = true;
        return;
    }

    // 6. Normal Win32 app (Notepad, Word, etc.) — no flags set
}

void HookEngine::NotifyModeChange() noexcept {
    if (modeChangeCallback_) {
        // Excluded apps always show E mode (IME is transparent to them)
        modeChangeCallback_(!isExcludedApp_ && vietnameseMode_);
    }
}


bool HookEngine::VerifyExcludedState() {
    if (!excludeApps_ || excludedAppSet_.empty()) {
        isExcludedApp_ = false;
        return false;
    }
    HWND fg = GetForegroundWindow();
    std::wstring exe = GetExeNameForHwnd(fg);
    if (exe.empty() || excludedAppSet_.count(exe)) {
        return true;  // Still excluded (or can't determine — safe default)
    }
    isExcludedApp_ = false;
    HOOK_LOG(L"  ExcludeApps: stale flag cleared (fg='%s')", exe.c_str());
    return false;
}

void HookEngine::ReloadAppOverrides() {
    auto overrides = ConfigManager::LoadAppOverrides(ConfigManager::GetConfigPath());
    appEncodingOverrides_.clear();
    appInputMethodOverrides_.clear();
    for (auto& [exe, entry] : overrides) {
        if (entry.encodingOverride >= 0)
            appEncodingOverrides_[exe] = entry.encodingOverride;
        if (entry.inputMethod >= 0)
            appInputMethodOverrides_[exe] = entry.inputMethod;
    }
}

void HookEngine::ReloadExcludedApps() {
    excludedAppSet_.clear();
    if (excludeApps_) {
        for (auto& app : ConfigManager::LoadAllExcludedApps(ConfigManager::GetConfigPath()))
            excludedAppSet_.insert(std::move(app));
    } else {
        isExcludedApp_ = false;
    }
}

void HookEngine::ReloadTsfApps() {
    tsfAppSet_.clear();
    if (tsfApps_) {
        for (auto& app : ConfigManager::LoadTsfApps(ConfigManager::GetConfigPath()))
            tsfAppSet_.insert(std::move(app));
    }
}

void HookEngine::ReloadMacroTable() {
    // Keys are stored verbatim from TOML. The matching rule in TryExpandMacro
    // reads the stored case to decide behavior: all-lowercase keys match any
    // typed case; keys with any uppercase require an exact case match.
    macroTable_ = ConfigManager::LoadMacros(ConfigManager::GetConfigPath());
    // Index keys containing spaces so the save-before-commit path can cheaply
    // decide whether to preserve the macro buffer across a space commit.
    spaceMacroKeys_.clear();
    for (const auto& [key, _] : macroTable_) {
        if (key.find(L' ') != std::wstring::npos) spaceMacroKeys_.insert(key);
    }
}

void HookEngine::SaveEnglishModeAppsIfDirty() {
    if (!appModeDirty_ || !smartSwitch_) return;
    appModeDirty_ = false;

    std::vector<std::wstring> englishApps;
    for (const auto& [exe, isVietnamese] : appModeMap_) {
        if (!isVietnamese) {
            englishApps.push_back(exe);
        }
    }

    // Cap at shared memory limit
    if (englishApps.size() > kMaxSmartSwitchEntries) {
        englishApps.resize(kMaxSmartSwitchEntries);
    }

    (void)ConfigManager::SaveEnglishModeApps(ConfigManager::GetConfigPath(), englishApps);
    HOOK_LOG(L"  SaveEnglishModeApps: persisted %zu English-mode apps", englishApps.size());
}

void HookEngine::CheckLayoutChange() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    bool compatible = !IsIncompatibleLayout(GetKeyboardLayout(tid));
    if (compatible != cachedIsCompatLayout_) {
        cachedIsCompatLayout_ = compatible;
        OnLayoutChanged(compatible);
    }
}

void HookEngine::OnLayoutChanged(bool isCompatibleNow) {
    if (!isCompatibleNow && !layoutSuppressed_) {
        // Entering CJK layout: save mode, auto-switch to E
        if (engine_->Count() > 0) CommitComposition();
        CancelCommitUndo();
        layoutSuppressed_ = true;
        modeBeforeCjk_ = vietnameseMode_;
        if (vietnameseMode_) {
            vietnameseMode_ = false;
            NotifyModeChange();
            if (beepOnSwitch_) MessageBeep(MB_ICONASTERISK);
        }
        HOOK_LOG(L"  CJK layout: auto-switched to E (saved=%d)", modeBeforeCjk_ ? 1 : 0);
    } else if (isCompatibleNow && layoutSuppressed_) {
        // Leaving CJK layout: restore saved mode
        layoutSuppressed_ = false;
        if (modeBeforeCjk_ != vietnameseMode_) {
            vietnameseMode_ = modeBeforeCjk_;
            if (beepOnSwitch_) MessageBeep(vietnameseMode_ ? MB_OK : MB_ICONASTERISK);
        }
        NotifyModeChange();
        HOOK_LOG(L"  CJK layout cleared: restored mode=%d", vietnameseMode_ ? 1 : 0);
    }
}

void CALLBACK HookEngine::FocusPollTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    try {
        HookEngine* self = s_instance.load(std::memory_order_relaxed);
        if (!self) return;
        HWND fg = GetForegroundWindow();
        if (!fg) return;

        // Main-thread writer path (timer callback). Lock to serialize with
        // hook-thread reads; CheckLayoutChange + OnFocusChanged mutate state.
        std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);

        // Always check layout — catches mouse-click language bar switches (no PID change, no keystroke).
        // GetKeyboardLayout is kernel-cached, negligible cost at 200ms interval.
        self->CheckLayoutChange();

        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == self->lastForegroundPid_ || fgPid == 0) return;
        // Foreground PID changed but OnFocusChanged didn't catch it (missed or phantom).
        // Update PID first (prevents re-triggering if OnFocusChanged early-returns).
        self->lastForegroundPid_ = fgPid;
        HOOK_LOG(L"FOCUS poll — PID changed (new pid=%u), re-evaluating", fgPid);
        self->OnFocusChanged(nullptr);  // nullptr → uses GetForegroundWindow()
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::FocusPollTimerProc", e.what());
    } catch (...) {
        CrashLog(L"HookEngine::FocusPollTimerProc", "(non-std exception)");
    }
}

void CALLBACK HookEngine::DelayedUrlCheckTimerProc(HWND, UINT, UINT_PTR idEvent, DWORD) {
    try {
        HookEngine* self = s_instance.load(std::memory_order_relaxed);
        if (!self) return;

        // Take lock: CheckUrlBarFocus reads config_ and state flags, mutates mode.
        std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);

        KillTimer(nullptr, idEvent);
        self->urlFocusTimer_ = 0;
        
        // Check if the foreground window is still the browser we were tracking
        HWND fg = GetForegroundWindow();
        if (fg && self->needBaitChar_) {
            self->CheckUrlBarFocus(fg);
        }
    } catch (...) {
        // Best effort timer check
    }
}

void HookEngine::RefreshFocusCache(HWND foreground) noexcept {
    cachedFocusedHwnd_ = ::NextKey::GetFocusedChildHwnd(foreground);
    if (cachedFocusedHwnd_) {
        wchar_t cls[64] = {};
        GetClassNameW(cachedFocusedHwnd_, cls, 64);
        cachedFocusedClass_.assign(cls);
    } else {
        cachedFocusedClass_.clear();
    }
}

void HookEngine::CheckUrlBarFocus(HWND /*activeHwnd*/) {
    if (!autoOffByUrl_) return;

    HWND fg = GetForegroundWindow();
    if (!fg) return;
    
    bool isUrlBar = false;

    // Use UIAutomation to detect Chromium Omnibox (which is windowless)
    if (uia_) {
        IUIAutomationElement* focusedElement = nullptr;
        if (SUCCEEDED(uia_->GetFocusedElement(&focusedElement)) && focusedElement) {
            BSTR className = nullptr;
            if (SUCCEEDED(focusedElement->get_CurrentClassName(&className)) && className) {
                if (_wcsicmp(className, L"OmniboxViewViews") == 0 ||
                    _wcsicmp(className, L"URLEdit") == 0) { // Safari/others
                    isUrlBar = true;
                }
                SysFreeString(className);
            }
            focusedElement->Release();
        }
    }

    if (!isUrlBar) {
        // Fallback for old browsers/Firefox Edit controls
        DWORD tid = GetWindowThreadProcessId(fg, nullptr);
        GUITHREADINFO gti = { sizeof(gti) };
        if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
            wchar_t cls[64] = {};
            GetClassNameW(gti.hwndFocus, cls, 64);
            isUrlBar = (_wcsicmp(cls, L"Edit") == 0 && needBaitChar_);
        }
    }

    if (isUrlBar && !urlSuppressed_) {
        // Entering URL bar: save mode, switch to E
        if (engine_->Count() > 0) CommitComposition();
        CancelCommitUndo();
        urlSuppressed_ = true;
        modeBeforeUrl_ = vietnameseMode_;
        if (vietnameseMode_) {
            vietnameseMode_ = false;
            NotifyModeChange();
            if (beepOnSwitch_) MessageBeep(MB_ICONASTERISK);
        }
        HOOK_LOG(L"  URL bar focus: auto-switched to E (saved=%d)", modeBeforeUrl_ ? 1 : 0);
    }
}

void HookEngine::ClearUrlSuppression() noexcept {
    if (urlSuppressed_) {
        urlSuppressed_ = false;
        if (modeBeforeUrl_ != vietnameseMode_) {
            vietnameseMode_ = modeBeforeUrl_;
            if (beepOnSwitch_) MessageBeep(vietnameseMode_ ? MB_OK : MB_ICONASTERISK);
        }
        NotifyModeChange();
        HOOK_LOG(L"  URL bar focus cleared: restored mode=%d", vietnameseMode_ ? 1 : 0);
    }
}

void HookEngine::OnFocusChanged(HWND triggerHwnd) {
    ResetComposition();
    tempEngineOff_ = false;
    // Immediate checks on focus change — layout and config may differ in new app
    CheckLayoutChange();
    QuickSyncFromSharedState();  // Detects configGeneration changes + feature flag changes

    HWND fg = GetForegroundWindow();
    // Use triggerHwnd (the window that fired EVENT_SYSTEM_FOREGROUND) when available.
    // With WINEVENT_OUTOFCONTEXT, our callback is async — by the time it runs,
    // GetForegroundWindow() may return a transient window (e.g. JumpList/taskbar) instead
    // of the app the user is actually switching to. triggerHwnd is captured at event time.
    HWND activeHwnd = triggerHwnd ? triggerHwnd : fg;
    if (!activeHwnd) return;  // nothing to classify against

    // Determine whether this HWND should skip the smart-switch / currentExe_
    // update path (hidden helpers, tray, zero-size trick windows, tool windows).
    // Classification ITSELF runs unconditionally below so dispatch flags stay
    // consistent with the app identity — needed when focus transits through
    // a helper HWND while the user is typing into the main window.
    bool skipAppTracking = false;
    if (!IsWindowVisible(activeHwnd) || IsIconic(activeHwnd) || IsTrayOrTaskbarWindow(activeHwnd)) {
        skipAppTracking = true;
    } else {
        RECT rect;
        if (GetWindowRect(activeHwnd, &rect) &&
            (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0 || rect.left <= -20000)) {
            skipAppTracking = true;  // trick message-pump windows (IDM et al.)
        } else if (GetWindowLongW(activeHwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
            skipAppTracking = true;  // tooltips, context menus, floating helpers
        }
    }

    // Classify app type — single GetClassNameW call covers all detection.
    // Runs unconditionally so opening NexusKey BEFORE a WebView2 host (e.g.
    // Dorion) doesn't leave flags stale at the initial all-zero state when
    // focus first transits through a helper window — the next keystroke would
    // otherwise route through the wrong dispatch path.
    // U+202F (narrow no-break space) is a "bait" char inserted before BS sequences so
    // BS always has something to delete (prevents BS being swallowed at empty positions).
    //   - Browsers: need bait (autocomplete/address bar)
    //   - Electron/Console: skip bait + split dispatch with Sleep (IPC reorder prevention)
    //   - GPU-rendered (Zed): skip bait + batch dispatch (single-process, no flicker)
    //   - VB6 (XYplorer): clipboard paste (ANSI-internal, VK_PACKET → '?')
    bool isBrowser = false, isElectron = false, isQtApp = false, isVB6 = false;
    isConsoleApp_ = false;
    ClassifyWindow(activeHwnd, isBrowser, isElectron, isQtApp, isConsoleApp_, isVB6);

    skipEmptyChar_ = isElectron || isConsoleApp_;
    needBaitChar_ = isBrowser;
    useClipboardPaste_ = isVB6;
    useEditMsgPath_ = false;
    isOutlookApp_ = false;

    // Normal apps: check for GPU-rendered or apps needing bait (Excel, Outlook)
    bool isWebView2 = false;
    if (!skipEmptyChar_ && !needBaitChar_ && !useClipboardPaste_) {
        std::wstring exeName = GetExeNameForHwnd(activeHwnd);
        if (!exeName.empty()) {
            if (_wcsicmp(exeName.c_str(), L"zed.exe") == 0) {
                skipEmptyChar_ = true;
            } else if (_wcsicmp(exeName.c_str(), L"notepad.exe") == 0) {
                // Notepad (both classic Win32 Edit and Win11 WinUI 3 RichEditBox)
                // share exe name + root class "Notepad". The new one renders async on
                // the compositor thread and flashes suppressed keys with the SendInput
                // batch path; EM_REPLACESEL on the Edit/RichEdit child is atomic and
                // fixes the flicker. Classic Notepad benefits too: one undo entry per
                // transform instead of per BS + per char.
                useEditMsgPath_ = true;
            } else {
                // Outlook 2016 RichEdit has two orthogonal quirks, both derived from
                // the same detection: (1) needs a U+202F bait char before BS (same
                // as Excel), (2) drops the trailing char of a word when physical
                // Shift+letter precedes other chars — passthrough must be disabled
                // (issue #97). Single exe scan; both flags fall out.
                const bool isOutlook = exeName.find(L"outlook") != std::wstring::npos;
                isOutlookApp_ = isOutlook;
                needBaitChar_ = exeName.find(L"excel") != std::wstring::npos || isOutlook;

                // Tauri / WebView2-embedding apps (e.g. Dorion): detected by
                // scanning for `Chrome_WidgetWin*` descendants. WebView2 is fundamentally
                // Chromium, so it suffers from the same autocomplete/suggest bug as Chrome.
                // We MUST use the bait character (needBaitChar_ = true).
                if (!needBaitChar_) {
                    std::wstring exeFullPath = GetExeFullPathForHwnd(activeHwnd);
                    isWebView2 = IsWebView2App(activeHwnd, exeFullPath);
                    if (isWebView2) {
                        needBaitChar_ = true;
                        skipEmptyChar_ = false;
                    }
                }
            }
        }
    }

    isElectronApp_ = (isElectron || isWebView2) && !isConsoleApp_;
    HOOK_LOG(L"  AppDetect: console=%d skipEmpty=%d electron=%d webview2=%d bait=%d clipboard=%d editMsg=%d",
             isConsoleApp_ ? 1 : 0, skipEmptyChar_ ? 1 : 0, isElectronApp_ ? 1 : 0,
             isWebView2 ? 1 : 0, needBaitChar_ ? 1 : 0, useClipboardPaste_ ? 1 : 0, useEditMsgPath_ ? 1 : 0);

    // Re-install hooks to guarantee NexusKey remains at the top of the hook chain.
    // We only do this for Chromium-based architectures (Electron, WebView2, Browsers)
    // because they install their own WH_KEYBOARD_LL hooks that aggressively drop
    // synthetic injected events (like our Backspaces) if they sit in front of us.
    // Doing it conditionally avoids unnecessary unhook/rehook overhead for normal apps.
    // We must do this even if the PID hasn't changed, because WebView2 creates child
    // windows that trigger focus events AFTER the initial app launch and hook setup.
    if (hookThreadId_ && (isElectronApp_ || isBrowser)) {
        PostThreadMessageW(hookThreadId_, WM_APP_REINSTALL_HOOKS, 0, 0);
    }

    if (useClipboardPaste_ || useEditMsgPath_) {
        RefreshFocusCache(activeHwnd);
    } else {
        cachedFocusedHwnd_ = nullptr;
        cachedFocusedClass_.clear();
    }

    // Layout auto-disable: check CJK layout on every focus change
    CheckLayoutChange();

    // Skip the smart-switch / currentExe_ update path for helper HWNDs
    // (hidden, tray, zero-size, tool-window). Classification already ran so
    // dispatch flags stay correct — we just don't save SmartSwitch state or
    // bump currentExe_ to a helper's process.
    if (skipAppTracking) {
        // Focus poll timer (200ms) will catch missed transitions
        return;
    }

    // Skip focus tracking entirely if no feature needs it
    if (!smartSwitch_ && !excludeApps_ && !tsfApps_
        && appEncodingOverrides_.empty() && appInputMethodOverrides_.empty()
        && !autoOffByUrl_) return;

    bool wasExcluded = isExcludedApp_;
    bool wasTsfApp = isTsfApp_;

    // Save mode for previous app (smart switch, skip excluded/TSF apps)
    if (smartSwitch_ && !currentExe_.empty()
        && !wasExcluded && !wasTsfApp) {
        if (appModeMap_.size() >= kMaxSmartSwitchEntries) {
            appModeMap_.clear();
        }
        appModeMap_[currentExe_] = vietnameseMode_;
        smartSwitchMgr_.SetAppMode(currentExe_, vietnameseMode_);
        appModeDirty_ = true;
    }

    // Get new app (save previous for tray menu context).
    // Resolve into a local first — don't wipe currentExe_ if both paths fail,
    // so subsequent focus events still have a valid previousExe_.
    std::wstring newExe = GetExeNameForHwnd(activeHwnd);
    // Fallback: triggerHwnd may be stale (destroyed/inaccessible by the time async callback runs).
    // Try current foreground window instead.
    if (newExe.empty() && activeHwnd != fg && fg) {
        newExe = GetExeNameForHwnd(fg);
        HOOK_LOG(L"  GetExeNameForHwnd: triggerHwnd failed, fallback to fg → '%s'", newExe.c_str());
    }
    if (newExe.empty()) return;

    if (!currentExe_.empty()) {
        previousExe_ = currentExe_;
    }
    currentExe_ = std::move(newExe);

    if (previousExe_ != currentExe_) {
        ClearUrlSuppression();
    }

    // Sync PID tracker so focus poll timer won't re-trigger for this app
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(activeHwnd, &pid);
        if (pid) lastForegroundPid_ = pid;
    }

    // Check excluded apps
    if (excludeApps_ && !excludedAppSet_.empty()) {
        isExcludedApp_ = excludedAppSet_.count(currentExe_) > 0;
    } else {
        isExcludedApp_ = false;
    }

    // Check TSF apps (hook passthrough — let TSF DLL handle input)
    // Excluded apps take priority — if both, treat as excluded (force English)
    if (!isExcludedApp_ && tsfApps_ && !tsfAppSet_.empty()) {
        isTsfApp_ = tsfAppSet_.count(currentExe_) > 0;
    } else {
        isTsfApp_ = false;
    }

    // Always log active engine for this focus — makes it easy to tell which engine handles the app
    HOOK_LOG(L"  Engine: %s for '%s' (tsf_feature=%d, in_tsf_list=%d, excluded=%d)",
             isTsfApp_ ? L"TSF (hook passthrough)" : L"HOOK",
             currentExe_.c_str(),
             tsfApps_ ? 1 : 0,
             (!currentExe_.empty() && tsfAppSet_.count(currentExe_) > 0) ? 1 : 0,
             isExcludedApp_ ? 1 : 0);

    // Notify SharedState of TSF_ACTIVE + TSF_READONLY flags (DLL reads these).
    // Fired on every focus change (idempotent via SetOrClearFlag).
    if (tsfModeCallback_) {
        const bool tsfReadonly = !isTsfApp_ && !isExcludedApp_;
        if (isTsfApp_ != wasTsfApp) {
            HOOK_LOG(L"  TSF_ACTIVE flag: %s → %s",
                     wasTsfApp ? L"true" : L"false", isTsfApp_ ? L"true" : L"false");
        }
        tsfModeCallback_(isTsfApp_, tsfReadonly);
    }

    if (isExcludedApp_) {
        // Excluded app — IME is transparent. vietnameseMode_ is never touched.
        // Cache PID for fast per-keystroke check in ProcessKeyDown.
        DWORD pid = 0;
        GetWindowThreadProcessId(activeHwnd, &pid);
        excludedPid_ = pid;
        HOOK_LOG(L"  ExcludeApps: '%s' is excluded, passthrough (pid=%u)", currentExe_.c_str(), pid);
        if (!wasExcluded) {
            NotifyModeChange();  // Update icon to E (effective mode = false)
        }
        return;  // Skip smart switch restore and code table restore for excluded apps
    }

    // TSF app — hook is passive, skip smart switch/code table restore
    if (isTsfApp_) {
        HOOK_LOG(L"  TsfApps: '%s' uses TSF engine, hook passthrough", currentExe_.c_str());
        return;
    }

    // Manual encoding override per app (restore to global if no override)
    {
        auto it = appEncodingOverrides_.find(currentExe_);
        CodeTable targetTable = (it != appEncodingOverrides_.end() && it->second >= 0)
            ? static_cast<CodeTable>(it->second) : globalCodeTable_;
        if (targetTable != currentCodeTable_) {
            currentCodeTable_ = targetTable;
            HOOK_LOG(L"  AppOverride: encoding=%d for '%s'",
                     static_cast<int>(currentCodeTable_), currentExe_.c_str());
        }
    }

    // Manual input method override per app (restore to global if no override)
    {
        auto it = appInputMethodOverrides_.find(currentExe_);
        InputMethod targetMethod = (it != appInputMethodOverrides_.end() && it->second >= 0)
            ? static_cast<InputMethod>(it->second) : globalInputMethod_;
        if (targetMethod != currentMethod_) {
            currentMethod_ = targetMethod;
            TypingConfig engineConfig = config_;
            engineConfig.inputMethod = targetMethod;
            engine_ = EngineFactory::Create(engineConfig);
            HOOK_LOG(L"  AppOverride: inputMethod=%d for '%s'",
                     static_cast<int>(currentMethod_), currentExe_.c_str());
        }
    }

    // Smart switch: restore mode for new app.
    // Hidden/tray windows are already filtered by the early return above.
    if (smartSwitch_) {
        auto it = appModeMap_.find(currentExe_);
        if (it != appModeMap_.end()) {
            // Known app — restore its saved mode
            if (it->second != vietnameseMode_) {
                vietnameseMode_ = it->second;
                HOOK_LOG(L"  SmartSwitch: restored %s for '%s'",
                         vietnameseMode_ ? L"Vietnamese" : L"English", currentExe_.c_str());
                NotifyModeChange();
            }
        } else {
            // Unknown app — inherit current mode (least surprising to the user)
            HOOK_LOG(L"  SmartSwitch: inherit %s for unknown '%s'",
                     vietnameseMode_ ? L"Vietnamese" : L"English", currentExe_.c_str());
        }
    }

    // Idempotent if NotifyModeChange was already called above.
    if (wasExcluded) {
        NotifyModeChange();
    }

    // Check for URL bar focus if it's a browser
    if (isBrowser) {
        CheckUrlBarFocus(activeHwnd);
    } else if (urlSuppressed_) {
        // Switched to a non-browser app while URL was suppressed (edge case)
        urlSuppressed_ = false;
        if (modeBeforeUrl_ != vietnameseMode_) {
            vietnameseMode_ = modeBeforeUrl_;
            NotifyModeChange();
        }
    }
}

/// Replace on-screen text by diffing previousComposition_ vs newText.
///
/// ## U+202F "needEmpty" mechanism (skipEmptyChar_ == false)
///
/// Some Win32 apps swallow BS at certain cursor positions (start of line, empty
/// field, after autocomplete selection in browsers). To guarantee BS always
/// deletes something, we insert U+202F (NARROW NO-BREAK SPACE) as a "bait"
/// character before the BS sequence, then include one extra BS to remove it:
///
///   [insert U+202F] → [BS × (n+1)] → [type new chars]
///
/// U+202F is chosen because:
///   - It is a real Unicode character that apps must insert into the text buffer
///   - It is NOT U+0020 (regular space), so it doesn't trigger word commit
///   - It is narrow/invisible in most fonts, minimizing visual flicker
///
/// This mechanism is ONLY safe for apps that reliably insert U+202F into their
/// text buffer. Apps that ignore or filter it will receive n+1 BS for n chars,
/// deleting one extra character and permanently desyncing previousComposition_.
///
/// Apps with skipEmptyChar_=true (block reinjectVk, skip U+202F bait):
///   - Electron/Console: also get split dispatch (isElectronApp_/isConsoleApp_)
///   - GPU-rendered apps (Zed): batch dispatch (single-process, no IPC reorder)
///
/// See OnFocusChanged() for the detection logic.
void HookEngine::ReplaceComposition(const std::wstring& newText, DWORD reinjectVk) {
    HWND target = GetInputTarget();
    if (!target) {
        previousComposition_ = newText;
        return;
    }

    // Find common prefix at Unicode level — only replace what actually changed
    size_t commonLen = 0;
    size_t minLen = (std::min)(previousComposition_.size(), newText.size());
    while (commonLen < minLen && previousComposition_[commonLen] == newText[commonLen]) {
        commonLen++;
    }

    // ── Non-Unicode code table path ──
    if (currentCodeTable_ != CodeTable::Unicode) {
        // Calculate backspace count from encoded widths of chars being replaced
        size_t backspaceCount = 0;
        for (size_t i = commonLen; i < previousEncodedWidths_.size(); ++i) {
            backspaceCount += previousEncodedWidths_[i];
        }

        // Convert new chars to encoded form
        std::wstring encodedToSend;
        std::vector<uint8_t> newWidths;
        for (size_t i = commonLen; i < newText.size(); ++i) {
            auto enc = CodeTableConverter::ConvertChar(newText[i], currentCodeTable_);
            encodedToSend += enc.units[0];
            if (enc.count == 2) encodedToSend += enc.units[1];
            newWidths.push_back(enc.count);
        }

        HOOK_LOG(L"  ReplaceComposition[encoded]: prev='%s' new='%s' common=%zu BS=%zu encodedLen=%zu",
                 previousComposition_.c_str(), newText.c_str(), commonLen, backspaceCount,
                 encodedToSend.size());

        {
            bool needEmpty = (backspaceCount > 0 && needBaitChar_);
            size_t bsTotal = backspaceCount + (needEmpty ? 1 : 0);

            if (bsTotal > 0 || !encodedToSend.empty()) {
                std::vector<INPUT> bsEvents;
                std::vector<INPUT> charEvents;
                WORD bsScan = static_cast<WORD>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));

                if (bsTotal > 0) {
                    bsEvents.reserve(bsTotal * 2 + (needEmpty ? 2 : 0));
                    if (needEmpty) {
                        AppendUnicodeEvent(bsEvents, 0x202F);
                    }
                    for (size_t i = 0; i < bsTotal; ++i) {
                        AppendVkEvent(bsEvents, VK_BACK, bsScan);
                    }
                }

                if (!encodedToSend.empty()) {
                    charEvents.reserve(encodedToSend.size() * 2);
                    for (wchar_t ch : encodedToSend) {
                        AppendUnicodeEvent(charEvents, ch);
                    }
                }

                DispatchSendInput(bsEvents, charEvents);
            }
        }

        // Update widths: keep [0..commonLen), append newWidths
        previousEncodedWidths_.resize(commonLen);
        previousEncodedWidths_.insert(previousEncodedWidths_.end(), newWidths.begin(), newWidths.end());
        previousComposition_ = newText;
        return;
    }

    // ── Unicode path (fast path, zero overhead) ──
    size_t backspaceCount = previousComposition_.size() - commonLen;
    std::wstring toSend = newText.substr(commonLen);

    HOOK_LOG(L"  ReplaceComposition: prev='%s' new='%s' common=%zu BS=%zu send='%s'",
             previousComposition_.c_str(), newText.c_str(), commonLen, backspaceCount,
             toSend.c_str());

    // ── Async-render apps (Win11 new Notepad) ──
    // WinUI 3 RichEditBox renders on the compositor thread async to input. SendInput
    // BS+replace arrives a frame too late → suppressed key flashes before replacement.
    // EM_REPLACESEL goes straight into the RichEdit child synchronously → atomic.
    // Fall through to SendInput on failure (not clipboard — preserve user's clipboard).
    if (useEditMsgPath_) {
        if (TryEditMessagePaste(toSend, backspaceCount)) {
            previousComposition_ = newText;
            if (synthEventsPending_ > 0) hadSynthInWord_ = true;
            return;
        }
        HOOK_LOG(L"  ReplaceComposition[editMsg]: fallback to SendInput BS=%zu send='%s'",
                 backspaceCount, toSend.c_str());
    }

    // ── VB6 / ANSI-internal windows ──
    // ANSI windows can't handle KEYEVENTF_UNICODE (VK_PACKET) — Vietnamese chars become '?'.
    // Primary path: EM_REPLACESEL directly into the focused Edit/RichEdit/ThunderRT6
    // child (no clipboard side-effect). Fallback: clipboard paste when the focused
    // child isn't a compatible Edit control.
    //
    // When reinjectVk != 0: HandleAlphaKey appended originalCh to previousComposition_
    // for game-compat tracking, but the physical key was blocked and never reached the
    // ANSI window. Subtract 1 BS to compensate. Reinject VK itself is skipped — ANSI
    // desktop apps (XYplorer, etc.) don't need game-style VK re-injection.
    if (ShouldUseClipboard()) {
        size_t bsCount = backspaceCount;
        if (reinjectVk != 0 && bsCount > 0) bsCount--;

        if (TryEditMessagePaste(toSend, bsCount)) {
            previousComposition_ = newText;
            if (synthEventsPending_ > 0) hadSynthInWord_ = true;
            return;
        }

        HOOK_LOG(L"  ReplaceComposition[clipboard]: fallback BS=%zu (raw=%zu reinject=0x%X) send='%s'",
                 bsCount, backspaceCount, reinjectVk, toSend.c_str());
        if (bsCount > 0) {
            SendBackspaceEvents(bsCount);
            Sleep(8);  // Let app process deletions before clipboard paste
        }
        if (!toSend.empty()) {
            ClipboardPaste(toSend);
        }
        previousComposition_ = newText;
        if (synthEventsPending_ > 0) hadSynthInWord_ = true;
        return;
    }

    {
        // Bait char (U+202F): prevents apps with autofill from swallowing the first
        // U+202F bait: dismiss autocomplete suggestions before BS. Must fire on EVERY
        // transform (not just empty-field), because suggest is active at any word length.
        // U+202F has visible width → triggers suggest recalculation → dismiss.
        // Zero-width chars (U+200B) don't work — Chrome ignores them for suggest.
        bool needEmpty = (backspaceCount > 0 && needBaitChar_);
        if (needEmpty) backspaceCount++;

        HOOK_LOG(L"  ReplaceComposition[send]: BS=%zu needEmpty=%d toSend='%s' skipEmpty=%d synthPending=%d",
                 backspaceCount, needEmpty ? 1 : 0, toSend.c_str(), skipEmptyChar_ ? 1 : 0, synthEventsPending_.load());

        if (backspaceCount > 0 || !toSend.empty()) {
            // Stack-allocated events: Vietnamese words max ~8 chars, transforms touch 1-3.
            // Max: bait(2) + 8 BS(16) + 8 chars(16) = 34 INPUT structs. 48 is generous.
            static constexpr size_t kMaxEvents = 48;
            INPUT bsBuf[kMaxEvents];
            size_t bsCount = 0;
            INPUT charBuf[kMaxEvents];
            size_t charCount = 0;
            WORD bsScan = static_cast<WORD>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));

            auto appendUnicode = [](INPUT* buf, size_t& count, wchar_t ch) {
                if (count + 2 > kMaxEvents) return;
                INPUT& down = buf[count++];
                down = {};
                down.type = INPUT_KEYBOARD;
                down.ki.wScan = ch;
                down.ki.dwFlags = KEYEVENTF_UNICODE;
                down.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
                INPUT& up = buf[count++];
                up = {};
                up.type = INPUT_KEYBOARD;
                up.ki.wScan = ch;
                up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                up.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
            };

            auto appendVk = [](INPUT* buf, size_t& count, WORD vk, WORD scan) {
                if (count + 2 > kMaxEvents) return;
                INPUT& down = buf[count++];
                down = {};
                down.type = INPUT_KEYBOARD;
                down.ki.wVk = vk;
                down.ki.wScan = scan;
                down.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
                INPUT& up = buf[count++];
                up = {};
                up.type = INPUT_KEYBOARD;
                up.ki.wVk = vk;
                up.ki.wScan = scan;
                up.ki.dwFlags = KEYEVENTF_KEYUP;
                up.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
            };

            // Re-inject VK keydown FIRST in the batch (game compatibility).
            // Games see WM_KEYDOWN(VK_W) for movement; BS+chars follow in the same
            // SendInput call so the app processes everything in one pump cycle → no flicker.
            if (reinjectVk != 0 && bsCount + 1 <= kMaxEvents) {
                INPUT& evt = bsBuf[bsCount++];
                evt = {};
                evt.type = INPUT_KEYBOARD;
                evt.ki.wVk = static_cast<WORD>(reinjectVk);
                evt.ki.wScan = static_cast<WORD>(MapVirtualKeyW(reinjectVk, MAPVK_VK_TO_VSC));
                evt.ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
            }

            if (backspaceCount > 0) {
                if (needEmpty) {
                    appendUnicode(bsBuf, bsCount, 0x202F);
                }
                for (size_t i = 0; i < backspaceCount; ++i) {
                    appendVk(bsBuf, bsCount, VK_BACK, bsScan);
                }
            }

            for (wchar_t ch : toSend) {
                appendUnicode(charBuf, charCount, ch);
            }

            // Dispatch using stack buffers
            sending_ = true;
            if (isElectronApp_ || isConsoleApp_) {
                // Split only for Electron/Console (multi-process IPC reorder risk).
                if (bsCount > 0) {
                    TrackedSendInput(bsBuf, static_cast<UINT>(bsCount));
                    int baseMs = isElectronApp_ ? 6 : 5;
                    int bsKeys = static_cast<int>(bsCount) / 2;
                    int delayMs = (std::min)(baseMs + (bsKeys > 1 ? bsKeys - 1 : 0), 12);
                    Sleep(delayMs);
                }
                if (charCount > 0) {
                    TrackedSendInput(charBuf, static_cast<UINT>(charCount));
                }
            } else {
                // Batch: merge into bsBuf and send once
                if (charCount > 0 && bsCount + charCount <= kMaxEvents) {
                    memcpy(&bsBuf[bsCount], charBuf, charCount * sizeof(INPUT));
                    bsCount += charCount;
                }
                if (bsCount > 0) {
                    TrackedSendInput(bsBuf, static_cast<UINT>(bsCount));
                }
            }
            sending_ = false;
            RecordSynthDispatch();
        }
    }

    previousComposition_ = newText;
    // Mark that at least one synthetic event was sent for this word.
    // Guards passthrough path in HandleAlphaKey from mixing physical+synthetic events mid-word.
    if (synthEventsPending_ > 0) hadSynthInWord_ = true;
}

void HookEngine::SendBackspaces(size_t count) {
    HOOK_LOG(L"  SendBackspaces: %zu bait=%d", count, needBaitChar_ ? 1 : 0);
    if (needBaitChar_ && count > 0) {
        // Insert bait to dismiss autocomplete suggest before BS
        INPUT bait[2] = {};
        bait[0].type = INPUT_KEYBOARD;
        bait[0].ki.wScan = 0x202F;
        bait[0].ki.dwFlags = KEYEVENTF_UNICODE;
        bait[1].type = INPUT_KEYBOARD;
        bait[1].ki.wScan = 0x202F;
        bait[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        TrackedSendInput(bait, 2);
        count++;  // Extra BS to delete bait
    }
    SendBackspaceEvents(count);
}

// ═══════════════════════════════════════════════════════════
// Modifier Tracking — feeds double-Alt + layout-change detection
// ═══════════════════════════════════════════════════════════

void HookEngine::TrackModifier(DWORD vkCode, bool isDown) {
    switch (vkCode) {
        case VK_LCONTROL: case VK_RCONTROL:
            if (isDown && !modCtrlDown_) { modCtrlDown_ = true; otherKeyPressed_ = false; }
            else if (!isDown) modCtrlDown_ = false;
            break;
        case VK_LSHIFT: case VK_RSHIFT:
            if (isDown && !modShiftDown_) { modShiftDown_ = true; otherKeyPressed_ = false; }
            else if (!isDown) modShiftDown_ = false;
            break;
        case VK_LMENU: case VK_RMENU:
            if (isDown && !modAltDown_) { modAltDown_ = true; otherKeyPressed_ = false; }
            else if (!isDown) modAltDown_ = false;
            break;
        case VK_LWIN: case VK_RWIN:
            if (isDown && !modWinDown_) { modWinDown_ = true; otherKeyPressed_ = false; }
            else if (!isDown) modWinDown_ = false;
            break;
    }
}

// ═══════════════════════════════════════════════════════════
// Commit Trigger Check
// ═══════════════════════════════════════════════════════════

void HookEngine::InjectKey(DWORD vkCode) {
    WORD scan = static_cast<WORD>(MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC));
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(vkCode);
    inputs[0].ki.wScan = scan;
    inputs[0].ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = static_cast<WORD>(vkCode);
    inputs[1].ki.wScan = scan;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[1].ki.dwExtraInfo = NEXUSKEY_EXTRA_INFO;
    sending_ = true;
    TrackedSendInput(inputs, _countof(inputs));
    sending_ = false;
    lastSynthSendTime_ = GetTickCount();
}

bool HookEngine::IsCommitTrigger(DWORD vkCode) {
    // Space, Enter, Escape
    if (vkCode == VK_SPACE || vkCode == VK_RETURN || vkCode == VK_ESCAPE) return true;

    // Tab
    if (vkCode == VK_TAB) return true;

    // Arrow keys
    if (vkCode >= VK_LEFT && vkCode <= VK_DOWN) return true;
    if (vkCode == VK_HOME || vkCode == VK_END ||
        vkCode == VK_PRIOR || vkCode == VK_NEXT) return true;

    // Number keys (0-9)
    if (vkCode >= 0x30 && vkCode <= 0x39) return true;

    // Numpad keys
    if (vkCode >= VK_NUMPAD0 && vkCode <= VK_DIVIDE) return true;

    // OEM keys (punctuation)
    if (vkCode >= VK_OEM_1 && vkCode <= VK_OEM_3) return true;
    if (vkCode >= VK_OEM_4 && vkCode <= VK_OEM_8) return true;
    if (vkCode == VK_OEM_PLUS || vkCode == VK_OEM_COMMA ||
        vkCode == VK_OEM_MINUS || vkCode == VK_OEM_PERIOD) return true;

    // Delete, Insert
    if (vkCode == VK_DELETE || vkCode == VK_INSERT) return true;

    return false;
}

bool HookEngine::IsMacroTrigger(DWORD vkCode) const {
    // If not a commit trigger natively, it shouldn't trigger macro either
    if (!IsCommitTrigger(vkCode)) return false;

    if (vkCode == VK_SPACE) return config_.macroTriggerSpace;
    if (vkCode == VK_RETURN) return config_.macroTriggerEnter;
    if (vkCode == VK_TAB) return config_.macroTriggerTab;
    
    // Direction / Navigation
    if (vkCode >= VK_LEFT && vkCode <= VK_DOWN) return config_.macroTriggerDir;
    if (vkCode == VK_HOME || vkCode == VK_END ||
        vkCode == VK_PRIOR || vkCode == VK_NEXT) return config_.macroTriggerDir;
        
    return true; // Numbers, Punctuation, Esc, etc. default to true if they are commit triggers
}

HookEngine::MacroResult HookEngine::TryExpandMacro(wchar_t triggerChar) {
    std::wstring lowerKey = ToLowerAscii(rawMacroBuffer_);
    bool isPartOfMacro = false;
    bool matchedViaComposition = false;  // true when matched via Priority 3/4
    bool matchedExact = false;            // true when match used the typed case verbatim

    // Matching contract (raw-buffer priorities 1 and 2):
    //   - Stored key has any uppercase → exact case match only.
    //   - Stored key is all lowercase    → case-insensitive; typed case adapts.
    // Implemented with a two-step find: exact first, then lowered. An exact hit
    // can be strict (upper key + typed matches) or a natural match on a lowercase key.
    // A lowered-only hit can only land on a lowercase key (upper keys wouldn't match
    // a lowercase probe), which is exactly the flexible branch.

    // Priority 1: full buffer (raw typed, includes accumulated trigger char)
    auto it = macroTable_.find(rawMacroBuffer_);
    if (it != macroTable_.end()) {
        matchedExact = true;
    } else {
        it = macroTable_.find(lowerKey);
    }
    if (it != macroTable_.end() && triggerChar > L' ') {
        isPartOfMacro = true;
    }
    // Priority 2: buffer without trigger char (e.g., "btw" from "btw.")
    if (it == macroTable_.end() && triggerChar > L' ' &&
        lowerKey.size() > 1 && lowerKey.back() == triggerChar) {
        std::wstring rawWithoutTrigger = rawMacroBuffer_.substr(0, rawMacroBuffer_.size() - 1);
        it = macroTable_.find(rawWithoutTrigger);
        if (it != macroTable_.end()) {
            matchedExact = true;
        } else {
            it = macroTable_.find(lowerKey.substr(0, lowerKey.size() - 1));
        }
    }
    // Priority 3/4: composition-based matches stay case-insensitive only — the
    // "raw" here is engine-composed output, not the user's keystroke case, so
    // the strict-case contract above doesn't apply cleanly.
    if (it == macroTable_.end() && !previousComposition_.empty()) {
        std::wstring compKey = ToLowerAscii(previousComposition_);
        if (triggerChar > L' ') {
            it = macroTable_.find(compKey + triggerChar);
            if (it != macroTable_.end()) { isPartOfMacro = true; matchedViaComposition = true; }
        }
        if (it == macroTable_.end()) {
            it = macroTable_.find(compKey);
            if (it != macroTable_.end()) matchedViaComposition = true;
        }
    }
    if (it == macroTable_.end()) return MacroResult::NoMatch;

    // Backspace count = on-screen characters that need erasing.
    // When matchedViaComposition: only the last composed word (previousComposition_)
    // matched — don't erase characters from earlier words accumulated in rawMacroBuffer_.
    size_t bsCount;
    if (matchedViaComposition) {
        if (currentCodeTable_ != CodeTable::Unicode) {
            bsCount = 0;
            for (auto w : previousEncodedWidths_) bsCount += w;
        } else {
            bsCount = previousComposition_.size();
        }
    } else if (macroCrossCommit_) {
        // Cross-commit macro (key contains punctuation like "a.i"): rawMacroBuffer_
        // spans multiple engine commits and matches the on-screen character count.
        // NOTE: For non-Unicode code tables (TCVN3/VNI), Vietnamese characters may
        // encode to multiple bytes, making this count wrong. Acceptable limitation
        // since macros with punctuation + non-Unicode encoding is extremely rare.
        bsCount = rawMacroBuffer_.size();
        if (triggerChar > L' ' && bsCount > 0) --bsCount;
    } else if (!previousComposition_.empty()) {
        if (currentCodeTable_ != CodeTable::Unicode) {
            bsCount = 0;
            for (auto w : previousEncodedWidths_) bsCount += w;
        } else {
            bsCount = previousComposition_.size();
        }
    } else {
        bsCount = rawMacroBuffer_.size();
        if (triggerChar > L' ' && bsCount > 0) --bsCount;
    }
    // Auto-capitalize expansion to match typed case — only fires when BOTH:
    //   (a) the match was flexible (stored key all-lowercase, so case-insensitive
    //       matching was in play — !matchedExact && !matchedViaComposition), and
    //   (b) the stored expansion is itself all-lowercase (no deliberate casing).
    // Transform: typed all-upper → expansion all-upper; typed first-upper → first-upper.
    // CharUpperBuffW is locale-aware so Vietnamese diacritics uppercase correctly
    // ('ô' → 'Ô'), unlike towupper() which only handles ASCII under the C locale.
    std::wstring expansion = it->second;
    if (autoCapsMacro_ && !matchedExact && !matchedViaComposition &&
        !rawMacroBuffer_.empty() && !expansion.empty()) {
        // Expansion has no upper iff lowercasing it is a no-op (locale-aware).
        std::wstring expansionLower = expansion;
        CharLowerBuffW(expansionLower.data(), static_cast<DWORD>(expansionLower.size()));
        const bool expansionAllLower = (expansionLower == expansion);
        if (expansionAllLower) {
            // Inspect only the letter chars — rawMacroBuffer_ may include an
            // appended trigger (e.g. "BTW." when trigger is '.'), and
            // iswupper('.') is false and would wrongly defeat all-upper detection.
            // Note: iswupper/iswalpha are ASCII-only under the C locale, which is
            // fine here — rawMacroBuffer_ only accumulates VK 0x41-0x5A and
            // VkToMacroChar() triggers, both ASCII. The transform below uses
            // locale-aware CharUpperBuffW so non-ASCII expansion chars ('ô'→'Ô')
            // still uppercase correctly.
            bool allUpper = true;
            bool anyAlpha = false;
            for (auto c : rawMacroBuffer_) {
                if (!iswalpha(c)) continue;
                anyAlpha = true;
                if (!iswupper(c)) { allUpper = false; break; }
            }
            allUpper = allUpper && anyAlpha && rawMacroBuffer_.size() > 1;
            bool firstUpper = iswupper(rawMacroBuffer_[0]);
            if (allUpper) {
                // Skip \n escape sequences — uppercasing 'n' → 'N' breaks newline detection
                for (size_t i = 0; i < expansion.size(); ++i) {
                    if (expansion[i] == L'\\' && i + 1 < expansion.size() && expansion[i + 1] == L'n') {
                        ++i;  // skip the 'n' in '\n'
                    } else {
                        CharUpperBuffW(&expansion[i], 1);
                    }
                }
            } else if (firstUpper) {
                CharUpperBuffW(&expansion[0], 1);
            }
        }
    }

    {
        WORD bsScan = static_cast<WORD>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));
        std::vector<INPUT> bsEvents;

        bsEvents.reserve(bsCount * 2);
        for (size_t i = 0; i < bsCount; ++i) {
            AppendVkEvent(bsEvents, VK_BACK, bsScan);
        }

        // Choose output method based on encoding and expansion size.
        // Non-Unicode code tables need per-char conversion → always SendInput.
        // Unicode macros >200 chars use clipboard paste for speed.
        bool useClipboard = (currentCodeTable_ == CodeTable::Unicode &&
                             expansion.size() > kMacroClipboardThreshold);

        if (useClipboard) {
            // Convert \n escape sequences to real \r\n for clipboard paste
            std::wstring clipText;
            clipText.reserve(expansion.size());
            for (size_t i = 0; i < expansion.size(); ++i) {
                if (expansion[i] == L'\\' && i + 1 < expansion.size() &&
                    expansion[i + 1] == L'n') {
                    clipText += L"\r\n";
                    ++i;
                } else {
                    clipText += expansion[i];
                }
            }

            // Send backspaces first, then clipboard paste
            if (!bsEvents.empty()) {
                sending_ = true;
                TrackedSendInput(bsEvents.data(), static_cast<UINT>(bsEvents.size()));
                sending_ = false;
                RecordSynthDispatch();
            }
            ClipboardPaste(clipText);

            HOOK_LOG(L"  TryExpandMacro: clipboard paste %zu chars (raw %zu)",
                     clipText.size(), expansion.size());
        } else {
            // SendInput path: per-character with encoding support
            WORD retScan = static_cast<WORD>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
            std::vector<INPUT> charEvents;
            charEvents.reserve(expansion.size() * 2);
            auto emitChar = [&](wchar_t ch) {
                if (currentCodeTable_ != CodeTable::Unicode) {
                    auto enc = CodeTableConverter::ConvertChar(ch, currentCodeTable_);
                    AppendUnicodeEvent(charEvents, enc.units[0]);
                    if (enc.count == 2) AppendUnicodeEvent(charEvents, enc.units[1]);
                } else {
                    AppendUnicodeEvent(charEvents, ch);
                }
            };
            for (size_t i = 0; i < expansion.size(); ++i) {
                if (expansion[i] == L'\\' && i + 1 < expansion.size() &&
                    expansion[i + 1] == L'n') {
                    AppendVkEvent(charEvents, VK_RETURN, retScan);
                    ++i;
                } else {
                    emitChar(expansion[i]);
                }
            }
            DispatchSendInput(bsEvents, charEvents);
        }
    }
    ClearWordState();
    CancelCommitUndo();
    return isPartOfMacro ? MacroResult::ExpandedEatTrigger : MacroResult::ExpandedPassTrigger;
}

wchar_t HookEngine::VkToMacroChar(DWORD vkCode) noexcept {
    // Translate VK → character with the current modifier state, so Shift/Caps/
    // AltGr yield the actual typed char (e.g. Shift+VK_OEM_PERIOD on US → '>'
    // instead of the unshifted '.'). Uses the foreground window's layout so
    // macros match what the target app would receive.
    //
    // Modifier state: GetKeyboardState is not reliable from a low-level hook
    // thread (LL hooks don't feed our message queue), so we build a minimal
    // key-state snapshot from GetAsyncKeyState for the modifiers ToUnicodeEx
    // actually consults.
    BYTE keyState[256] = {};
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) keyState[VK_SHIFT]   = 0x80;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) keyState[VK_CONTROL] = 0x80;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) keyState[VK_MENU]    = 0x80;
    if (GetKeyState(VK_CAPITAL) & 0x0001)      keyState[VK_CAPITAL] = 0x01;

    UINT scan = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    HWND fg = GetForegroundWindow();
    HKL layout = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);

    // wFlags bit 2 (0x4) = "do not change the keyboard state" — required so
    // ToUnicodeEx doesn't advance pending dead-key state. Win10 1607+.
    wchar_t buf[4] = {};
    int result = ToUnicodeEx(vkCode, scan, keyState, buf, 4, 0x4, layout);
    if (result > 0) {
        return static_cast<wchar_t>(towlower(buf[0]));
    }

    // result <= 0: dead key (-1) or no translation (0). Fall back to the
    // unshifted mapping — matches the pre-ToUnicodeEx behavior for these keys.
    UINT ch = MapVirtualKeyW(vkCode, MAPVK_VK_TO_CHAR);
    return ch ? static_cast<wchar_t>(towlower(static_cast<wchar_t>(ch))) : 0;
}

bool HookEngine::CreateRawInputMonitor() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = RawInputWndProc;
    wc.hInstance = cachedHInstance_;
    wc.lpszClassName = L"NexusKey_RawInputMonitor";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // Hidden desktop window (NOT HWND_MESSAGE — RIDEV_INPUTSINK requires desktop hierarchy)
    rawInputHwnd_ = CreateWindowExW(0, wc.lpszClassName, nullptr,
                                     0, 0, 0, 0, 0,
                                     nullptr, nullptr, cachedHInstance_, nullptr);
    if (!rawInputHwnd_) return false;

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;  // Generic Desktop
    rid.usUsage     = 0x06;  // Keyboard
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = rawInputHwnd_;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        DestroyWindow(rawInputHwnd_);
        rawInputHwnd_ = nullptr;
        return false;
    }
    return true;
}

void HookEngine::DestroyRawInputMonitor() {
    if (rawInputHwnd_) {
        KillTimer(rawInputHwnd_, kSelfHealTimerId);
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x06;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        DestroyWindow(rawInputHwnd_);
        rawInputHwnd_ = nullptr;
    }
    UnregisterClassW(L"NexusKey_RawInputMonitor", cachedHInstance_);
}

LRESULT CALLBACK HookEngine::RawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HookEngine* self = s_instance.load(std::memory_order_relaxed);

    // ── WM_TIMER: deferred self-heal (one-shot, from detection below) ──
    if (msg == WM_TIMER && wParam == kSelfHealTimerId) {
        KillTimer(hwnd, kSelfHealTimerId);  // One-shot: cancel immediately
        if (!self) return 0;

        HOOK_LOG(L"  SelfHeal: timer fired — reinstalling hooks");
        NEXTKEY_LOG(L"SelfHeal: reinstalling hooks (timer)");

        if (self->keyboardHook_) {
            UnhookWindowsHookEx(self->keyboardHook_);
            self->keyboardHook_ = SetWindowsHookExW(
                WH_KEYBOARD_LL, LowLevelKeyboardProc, self->cachedHInstance_, 0);
        }
        if (self->mouseHook_) {
            UnhookWindowsHookEx(self->mouseHook_);
            self->mouseHook_ = SetWindowsHookExW(
                WH_MOUSE_LL, LowLevelMouseProc, self->cachedHInstance_, 0);
        }

        self->consecutiveRawMisses_ = 0;
        self->lastSelfHealTime_ = GetTickCount();
        HOOK_LOG(L"  SelfHeal: hooks reinstalled OK");
        return 0;
    }

    if (msg != WM_INPUT)
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    // ── Extract Raw Input (stack-only, no allocation) ──
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                    RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > sizeof(RAWINPUT) + 32)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    alignas(RAWINPUT) BYTE buf[sizeof(RAWINPUT) + 32];
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                        RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    auto* raw = reinterpret_cast<RAWINPUT*>(buf);

    // Filter: physical keyboard key-down only
    if (raw->header.dwType != RIM_TYPEKEYBOARD) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (raw->header.hDevice == nullptr) return DefWindowProcW(hwnd, msg, wParam, lParam);  // Skip SendInput
    if (raw->data.keyboard.Flags & RI_KEY_BREAK) return DefWindowProcW(hwnd, msg, wParam, lParam);  // Key-up

    // ── Dual-channel comparison (< 0.5μs total) ──
    DWORD now = GetTickCount();
    DWORD elapsed = now - self->lastLlHookTime_;

    if (elapsed < kSelfHealHookFreshnessMs) {
        self->consecutiveRawMisses_ = 0;  // Hook alive
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // LL hook missed this key
    self->consecutiveRawMisses_++;
    HOOK_LOG(L"  SelfHeal: LL hook miss #%u (elapsed=%ums, vk=0x%02X)",
             self->consecutiveRawMisses_, elapsed, raw->data.keyboard.VKey);

    if (self->consecutiveRawMisses_ >= kSelfHealMissThreshold) {
        DWORD sinceLast = now - self->lastSelfHealTime_;
        if (sinceLast < kSelfHealCooldownMs) {
            HOOK_LOG(L"  SelfHeal: cooldown (%ums left)", kSelfHealCooldownMs - sinceLast);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        // Schedule deferred reinstall via one-shot timer (Anti-Dorion: random 10-50ms)
        // NEVER Sleep() on hook thread — blocks pump → Windows removes LL hooks!
        UINT delay = 10 + (GetTickCount() % 41);
        SetTimer(hwnd, kSelfHealTimerId, delay, nullptr);
        HOOK_LOG(L"  SelfHeal: HOOK DEAD — scheduled reinstall in %ums", delay);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace NextKey
