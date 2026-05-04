// NexusKey - Core Application Entry Point
// SPDX-License-Identifier: GPL-3.0-only

#include "system/TrayIcon.h"
#include "system/FloatingIcon.h"
#include "system/SubprocessHelper.h"
#include "system/SubprocessRunners.h"
#include "system/UpdateChecker.h"
#include "system/UpdateInstaller.h"
#include "system/PendingDllApply.h"
#include "system/ToastPopup.h"
#include "core/Version.h"
#include "core/config/TypingConfig.h"
#include "core/config/ConfigManager.h"
#include "core/config/ConfigEvent.h"
#include "core/ipc/SharedState.h"
#include "core/Strings.h"
#include "core/Debug.h"
#include "core/CrashLog.h"

#include "system/TsfRegistration.h"
#include "system/StartupHelper.h"
#include "helpers/AppHelpers.h"

#include "system/HotkeyManager.h"
#include "system/HotkeyWiring.h"
#include "core/ipc/SharedStateManager.h"
#ifdef NEXUSKEY_HOOK_ENGINE
#include "system/HookEngine.h"
#include "system/QuickConvert.h"
#endif

#include <Windows.h>
#include <ole2.h>
#include <timeapi.h>
#include <exception>
#include <memory>
#include <string>
#include <atomic>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

// Common Controls v6 is declared in NexusKey.exe.manifest (DPI awareness + CC v6)

using namespace NextKey;

// Forward declarations
void SpawnSettingsSubprocess();

// Global state
static std::atomic<bool> g_running{true};
static TrayIcon g_trayIcon;
static FloatingIcon g_floatingIcon;
static HINSTANCE g_hInstance = nullptr;

static SharedStateManager g_sharedState;  // Shared memory for Settings subprocess IPC
static HotkeyManager g_hotkeyManager;

#ifdef NEXUSKEY_HOOK_ENGINE
static HookEngine g_hookEngine;
static std::unique_ptr<QuickConvert> g_quickConvert;
static HotkeyManager::SlotId g_toggleHotkeySlot = 0;
static HotkeyManager::SlotId g_convertHotkeySlot = 0;
#endif

// Forward declaration
void OnMenuCommand(TrayMenuId id);

// Settings window helpers — search by title (subprocess may or may not be open)
static HWND GetSettingsHwnd() noexcept {
    return FindWindowW(nullptr, L"NexusKey Settings");
}
static void NotifySettingsMode(bool vietnamese) noexcept {
    if (HWND h = GetSettingsHwnd()) {
        PostMessageW(h, WM_NEXUSKEY_MODE_CHANGED, vietnamese ? 1 : 0, 0);
    }
}

#ifndef NEXUSKEY_HOOK_ENGINE
// V/E icon sync: 250ms poll of SharedState flags (atomic read, no IPC)
static constexpr UINT_PTR TIMER_ID_ICON_POLL = 100;

static void CALLBACK IconPollTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    try {
        uint32_t flags = g_sharedState.ReadFlags();
        bool vietnamese = (flags & SharedFlags::VIETNAMESE_MODE) != 0;
        g_trayIcon.SetVietnameseMode(vietnamese);  // no-op if unchanged
        g_floatingIcon.SetVietnameseMode(vietnamese);  // no-op if unchanged
    } catch (const std::exception& e) {
        CrashLog(L"IconPollTimerProc", e.what());
    } catch (...) {
        CrashLog(L"IconPollTimerProc", "(non-std exception)");
    }
}
#endif

/// Ensure floating icon window exists (lazy-create on first use).
static void EnsureFloatingIconCreated() {
    if (g_floatingIcon.IsCreated()) return;
    (void)g_floatingIcon.Create(g_hInstance, g_hookEngine.IsVietnameseMode());
}

/// Initialize floating icon overlay + config callbacks.
/// Shared by both HookEngine and TSF modes.
static void InitFloatingIcon(HINSTANCE hInstance, const SystemConfig& sc) {
    bool startVietnamese = (sc.startupMode != 1);
    // Only create resources if actually showing — saves RAM when disabled
    if (sc.showFloatingIcon) {
        if (g_floatingIcon.Create(hInstance, startVietnamese)) {
            g_floatingIcon.SetPosition(sc.floatingIconX, sc.floatingIconY);
            g_floatingIcon.SetVisible(true);
        }
    }

    // No TOML save on drag — position lives in g_floatingIcon.posX_/posY_,
    // survives Destroy()/Create() cycles. Persisted to TOML at app exit only.

    // Re-read config when Settings changes icon/system config
    g_trayIcon.SetIconConfigChangedCallback([]() {
        auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
        if (sysConfig.showFloatingIcon) {
            EnsureFloatingIconCreated();
            // Don't overwrite in-memory position — object already knows where it was
            g_floatingIcon.SetVisible(true);
        } else {
            g_floatingIcon.Destroy();
        }
    });
}

