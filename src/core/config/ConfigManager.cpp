// NexusKey - Configuration Manager Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ConfigManager.h"
#include "core/Debug.h"

#define TOML_HEADER_ONLY 1
#include "toml.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>

#ifdef _WIN32
#include "core/WinStrings.h"
#include <ShlObj.h>
#endif

namespace NextKey {

namespace {

#ifdef _WIN32
/// Named mutex to serialize TOML read-modify-write across processes.
/// Prevents race where Settings deferred save overwrites Macro/ExcludedApps changes.
class ConfigFileLock {
public:
    ConfigFileLock() noexcept {
        hMutex_ = CreateMutexW(nullptr, FALSE, L"Local\\NexusKeyConfigLock");
        if (hMutex_) {
            DWORD result = WaitForSingleObject(hMutex_, 5000);
            // WAIT_OBJECT_0: acquired normally
            // WAIT_ABANDONED: previous owner crashed — we now own it
            // WAIT_TIMEOUT/WAIT_FAILED: proceed without lock (better than blocking user)
            owned_ = (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED);
        }
    }
    ~ConfigFileLock() noexcept {
        if (hMutex_) {
            if (owned_) ReleaseMutex(hMutex_);
            CloseHandle(hMutex_);
        }
    }
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;
private:
    HANDLE hMutex_ = nullptr;
    bool owned_ = false;
};
#else
struct ConfigFileLock {};  // No-op on Linux (test builds)
#endif

/// Load existing TOML file or return empty table (for merge-and-save pattern)
toml::table LoadExistingToml(const std::string& utf8Path) {
    try { return toml::parse_file(utf8Path); } catch (...) { return {}; }
}

/// Write TOML table to file atomically
bool WriteToml(const std::string& utf8Path, const toml::table& tbl) {
    std::string tempPath = utf8Path + ".tmp";
    std::ofstream file(tempPath);
    if (!file.is_open()) return false;
    file << tbl;
    file.close();
    if (file.fail()) return false;

#ifdef _WIN32
    std::wstring wDest = Utf8ToWide(utf8Path);
    std::wstring wTemp = Utf8ToWide(tempPath);
    if (!MoveFileExW(wTemp.c_str(), wDest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wTemp.c_str());
        return false;
    }
#else
    if (std::rename(tempPath.c_str(), utf8Path.c_str()) != 0) {
        std::remove(tempPath.c_str());
        return false;
    }
#endif
    return true;
}

// Security limits — shared across all Load* functions
constexpr uintmax_t kMaxConfigFileSizeBytes = 5 * 1024 * 1024;  // 5 MB
constexpr size_t    kMaxMacroKeyLen         = 32;
constexpr size_t    kMaxMacroValueLen       = 20480;
constexpr size_t    kMaxPerAppEntries       = 256;
constexpr size_t    kMaxAppListEntries      = 1000;

}  // namespace

std::optional<TypingConfig> ConfigManager::LoadFromFile(const std::wstring& path) {
    // Guard: reject config files larger than 5 MB to prevent OOM via crafted macros
    std::error_code sizeEc;
    auto fileSize = std::filesystem::file_size(path, sizeEc);
    if (sizeEc || fileSize > kMaxConfigFileSizeBytes) {
        NEXTKEY_LOG(L"[ConfigManager] Config file too large or unreadable (%llu bytes), using defaults",
                    sizeEc ? 0ULL : static_cast<unsigned long long>(fileSize));
        return std::nullopt;
    }

    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);
        
        TypingConfig config;
        
        // [input] section
        if (auto input = table["input"].as_table()) {
            if (auto method = input->get("method")) {
                std::string methodStr = method->value_or<std::string>("telex");
                if (methodStr == "vni") {
                    config.inputMethod = InputMethod::VNI;
                } else if (methodStr == "simple_telex") {
                    config.inputMethod = InputMethod::SimpleTelex;
                } else if (methodStr == "combined") {
                    config.inputMethod = InputMethod::Combined;
                } else {
                    config.inputMethod = InputMethod::Telex;
                }
            }
            if (auto ct = input->get("code_table")) {
                auto val = ct->value_or(0);
                if (val >= 0 && val <= 4) {
                    config.codeTable = static_cast<CodeTable>(val);
                }
            }
        }

