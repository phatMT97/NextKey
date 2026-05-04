// NexusKey - Keyboard Hook Engine
// SPDX-License-Identifier: GPL-3.0-only
//
// Single-process Vietnamese input using WH_KEYBOARD_LL.
// Replaces TSF DLL for MVP — no COM registration, no admin elevation.

#pragma once

#include "core/engine/IInputEngine.h"
#include "core/engine/CodeTableConverter.h"
#include "core/config/TypingConfig.h"
#include "core/config/ConfigEvent.h"
#include "core/SmartSwitchManager.h"
#include <Windows.h>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct IUIAutomation;

namespace NextKey {

class SharedStateManager;  // Forward declaration (defined in core/ipc/SharedStateManager.h)

/// Callback when Vietnamese/English mode changes
using ModeChangeCallback = std::function<void(bool vietnamese)>;

/// Keyboard hook engine — intercepts keystrokes, processes Vietnamese input,
/// outputs via SendInput backspace+retype. Absorbs HotkeyManager logic.
class HookEngine {
public:
    HookEngine();
    ~HookEngine();

    HookEngine(const HookEngine&) = delete;
    HookEngine& operator=(const HookEngine&) = delete;

    /// Start the hook engine (installs keyboard hook + focus hook)
    bool Start(HINSTANCE hInstance, const TypingConfig& config,
               bool initialVietnamese = true, uint8_t startupMode = 0);

    /// Stop and unhook everything
    void Stop();

    /// Toggle Vietnamese/English mode
    void ToggleVietnameseMode();

    /// Commit any pending composition — called by hotkey callbacks before firing actions
    /// (e.g., Quick Convert) so the text in the document reflects what's on screen.
    void CommitPending();

    /// Set callback for mode changes (to update tray icon)
    void SetModeChangeCallback(ModeChangeCallback callback) { modeChangeCallback_ = std::move(callback); }

    /// Set callback for config reload (notifies main to update QuickConvert etc.)
    void SetConfigReloadCallback(std::function<void()> callback) { configReloadCallback_ = std::move(callback); }

    /// Callback fired on focus changes. Args: (tsfActive, tsfReadonly).
    ///   tsfActive   = foreground app is in TSF list (full TIP consumes keys).
    ///   tsfReadonly = Hook handles keys; TSF DLL should publish doc anchor.
    /// Fires on every focus change (SetOrClearFlag is idempotent).
    void SetTsfModeCallback(std::function<void(bool, bool)> callback) {
        tsfModeCallback_ = std::move(callback);
    }

    /// Check for config changes and reload if needed
    bool CheckConfigEvent();

    /// Re-read SharedState and reload TOML if configGeneration changed.
    /// Safe cross-process: uses the configGeneration counter, not the Named Event
    /// (which is auto-reset and reserved for the TSF DLL).
    void SyncConfigFromSharedState();

    /// Set SharedState pointer for direct reading (must be the global instance from main.cpp)
    void SetSharedStateReader(SharedStateManager* ptr) { sharedStatePtr_ = ptr; }

    /// Change code table (commits pending composition, updates per-app map)
    void SetCodeTable(CodeTable ct);

    /// Get effective code table (checks manual per-app override map)
    [[nodiscard]] CodeTable GetCodeTable() const noexcept;

    [[nodiscard]] bool IsVietnameseMode() const noexcept { return vietnameseMode_; }
    [[nodiscard]] bool IsRunning() const noexcept { return keyboardHook_ != nullptr; }

    // Magic number to mark our own SendInput events (prevents other hooks from processing them)
    static constexpr ULONG_PTR NEXUSKEY_EXTRA_INFO = 0x4E4B;  // "NK"

    // Get exe name (lowercase) from window handle — used by ClassifyWindow() and smart switch
    [[nodiscard]] static std::wstring GetExeNameForHwnd(HWND hwnd) noexcept;

private:
    // Hook callbacks (static → instance dispatch)
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
                                       LONG idObject, LONG idChild,
                                       DWORD dwEventThread, DWORD dwmsEventTime);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK FocusPollTimerProc(HWND, UINT, UINT_PTR, DWORD);
    static void CALLBACK DelayedUrlCheckTimerProc(HWND, UINT, UINT_PTR, DWORD);

    // Config application (shared between Start and CheckConfigEvent)
    void ApplyConfig(const TypingConfig& config);