/// Save floating icon position to TOML for next launch, then destroy.
static void CleanupFloatingIcon() noexcept {
    if (g_floatingIcon.GetPosX() != INT32_MIN) {
        auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
        sysConfig.floatingIconX = g_floatingIcon.GetPosX();
        sysConfig.floatingIconY = g_floatingIcon.GetPosY();
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), sysConfig);
    }
    g_floatingIcon.Destroy();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    g_hInstance = hInstance;
    InstallCursorCrashHandler();  // Restore system cursors if we crash during window picking

    // Last-resort catch: if a C++ throw ever escapes all try/catch at thread boundaries
    // (shouldn't happen after issue #103 fix, but belt-and-suspenders), log before dying
    // so the next crash report shows WHAT escaped, not a silent std::terminate.
    std::set_terminate([]() noexcept {
        CrashLog(L"std::terminate", "uncaught exception reached std::terminate");
        std::abort();
    });

    // ═══════════════════════════════════════════════════════════
    // Command-line Router
    // ═══════════════════════════════════════════════════════════

    // TSF Unregistration (runs elevated, then exits)
    // NOTE: Must check --unregister-tsf BEFORE --register-tsf
    // because "--register-tsf" is a substring of "--unregister-tsf"
    if (lpCmdLine && wcsstr(lpCmdLine, L"--unregister-tsf") != nullptr) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = UnregisterTsf();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    // TSF Registration (runs elevated, then exits)
    if (lpCmdLine && wcsstr(lpCmdLine, L"--register-tsf") != nullptr) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = RegisterTsf();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    // Diagnostics mode (shows HKL/TSF/SharedState info in MessageBox)
    if (lpCmdLine && wcsstr(lpCmdLine, L"--diag") != nullptr) {
        RunDiagnostics();
        return 0;
    }

    // Self-update installer mode (MUST be before Sciter dialog routes — sciter.dll not loaded yet)
    if (lpCmdLine && wcsstr(lpCmdLine, L"--install-update") != nullptr) {
        const wchar_t* arg = wcsstr(lpCmdLine, L"--install-update");
        arg += wcslen(L"--install-update");  // Skip "--install-update"
        // Skip whitespace
        while (*arg == L' ' || *arg == L'\t') arg++;
        // Strip surrounding quotes if present
        std::wstring zipPath(arg);
        if (zipPath.size() >= 2 && zipPath.front() == L'"' && zipPath.back() == L'"') {
            zipPath = zipPath.substr(1, zipPath.size() - 2);
        }
        if (!zipPath.empty()) {
            // Validate that the zip is inside %TEMP% — reject arbitrary paths to prevent
            // local malware from replacing the update archive with a crafted DLL payload.
            // Normalize first to defeat path traversal (e.g. C:\Temp\..\evil.zip).
            wchar_t resolvedPath[MAX_PATH] = {};
            if (!GetFullPathNameW(zipPath.c_str(), MAX_PATH, resolvedPath, nullptr)) {
                NEXTKEY_LOG(L"[main] --install-update rejected: path resolution failed");
                return 1;
            }
            wchar_t tempDir[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tempDir);  // Guarantees trailing backslash
            int cmpLen = static_cast<int>(wcslen(tempDir));
            int resolvedLen = static_cast<int>(wcslen(resolvedPath));
            if (resolvedLen < cmpLen ||
                CompareStringOrdinal(resolvedPath, cmpLen, tempDir, cmpLen, TRUE) != CSTR_EQUAL) {
                NEXTKEY_LOG(L"[main] --install-update rejected: '%s' not inside TEMP", resolvedPath);
                return 1;
            }
            RunUpdateInstaller(resolvedPath);  // [[noreturn]]
        }
        return 1;  // Missing zip path
    }

    // Subprocess dialog routes
    if (lpCmdLine && wcsstr(lpCmdLine, L"--settings") != nullptr) {
        RunSettingsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--excludedapps") != nullptr) {
        RunExcludedAppsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--tsfapps") != nullptr) {
        RunTsfAppsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--macro") != nullptr) {
        RunMacroSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--convert") != nullptr) {
        RunConvertToolSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--about") != nullptr) {
        RunAboutSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--appoverrides") != nullptr) {
        RunAppOverridesSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--spellexclusions") != nullptr) {
        RunSpellExclusionsSubprocess();  // [[noreturn]]
    }

    // ═══════════════════════════════════════════════════════════
    // Main Process
    // ═══════════════════════════════════════════════════════════

    const bool isAdminRestart = HasCmdlineFlag(lpCmdLine, ADMIN_RESTART_FLAG);

    // Self-elevate if "Run as Admin" is enabled but we're not elevated.
    // Must be before mutex — the elevated instance will acquire the mutex instead.
    {
        auto preConfig = ConfigManager::LoadSystemConfigOrDefault();
        if (SelfElevateIfNeeded(ConfigManager::GetConfigPath(), preConfig.runAsAdmin)) {
            return 0;  // Elevated instance launching, exit this one
        }
    }

    // Ensure only one background instance of NexusKey runs at a time.
    // We check this AFTER subprocess routing so settings/macro dialogs
    // can spawn freely, but a second background process cannot.
    // NOTE: Use default DACL (nullptr). MakeCreatorOnlySecurityAttributes() uses
    // CO (Creator Owner) SID which does NOT resolve for non-container objects like
    // mutexes — second instance gets ERROR_ACCESS_DENIED instead of ERROR_ALREADY_EXISTS.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\NexusKey_Main_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (isAdminRestart) {
            // Admin-restart path: old instance is in the middle of cleanup.
            // Wait for ownership transfer (either clean release or WAIT_ABANDONED
            // if old is killed). This closes the restart race: by the time we
            // hold the mutex, old has unhooked and released shared resources.
            DWORD r = WaitForSingleObject(hMutex, 10000);
            if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
                NEXTKEY_LOG(L"Admin-restart: timeout waiting for old instance mutex (r=%lu)", r);
                CloseHandle(hMutex);
                return 1;
            }
            NEXTKEY_LOG(L"Admin-restart: mutex ownership acquired (%s)",
                         r == WAIT_ABANDONED ? L"abandoned" : L"released");
        } else {
            // Another background instance is already running.
            // If the user has "show on startup" configured, popup the settings dialog of the
            // existing instance to indicate the app is active. Otherwise, strictly silent.
            auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
            if (sysConfig.showOnStartup) {
                HWND existingTrayWnd = FindWindowW(L"NexusKeyTrayClass", nullptr);
                if (existingTrayWnd) {
                    PostMessageW(existingTrayWnd, WM_NEXUSKEY_SHOW_SETTINGS, 0, 0);

                    HWND existingSettings = GetSettingsHwnd();
                    if (existingSettings) {
                        SetForegroundWindow(existingSettings);
                    }
                }
            }

            NEXTKEY_LOG(L"Another instance is already running. Exiting.");
            CloseHandle(hMutex);
            return 0;
        }
    }

    // Remove any HKCU CLSID override that malware may have planted to hijack TSF DLL loading
    CleanupHkcuClsidOverride();

    // Load config
    auto config = ConfigManager::LoadOrDefault();
    auto hotkeyConfig = ConfigManager::LoadHotkeyConfigOrDefault();

    // Initialize UI language from system config
    auto systemConfig = ConfigManager::LoadSystemConfigOrDefault();
    SetLanguage(static_cast<Language>(systemConfig.language));

    // Apply any deferred TSF DLL swap before the generic update-file cleanup
    // (which removes _old_version/ and would delete the parked copy if the
    // order were reversed). Only runs in the main process; subprocess routes
    // returned above. Result published into SharedState flags after Create().
    PendingDllState pendingDllState = ApplyPendingDllUpdate();

    // Clean up leftover files from a previous update.
    // If files were cleaned up, it means we just finished an update.
    bool updateJustCompleted = CleanupOldUpdateFiles();
    if (updateJustCompleted) {
        NEXTKEY_LOG(L"Update completed successfully, old files cleaned up");
    }

    // Ensure startup registration is intact (never prompts UAC).
    // If admin task was lost, falls back to registry and syncs config.
    if (EnsureStartupRegistration(systemConfig.runAtStartup, systemConfig.runAsAdmin)) {
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), systemConfig);
        NEXTKEY_LOG(L"Startup task missing — fell back to registry, disabled admin mode in config");
    }

    // Check for update failure marker (installer failed and relaunched us)
    bool updateJustFailed = false;
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir(exePath);
        auto pos = exeDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) exeDir = exeDir.substr(0, pos);
        std::wstring markerPath = exeDir + L"\\_update_failed";
        if (DeleteFileW(markerPath.c_str())) {
            updateJustFailed = true;
            NEXTKEY_LOG(L"Update failed marker found and cleaned up");
        }
    }