        // [features] section — use node_view [] operator for safe access to optional keys
        if (auto features = table["features"].as_table()) {
            config.spellCheckEnabled = (*features)["spell_check"].value_or(false);
            config.beepOnSwitch = (*features)["beep_on_switch"].value_or(false);
            config.smartSwitch = (*features)["smart_switch"].value_or(false);
            config.excludeApps = (*features)["exclude_apps"].value_or(false);
            config.tsfApps = (*features)["tsf_apps"].value_or(false);
            config.optimizeLevel = static_cast<uint8_t>(
                (*features)["optimize_level"].value_or(0)
            );
            config.modernOrtho = (*features)["modern_ortho"].value_or(false);
            config.autoCaps = (*features)["auto_caps"].value_or(false);
            config.allowZwjf = (*features)["allow_zwjf"].value_or(false);
            config.autoRestoreEnabled = (*features)["auto_restore"].value_or(false);
            config.tempOffByAlt = (*features)["temp_off_by_alt"].value_or(false);
            config.macroEnabled = (*features)["macro_enabled"].value_or(false);
            config.macroInEnglish = (*features)["macro_in_english"].value_or(false);
            config.quickConsonant = (*features)["quick_consonant"].value_or(false);
            config.quickStartConsonant = (*features)["quick_start_consonant"].value_or(false);
            config.quickEndConsonant = (*features)["quick_end_consonant"].value_or(false);
            config.tempOffMacroByEsc = (*features)["temp_off_macro_esc"].value_or(false);
            config.autoCapsMacro = (*features)["auto_caps_macro"].value_or(false);
            config.autoOffByUrl = (*features)["auto_off_by_url"].value_or(false);
            config.allowEnglishBypass = (*features)["allow_english_bypass"].value_or(false);
            config.macroTriggerSpace = (*features)["macro_trigger_space"].value_or(true);
            config.macroTriggerEnter = (*features)["macro_trigger_enter"].value_or(true);
            config.macroTriggerTab = (*features)["macro_trigger_tab"].value_or(true);
            config.macroTriggerDir = (*features)["macro_trigger_dir"].value_or(true);
            // Spell exclusion prefixes (e.g. ["hđ", "đp"]) — stored pre-lowercased
            if (auto arr = (*features)["spell_exclusions"].as_array()) {
                for (auto& item : *arr) {
                    if (auto str = item.value<std::string>()) {
                        auto wide = Utf8ToWide(*str);
                        if (wide.size() >= 2) {
                            for (auto& ch : wide) ch = towlower(ch);
                            config.spellExclusions.push_back(std::move(wide));
                        }
                    }
                }
            }
        }

        return config;
    } catch (const toml::parse_error&) {
        // Fall through to return nullopt
    } catch (const std::exception&) {
        // Fall through to return nullopt
    }
    return std::nullopt;
}