    // Core processing
    bool ProcessKeyDown(DWORD vkCode, DWORD scanCode, DWORD flags);
    bool ProcessKeyUp(DWORD vkCode, DWORD flags);

    // Input engine interaction
    [[nodiscard]] bool HandleAlphaKey(DWORD vkCode, bool shift, bool capsLock);
    [[nodiscard]] bool HandleVniDigitKey(DWORD vkCode); // VNI digit 1-9: push to engine, replace composition
    void HandleBackspace();
    bool CommitComposition();  // Returns true if auto-restore changed text
    void ResetComposition();
    void CancelCommitUndo();   // commitUndoState_ = Idle + commitStack_.clear()
    void RecordSynthDispatch() noexcept;  // Update both lastSynthSendTime_ and lastRealSynthTime_
    void SetCommitUndoReady(); // commitUndoState_ = Ready + timestamp

    // Output — universal SendInput with KEYEVENTF_UNICODE
    void ReplaceComposition(const std::wstring& newText, DWORD reinjectVk = 0);
    void TrackedSendInput(INPUT* events, UINT count) noexcept;
    void DispatchSendInput(std::vector<INPUT>& bsEvents, std::vector<INPUT>& charEvents);
    void SendBackspaces(size_t count);
    void SendBackspaceEvents(size_t count);
    void SendCharEvents(const std::wstring& text);

    // Clipboard paste fallback for VB6/ANSI-internal apps (can't handle KEYEVENTF_UNICODE)
    [[nodiscard]] bool ShouldUseClipboard() const noexcept;
    void ClipboardPaste(const std::wstring& text);

    // Direct EM_REPLACESEL into focused Edit/RichEdit/VB6 TextBox — no clipboard touched.
    // Primary VB6/ANSI-window path. Returns false if the focused control isn't a
    // compatible Edit class; caller falls back to ClipboardPaste.
    [[nodiscard]] bool TryEditMessagePaste(const std::wstring& text, size_t backspaceCount) noexcept;

    // Modifier state tracking (used by double-Alt and layout change detection)
    void TrackModifier(DWORD vkCode, bool isDown);

    // Backspace-into-committed-word: replay saved chars to restore engine state
    void ReplayCommittedChars();

    // Commit trigger check
    static bool IsCommitTrigger(DWORD vkCode);
    bool IsMacroTrigger(DWORD vkCode) const;

    // Convert VK code to macro-usable char (for special-char macro keys).
    // Uses MapVirtualKeyW — returns unshifted character only (Shift state ignored).
    [[nodiscard]] static wchar_t VkToMacroChar(DWORD vkCode) noexcept;

    // Result of TryExpandMacro
    enum class MacroResult { NoMatch, ExpandedEatTrigger, ExpandedPassTrigger };

    // Shared macro expansion logic (used by both English and Vietnamese mode paths).
    [[nodiscard]] MacroResult TryExpandMacro(wchar_t triggerChar);

    // Re-inject a key after auto-restore replacement
    void InjectKey(DWORD vkCode);

    // Clear per-word engine state (shared by CommitComposition, ResetComposition, TryExpandMacro)
    void ClearWordState();

    [[nodiscard]] static bool IsTrayOrTaskbarWindow(HWND hwnd) noexcept;
    // WebView2 host detection. Cache is positive-only (see .cpp for rationale).
    // REQUIRES: stateMutex_ held by caller.
    [[nodiscard]] bool IsWebView2App(HWND topLevel, const std::wstring& exeFullPath) noexcept;
    void NotifyModeChange() noexcept;  // Fire modeChangeCallback_ with effective mode
    void ClearUrlSuppression() noexcept;
    bool VerifyExcludedState();        // Check if foreground is still excluded; clears stale flag if not
    void OnFocusChanged(HWND triggerHwnd = nullptr);
    void CheckUrlBarFocus(HWND activeHwnd);
    // Populate cachedFocusedHwnd_/cachedFocusedClass_ from `foreground` via AttachThreadInput.
    // Called from OnFocusChanged and on-demand from TryEditMessagePaste when cache is stale.
    void RefreshFocusCache(HWND foreground) noexcept;
    void OnLayoutChanged(bool isCompatibleNow);
    void CheckLayoutChange();  // Query current layout and call OnLayoutChanged if it changed
    void ReloadAppOverrides();
    void ReloadExcludedApps();   // Reload excluded app set from TOML
    void ReloadTsfApps();        // Reload TSF app set from TOML
    // Reload macros from TOML with keys lowercased for case-insensitive lookup.
    // Runtime matching already lowercases the typed buffer; normalizing the
    // map keys mirrors that so capitalized TOML keys (e.g. `Chol = "Chôl"`) match.
    void ReloadMacroTable();
    void SaveEnglishModeAppsIfDirty();  // Persist English-mode apps to TOML

