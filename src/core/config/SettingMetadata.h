// NexusKey - Setting Metadata Table
// Shared mapping for dual-UI (Sciter + Classic Win32) settings binding
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/config/TypingConfig.h"
#include "core/SystemConfig.h"

namespace NextKey {

/// What kind of control this setting maps to
enum class SettingType : uint8_t {
    Toggle,    // Checkbox (bool)
    Dropdown,  // Combo box (enum)
    Action     // Button (no backing field)
};

/// Which config struct owns this field
enum class SettingOwner : uint8_t {
    Typing,  // TypingConfig
    Hotkey,  // HotkeyConfig
    System,  // SystemConfig
    UI       // UI-only (no persistent field)
};

/// Metadata for one user-configurable setting
struct SettingMeta {
    const wchar_t* id;         // Sciter DOM id (e.g. L"spell-check")
    SettingType    type;        // Toggle, Dropdown, Action
    SettingOwner   owner;       // Typing, Hotkey, System, UI
    ptrdiff_t      offset;      // offsetof(OwnerStruct, field)
    const wchar_t* label;       // Vietnamese label
    const wchar_t* labelEn;     // English label
    const wchar_t* tooltip;     // Vietnamese tooltip (nullptr = none)
    const wchar_t* tooltipEn;   // English tooltip (nullptr = none)
    uint16_t       win32Id;     // IDC_ control ID (0 = no control)
    uint8_t        tab;         // 0=Cơ bản, 1=Phím tắt, 2=Hệ thống
    uint8_t        column;      // 0=left, 1=right
};

// ── Helper macros to reduce verbosity ──────────────────────────────
#define NK_TYPING(id, field, vi, en, tip, tip_en, idc, tab, col)             \
    { L##id, ::NextKey::SettingType::Toggle, ::NextKey::SettingOwner::Typing, \
      static_cast<ptrdiff_t>(offsetof(::NextKey::TypingConfig, field)),       \
      L##vi, L##en, tip, tip_en, idc, tab, col }

#define NK_HOTKEY(id, field, vi, en, tip, tip_en, idc, tab, col)             \
    { L##id, ::NextKey::SettingType::Toggle, ::NextKey::SettingOwner::Hotkey, \
      static_cast<ptrdiff_t>(offsetof(::NextKey::HotkeyConfig, field)),       \
      L##vi, L##en, tip, tip_en, idc, tab, col }

#define NK_SYSTEM(id, field, vi, en, tip, tip_en, idc, tab, col)             \
    { L##id, ::NextKey::SettingType::Toggle, ::NextKey::SettingOwner::System, \
      static_cast<ptrdiff_t>(offsetof(::NextKey::SystemConfig, field)),        \
      L##vi, L##en, tip, tip_en, idc, tab, col }

#define NK_DROPDOWN(id, field, vi, en, tip, tip_en, idc, tab, col)           \
    { L##id, ::NextKey::SettingType::Dropdown, ::NextKey::SettingOwner::System, \
      static_cast<ptrdiff_t>(offsetof(::NextKey::SystemConfig, field)),        \
      L##vi, L##en, tip, tip_en, idc, tab, col }

#define NK_ACTION(id, vi, en, tip, tip_en, idc, tab, col)                    \
    { L##id, ::NextKey::SettingType::Action, ::NextKey::SettingOwner::UI,      \
      0,                                                                       \
      L##vi, L##en, tip, tip_en, idc, tab, col }

/// All toggle settings, ordered by tab → column → visual position
inline constexpr SettingMeta kSettings[] = {

    // ── Tab 0: Bảng gõ — Left column (col 0) ──
    NK_TYPING("modern-ortho",         modernOrtho,
              "Đặt dấu oà, uý",       "Modern tone placement",
              nullptr, nullptr,                                          2202, 0, 0),
    NK_TYPING("allow-english-bypass", allowEnglishBypass,
              "Gõ tự do",             "Bypass English blocking",
              L"Cho phép đặt dấu tiếng Việt trên các từ tiếng Anh hoặc ngoại lệ",
              L"Allow Vietnamese tones on English words or exceptions",  2208, 0, 0),
    NK_TYPING("auto-caps",            autoCaps,
              "Viết hoa chữ cái đầu", "Auto capitalize",
              nullptr, nullptr,                                          2203, 0, 0),
    NK_TYPING("spell-check",          spellCheckEnabled,
              "Kiểm tra chính tả",    "Spell check",
              nullptr, nullptr,                                          2201, 0, 0),
    NK_TYPING("allow-zwjf",           allowZwjf,
              "Cho phép z, w, j, f",  "Allow z, w, j, f",
              L"Mặc định đã cho phép. Chỉ cần bật khi kiểm tra chính tả được bật",
              L"Allowed by default. Only needed when spell check is on",
                                                                         2204, 0, 0),
    NK_TYPING("restore-key",          autoRestoreEnabled,
              "Tự khôi phục phím sai","Restore key on invalid",
              nullptr, nullptr,                                          2205, 0, 0),

    // ── Tab 0: Bảng gõ — Right column (col 1) ──
    NK_TYPING("beep-sound",           beepOnSwitch,
              "Tiếng bíp khi chuyển",   "Beep on switch",
              L"Phát âm báo khi chuyển đổi ngôn ngữ",
              L"Play sound when switching language",                     2211, 0, 1),
    NK_TYPING("temp-off-openkey",     tempOffByAlt,
              "Tạm tắt bộ gõ bằng Alt",    "Alt temps off Vietnamese",
              L"Nhấn đúp phím Alt để tạm tắt tiếng Việt (tránh xung đột menu ứng dụng)",
              L"Double-tap Alt to disable Vietnamese (avoid app menu conflicts)", 2218, 0, 1),
    NK_TYPING("smart-switch",         smartSwitch,
              "Lưu chế độ gõ theo app",   "Smart input switch",
              L"Tự động ghi nhớ chế độ gõ cho từng ứng dụng",
              L"Auto-remember input mode per app",                       2206, 0, 1),
    NK_TYPING("exclude-apps",         excludeApps,
              "Loại trừ ứng dụng",        "Exclude apps",
              L"Tắt gõ tiếng Việt cho ứng dụng trong danh sách",
              L"Disable Vietnamese for listed apps",                     2207, 0, 1),
    NK_ACTION("btn-exclude-apps",     "...", "",
              nullptr, nullptr,                                          2502, 0, 1),
    NK_TYPING("auto-off-by-url",      autoOffByUrl,
              "Tắt TV tại thanh địa chỉ trình duyệt", "Auto-off on address bar",
              L"Tự động tắt tiếng Việt khi nhập địa chỉ trình duyệt (Chrome, Edge...)",
              L"Automatically disable Vietnamese when typing in browser address bars", 2221, 0, 1),
    // ── Action buttons (grouped at bottom) ──
    NK_ACTION("btn-app-overrides",    "Cấu hình từng ứng dụng", "Per-app config",
              nullptr, nullptr,                                          2500, 0, 1),
    NK_ACTION("btn-spell-exclusions", "Loại trừ chính tả...", "Spell exclusions...",
              nullptr, nullptr,                                          2801, 0, 1),

    // ── Tab 1: Gõ tắt (col 0) ──
    NK_TYPING("use-macro",            macroEnabled,
              "Cho phép gõ tắt",      "Enable macros",
              nullptr, nullptr,                                          2212, 1, 0),
    NK_TYPING("macro-english",        macroInEnglish,
              "Gõ tắt khi tắt tiếng việt", "Macros in English mode",
              nullptr, nullptr,                                          2213, 1, 0),
    NK_TYPING("auto-caps-macro",      autoCapsMacro,
              "Tự động viết hoa theo phím", "Auto capitalize macros",
              nullptr, nullptr,                                          2219, 1, 0),
    NK_TYPING("cancel-macro-esc",     tempOffMacroByEsc,
              "Tạm bỏ gõ tắt bằng Esc",     "Temp skip macro by Esc",
              L"Nhấn Esc trước khi gõ để tạm bỏ qua gõ tắt cho từ tiếp theo",
              L"Press Esc before typing to skip macro for the next word", 2220, 1, 0),
    NK_ACTION("btn-macro-table",      "Bảng gõ tắt", "Macro Table",
              nullptr, nullptr,                                          2503, 1, 0),

    // ── Hotkeys (rendered in Compact mode, logically attached to Settings) ──
    NK_HOTKEY("key-ctrl",             ctrl,
              "Ctrl",                  "Ctrl",
              nullptr, nullptr,                                          2301, 1, 0),
    NK_HOTKEY("key-shift",            shift,
              "Shift",                 "Shift",
              nullptr, nullptr,                                          2302, 1, 0),
    NK_HOTKEY("key-alt",              alt,
              "Alt",                   "Alt",
              nullptr, nullptr,                                          2303, 1, 0),
    NK_HOTKEY("key-win",              win,
              "Win",                   "Win",
              nullptr, nullptr,                                          2304, 1, 0),

    // ── Tab 1: Gõ tắt (col 1) ──
    NK_TYPING("quick-telex",          quickConsonant,
              "Gõ nhanh phụ âm kép",  "Quick double consonant",
              L"cc=ch, gg=gi, kk=kh, nn=ng, qq=qu, pp=ph, tt=th, uu=\u01B0\u01A1",
              L"cc=ch, gg=gi, kk=kh, nn=ng, qq=qu, pp=ph, tt=th, uu=\u01B0\u01A1", 2214, 1, 1),
    NK_TYPING("quick-start",          quickStartConsonant,
              "Gõ tắt phụ âm đầu",    "Quick start consonant",
              L"f\u2192ph, j\u2192gi, w\u2192qu",
              L"f\u2192ph, j\u2192gi, w\u2192qu",                       2215, 1, 1),
    NK_TYPING("quick-end",            quickEndConsonant,
              "Gõ tắt phụ âm cuối",   "Quick end consonant",
              L"g\u2192ng, h\u2192nh, k\u2192ch",
              L"g\u2192ng, h\u2192nh, k\u2192ch",                       2216, 1, 1),

    // ── Tab 2: Hệ thống — Left column (col 0) ──
    NK_SYSTEM("run-startup",          runAtStartup,
              "Khởi động cùng Windows", "Run at startup",
              nullptr, nullptr,                                          2401, 2, 0),
    NK_DROPDOWN("startup-mode",       startupMode,
              "Chế độ mặc định",      "Default mode",
              L"Chế độ gõ khi khởi động ứng dụng",
              L"Typing mode when app starts",                            2409, 2, 0),
    NK_SYSTEM("show-on-startup",      showOnStartup,
              "Bật bảng này khi khởi động", "Show window on startup",
              nullptr, nullptr,                                          2403, 2, 0),
    NK_SYSTEM("run-admin",            runAsAdmin,
              "Chạy với quyền admin",  "Run as admin",
              nullptr, nullptr,                                          2402, 2, 0),
    NK_SYSTEM("desktop-shortcut",     desktopShortcut,
              "Tạo biểu tượng desktop","Desktop shortcut",
              nullptr, nullptr,                                          2404, 2, 0),

    NK_SYSTEM("english-ui",           language,
              "Giao diện tiếng Anh",  "English interface",
              L"Chuyển menu, thông báo sang tiếng Anh",
              L"Switch menus, notifications to English",            2407, 2, 0),

    // ── Tab 2: Hệ thống — Right column (col 1) ──
    NK_SYSTEM("floating-icon",        showFloatingIcon,
              "Icon V/E nổi",          "Floating icon",
              L"Hiện icon V/E nhỏ luôn nổi trên màn hình, hữu ích khi dùng app toàn màn hình",
              L"Show floating V/E indicator on screen, useful for fullscreen apps", 2405, 2, 1),
    NK_DROPDOWN("custom-icon-style",  iconStyle,
              "Tuỳ chỉnh icon",       "Icon Style",
              nullptr, nullptr,                                          2408, 2, 1),
    NK_SYSTEM("check-update",         autoCheckUpdate,
              "Tự kiểm tra cập nhật", "Auto check update",
              L"Tự động kiểm tra phiên bản mới khi khởi động ứng dụng",
              L"Auto-check for new version on startup",                  2406, 2, 1),
    NK_ACTION("btn-check-update",     "Kiểm tra", "Check",
              nullptr, nullptr,                                          2504, 2, 1),
    NK_SYSTEM("force-light-theme",    forceLightTheme,
              "Luôn dùng giao diện sáng", "Always use light theme",
              L"Bỏ qua chế độ tối của Windows, luôn hiển thị giao diện sáng",
              L"Ignore Windows dark mode, always show light theme",     2410, 2, 1),
};

#undef NK_TYPING
#undef NK_HOTKEY
#undef NK_SYSTEM
#undef NK_DROPDOWN
#undef NK_ACTION

/// Total number of entries
inline constexpr size_t kSettingsCount = sizeof(kSettings) / sizeof(kSettings[0]);

// ── Lookup functions ───────────────────────────────────────────────

/// Find setting by Sciter DOM id string. Returns nullptr if not found.
[[nodiscard]] inline constexpr const SettingMeta* FindSetting(const wchar_t* id) noexcept {
    for (size_t i = 0; i < kSettingsCount; ++i) {
        // constexpr-friendly wchar_t comparison
        const wchar_t* a = kSettings[i].id;
        const wchar_t* b = id;
        bool match = true;
        while (*a || *b) {
            if (*a != *b) { match = false; break; }
            ++a; ++b;
        }
        if (match) return &kSettings[i];
    }
    return nullptr;
}

/// Find setting by Win32 control ID. Returns nullptr if not found.
[[nodiscard]] inline constexpr const SettingMeta* FindSettingByControlId(uint16_t controlId) noexcept {
    if (controlId == 0) return nullptr;
    for (size_t i = 0; i < kSettingsCount; ++i) {
        if (kSettings[i].win32Id == controlId) return &kSettings[i];
    }
    return nullptr;
}

}  // namespace NextKey