bool ConfigManager::SaveToFile(const std::wstring& path, const TypingConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table input;
        const char* methodStr = "telex";
        if (config.inputMethod == InputMethod::VNI) methodStr = "vni";
        else if (config.inputMethod == InputMethod::SimpleTelex) methodStr = "simple_telex";
        else if (config.inputMethod == InputMethod::Combined) methodStr = "combined";
        input.insert_or_assign("method", methodStr);
        input.insert_or_assign("code_table", static_cast<int64_t>(config.codeTable));
        tbl.insert_or_assign("input", std::move(input));

        // Update [features] section
        toml::table features;
        features.insert_or_assign("spell_check", config.spellCheckEnabled);
        features.insert_or_assign("beep_on_switch", config.beepOnSwitch);
        features.insert_or_assign("smart_switch", config.smartSwitch);
        features.insert_or_assign("exclude_apps", config.excludeApps);
        features.insert_or_assign("tsf_apps", config.tsfApps);
        features.insert_or_assign("optimize_level", static_cast<int64_t>(config.optimizeLevel));
        features.insert_or_assign("modern_ortho", config.modernOrtho);
        features.insert_or_assign("auto_caps", config.autoCaps);
        features.insert_or_assign("allow_zwjf", config.allowZwjf);
        features.insert_or_assign("auto_restore", config.autoRestoreEnabled);
        features.insert_or_assign("temp_off_by_alt", config.tempOffByAlt);
        features.insert_or_assign("macro_enabled", config.macroEnabled);
        features.insert_or_assign("macro_in_english", config.macroInEnglish);
        features.insert_or_assign("quick_consonant", config.quickConsonant);
        features.insert_or_assign("quick_start_consonant", config.quickStartConsonant);
        features.insert_or_assign("quick_end_consonant", config.quickEndConsonant);
        features.insert_or_assign("temp_off_macro_esc", config.tempOffMacroByEsc);
        features.insert_or_assign("auto_caps_macro", config.autoCapsMacro);
        features.insert_or_assign("auto_off_by_url", config.autoOffByUrl);
        features.insert_or_assign("allow_english_bypass", config.allowEnglishBypass);
        features.insert_or_assign("macro_trigger_space", config.macroTriggerSpace);
        features.insert_or_assign("macro_trigger_enter", config.macroTriggerEnter);
        features.insert_or_assign("macro_trigger_tab", config.macroTriggerTab);
        features.insert_or_assign("macro_trigger_dir", config.macroTriggerDir);
        // Spell exclusion prefixes
        toml::array exclArr;
        for (const auto& excl : config.spellExclusions) {
            if (excl.size() >= 2) {
                exclArr.push_back(WideToUtf8(excl));
            }
        }
        features.insert_or_assign("spell_exclusions", std::move(exclArr));
        tbl.insert_or_assign("features", std::move(features));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::wstring ConfigManager::GetConfigPath() {
    // Primary: exe directory
    std::wstring exeDir = GetExeDirectory();
    if (DirectoryWritable(exeDir)) {
        return exeDir + L"\\config.toml";
    }
    
    // Fallback: %APPDATA%/NexusKey/
    std::wstring appDataDir = GetAppDataDirectory();
    return appDataDir + L"\\config.toml";
}

TypingConfig ConfigManager::LoadOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadFromFile(configPath);
    if (config) {
        return *config;
    }
    
    // Return compiled defaults (FR8 - engine autonomy)
    return TypingConfig{};
}

std::wstring ConfigManager::GetExeDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len > 0) {
        std::wstring fullPath(path);
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return fullPath.substr(0, lastSlash);
        }
    }
#endif
    return L".";
}

std::wstring ConfigManager::GetAppDataDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::wstring appData(path);
        std::wstring nexusKeyDir = appData + L"\\NexusKey";
        
        // Create directory if it doesn't exist
        CreateDirectoryW(nexusKeyDir.c_str(), nullptr);
        return nexusKeyDir;
    }
#endif
    return L".";
}

bool ConfigManager::DirectoryWritable(const std::wstring& path) {
#ifdef _WIN32
    // Try to create a temp file
    std::wstring testFile = path + L"\\__nexuskey_test_write__.tmp";
    HANDLE hFile = CreateFileW(
        testFile.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr
    );
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        return true;
    }
#endif
    return false;
}

