// NexusKey - Vietnamese Phonotactics Implementation
// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-NexusKey-Commercial

#include "Phonotactics.h"

#include <array>
#include <cstddef>
#include <string_view>

#include "VietnameseTables.h"

namespace NextKey {
namespace Phonology {
namespace {

// =============================================================================
// Vowel decomposition: rendered Vietnamese wchar_t → (base, modifier kind).
// `base` is one of L'a', L'e', L'i', L'o', L'u', L'y'. Other chars return base=0.
// =============================================================================

enum class VowelMod : uint8_t {
    None,
    Circumflex,  // â ê ô
    Breve,       // ă
    Horn         // ơ ư
};

struct VowelInfo {
    wchar_t base;
    VowelMod mod;
};

[[nodiscard]] VowelInfo Decompose(wchar_t c) noexcept {
    switch (c) {
        case L'a': return {L'a', VowelMod::None};
        case L'\x0103': return {L'a', VowelMod::Breve};        // ă
        case L'\x00E2': return {L'a', VowelMod::Circumflex};   // â
        case L'e': return {L'e', VowelMod::None};
        case L'\x00EA': return {L'e', VowelMod::Circumflex};   // ê
        case L'i': return {L'i', VowelMod::None};
        case L'o': return {L'o', VowelMod::None};
        case L'\x00F4': return {L'o', VowelMod::Circumflex};   // ô
        case L'\x01A1': return {L'o', VowelMod::Horn};         // ơ
        case L'u': return {L'u', VowelMod::None};
        case L'\x01B0': return {L'u', VowelMod::Horn};         // ư
        case L'y': return {L'y', VowelMod::None};
        default:   return {0, VowelMod::None};
    }
}

// Tone-placement tables and triphthong predicate are shared with TypingEngine
// via VietnameseTables.h — see NextKey::kDiphthongClassic, NextKey::kDiphthongModern,
// NextKey::DiphthongVowelIndex, NextKey::IsTriphthong.

// =============================================================================
// Closed vowel sequences — must NOT have a coda (28 entries from RuleTiengViet).
// Comparing rendered Vietnamese strings directly for clarity.
// =============================================================================
constexpr std::wstring_view kClosedVowels[] = {
    L"ai", L"ao", L"au", L"ay",
    L"\x00E2u",                 // âu
    L"\x00E2y",                 // ây
    L"eo",
    L"\x00EAu",                 // êu
    L"ia", L"iu", L"oi",
    L"\x00F4i",                 // ôi
    L"\x01A1i",                 // ơi
    L"ui",
    L"\x01B0a",                 // ưa
    L"\x01B0i",                 // ưi
    L"\x01B0u",                 // ưu
    L"i\x00EAu",                // iêu
    L"u\x00F4i",                // uôi
    L"uyu",
    L"\x01B0\x01A1i",           // ươi
    L"\x01B0\x01A1u",           // ươu
    L"oai", L"oay",
    L"u\x00E2y",                // uây
    L"uya", L"oeo", L"oao",
};

// =============================================================================
// Pending vowel sequences — REQUIRE a coda (10 entries from RuleTiengViet).
// =============================================================================
constexpr std::wstring_view kPendingVowels[] = {
    L"\x0103",                  // ă
    L"\x00E2",                  // â
    L"i\x00EA",                 // iê
    L"o\x0103",                 // oă
    L"u\x00E2",                 // uâ
    L"u\x00F4",                 // uô
    L"oo", L"\x00F4\x00F4",     // oo, ôô
    L"\x01B0\x01A1",            // ươ
    L"uy\x00EA",                // uyê
};

[[nodiscard]] bool IsClosedVowelSeq(std::wstring_view vowelSeq) noexcept {
    for (auto closedSeq : kClosedVowels) {
        if (closedSeq == vowelSeq) return true;
    }
    return false;
}

[[nodiscard]] bool IsPendingVowelSeq(std::wstring_view vowelSeq) noexcept {
    for (auto pendingSeq : kPendingVowels) {
        if (pendingSeq == vowelSeq) return true;
    }
    return false;
}

// =============================================================================
// Stop-final coda (c, ch, p, t) → tone restricted to Acute (sắc) or Dot (nặng).
// =============================================================================
[[nodiscard]] constexpr bool IsStopFinalCoda(std::wstring_view coda) noexcept {
    return coda == L"c" || coda == L"ch" || coda == L"p" || coda == L"t";
}

[[nodiscard]] constexpr bool ToneAllowedForCoda(std::wstring_view coda, Tone tone) noexcept {
    if (!IsStopFinalCoda(coda)) return true;
    return tone == Tone::None || tone == Tone::Acute || tone == Tone::Dot;
}

// =============================================================================
// Onset / coda lexicon for CanComplete parsing.
// =============================================================================
constexpr std::wstring_view kValidOnsets[] = {
    L"",                              // vowel-initial syllable
    L"b", L"c", L"d", L"\x0111",      // d, đ
    L"g", L"h", L"k", L"l", L"m", L"n",
    L"p", L"q", L"r", L"s", L"t", L"v", L"x",
    L"ch", L"gh", L"gi", L"kh", L"ng", L"nh", L"ph", L"qu", L"th", L"tr",
    L"ngh",
};

[[nodiscard]] bool IsKnownOnset(std::wstring_view text) noexcept {
    for (auto onset : kValidOnsets) {
        if (onset == text) return true;
    }
    return false;
}

// True if `text` is a valid lowercase ASCII onset prefix (incl. mid-typing like
// "n" before completing to "ng" or "nh"). Used by CanComplete's no-vowel branch.
[[nodiscard]] bool IsKnownOnsetPrefix(std::wstring_view text) noexcept {
    if (text.empty()) return true;
    // Any single ASCII consonant that can begin a valid onset.
    if (text.size() == 1) {
        wchar_t leadChar = text[0];
        // Reject pure non-letters / vowels.
        if (Decompose(leadChar).base != 0) return false;
        return (leadChar >= L'a' && leadChar <= L'z') || leadChar == L'\x0111';  // đ
    }
    // For 2+ chars, must match a known onset exactly. The only 3-char onset
    // is "ngh"; its 2-char prefix "ng" is itself a known onset, so no separate
    // prefix branch is needed.
    return IsKnownOnset(text);
}

constexpr std::wstring_view kValidCodas[] = {
    L"c", L"m", L"n", L"p", L"t",
    L"ch", L"ng", L"nh",
};

[[nodiscard]] bool IsKnownCoda(std::wstring_view text) noexcept {
    for (auto coda : kValidCodas) {
        if (coda == text) return true;
    }
    return false;
}

// Coda groups (RuleTiengViet):
//   C1 = ng, c       (velar)
//   C2 = nh, ch      (palatal)
//   C3 = m, n, p, t  (labial / alveolar)
enum class CodaGroup : uint8_t { None, C1, C2, C3 };

[[nodiscard]] CodaGroup ClassifyCoda(std::wstring_view coda) noexcept {
    if (coda.empty()) return CodaGroup::None;
    if (coda == L"ng" || coda == L"c") return CodaGroup::C1;
    if (coda == L"nh" || coda == L"ch") return CodaGroup::C2;
    if (coda == L"m" || coda == L"n" || coda == L"p" || coda == L"t") return CodaGroup::C3;
    return CodaGroup::None;  // unknown coda — let other rules reject if needed
}

// Vowel groups for vowel-coda compatibility (RuleTiengViet):
//   N1 nuclei accept C1 + C3, reject C2
//   N2 nuclei accept C2 + C3, reject C1
//   N3 nuclei accept everything
//   Other = nucleus not classified — lenient fall-through (allow any coda)
//
// Coarser than the per-nucleus `kVCPairRules` bitmask in PhonotacticsValidator.cpp,
// which is the source-of-truth for the production CharState path. Examples of
// where VCPair is stricter: "ơ" (only m/n/p/t — N-group says C1+C3 = also ng/c),
// "iê" (no ch/nh — N-group says all). If/when Phonotactics::IsValidSyllable
// gains a production caller, port the VCPair encoding rather than relying on
// the N-group approximation here. See REFACTOR_STATUS T2.1.
enum class VowelGroup : uint8_t { Other, N1, N2, N3 };

constexpr std::wstring_view kN1Nuclei[] = {
    L"\x00E2",                 // â
    L"e",
    L"o",
    L"\x00F4",                 // ô
    L"u",
    L"\x01B0",                 // ư
    L"\x01A1",                 // ơ
    L"\x0103",                 // ă
    L"o\x0103",                // oă
    L"oe",
    L"u\x00E2",                // uâ
    L"u\x00F4",                // uô
    L"u\x01A1",                // uơ
    L"\x01B0\x01A1",           // ươ
};

constexpr std::wstring_view kN2Nuclei[] = {
    L"\x00EA",                 // ê
    L"i",
    L"u\x00EA",                // uê
    L"uy",
    L"ua",
};

constexpr std::wstring_view kN3Nuclei[] = {
    L"a",
    L"oa",
    L"i\x00EA",                // iê
    L"uy\x00EA",               // uyê
};

[[nodiscard]] VowelGroup ClassifyVowelGroup(std::wstring_view vowelSeq) noexcept {
    for (auto v : kN1Nuclei) if (v == vowelSeq) return VowelGroup::N1;
    for (auto v : kN2Nuclei) if (v == vowelSeq) return VowelGroup::N2;
    for (auto v : kN3Nuclei) if (v == vowelSeq) return VowelGroup::N3;
    return VowelGroup::Other;
}

[[nodiscard]] bool IsCodaCompatibleWithVowelGroup(VowelGroup group, CodaGroup codaGroup) noexcept {
    if (group == VowelGroup::Other)  return true;   // unclassified — lenient
    if (codaGroup == CodaGroup::None) return true;
    if (codaGroup == CodaGroup::C3)  return true;   // C3 accepted by all groups
    if (group == VowelGroup::N3)     return true;   // N3 accepts everything
    if (group == VowelGroup::N1)     return codaGroup == CodaGroup::C1;
    return codaGroup == CodaGroup::C2;              // group == N2
}

// Vietnamese orthography splits c/k, g/gh, ng/ngh by vowel frontness.
// Mirror of the rule encoded against CharState in PhonotacticsValidator.cpp;
// the two paths differ in input type (rendered text vs engine state) so the
// shared classifier is the per-base helper below, not the agreement function.
[[nodiscard]] constexpr bool IsFrontBaseVowel(wchar_t base) noexcept {
    return base == L'e' || base == L'i' || base == L'y';
}

[[nodiscard]] bool IsOnsetVowelAgreementValid(
        std::wstring_view onset,
        std::wstring_view vowelSeq) noexcept {
    // qu intentionally exempted: book lists oa/oă/oe/uy/uơ/uô/uê/uâ as the
    // canonical labial diphthongs after qu, but qua/quan/quát/quanh sit
    // outside that list and are everyday words.
    if (onset == L"qu") return true;

    const bool needsAgreement =
        onset == L"c" || onset == L"k"  ||
        onset == L"g" || onset == L"gh" ||
        onset == L"ng" || onset == L"ngh";
    if (!needsAgreement) return true;

    wchar_t firstBase = 0;
    for (wchar_t ch : vowelSeq) {
        VowelInfo info = Decompose(ch);
        if (info.base != 0) { firstBase = info.base; break; }
    }
    if (firstBase == 0) return true;

    const bool wantsFront = (onset == L"k" || onset == L"gh" || onset == L"ngh");
    return wantsFront == IsFrontBaseVowel(firstBase);
}

// =============================================================================
// Core priority logic for tone placement: P1 horn > P2 modified > P3 diphthong /
// triphthong > P4 rightmost. Operates on a rendered wstring_view of the vowel
// nucleus (Decompose maps each char to base + modifier).
// =============================================================================
[[nodiscard]] size_t ComputeTonePosition(
        std::wstring_view vowelSeq,
        std::wstring_view coda,
        bool modernOrtho) noexcept {
    // Real Vietnamese vowel nuclei are at most 3 chars, but the engine may pass
    // longer sequences for typo cases ("máaaaaaaa" — 9+ repeated vowels). Cap
    // generously to keep all such inputs in scope: P1/P2 must scan the full
    // sequence to find any horn / modifier regardless of where the user typed
    // it, and the rightmost-fallback (P4) must point at the actual last vowel.
    std::array<VowelInfo, 16> vowels{};
    size_t count = 0;
    for (wchar_t ch : vowelSeq) {
        if (count >= vowels.size()) break;
        VowelInfo info = Decompose(ch);
        if (info.base == 0) continue;  // skip non-vowels defensively
        vowels[count++] = info;
    }
    if (count == 0) return SIZE_MAX;

    // P1: last horn vowel wins (covers ươ → ơ).
    for (size_t i = count; i-- > 0; ) {
        if (vowels[i].mod == VowelMod::Horn) return i;
    }

    // P2: first non-horn modified vowel (â, ê, ô, ă).
    for (size_t i = 0; i < count; ++i) {
        if (vowels[i].mod == VowelMod::Circumflex || vowels[i].mod == VowelMod::Breve) {
            return i;
        }
    }

    // P3: diphthong / triphthong rules (need ≥ 2 vowels).
    if (count >= 2) {
        // Modern triphthong: tone on MIDDLE.
        if (modernOrtho && count >= 3) {
            if (NextKey::IsTriphthong(vowels[count - 3].base,
                                       vowels[count - 2].base,
                                       vowels[count - 1].base)) {
                return count - 2;
            }
        }

        // Default pair: last two vowels.
        size_t firstPos = count - 2;
        size_t lastPos  = count - 1;
        int firstDiphIndex = NextKey::DiphthongVowelIndex(vowels[firstPos].base);
        int lastDiphIndex  = NextKey::DiphthongVowelIndex(vowels[lastPos].base);
        bool shifted = false;

        // Shift onto first two of a 3-vowel cluster when those have a diphthong
        // rule. Handles typo cases like "gaoi" (gạo + extra i) and classic "oai".
        if (count >= 3) {
            int shiftFirstIndex = NextKey::DiphthongVowelIndex(vowels[count - 3].base);
            if (shiftFirstIndex >= 0 && firstDiphIndex >= 0) {
                uint8_t shiftRule = modernOrtho
                    ? NextKey::kDiphthongModern[shiftFirstIndex][firstDiphIndex]
                    : NextKey::kDiphthongClassic[shiftFirstIndex][firstDiphIndex];
                if (shiftRule != 0) {
                    lastDiphIndex = firstDiphIndex;
                    firstDiphIndex = shiftFirstIndex;
                    firstPos = count - 3;
                    lastPos  = count - 2;
                    shifted = true;
                }
            }
        }

        if (firstDiphIndex >= 0 && lastDiphIndex >= 0) {
            uint8_t rule = modernOrtho
                ? NextKey::kDiphthongModern[firstDiphIndex][lastDiphIndex]
                : NextKey::kDiphthongClassic[firstDiphIndex][lastDiphIndex];
            if (rule == 3) {
                // Shifted with vowel-repeat at end → tone stays on first vowel
                // (typo "hoaa" / "oaa": don't slide tone onto the duplicated 'a').
                if (shifted && vowels[count - 1].base == vowels[count - 2].base) {
                    rule = 1;
                } else {
                    bool hasRemainder = (lastPos + 1 < count) || !coda.empty();
                    rule = hasRemainder ? 2 : 1;
                }
            }
            if (rule == 1) return firstPos;
            if (rule == 2) return lastPos;
        }
    }

    // P4: rightmost vowel.
    return count - 1;
}

// =============================================================================
// Simple parser for CanComplete: split partial → (onset, vowels, coda, leftover).
// Returns false if no plausible split exists.
// =============================================================================

struct ParsedSyllable {
    std::wstring_view onset;
    std::wstring_view vowels;
    std::wstring_view coda;
    std::wstring_view leftover;  // anything past the coda
    bool hasVowel = false;
};

[[nodiscard]] ParsedSyllable Parse(std::wstring_view text) noexcept {
    ParsedSyllable result;
    // Find first vowel.
    size_t firstVowelIdx = std::wstring_view::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        if (Decompose(text[i]).base != 0) { firstVowelIdx = i; break; }
    }
    if (firstVowelIdx == std::wstring_view::npos) {
        result.onset = text;
        return result;
    }
    result.onset = text.substr(0, firstVowelIdx);
    result.hasVowel = true;