    // Engine state
    std::unique_ptr<IInputEngine> engine_;
    TypingConfig config_;                            // Last applied config (for per-app engine recreation)
    InputMethod currentMethod_ = InputMethod::Telex;
    std::wstring previousComposition_;  // What's currently displayed in the app
    std::vector<uint8_t> previousEncodedWidths_;  // Output unit count per Unicode char (for non-Unicode code tables)
    bool vietnameseMode_ = true;
    uint8_t startupMode_ = 0;  // 0=Vietnamese, 1=English, 2=Remember
    std::atomic<bool> sending_{false};  // True while SendInput is in progress (skip re-entrant hook calls)
    std::atomic<int> synthEventsPending_{0};  // Count of synthetic INPUT structs sent but not yet processed by hook
    DWORD lastSynthSendTime_ = 0;  // GetTickCount() of last SendInput call (watchdog: reset if stuck > 500ms)
    DWORD lastRealSynthTime_ = 0;  // GetTickCount() of last typing-related dispatch (not InjectKey re-injection)
    bool hadSynthInWord_ = false;  // True if any synthetic event was sent for the current word (blocks passthrough mixing)
    bool beepOnSwitch_ = false;
    bool smartSwitch_ = false;
    bool excludeApps_ = false;
    bool tsfApps_ = false;
    bool autoCaps_ = false;
    bool autoCapsMacro_ = false;
    bool autoOffByUrl_ = false;
    bool tempOffByAlt_ = false;
    bool tempEngineOff_ = false;       // True = Vietnamese bypassed for current word
    int altTapCount_ = 0;              // 0 or 1 (waiting for second tap)
    DWORD lastAltReleaseTime_ = 0;     // GetTickCount() of first Alt release
    static constexpr DWORD DOUBLE_ALT_TIMEOUT_MS = 400;
    /// Keystroke-based auto-capitalize state machine (used when no TSF anchor truth).
    enum class AutoCapState : uint8_t {
        Idle = 0,            // normal typing
        AfterPunct,          // just saw . ? !
        ReadyToCapitalize,   // saw punct + space/Enter → next A-Z is the sentence start
    };
    AutoCapState autoCapState_ = AutoCapState::Idle;
    std::unordered_set<std::wstring> excludedAppSet_;  // excluded apps: force English on focus
    bool isExcludedApp_ = false;      // cached: current app is excluded
    DWORD excludedPid_ = 0;           // PID of excluded app (fast check in ProcessKeyDown)
    std::unordered_set<std::wstring> tsfAppSet_;  // apps that should use TSF engine instead of hook
    bool isTsfApp_ = false;       // cached: is current foreground app in TSF list?
    bool isConsoleApp_ = false;   // cached: is current foreground app a console emulator?
    bool isElectronApp_ = false;  // cached: Electron/Qt but NOT console (skipEmptyChar_ && !isConsoleApp_)
    bool urlSuppressed_ = false;  // True when currently suppressed by browser URL bar
    bool modeBeforeUrl_ = true;   // Saved mode before URL bar suppression
    std::unordered_set<std::wstring> webView2PositiveCache_;  // full exe path → known WebView2 host (positive-only; see IsWebView2App)
    bool skipEmptyChar_ = false;  // Skip U+202F for Qt/Electron and Console apps
    bool needBaitChar_ = false;   // Apps with autocomplete/suggest need U+202F bait before BS
    bool useClipboardPaste_ = false;  // VB6 and legacy ANSI-internal apps need clipboard paste
    bool useEditMsgPath_ = false;     // Async-render apps (Win11 new Notepad) — try EM_REPLACESEL first, fall to SendInput
    bool isOutlookApp_ = false;   // Outlook 2016 RichEdit drops trailing char of a word when physical Shift+letter precedes it — force SendInput path (issue #97)
    DWORD lastForegroundPid_ = 0;  // PID of last known foreground (updated by OnFocusChanged + timer)
    std::unordered_map<std::wstring, bool> appModeMap_;  // exe name → vietnamese mode
    bool appModeDirty_ = false;  // True when appModeMap_ changed since last TOML save
    SmartSwitchManager smartSwitchMgr_;  // Shared memory for per-app mode
    std::wstring currentExe_;  // Currently focused app
    std::wstring previousExe_;  // Previously focused app (for tray menu context)
    CodeTable currentCodeTable_ = CodeTable::Unicode;
    CodeTable globalCodeTable_ = CodeTable::Unicode;     // config value, restored when no override
    std::unordered_map<std::wstring, int8_t> appEncodingOverrides_;   // exe → encoding override (-1=inherit)
    InputMethod globalInputMethod_ = InputMethod::Telex; // config value, restored when no override
    std::unordered_map<std::wstring, int8_t> appInputMethodOverrides_; // exe → method override (-1=inherit)