std::optional<UIConfig> ConfigManager::LoadUIConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        UIConfig config;

        // [ui] section — use [] for safe access to optional keys
        if (auto ui = table["ui"].as_table()) {
            config.showAdvanced = (*ui)["show_advanced"].value_or(false);
            config.backgroundOpacity = static_cast<uint8_t>(
                (*ui)["background_opacity"].value_or(80)
            );
            config.pinned = (*ui)["pinned"].value_or(false);
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveUIConfig(const std::wstring& path, const UIConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table ui;
        ui.insert_or_assign("show_advanced", config.showAdvanced);
        ui.insert_or_assign("background_opacity", static_cast<int64_t>(config.backgroundOpacity));
        ui.insert_or_assign("pinned", config.pinned);
        tbl.insert_or_assign("ui", std::move(ui));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

UIConfig ConfigManager::LoadUIConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadUIConfig(configPath);
    if (config) {
        return *config;
    }
    return UIConfig{};
}

std::optional<HotkeyConfig> ConfigManager::LoadHotkeyConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        HotkeyConfig config;

        if (auto hotkey = table["hotkey"].as_table()) {
            config.ctrl = (*hotkey)["ctrl"].value_or(true);
            config.shift = (*hotkey)["shift"].value_or(true);
            config.alt = (*hotkey)["alt"].value_or(false);
            config.win = (*hotkey)["win"].value_or(false);

            auto keyStr = (*hotkey)["key"].value_or<std::string>("");
            if (!keyStr.empty()) {
                auto wideKey = Utf8ToWide(keyStr);
                config.key = wideKey.empty() ? 0 : towupper(wideKey[0]);
            } else {
                config.key = 0;
            }
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveHotkeyConfig(const std::wstring& path, const HotkeyConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table hotkey;
        hotkey.insert_or_assign("ctrl", config.ctrl);
        hotkey.insert_or_assign("shift", config.shift);
        hotkey.insert_or_assign("alt", config.alt);
        hotkey.insert_or_assign("win", config.win);

        if (config.key != 0) {
            std::wstring wkey(1, config.key);
            hotkey.insert_or_assign("key", WideToUtf8(wkey));
        } else {
            hotkey.insert_or_assign("key", "");
        }

        tbl.insert_or_assign("hotkey", std::move(hotkey));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

HotkeyConfig ConfigManager::LoadHotkeyConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadHotkeyConfig(configPath);
    if (config) {
        return *config;
    }
    return HotkeyConfig{};  // Default: Ctrl+Shift
}

std::vector<std::wstring> ConfigManager::LoadAllExcludedApps(const std::wstring& path) {
    std::vector<std::wstring> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        if (auto section = table["excluded_apps"].as_table()) {
            // Load [excluded_apps].list
            if (auto arr = (*section)["list"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        apps.push_back(Utf8ToWide(*str));
                    }
                }
            }
            // Backward compat: merge [excluded_apps].soft into the same list
            if (auto arr = (*section)["soft"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        auto wide = Utf8ToWide(*str);
                        if (std::find(apps.begin(), apps.end(), wide) == apps.end()) {
                            apps.push_back(std::move(wide));
                        }
                    }
                }
            }
        }
    } catch (...) {}
    return apps;
}

bool ConfigManager::SaveExcludedApps(const std::wstring& path,
                                      const std::vector<std::wstring>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        for (auto& app : apps) {
            arr.push_back(WideToUtf8(app));
        }
        toml::table section;
        section.insert_or_assign("list", std::move(arr));
        tbl.insert_or_assign("excluded_apps", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::vector<std::wstring> ConfigManager::LoadEnglishModeApps(const std::wstring& path) {
    std::vector<std::wstring> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        if (auto section = table["smart_switch"].as_table()) {
            if (auto arr = (*section)["english_mode_apps"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        apps.push_back(Utf8ToWide(*str));
                    }
                }
            }
        }
    } catch (...) {}
    return apps;
}

bool ConfigManager::SaveEnglishModeApps(const std::wstring& path,
                                         const std::vector<std::wstring>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        for (auto& app : apps) {
            arr.push_back(WideToUtf8(app));
        }
        toml::table section;
        section.insert_or_assign("english_mode_apps", std::move(arr));
        tbl.insert_or_assign("smart_switch", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::vector<std::wstring> ConfigManager::LoadTsfApps(const std::wstring& path) {
    std::vector<std::wstring> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        if (auto section = table["tsf_apps"].as_table()) {
            if (auto arr = (*section)["list"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        apps.push_back(Utf8ToWide(*str));
                    }
                }
            }
        }
    } catch (...) {}
    return apps;
}