    // Collect contiguous vowels.
    size_t lastVowelIdx = firstVowelIdx;
    for (size_t i = firstVowelIdx; i < text.size(); ++i) {
        if (Decompose(text[i]).base == 0) break;
        lastVowelIdx = i;
    }
    result.vowels = text.substr(firstVowelIdx, lastVowelIdx - firstVowelIdx + 1);

    // Coda is up to 2 consonants after vowels.
    std::wstring_view rest = text.substr(lastVowelIdx + 1);
    size_t codaLen = 0;
    while (codaLen < rest.size() && codaLen < 2 &&
           Decompose(rest[codaLen]).base == 0) {
        ++codaLen;
    }
    result.coda = rest.substr(0, codaLen);
    result.leftover = rest.substr(codaLen);
    return result;
}

}  // anonymous namespace

// =============================================================================
// IPhonotactics implementation
// =============================================================================

const Phonotactics& Phonotactics::Default() noexcept {
    static const Phonotactics instance;
    return instance;
}

size_t Phonotactics::TonePosition(
        std::wstring_view vowelSeq,
        std::wstring_view coda,
        bool modernOrtho) const noexcept {
    return ComputeTonePosition(vowelSeq, coda, modernOrtho);
}

bool Phonotactics::IsValidSyllable(
        std::wstring_view onset,
        std::wstring_view vowelSeq,
        std::wstring_view coda,
        Tone tone,
        bool /*modernOrtho*/) const noexcept {
    if (vowelSeq.empty()) return false;

    // Onset / vowel front-back agreement (c/k, g/gh, ng/ngh). qu exempted.
    if (!IsOnsetVowelAgreementValid(onset, vowelSeq)) return false;

    // Closed vowels must NOT have a coda.
    if (!coda.empty() && IsClosedVowelSeq(vowelSeq)) return false;

    // Pending vowels MUST have a coda.
    if (coda.empty() && IsPendingVowelSeq(vowelSeq)) return false;

    // N1/N2/N3 vowel-coda group compatibility.
    if (!IsCodaCompatibleWithVowelGroup(ClassifyVowelGroup(vowelSeq), ClassifyCoda(coda))) {
        return false;
    }

    // Stop-final coda restricts tone to Acute or Dot.
    if (!ToneAllowedForCoda(coda, tone)) return false;

    return true;
}

bool Phonotactics::CanComplete(std::wstring_view partial) const noexcept {
    if (partial.empty()) return true;

    ParsedSyllable parsed = Parse(partial);

    // No vowel at all — must be a valid onset prefix.
    if (!parsed.hasVowel) {
        return IsKnownOnsetPrefix(parsed.onset);
    }

    // Onset must be a known cluster (or empty for vowel-initial).
    if (!IsKnownOnset(parsed.onset)) return false;

    // Anything past the coda is a leftover keystroke past a closed syllable.
    if (!parsed.leftover.empty()) return false;

    // Coda (if any) must be a known coda. Single-char prefixes of multi-char
    // codas (c → ch, n → ng/nh) are themselves already valid codas, so any
    // non-known multi-char value is a hard fail.
    if (!parsed.coda.empty() && !IsKnownCoda(parsed.coda)) {
        return false;
    }

    return true;
}

}  // namespace Phonology
}  // namespace NextKey