    // Backspace-into-committed-word (re-enter composition after commit + backspace)
    // inputHistory_ records exact user keystrokes (including backspace as '\b')
    // so replay produces identical engine state. This differs from engine's rawInput_
    // which mutates on escape sequences (EraseConsumedRaw).
    static constexpr wchar_t kBackspaceMarker = L'\b';
    static constexpr size_t kMaxCommitStack = 3;  // Max words to remember for backward
    static constexpr size_t kMaxSmartSwitchEntries = 200;  // Cap per-app mode memory
    // Auto-expire the Ready state after this many ms — cheap insurance against any
    // cursor-movement event that bypasses ResetComposition (e.g. future edge cases).
    static constexpr DWORD kCommitUndoTimeoutMs = 4000;
    static constexpr DWORD kSynthSettleMs = 100;  // Max time (ms) for synthetic events to be processed by app

    enum class CommitUndoState : uint8_t {
        Idle   = 0,  // No pending undo
        Ready  = 1,  // Just committed with Space/Enter — waiting for first BS
        Primed = 2,  // Space deleted — next Alpha/BS triggers replay
    };

    struct CommitEntry {
        std::vector<wchar_t> history;   // User keystrokes for replay
        std::wstring text;              // What was on screen when committed
        std::vector<uint8_t> widths;    // Encoded widths for non-Unicode code tables
    };

    std::vector<wchar_t> inputHistory_;         // User keystrokes for current composition
    std::vector<CommitEntry> commitStack_;       // Stack of committed words (LIFO, max kMaxCommitStack)
    bool pushedToStack_ = false;                 // True if last CommitComposition pushed to stack
    CommitUndoState commitUndoState_ = CommitUndoState::Idle;
    uint8_t pendingTriggerCount_ = 0;           // Extra commit triggers typed while Ready (need BS before Primed)
    DWORD commitReadyTime_ = 0;                 // GetTickCount() when entering Ready state

    // Macro expansion
    bool macroEnabled_ = false;
    bool macroInEnglish_ = false;
    bool tempOffMacroByEsc_ = false;  // Config: Esc can temp-disable macro
    bool tempMacroOff_ = false;       // Runtime: macro disabled for current word
    bool macroCrossCommit_ = false;   // rawMacroBuffer_ spans multiple engine commits (macro key has punctuation)
    std::unordered_map<std::wstring, std::wstring> macroTable_;
    std::unordered_set<std::wstring> spaceMacroKeys_;  // subset of macroTable_ keys that contain ' '
    std::wstring rawMacroBuffer_;

    // Hooks
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HWINEVENTHOOK focusHook_ = nullptr;     // EVENT_SYSTEM_FOREGROUND
    HWINEVENTHOOK minimizeHook_ = nullptr;  // EVENT_SYSTEM_MINIMIZEEND
    HWINEVENTHOOK objectFocusHook_ = nullptr; // EVENT_OBJECT_FOCUS
    UINT_PTR focusPollTimer_ = 0;          // 200ms PID poll — catches missed/phantom focus events
    UINT_PTR urlFocusTimer_ = 0;           // 100ms URL focus re-eval — fixes Chromium UIA tree lag
    IUIAutomation* uia_ = nullptr;         // UIAutomation for address bar detection

