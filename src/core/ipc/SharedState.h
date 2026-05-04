// NexusKey - SharedState Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "core/config/TypingConfig.h"

namespace NextKey {

// Flag bit definitions (must be before SharedState)
namespace SharedFlags {
    constexpr uint32_t VIETNAMESE_MODE = 0x0001;
    constexpr uint32_t ENGINE_ENABLED  = 0x0002;
    constexpr uint32_t SPELL_CHECK     = 0x0004;
    constexpr uint32_t TSF_ACTIVE      = 0x0008;  // Foreground app uses TSF engine (hook sets, DLL reads)
    constexpr uint32_t TSF_READONLY    = 0x0010;  // Hook active, TSF sinks doc events + pushes contextAnchor
    // Restart-banner triggers — set cross-process by main EXE / TSF DLL.
    constexpr uint32_t TSF_ABI_MISMATCH       = 0x0020;  // DLL: mapped SharedState layout doesn't match this DLL
    constexpr uint32_t TSF_PENDING_DLL_SWAP   = 0x0040;  // EXE: startup swap failed, reboot needed
    constexpr uint32_t TSF_POST_UPDATE_REBOOT = 0x0080;  // EXE: swap succeeded, hosts may still hold old DLL
}

// Feature flag bit definitions (uint32_t packed into 3 bytes: featureFlags[2] + extFeatureFlags)
namespace FeatureFlags {
    // Byte 0 (bits 0-7)
    constexpr uint16_t MODERN_ORTHO         = 0x0001;
    constexpr uint16_t AUTO_CAPS            = 0x0002;
    constexpr uint16_t ALLOW_ZWJF           = 0x0004;
    constexpr uint16_t AUTO_RESTORE         = 0x0008;
    constexpr uint16_t TEMP_OFF_SPELL_CTRL  = 0x0010;
    constexpr uint16_t TEMP_OFF_BY_ALT      = 0x0040;
    constexpr uint16_t BEEP_ON_SWITCH      = 0x0080;
    // Byte 1 (bits 8-15)
    constexpr uint16_t MACRO_ENABLED        = 0x0100;
    constexpr uint16_t MACRO_IN_ENGLISH     = 0x0200;
    constexpr uint16_t QUICK_CONSONANT      = 0x0400;
    constexpr uint16_t QUICK_START_CONSONANT = 0x0800;
    constexpr uint16_t QUICK_END_CONSONANT   = 0x1000;
    constexpr uint16_t TEMP_OFF_MACRO_ESC    = 0x2000;
    constexpr uint16_t SMART_SWITCH          = 0x4000;
    constexpr uint16_t EXCLUDE_APPS          = 0x8000;
    // Extended flags (byte 2, bits 16-23) — stored in extFeatureFlags
    constexpr uint32_t AUTO_CAPS_MACRO       = 0x00010000;
    constexpr uint32_t ALLOW_ENGLISH_BYPASS  = 0x00020000;
    constexpr uint32_t AUTO_OFF_BY_URL       = 0x00040000;
}

/// Document context anchor published by TSF (readonly mode) for HookEngine.
/// Written per-OnEndEdit by the TSF DLL running in the foreground process;
/// read by HookEngine in its ProcessKeyDown path.
///
/// Uses its own seqlock (generation) independent of SharedState.epoch — TSF writes
/// anchor without disturbing config readers. SharedStateManager::Write() MUST NOT
/// copy this field (TSF owns writes).
///
/// Layout pinned to 44 bytes for IPC stability. uint16_t used for currentSyllable
/// (instead of wchar_t) to keep size consistent across Win/Linux for unit tests.
struct HookContextAnchor {
    uint32_t generation;              // seqlock: odd = writing, even = stable
    uint8_t  isAvailable;             // 0 = TSF couldn't read (password/console/fail)
    uint8_t  isSentenceStart;         // doc start or after '.' '?' '!' (+ optional whitespace)
    uint8_t  isLineStart;             // doc start or after '\n' (+ optional whitespace, no punct required)
    uint8_t  isWordStart;             // preceding char is whitespace/nothing
    uint8_t  syllableLen;             // 0..16, length of currentSyllable
    uint8_t  padding[3];              // align currentSyllable to 4
    uint16_t currentSyllable[16];     // UTF-16 non-whitespace run immediately before cursor (phase 2+)
};
static_assert(sizeof(HookContextAnchor) == 44, "HookContextAnchor ABI frozen");
// generation must be aligned for std::atomic_ref<uint32_t> (writer path).
static_assert(alignof(HookContextAnchor) >= 4,
              "HookContextAnchor.generation requires 4-byte alignment for atomic_ref");
static_assert(offsetof(HookContextAnchor, generation) == 0,
              "generation expected at offset 0 for atomic_ref alignment");

/// Pure scan: derive anchor content from the last N preceding characters.
/// Caller sets `generation` and `isAvailable` separately.
/// `preceding` points to chars immediately before the cursor, in forward order
/// (i.e., preceding[len-1] is the char right before the caret).
inline void DeriveAnchorFromPreceding(const uint16_t* preceding, size_t len,
                                     HookContextAnchor& out) noexcept {
    // Empty buffer (doc start / no read) → treat as fresh start for everything.
    out.isSentenceStart = 1;
    out.isLineStart     = 1;
    out.isWordStart     = 1;
    out.syllableLen     = 0;
    for (auto& c : out.currentSyllable) c = 0;

    if (preceding == nullptr || len == 0) return;

    // isWordStart: cursor is after any whitespace (space/tab/newline).
    uint16_t last = preceding[len - 1];
    out.isWordStart = (last == u' ' || last == u'\t' ||
                       last == u'\n' || last == u'\r') ? 1 : 0;

    // Walk back over spaces/tabs (not newlines — newline is its own trigger).
    // Track whether any whitespace was skipped: sentence-start requires at least one
    // space between '.?!' and the cursor, otherwise domains/extensions like ".com"
    // get force-capped to ".Com".
    size_t i = len;
    bool skippedWhitespace = false;
    while (i > 0) {
        uint16_t c = preceding[i - 1];
        if (c == u' ' || c == u'\t') { --i; skippedWhitespace = true; continue; }
        break;
    }

    if (i == 0) {
        // Buffer is only spaces/tabs. Conservative: sentence + line start both true.
        // (Over-cap in pathological mid-doc whitespace runs is benign.)
        out.isSentenceStart = 1;
        out.isLineStart     = 1;
    } else {
        uint16_t prev = preceding[i - 1];
        if (prev == u'\n' || prev == u'\r') {
            out.isSentenceStart = 0;
            out.isLineStart     = 1;
        } else if ((prev == u'.' || prev == u'?' || prev == u'!') && skippedWhitespace) {
            out.isSentenceStart = 1;
            out.isLineStart     = 0;
        } else {
            out.isSentenceStart = 0;
            out.isLineStart     = 0;
        }
    }

    // currentSyllable: non-whitespace run ending at cursor. Meaningful only when
    // cursor is NOT after whitespace (phase 2+ uses for cross-boundary tone).
    if (out.isWordStart) return;

    size_t wordEnd = len;
    size_t wordBegin = wordEnd;
    while (wordBegin > 0) {
        uint16_t c = preceding[wordBegin - 1];
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') break;
        --wordBegin;
    }
    size_t wordLen = wordEnd - wordBegin;
    if (wordLen > 16) {
        wordBegin = wordEnd - 16;
        wordLen = 16;
    }
    for (size_t k = 0; k < wordLen; ++k) {
        out.currentSyllable[k] = preceding[wordBegin + k];
    }
    out.syllableLen = static_cast<uint8_t>(wordLen);
}

/// Seqlock read of HookContextAnchor from shared memory.
/// Returns true if a consistent snapshot was obtained within 3 retries.
/// On failure, `out` is left in an indeterminate state — callers should
/// check the return value before using `out` (and treat failure as "anchor
/// unavailable", equivalent to isAvailable = 0).
[[nodiscard]] inline bool ReadAnchorSeqlock(const volatile HookContextAnchor* src,
                              HookContextAnchor& out) noexcept {
    if (src == nullptr) return false;
    for (int retry = 0; retry < 3; ++retry) {
        uint32_t g1 = src->generation;  // single aligned 32-bit load is atomic
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g1 & 1u) continue;  // writer in progress → retry

        // Copy fields explicitly (volatile struct has no assignment operator).
        out.generation      = g1;
        out.isAvailable     = src->isAvailable;
        out.isSentenceStart = src->isSentenceStart;
        out.isLineStart     = src->isLineStart;
        out.isWordStart     = src->isWordStart;
        out.syllableLen     = src->syllableLen;
        for (size_t k = 0; k < 16; ++k) {
            out.currentSyllable[k] = src->currentSyllable[k];
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t g2 = src->generation;
        if (g1 == g2) return true;  // stable snapshot
    }
    return false;
}

/// Seqlock write of HookContextAnchor to shared memory.
///
/// INVARIANT: single-writer pattern. Only the foreground TSF DLL should write
/// at any given time (EXE's TSF_READONLY flag coordinates this). If two
/// writers race during focus transition, the atomic increments keep generation
/// advancing without collision, but readers may briefly see torn fields —
/// they'll retry via the seqlock protocol. Acceptable phase 1 degradation.
///
/// Both generation bumps use `std::atomic_ref<uint32_t>::fetch_add` with
/// `memory_order_acq_rel` (C++20). This avoids the non-atomic read-modify-write
/// bug where two writers would read the same counter value and produce the
/// same odd sequence, making readers accept a torn snapshot. Cross-platform;
/// keeps SharedState.h free of <Windows.h>.
inline void WriteAnchorSeqlock(volatile HookContextAnchor* dst,
                              const HookContextAnchor& src) noexcept {
    if (dst == nullptr) return;

    // Atomic RMW defends against multi-writer race. std::atomic_ref (C++20)
    // keeps this header free of <Windows.h>; memory_order_acq_rel matches
    // InterlockedIncrement semantics on Windows.
    uint32_t* genPtr = const_cast<uint32_t*>(&dst->generation);

    // gen++ → now odd = writing.
    std::atomic_ref<uint32_t>(*genPtr).fetch_add(1u, std::memory_order_acq_rel);

    dst->isAvailable     = src.isAvailable;
    dst->isSentenceStart = src.isSentenceStart;
    dst->isLineStart     = src.isLineStart;
    dst->isWordStart     = src.isWordStart;
    dst->syllableLen     = src.syllableLen;
    for (size_t k = 0; k < 16; ++k) {
        dst->currentSyllable[k] = src.currentSyllable[k];
    }

    std::atomic_thread_fence(std::memory_order_release);

    // gen++ → now even = stable.
    std::atomic_ref<uint32_t>(*genPtr).fetch_add(1u, std::memory_order_acq_rel);
}

/// SharedState struct for IPC between Core and Engine
/// Layout is versioned for forward compatibility (Phase 3+ expansion).
/// Magic: 0x59454B4E ('NKEY')
///
/// Seqlock protocol using epoch field (for config fields only):
///   Writer: epoch++ (now odd = writing), copy data, epoch++ (now even = done)
///   Reader: read epoch, copy data, verify epoch unchanged AND even → retry if not
///
/// HookContextAnchor uses its own independent seqlock (generation field).
struct SharedState {
    // ── Header (12 bytes) ──
    uint32_t magic;           // Magic identifier: 'NKEY' = 0x59454B4E
    uint32_t structVersion;   // Struct layout version (increment on layout change)
    uint32_t structSize;      // sizeof(SharedState) for forward compat