bool ConfigManager::SaveTsfApps(const std::wstring& path, const std::vector<std::wstring>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        for (auto& app : apps) {
            arr.push_back(WideToUtf8(app));
        }
        toml::table section;
        section.insert_or_assign("list", std::move(arr));
        tbl.insert_or_assign("tsf_apps", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::unordered_map<std::wstring, AppOverrideEntry> ConfigManager::LoadAppOverrides(const std::wstring& path) {
    std::unordered_map<std::wstring, AppOverrideEntry> data;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        size_t entryCount = 0;

        if (auto section = table["app_overrides"].as_table()) {
            for (auto& [key, val] : *section) {
                if (++entryCount > kMaxPerAppEntries) {
                    NEXTKEY_LOG(L"[ConfigManager] Per-app table exceeds limit (%zu), truncating",
                                kMaxPerAppEntries);
                    break;
                }
                if (auto* entry = val.as_table()) {
                    AppOverrideEntry e;
                    e.inputMethod = static_cast<int8_t>((*entry)["input_method"].value_or(-1));
                    e.encodingOverride = static_cast<int8_t>((*entry)["encoding"].value_or(-1));
                    data[Utf8ToWide(std::string(key.str()))] = e;
                }
            }
        }
    } catch (...) {}
    return data;
}

bool ConfigManager::SaveAppOverrides(const std::wstring& path,
                                      const std::unordered_map<std::wstring, AppOverrideEntry>& entries) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table section;
        for (auto& [exe, e] : entries) {
            toml::table entry;
            entry.insert_or_assign("input_method", static_cast<int64_t>(e.inputMethod));
            entry.insert_or_assign("encoding", static_cast<int64_t>(e.encodingOverride));
            section.insert_or_assign(WideToUtf8(exe), std::move(entry));
        }
        tbl.insert_or_assign("app_overrides", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::optional<SystemConfig> ConfigManager::LoadSystemConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        SystemConfig config;

        if (auto system = table["system"].as_table()) {
            config.runAtStartup = (*system)["run_at_startup"].value_or(false);
            config.runAsAdmin = (*system)["run_as_admin"].value_or(false);
            config.showOnStartup = (*system)["show_on_startup"].value_or(false);
            config.desktopShortcut = (*system)["desktop_shortcut"].value_or(false);
            config.language = static_cast<uint8_t>((*system)["language"].value_or(0));
            config.iconStyle = static_cast<uint8_t>((*system)["icon_style"].value_or(0));
            config.customColorV = static_cast<uint32_t>((*system)["custom_color_v"].value_or(int64_t(0)));
            config.customColorE = static_cast<uint32_t>((*system)["custom_color_e"].value_or(int64_t(0)));
            config.showFloatingIcon = (*system)["show_floating_icon"].value_or(false);
            config.floatingIconX = static_cast<int32_t>((*system)["floating_icon_x"].value_or(int64_t(INT32_MIN)));
            config.floatingIconY = static_cast<int32_t>((*system)["floating_icon_y"].value_or(int64_t(INT32_MIN)));
            config.autoCheckUpdate = (*system)["auto_check_update"].value_or(true);
            config.startupMode = static_cast<uint8_t>((*system)["startup_mode"].value_or(0));
            config.forceLightTheme = (*system)["force_light_theme"].value_or(false);
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveSystemConfig(const std::wstring& path, const SystemConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table system;
        system.insert_or_assign("run_at_startup", config.runAtStartup);
        system.insert_or_assign("run_as_admin", config.runAsAdmin);
        system.insert_or_assign("show_on_startup", config.showOnStartup);
        system.insert_or_assign("desktop_shortcut", config.desktopShortcut);
        system.insert_or_assign("language", static_cast<int64_t>(config.language));
        system.insert_or_assign("icon_style", static_cast<int64_t>(config.iconStyle));
        system.insert_or_assign("custom_color_v", static_cast<int64_t>(config.customColorV));
        system.insert_or_assign("custom_color_e", static_cast<int64_t>(config.customColorE));
        system.insert_or_assign("show_floating_icon", config.showFloatingIcon);
        system.insert_or_assign("floating_icon_x", static_cast<int64_t>(config.floatingIconX));
        system.insert_or_assign("floating_icon_y", static_cast<int64_t>(config.floatingIconY));
        system.insert_or_assign("auto_check_update", config.autoCheckUpdate);
        system.insert_or_assign("startup_mode", static_cast<int64_t>(config.startupMode));
        system.insert_or_assign("force_light_theme", config.forceLightTheme);
        tbl.insert_or_assign("system", std::move(system));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

SystemConfig ConfigManager::LoadSystemConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadSystemConfig(configPath);
    if (config) {
        return *config;
    }
    return SystemConfig{};
}

std::optional<ConvertConfig> ConfigManager::LoadConvertConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        ConvertConfig config;

        if (auto convert = table["convert"].as_table()) {
            config.allCaps = (*convert)["all_caps"].value_or(false);
            config.allLower = (*convert)["all_lower"].value_or(false);
            config.capsFirst = (*convert)["caps_first"].value_or(false);
            config.capsEach = (*convert)["caps_each"].value_or(false);
            config.removeMark = (*convert)["remove_mark"].value_or(false);
            config.alertDone = (*convert)["alert_done"].value_or(false);
            config.autoPaste = (*convert)["auto_paste"].value_or(false);
            config.sequential = (*convert)["sequential"].value_or(false);
            config.enableLog = (*convert)["enable_log"].value_or(false);
            config.sourceEncoding = static_cast<uint8_t>(
                (*convert)["source_encoding"].value_or(0));
            config.destEncoding = static_cast<uint8_t>(
                (*convert)["dest_encoding"].value_or(0));

            // Nested [convert.hotkey] table
            if (auto hk = (*convert)["hotkey"].as_table()) {
                config.hotkey.ctrl = (*hk)["ctrl"].value_or(false);
                config.hotkey.shift = (*hk)["shift"].value_or(false);
                config.hotkey.alt = (*hk)["alt"].value_or(false);
                config.hotkey.win = (*hk)["win"].value_or(false);

                auto keyStr = (*hk)["key"].value_or<std::string>("");
                if (!keyStr.empty()) {
                    auto wideKey = Utf8ToWide(keyStr);
                    config.hotkey.key = wideKey.empty() ? 0 : towupper(wideKey[0]);
                }
            }
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveConvertConfig(const std::wstring& path, const ConvertConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table convert;
        convert.insert_or_assign("all_caps", config.allCaps);
        convert.insert_or_assign("all_lower", config.allLower);
        convert.insert_or_assign("caps_first", config.capsFirst);
        convert.insert_or_assign("caps_each", config.capsEach);
        convert.insert_or_assign("remove_mark", config.removeMark);
        convert.insert_or_assign("alert_done", config.alertDone);
        convert.insert_or_assign("auto_paste", config.autoPaste);
        convert.insert_or_assign("sequential", config.sequential);
        convert.insert_or_assign("enable_log", config.enableLog);
        convert.insert_or_assign("source_encoding", static_cast<int64_t>(config.sourceEncoding));
        convert.insert_or_assign("dest_encoding", static_cast<int64_t>(config.destEncoding));

        toml::table hotkey;
        hotkey.insert_or_assign("ctrl", config.hotkey.ctrl);
        hotkey.insert_or_assign("shift", config.hotkey.shift);
        hotkey.insert_or_assign("alt", config.hotkey.alt);
        hotkey.insert_or_assign("win", config.hotkey.win);

        if (config.hotkey.key != 0) {
            std::wstring wkey(1, config.hotkey.key);
            hotkey.insert_or_assign("key", WideToUtf8(wkey));
        } else {
            hotkey.insert_or_assign("key", "");
        }

        convert.insert_or_assign("hotkey", std::move(hotkey));
        tbl.insert_or_assign("convert", std::move(convert));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

ConvertConfig ConfigManager::LoadConvertConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadConvertConfig(configPath);
    if (config) {
        return *config;
    }
    return ConvertConfig{};
}

std::unordered_map<std::wstring, std::wstring> ConfigManager::LoadMacros(const std::wstring& path) {
    std::unordered_map<std::wstring, std::wstring> data;
    // Guard: reject files larger than 1 MB — a large macro section triggers OOM when
    // HookEngine allocates SendInput arrays proportional to the expansion length.
    std::error_code sizeEc;
    auto fileSize = std::filesystem::file_size(path, sizeEc);
    if (sizeEc || fileSize > kMaxConfigFileSizeBytes) {
        NEXTKEY_LOG(L"[ConfigManager] Macro file too large or unreadable (%llu bytes), skipping macros",
                    sizeEc ? 0ULL : static_cast<unsigned long long>(fileSize));
        return data;
    }
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = toml::parse_file(utf8Path);

        if (auto section = table["macros"].as_table()) {
            for (auto& [key, val] : *section) {
                auto str = val.value<std::string>();
                if (!str) continue;

                if (key.str().size() > kMaxMacroKeyLen || str->size() > kMaxMacroValueLen) {
                    NEXTKEY_LOG(L"[ConfigManager] Macro entry too long, skipping (key=%zu, value=%zu)",
                                key.str().size(), str->size());
                    continue;
                }
                data[Utf8ToWide(std::string(key.str()))] = Utf8ToWide(*str);
            }
        }
    } catch (...) {}
    return data;
}

bool ConfigManager::SaveMacros(const std::wstring& path,
                                const std::unordered_map<std::wstring, std::wstring>& macros) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table section;
        for (auto& [key, value] : macros) {
            section.insert_or_assign(WideToUtf8(key), WideToUtf8(value));
        }
        tbl.insert_or_assign("macros", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

}  // namespace NextKey