    // Dedicated hook thread: owns keyboardHook_ + mouseHook_ and runs its own
    // GetMessage pump so LL hook callbacks never block on the main (UI) thread's
    // message queue. Win10 silently removes LL hooks whose installer-thread pump
    // can't service hook events within LowLevelHooksTimeout (max 1000ms). Sciter
    // rendering, SharedState lock contention, and config reloads on main were
    // causing that — isolating the hook thread fixes it.
    std::thread hookThread_;
    DWORD hookThreadId_ = 0;                       // GetCurrentThreadId() of hookThread_ (for PostThreadMessage)
    std::atomic<bool> hookThreadReady_{false};     // true once hooks installed (or failed)
    std::mutex hookStartMutex_;                    // pairs with hookStartCv_ for handshake
    std::condition_variable hookStartCv_;
    HINSTANCE cachedHInstance_ = nullptr;          // captured in Start(), used by HookThreadProc
    // Recursive: main-thread API methods (Start, ApplyConfig, etc.) call each other
    // while holding the lock. Callback on hook thread acquires for brief read-modify.
    mutable std::recursive_mutex stateMutex_;
    void HookThreadProc();                         // runs on hookThread_

    // ── Self-healing: Dual-channel hook integrity detection ──
    // Raw Input (WM_INPUT) always fires for physical keys, even if our LL hook
    // is dead/removed. Comparing timestamps detects hook death without synthetic
    // keys — no false positives, no blocking, no mutex.
    HWND rawInputHwnd_ = nullptr;                    // Hidden window for WM_INPUT (hook thread only)
    DWORD lastLlHookTime_ = 0;                       // Updated in LLKeyboardProc (same thread — no atomic)
    uint8_t consecutiveRawMisses_ = 0;               // WM_INPUT events without LL hook activity
    DWORD lastSelfHealTime_ = 0;                     // Cooldown tracking
    static constexpr UINT_PTR kSelfHealTimerId = 42;          // One-shot timer ID
    static constexpr uint8_t kSelfHealMissThreshold = 3;      // Consecutive misses to trigger
    static constexpr DWORD kSelfHealCooldownMs = 10000;       // 10s between reinstalls
    static constexpr DWORD kSelfHealHookFreshnessMs = 200;    // LL hook must fire within this window

    static LRESULT CALLBACK RawInputWndProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateRawInputMonitor();
    void DestroyRawInputMonitor();

    // Modifier tracking state (for double-Alt and layout change detection)
    bool modCtrlDown_ = false;
    bool modShiftDown_ = false;
    bool modAltDown_ = false;
    bool modWinDown_ = false;
    bool otherKeyPressed_ = false;

    // CJK layout auto-toggle: auto-switch to E mode when CJK detected, restore on return
    bool layoutSuppressed_     = false;  // True when CJK layout active
    bool modeBeforeCjk_        = true;   // Saved vietnameseMode_ before CJK auto-switch
    bool cachedIsCompatLayout_ = true;   // Last known layout compatibility (updated in OnFocusChanged + key-up)

    // Focused child HWND + class name, cached to avoid AttachThreadInput per keystroke
    // (used by TryEditMessagePaste for VB6/ANSI apps). Refreshed in OnFocusChanged and
    // invalidated on mouse click (within-app focus change).
    HWND cachedFocusedHwnd_ = nullptr;
    std::wstring cachedFocusedClass_;

    // Config reload
    ConfigEvent configEvent_;

    // Direct SharedState reader — pointer to the global SharedStateManager (same process)
    SharedStateManager* sharedStatePtr_ = nullptr;
    uint32_t lastFeatureFlags_ = 0;
    uint8_t lastSpellCheck_ = 0;
    uint8_t lastInputMethod_ = 0;
    uint8_t lastCodeTable_ = 0;
    void QuickSyncFromSharedState();
    void ReloadFromToml();  // Full TOML reload (macros, excluded apps, hotkeys, etc.)
    uint32_t lastEpoch_ = 0;             // Epoch fast path — skip full Read() when unchanged
    uint8_t lastConfigGeneration_ = 0;   // Tracks configGeneration from SharedState

    // Callbacks
    ModeChangeCallback modeChangeCallback_;
    std::function<void()> configReloadCallback_;
    std::function<void(bool, bool)> tsfModeCallback_;

    // Singleton for static callback dispatch (read from hook callback thread)
    static std::atomic<HookEngine*> s_instance;
};

}  // namespace NextKey