    // ── Synchronization (4 bytes) ──
    uint32_t epoch;           // Seqlock counter (even = stable, odd = write in progress)

    // ── Runtime flags (4 bytes) ──
    uint32_t flags;           // Runtime flags (Vietnamese mode, engine enabled, etc.)

    // ── Config data (3 bytes) ──
    uint8_t  inputMethod;     // 0=Telex, 1=VNI, 2=SimpleTelex
    uint8_t  spellCheck;      // Spell check enabled
    uint8_t  optimizeLevel;   // Optimization level

    // ── Feature flags (3 bytes, little-endian) ──
    uint8_t  featureFlags[2]; // Bitmask for optional features (bits 0-15)
    uint8_t  extFeatureFlags; // Extended feature flags (bits 16-23)

    // ── Extended config (1 byte) ──
    uint8_t  codeTable;       // CodeTable enum value (0=Unicode, 1=TCVN3, etc.)

    // ── Hotkey config (6 bytes, packed: 1 byte modifiers + 2 bytes key each) ──
    uint8_t  hotkeyMods;      // bits: [0]=ctrl [1]=shift [2]=alt [3]=win
    uint8_t  hotkeyKeyLo;     // wchar_t key, low byte
    uint8_t  hotkeyKeyHi;     // wchar_t key, high byte
    uint8_t  convertMods;     // convert hotkey modifiers (same encoding)
    uint8_t  convertKeyLo;    // convert hotkey key, low byte
    uint8_t  convertKeyHi;    // convert hotkey key, high byte