#ifdef NEXUSKEY_HOOK_ENGINE
    // ═══════════════════════════════════════════════════════════
    // Hook Engine Mode — single process, no DLL/COM needed
    // (Except for UIAutomation which requires COM initialization)
    // ═══════════════════════════════════════════════════════════
    
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool startVietnamese = (systemConfig.startupMode != 1);

    // Create SharedState for Settings subprocess IPC
    if (g_sharedState.Create()) {
        SharedState state;
        state.InitDefaults();
        state.inputMethod = static_cast<uint8_t>(config.inputMethod);
        state.spellCheck = config.spellCheckEnabled ? 1 : 0;
        state.optimizeLevel = config.optimizeLevel;
        state.codeTable = static_cast<uint8_t>(config.codeTable);
        state.SetFeatureFlags(EncodeFeatureFlags(config));
        if (!startVietnamese) {
            state.flags &= ~SharedFlags::VIETNAMESE_MODE;
        }
        state.SetHotkey(hotkeyConfig);
        g_sharedState.Write(state);
        NEXTKEY_LOG(L"SharedState created for HookEngine mode");

        // Publish the startup DLL-swap outcome ONLY if TSF is currently
        // registered — users who haven't opted into TSF don't care about
        // DLL-host synchronization and shouldn't be nagged to reboot.
        // Bits clear on reboot (SharedState is recreated fresh).
        const bool tsfInUse = IsTsfRegistered();
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_PENDING_DLL_SWAP,
            tsfInUse && pendingDllState == PendingDllState::SwapFailed);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_POST_UPDATE_REBOOT,
            tsfInUse && pendingDllState == PendingDllState::SwapDoneNeedsReboot);
        // TSF_ABI_MISMATCH is set cross-process by the DLL itself and only if
        // a host process loaded our DLL. DLL can't load without registration,
        // so this flag implicitly requires TSF-in-use. No gate needed.
    }

    // Tray Icon
    if (!g_trayIcon.Create(hInstance, startVietnamese)) {
        // MessageBox acceptable: fatal startup error, app cannot function without tray icon.
        // No matching StringId — using English string (language config not yet applied to UI).
        MessageBoxW(nullptr, L"Failed to create tray icon", L"NexusKey", MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }
    g_trayIcon.SetMenuCallback(OnMenuCommand);
    g_trayIcon.SetSharedState(&g_sharedState);  // for TSF-update restart menu item

    // Wire mode change callback: HookEngine → defer icon update via PostMessage
    g_hookEngine.SetModeChangeCallback([](bool vietnamese) {
        g_sharedState.SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, vietnamese);
        WPARAM wp = vietnamese ? 1 : 0;
        HWND trayWnd = g_trayIcon.GetMessageWindow();
        if (trayWnd) {
            PostMessageW(trayWnd, WM_NEXUSKEY_TRAY_MODE_SYNC, wp, 0);
        }
        // Floating icon: direct update (same thread, no PostMessage needed)
        g_floatingIcon.SetVietnameseMode(vietnamese);
        // Notify settings dialog directly (1 hop instead of 2 via TRAY_MODE_SYNC).
        // This keeps the toggle in sync with the tray icon even on rapid clicks.
        NotifySettingsMode(vietnamese);
    });

    // Wire TSF mode callback: HookEngine → SharedState flags for DLL.
    // TSF_ACTIVE   = DLL consumes keys (foreground app in TSF list).
    // TSF_READONLY = DLL publishes HookContextAnchor for auto-cap (ordinary app).
    g_hookEngine.SetTsfModeCallback([](bool tsfActive, bool tsfReadonly) {
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_ACTIVE, tsfActive);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_READONLY, tsfReadonly);
    });

    // Wire hook-reload callback: sub-dialog subprocess → main EXE eager sync.
    // Without this, new lists (TSF apps, excluded apps, macros, …) only apply on the next
    // keystroke / focus change in the target app. SyncConfigFromSharedState reads
    // configGeneration; it must not touch the Named Event (auto-reset, reserved for TSF DLL).
    g_trayIcon.SetHookReloadCallback([]() {
        g_hookEngine.SyncConfigFromSharedState();
    });

    // Wire settings dialog → HookEngine mode set (cross-process)
    g_trayIcon.SetModeRequestCallback([](bool vietnamese) {
        if (g_hookEngine.IsVietnameseMode() != vietnamese) {
            g_hookEngine.ToggleVietnameseMode();
        }
    });

    // Wire menu state getter — reads from SharedState only (no TOML)
    g_trayIcon.SetMenuStateGetter([]() -> TrayMenuState {
        SharedState state = g_sharedState.Read();
        uint32_t ff = state.GetFeatureFlags();
        return {
            g_hookEngine.IsVietnameseMode(),
            state.spellCheck != 0,
            (ff & FeatureFlags::SMART_SWITCH) != 0,
            (ff & FeatureFlags::MACRO_ENABLED) != 0,
            state.inputMethod,
            static_cast<CodeTable>(state.codeTable)
        };
    });

    // Wire hotkeys (toggle V/E + quick convert) and config reload callback
    WireHotkeys(g_hotkeyManager, g_hookEngine, g_trayIcon, g_quickConvert,
                g_toggleHotkeySlot, g_convertHotkeySlot, hInstance, hotkeyConfig);

    // Set 1ms timer resolution so Sleep(1) actually sleeps ~1ms instead of ~15ms.
    // Required for smooth Vietnamese input — backspace-then-retype needs a short gap
    // between SendInput calls for Electron/Console apps, but 15ms (default) is noticeable.
    timeBeginPeriod(1);

    // Share g_sharedState with HookEngine for direct reading (same process, no Open needed)
    g_hookEngine.SetSharedStateReader(&g_sharedState);

    // Start keyboard hook engine
    if (!g_hookEngine.Start(hInstance, config, startVietnamese, systemConfig.startupMode)) {
        // MessageBox acceptable: fatal startup error, app cannot function without keyboard hook.
        // No matching StringId — using English string (language config not yet applied to UI).
        timeEndPeriod(1);
        MessageBoxW(nullptr, L"Failed to install keyboard hook", L"NexusKey", MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    NEXTKEY_LOG(L"HookEngine started, entering message loop");

    // Apply system config (icon style, show-on-startup)
    g_trayIcon.SetIconConfig(systemConfig.iconStyle, systemConfig.customColorV, systemConfig.customColorE);

    InitFloatingIcon(hInstance, systemConfig);

    if (systemConfig.showOnStartup) {
        SpawnSettingsSubprocess();
    }

    // Show post-update notification (after tray icon is ready)
    if (updateJustCompleted) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_SUCCESS), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::PostUpdateToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::PostUpdateToast::thread", "(non-std exception)");
            }
        }).detach();
    } else if (updateJustFailed) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_INSTALL_FAILED), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::UpdateFailedToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::UpdateFailedToast::thread", "(non-std exception)");
            }
        }).detach();
    }

    // Auto-check for updates on startup (background thread, 3s delay)
    if (systemConfig.autoCheckUpdate) {
        std::thread([]() {
            // URLDownloadToFileW (used by CheckForUpdate) requires COM init on the
            // calling thread — sibling threads in this file all call it; match them.
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                Sleep(3000);
                auto info = UpdateChecker::CheckForUpdate();
                if (info.available) {
                    HWND trayWnd = FindWindowW(L"NexusKeyTrayClass", nullptr);
                    if (trayWnd) {
                        auto* pInfo = new (std::nothrow) UpdateInfo(std::move(info));
                        if (pInfo) {
                            // WndProc returns true (1) on success and takes ownership of pInfo.
                            // If window was destroyed, SendMessageW returns 0 — we still own pInfo.
                            if (!SendMessageW(trayWnd, WM_NEXUSKEY_UPDATE_AVAILABLE, 0,
                                              reinterpret_cast<LPARAM>(pInfo))) {
                                delete pInfo;
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                CrashLog(L"main::AutoUpdateCheck::thread", e.what());
            } catch (...) {
                CrashLog(L"main::AutoUpdateCheck::thread", "(non-std exception)");
            }
            CoUninitialize();
        }).detach();
    }

    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    TerminateAllSubprocesses();
    CleanupFloatingIcon();
    g_trayIcon.Destroy();
    g_hotkeyManager.Uninstall();
    g_hookEngine.Stop();
    timeEndPeriod(1);
    CoUninitialize();

#else
    // ═══════════════════════════════════════════════════════════
    // TSF Mode — SharedState IPC + DLL
    // ═══════════════════════════════════════════════════════════

    // Initialize COM for TSF registration check
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Check and register TSF if needed
    if (!IsTsfRegistered()) {
        NEXTKEY_LOG(L"TSF not registered, attempting registration...");

        if (!RegisterTsf()) {
            // MessageBox acceptable: requires user consent before UAC elevation prompt.
            // No matching StringId — using English strings (first-run scenario, language not configured yet).
            int result = MessageBoxW(
                nullptr,
                L"NexusKey needs to register its input method.\n\n"
                L"This requires administrator privileges.\n"
                L"Click OK to continue with elevation.",
                L"NexusKey Setup",
                MB_OKCANCEL | MB_ICONINFORMATION
            );

            if (result == IDOK) {
                if (!RegisterTsfElevated()) {
                    MessageBoxW(nullptr,
                        L"Failed to register input method.\n"
                        L"Please run as administrator.",
                        L"NexusKey", MB_ICONERROR);
                }
            }
        }
    } else {
        NEXTKEY_LOG(L"TSF already registered");
    }

    // Initialize shared state for Engine IPC
    bool sharedStateOk = g_sharedState.Create();
    if (!sharedStateOk) {
        NEXTKEY_LOG(L"Failed to create shared memory, TSF will use TOML fallback");
    } else {
        SharedState state;
        state.InitDefaults();
        // TSF-only mode: DLL is the only engine, always active
        state.flags |= SharedFlags::TSF_ACTIVE;
        state.inputMethod = static_cast<uint8_t>(config.inputMethod);
        state.spellCheck = config.spellCheckEnabled ? 1 : 0;
        state.optimizeLevel = config.optimizeLevel;
        state.codeTable = static_cast<uint8_t>(config.codeTable);
        state.SetFeatureFlags(EncodeFeatureFlags(config));
        g_sharedState.Write(state);
        NEXTKEY_LOG(L"SharedState created and initialized (TSF_ACTIVE=1, TSF-only mode)");

        // Publish startup DLL-swap outcome. We're in the TSF-only mode branch,
        // so TSF is registered by construction (earlier `if (!IsTsfRegistered())`
        // path handled the register prompt) — no IsTsfRegistered() gate needed.
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_PENDING_DLL_SWAP,
            pendingDllState == PendingDllState::SwapFailed);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_POST_UPDATE_REBOOT,
            pendingDllState == PendingDllState::SwapDoneNeedsReboot);
    }

    // Tray Icon
    if (!g_trayIcon.Create(hInstance)) {
        // MessageBox acceptable: fatal startup error, app cannot function without tray icon.
        // No matching StringId — using English string (language config not yet applied to UI).
        MessageBoxW(nullptr, L"Failed to create tray icon", L"NexusKey", MB_ICONERROR);
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }
    g_trayIcon.SetMenuCallback(OnMenuCommand);
    g_trayIcon.SetSharedState(&g_sharedState);  // for TSF-update restart menu item

    // Wire settings dialog → TSF mode set (cross-process)
    g_trayIcon.SetModeRequestCallback([](bool vietnamese) {
        g_sharedState.SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, vietnamese);
        g_trayIcon.SetVietnameseMode(vietnamese);
    });

    // Wire menu state getter — reads from SharedState only (no TOML)
    g_trayIcon.SetMenuStateGetter([]() -> TrayMenuState {
        SharedState state = g_sharedState.Read();
        uint32_t ff = state.GetFeatureFlags();
        return {
            (state.flags & SharedFlags::VIETNAMESE_MODE) != 0,
            state.spellCheck != 0,
            (ff & FeatureFlags::SMART_SWITCH) != 0,
            (ff & FeatureFlags::MACRO_ENABLED) != 0,
            state.inputMethod,
            static_cast<CodeTable>(state.codeTable)
        };
    });

    // Poll SharedState flags every 250ms to sync icon V/E state
    SetTimer(g_trayIcon.GetMessageWindow(), TIMER_ID_ICON_POLL, 250, IconPollTimerProc);

    // Internal Hotkey — TSF mode has toggle only (no QuickConvert)
    auto hotkeyOpt = ConfigManager::LoadHotkeyConfig(ConfigManager::GetConfigPath());
    if (hotkeyOpt && hotkeyOpt->HasAny()) {
        HWND trayWnd = g_trayIcon.GetMessageWindow();
        g_hotkeyManager.AddHotkey(*hotkeyOpt, [trayWnd]() {
            if (trayWnd) PostMessageW(trayWnd, WM_HOTKEY, 0, 0);
        });
        g_hotkeyManager.Initialize(hInstance);
        NEXTKEY_LOG(L"Internal hotkey installed (ctrl=%d, shift=%d, alt=%d, win=%d, key=0x%02X)",
                    hotkeyOpt->ctrl, hotkeyOpt->shift, hotkeyOpt->alt, hotkeyOpt->win, hotkeyOpt->key);
    } else {
        NEXTKEY_LOG(L"No internal hotkey configured");
    }

    NEXTKEY_LOG(L"Tray icon created, entering message loop");

    // Apply system config (icon style, show-on-startup)
    g_trayIcon.SetIconConfig(systemConfig.iconStyle, systemConfig.customColorV, systemConfig.customColorE);

    InitFloatingIcon(hInstance, systemConfig);

    if (systemConfig.showOnStartup) {
        SpawnSettingsSubprocess();
    }

    // Show post-update notification (after tray icon is ready)
    if (updateJustCompleted) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_SUCCESS), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::PostUpdateToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::PostUpdateToast::thread", "(non-std exception)");
            }
        }).detach();
    } else if (updateJustFailed) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_INSTALL_FAILED), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::UpdateFailedToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::UpdateFailedToast::thread", "(non-std exception)");
            }
        }).detach();
    }

    // Auto-check for updates on startup (background thread, 3s delay)
    if (systemConfig.autoCheckUpdate) {
        std::thread([]() {
            // URLDownloadToFileW (used by CheckForUpdate) requires COM init on the
            // calling thread — sibling threads in this file all call it; match them.
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                Sleep(3000);
                auto info = UpdateChecker::CheckForUpdate();
                if (info.available) {
                    HWND trayWnd = FindWindowW(L"NexusKeyTrayClass", nullptr);
                    if (trayWnd) {
                        auto* pInfo = new (std::nothrow) UpdateInfo(std::move(info));
                        if (pInfo) {
                            // WndProc returns true (1) on success and takes ownership of pInfo.
                            // If window was destroyed, SendMessageW returns 0 — we still own pInfo.
                            if (!SendMessageW(trayWnd, WM_NEXUSKEY_UPDATE_AVAILABLE, 0,
                                              reinterpret_cast<LPARAM>(pInfo))) {
                                delete pInfo;
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                CrashLog(L"main::AutoUpdateCheck::thread", e.what());
            } catch (...) {
                CrashLog(L"main::AutoUpdateCheck::thread", "(non-std exception)");
            }
            CoUninitialize();
        }).detach();
    }

    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    CleanupFloatingIcon();
    KillTimer(g_trayIcon.GetMessageWindow(), TIMER_ID_ICON_POLL);
    g_hotkeyManager.Uninstall();

    // Disable engine in SharedState so TSF stops processing
    if (sharedStateOk) {
        SharedState state = g_sharedState.Read();
        if (state.IsValid()) {
            state.flags &= ~SharedFlags::ENGINE_ENABLED;
            g_sharedState.Write(state);
            NEXTKEY_LOG(L"ENGINE_ENABLED cleared in SharedState");
        }
    }

    TerminateAllSubprocesses();
    g_trayIcon.Destroy();
    CoUninitialize();
#endif

    NEXTKEY_LOG(L"Exiting");
    CloseHandle(hMutex);
    return 0;
}

/// Save config to TOML + sync SharedState + signal ConfigEvent.
/// Used by tray menu toggle handlers to propagate changes.
static void ApplyConfigChange(const TypingConfig& config) {
    // 1. Save to TOML
    (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);

    // 2. Sync to SharedState + bump configGeneration
    SharedStateManager sm;
    if (sm.OpenReadWrite()) {
        SharedState state = sm.Read();
        if (state.IsValid()) {
            state.inputMethod = static_cast<uint8_t>(config.inputMethod);
            state.spellCheck = config.spellCheckEnabled ? 1 : 0;
            state.codeTable = static_cast<uint8_t>(config.codeTable);
            state.SetFeatureFlags(EncodeFeatureFlags(config));
            state.configGeneration++;  // HookEngine detects on next keystroke
            sm.Write(state);
        }
    }

    // 3. Signal ConfigEvent for TSF DLL (still uses Named Event)
    ConfigEvent event;
    if (event.Initialize()) {
        event.Signal();
    }

    // 4. Notify Settings dialog (if open) to refresh UI
    if (HWND settingsWnd = GetSettingsHwnd()) {
        PostMessageW(settingsWnd, WM_NEXUSKEY_CONFIG_CHANGED, 0, 0);
    }
}

void OnMenuCommand(TrayMenuId id) {
    switch (id) {
        case TrayMenuId::Settings:
            SpawnSettingsSubprocess();
            break;

        case TrayMenuId::About:
            SpawnSubprocess(L"NexusKey - About", L"--about");
            break;

        case TrayMenuId::ToggleMode:
#ifdef NEXUSKEY_HOOK_ENGINE
            g_hookEngine.ToggleVietnameseMode();
#else
            g_sharedState.ToggleFlag(SharedFlags::VIETNAMESE_MODE);
            g_trayIcon.SetVietnameseMode(
                (g_sharedState.ReadFlags() & SharedFlags::VIETNAMESE_MODE) != 0);
#endif
            break;

        case TrayMenuId::SpellCheck: {
            auto config = ConfigManager::LoadOrDefault();
            config.spellCheckEnabled = !config.spellCheckEnabled;
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::SmartSwitch: {
            auto config = ConfigManager::LoadOrDefault();
            config.smartSwitch = !config.smartSwitch;
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::MacroEnabled: {
            auto config = ConfigManager::LoadOrDefault();
            config.macroEnabled = !config.macroEnabled;
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::MacroTable:
            SpawnSubprocess(L"NexusKey - Macro Table", L"--macro");
            break;

        case TrayMenuId::ConvertTool:
            SpawnSubprocess(L"NexusKey - Convert Tool", L"--convert");
            break;

#ifdef NEXUSKEY_HOOK_ENGINE
        case TrayMenuId::QuickConvert:
            if (g_quickConvert) {
                g_quickConvert->Execute();
            }
            break;
#endif

        case TrayMenuId::InputTelex:
        case TrayMenuId::InputVNI:
        case TrayMenuId::InputSimpleTelex: {
            auto config = ConfigManager::LoadOrDefault();
            int method = static_cast<int>(id) - static_cast<int>(TrayMenuId::InputTelex);
            config.inputMethod = static_cast<InputMethod>(method);
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::Exit:
            TerminateAllSubprocesses();
            g_running.store(false, std::memory_order_relaxed);
            PostQuitMessage(0);
            break;

        case TrayMenuId::RestartWindows:
            RestartWindowsWithPrompt(g_trayIcon.GetMessageWindow());
            break;

        default: {
            // Code table menu items (1010-1014)
            auto rawId = static_cast<UINT>(id);
            if (rawId >= static_cast<UINT>(TrayMenuId::CodeTableUnicode) &&
                rawId <= static_cast<UINT>(TrayMenuId::CodeTableCP1258)) {
                auto ct = static_cast<CodeTable>(rawId - static_cast<UINT>(TrayMenuId::CodeTableUnicode));
#ifdef NEXUSKEY_HOOK_ENGINE
                g_hookEngine.SetCodeTable(ct);
#endif
                // Update SharedState immediately (source of truth at runtime)
                {
                    SharedState state = g_sharedState.Read();
                    if (state.IsValid()) {
                        state.codeTable = static_cast<uint8_t>(ct);
                        g_sharedState.Write(state);
                    }
                }

                // Persist to TOML for next startup
                auto config = ConfigManager::LoadOrDefault();
                config.codeTable = ct;
                (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);

                // Notify Settings dialog to refresh UI
                if (HWND settingsWnd = GetSettingsHwnd()) {
                    PostMessageW(settingsWnd, WM_NEXUSKEY_CONFIG_CHANGED, 0, 0);
                }
                NEXTKEY_LOG(L"Code table changed via tray menu: %d", static_cast<int>(ct));
            }
            break;
        }
    }
}

void SpawnSettingsSubprocess() {
    // Check if already open (single-instance)
    HWND existing = GetSettingsHwnd();
    if (existing) {
        SetForegroundWindow(existing);
#ifdef NEXUSKEY_HOOK_ENGINE
        NotifySettingsMode(g_hookEngine.IsVietnameseMode());
#endif
        return;
    }

    // Get exe path
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Build command line (pass current V/E mode to subprocess)
    wchar_t cmdLine[MAX_PATH + 64];
#ifdef NEXUSKEY_HOOK_ENGINE
    int mode = g_hookEngine.IsVietnameseMode() ? 1 : 0;
#else
    int mode = (g_sharedState.ReadFlags() & SharedFlags::VIETNAMESE_MODE) ? 1 : 0;
#endif
    swprintf_s(cmdLine, L"\"%s\" --settings --mode %d", exePath, mode);

    // Spawn subprocess
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    NEXTKEY_LOG(L"Spawning settings with cmdLine: %s", cmdLine);

    if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        TrackChildProcess(pi.hProcess);
        NEXTKEY_LOG(L"Settings subprocess spawned successfully");
    } else {
        NEXTKEY_LOG(L"Failed to spawn settings subprocess (error=%lu, path=%s)", GetLastError(), exePath);
    }
}