    // ── Config reload signal (2 bytes) ──
    // Incremented by Settings/subdialogs after saving TOML.
    // HookEngine detects change during QuickSyncFromSharedState() and triggers full reload.
    // Replaces Named Event (ConfigEvent) — eliminates per-keystroke WaitForSingleObject syscall.
    uint8_t  configGeneration;   // Wraps at 255 — use != comparison, not >
    uint8_t  reserved0;          // Padding to maintain alignment

    // ── Reserved for future expansion (1024 bytes) ──
    // Draw from this pool for new fields; do NOT bump CURRENT_VERSION unless
    // resizing/reordering existing fields. See docs/CODING_RULES/5-struct-versioning.md.
    uint8_t  reserved[1024];

    // ── Readonly context anchor (44 bytes, v3+) ──
    // Written by TSF DLL in readonly mode; read by HookEngine.
    // Uses its own seqlock (generation). SharedStateManager::Write() MUST NOT copy.
    HookContextAnchor contextAnchor;

    static constexpr uint32_t MAGIC_VALUE = 0x59454B4E;    // 'NKEY'
    static constexpr uint32_t CURRENT_VERSION = 4;          // v4: reserved pool grown to 1024 (hybrid DLL update headroom)

    [[nodiscard]] bool IsValid() const noexcept {
        return magic == MAGIC_VALUE
            && structVersion <= CURRENT_VERSION
            && structSize >= 24;  // Minimum: header + epoch + flags + config
    }

    [[nodiscard]] uint32_t GetFeatureFlags() const noexcept {
        return featureFlags[0]
             | (static_cast<uint32_t>(featureFlags[1]) << 8)
             | (static_cast<uint32_t>(extFeatureFlags) << 16);
    }

    void SetFeatureFlags(uint32_t ff) noexcept {
        featureFlags[0] = static_cast<uint8_t>(ff);
        featureFlags[1] = static_cast<uint8_t>(ff >> 8);
        extFeatureFlags = static_cast<uint8_t>(ff >> 16);
    }

    // ── Hotkey encode/decode helpers ──
    void SetHotkey(const HotkeyConfig& hk) noexcept {
        hotkeyMods = (hk.ctrl ? 1 : 0) | (hk.shift ? 2 : 0) | (hk.alt ? 4 : 0) | (hk.win ? 8 : 0);
        hotkeyKeyLo = static_cast<uint8_t>(hk.key);
        hotkeyKeyHi = static_cast<uint8_t>(hk.key >> 8);
    }
    [[nodiscard]] HotkeyConfig GetHotkey() const noexcept {
        HotkeyConfig hk;
        hk.ctrl  = (hotkeyMods & 1) != 0;
        hk.shift = (hotkeyMods & 2) != 0;
        hk.alt   = (hotkeyMods & 4) != 0;
        hk.win   = (hotkeyMods & 8) != 0;
        hk.key   = static_cast<wchar_t>(hotkeyKeyLo | (static_cast<uint16_t>(hotkeyKeyHi) << 8));
        return hk;
    }
    // Convert hotkey fields are reserved for future migration.
    // Currently convert hotkey is managed by ConvertToolDialog (separate subprocess)
    // and read from TOML only. Wire here when ConvertToolDialog gets SharedState access.

    /// Initialize with defaults
    void InitDefaults() noexcept {
        magic = MAGIC_VALUE;
        structVersion = CURRENT_VERSION;
        structSize = sizeof(SharedState);
        epoch = 0;
        flags = SharedFlags::VIETNAMESE_MODE | SharedFlags::ENGINE_ENABLED;
        inputMethod = 0;  // Telex
        spellCheck = 0;
        optimizeLevel = 0;
        SetFeatureFlags(FeatureFlags::ALLOW_ZWJF);  // Default: tone keys enabled
        codeTable = 0;  // Unicode
        hotkeyMods = 0; hotkeyKeyLo = 0; hotkeyKeyHi = 0;
        convertMods = 0; convertKeyLo = 0; convertKeyHi = 0;
        configGeneration = 0;
        reserved0 = 0;
        for (auto& b : reserved) b = 0;
        contextAnchor = HookContextAnchor{};  // zero all fields (generation=0=stable)
    }
};

// Ensure SharedState layout is stable across EXE and DLL builds.
// sizeof breakdown: 12 header + 4 epoch + 4 flags + 3 config + 3 featureFlags +
// 1 codeTable + 6 hotkey + 2 configGen/reserved0 + 1024 reserved
//   = 1059 bytes, rounded up by 1 byte of alignment padding before contextAnchor
//   (alignof >= 4) → contextAnchor at offset 1060 + 44 = 1104.
static_assert(sizeof(SharedState) == 1104, "SharedState size changed — update structVersion");

// Layout-freeze guards — failing any of these means a field was reordered or
// resized and CURRENT_VERSION MUST be bumped (DLLs built against the old
// layout will then hit IsValid() == false and enter passthrough).
static_assert(offsetof(SharedState, magic) == 0,
              "magic must stay at offset 0");
static_assert(offsetof(SharedState, structVersion) == 4,
              "structVersion offset frozen");
static_assert(offsetof(SharedState, structSize) == 8,
              "structSize offset frozen");
static_assert(offsetof(SharedState, epoch) == 12,
              "epoch offset frozen");
static_assert(offsetof(SharedState, flags) == 16,
              "flags offset frozen");
static_assert(offsetof(SharedState, configGeneration) == 33,
              "configGeneration offset frozen");
// contextAnchor offset moves with reserved[] size. Pin it so any accidental
// field insert/reorder upstream gets caught at compile time. Accounts for
// 1 byte of alignment padding after reserved[1024] (ends at 1059, anchor
// requires alignof >= 4 so lands at 1060).
static_assert(offsetof(SharedState, contextAnchor) == 1060,
              "contextAnchor offset frozen (must account for reserved[1024] + pad)");

/// Encode TypingConfig feature bools → uint32_t bitmask (3 bytes used)
[[nodiscard]] inline uint32_t EncodeFeatureFlags(const TypingConfig& config) noexcept {
    uint32_t flags = 0;
    if (config.modernOrtho)        flags |= FeatureFlags::MODERN_ORTHO;
    if (config.autoCaps)           flags |= FeatureFlags::AUTO_CAPS;
    if (config.allowZwjf)          flags |= FeatureFlags::ALLOW_ZWJF;
    if (config.autoRestoreEnabled) flags |= FeatureFlags::AUTO_RESTORE;
    // Bit 0x0010 (TEMP_OFF_SPELL_CTRL) removed — now using spell exclusion list
    if (config.tempOffByAlt)       flags |= FeatureFlags::TEMP_OFF_BY_ALT;
    if (config.beepOnSwitch)       flags |= FeatureFlags::BEEP_ON_SWITCH;
    if (config.macroEnabled)       flags |= FeatureFlags::MACRO_ENABLED;
    if (config.macroInEnglish)     flags |= FeatureFlags::MACRO_IN_ENGLISH;
    if (config.quickConsonant)     flags |= FeatureFlags::QUICK_CONSONANT;
    if (config.quickStartConsonant) flags |= FeatureFlags::QUICK_START_CONSONANT;
    if (config.quickEndConsonant)   flags |= FeatureFlags::QUICK_END_CONSONANT;
    if (config.tempOffMacroByEsc)   flags |= FeatureFlags::TEMP_OFF_MACRO_ESC;
    if (config.smartSwitch)         flags |= FeatureFlags::SMART_SWITCH;
    if (config.excludeApps)         flags |= FeatureFlags::EXCLUDE_APPS;
    if (config.autoCapsMacro)       flags |= FeatureFlags::AUTO_CAPS_MACRO;
    if (config.allowEnglishBypass)  flags |= FeatureFlags::ALLOW_ENGLISH_BYPASS;
    if (config.autoOffByUrl)        flags |= FeatureFlags::AUTO_OFF_BY_URL;
    return flags;
}

/// Decode uint32_t bitmask → TypingConfig feature bools
inline void DecodeFeatureFlags(uint32_t flags, TypingConfig& config) noexcept {
    config.modernOrtho        = (flags & FeatureFlags::MODERN_ORTHO) != 0;
    config.autoCaps           = (flags & FeatureFlags::AUTO_CAPS) != 0;
    config.allowZwjf          = (flags & FeatureFlags::ALLOW_ZWJF) != 0;
    config.autoRestoreEnabled = (flags & FeatureFlags::AUTO_RESTORE) != 0;
    // Bit 0x0010 (TEMP_OFF_SPELL_CTRL) removed — now using spell exclusion list
    config.tempOffByAlt       = (flags & FeatureFlags::TEMP_OFF_BY_ALT) != 0;
    config.beepOnSwitch       = (flags & FeatureFlags::BEEP_ON_SWITCH) != 0;
    config.macroEnabled       = (flags & FeatureFlags::MACRO_ENABLED) != 0;
    config.macroInEnglish     = (flags & FeatureFlags::MACRO_IN_ENGLISH) != 0;
    config.quickConsonant     = (flags & FeatureFlags::QUICK_CONSONANT) != 0;
    config.quickStartConsonant = (flags & FeatureFlags::QUICK_START_CONSONANT) != 0;
    config.quickEndConsonant   = (flags & FeatureFlags::QUICK_END_CONSONANT) != 0;
    config.tempOffMacroByEsc   = (flags & FeatureFlags::TEMP_OFF_MACRO_ESC) != 0;
    config.smartSwitch         = (flags & FeatureFlags::SMART_SWITCH) != 0;
    config.excludeApps         = (flags & FeatureFlags::EXCLUDE_APPS) != 0;
    config.autoCapsMacro       = (flags & FeatureFlags::AUTO_CAPS_MACRO) != 0;
    config.allowEnglishBypass  = (flags & FeatureFlags::ALLOW_ENGLISH_BYPASS) != 0;
    config.autoOffByUrl        = (flags & FeatureFlags::AUTO_OFF_BY_URL) != 0;
}

}  // namespace NextKey
