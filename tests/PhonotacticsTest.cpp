// NexusKey - Phonotactics Unit Tests
// SPDX-License-Identifier: GPL-3.0-only
//
// Tests for IPhonotactics rule engine: tone position, syllable validity,
// completability. Operates on rendered Vietnamese text (wstring_view).

#include <gtest/gtest.h>
#include "core/engine/IPhonotactics.h"
#include "core/engine/Phonotactics.h"
#include "core/engine/TypingEngine.h"
#include "core/config/TypingConfig.h"

namespace NextKey {
namespace Phonology {
namespace {

using ::testing::Test;

//=============================================================================
// TonePosition — locates index in vowelSeq where tone diacritic belongs
//=============================================================================

class PhonotacticsTonePosition : public ::testing::Test {
protected:
    Phonotactics phon_;
    static constexpr bool kClassic = false;
    static constexpr bool kModern  = true;
};

TEST_F(PhonotacticsTonePosition, EmptyVowelReturnsSizeMax) {
    EXPECT_EQ(phon_.TonePosition(L"", L"", kClassic), SIZE_MAX);
}

TEST_F(PhonotacticsTonePosition, SingleVowelReturnsZero) {
    EXPECT_EQ(phon_.TonePosition(L"a", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"e", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"i", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"o", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"u", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"y", L"", kClassic), 0u);
}

TEST_F(PhonotacticsTonePosition, SingleHornVowelReturnsZero) {
    // ư (U+01B0), ơ (U+01A1) — horn vowels alone → tone on themselves
    EXPECT_EQ(phon_.TonePosition(L"ư", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"ơ", L"", kClassic), 0u);
}

TEST_F(PhonotacticsTonePosition, DiphthongAiToneOnFirst) {
    // "ai" (closed) — table rule 1 → first vowel: ái
    EXPECT_EQ(phon_.TonePosition(L"ai", L"", kClassic), 0u);
    EXPECT_EQ(phon_.TonePosition(L"ai", L"", kModern), 0u);
}

TEST_F(PhonotacticsTonePosition, DiphthongAoToneOnFirst) {
    // "ao" (closed) — rule 1 → first: áo
    EXPECT_EQ(phon_.TonePosition(L"ao", L"", kClassic), 0u);
}

TEST_F(PhonotacticsTonePosition, DiphthongOaClassicNoCodaToneOnFirst) {
    // "oa" classic, no coda → "hòa": tone on FIRST (rule 3 → first when no coda)
    EXPECT_EQ(phon_.TonePosition(L"oa", L"", kClassic), 0u);
}

TEST_F(PhonotacticsTonePosition, DiphthongOaModernNoCodaToneOnSecond) {
    // "oa" modern, no coda → "hoà": tone on SECOND (rule 2 in modern table)
    EXPECT_EQ(phon_.TonePosition(L"oa", L"", kModern), 1u);
}

TEST_F(PhonotacticsTonePosition, DiphthongOaClassicWithCodaToneOnSecond) {
    // "oa" classic, with coda → "hoàn": rule 3 with coda → SECOND
    EXPECT_EQ(phon_.TonePosition(L"oa", L"n", kClassic), 1u);
}

TEST_F(PhonotacticsTonePosition, HornDiphthongUOToneOnHorn) {
    // "ươ" (U+01B0 + U+01A1) — P1 horn priority, last horn = ơ
    EXPECT_EQ(phon_.TonePosition(L"ươ", L"", kClassic), 1u);
    EXPECT_EQ(phon_.TonePosition(L"ươ", L"", kModern), 1u);
}

TEST_F(PhonotacticsTonePosition, ModifiedVowelGetsTonePriority) {
    // "ie" with ê (e+circumflex U+00EA) → P2 modified vowel: tone on ê
    EXPECT_EQ(phon_.TonePosition(L"iê", L"", kClassic), 1u);  // iê → tone on ê
    EXPECT_EQ(phon_.TonePosition(L"âu", L"", kClassic), 0u);  // âu → tone on â
}

TEST_F(PhonotacticsTonePosition, TriphthongOaiToneOnMiddle) {
    // "oai" modern → triphthong, tone on MIDDLE: hoài
    EXPECT_EQ(phon_.TonePosition(L"oai", L"", kModern), 1u);
}

TEST_F(PhonotacticsTonePosition, TriphthongUyuToneOnMiddle) {
    // "uyu" modern → triphthong, tone on MIDDLE: khuỷu
    EXPECT_EQ(phon_.TonePosition(L"uyu", L"", kModern), 1u);
}

TEST_F(PhonotacticsTonePosition, ThreeVowelUyeToneOnModifiedThird) {
    // "uyê" — ê is modified (P2 priority) → tone on ê (index 2)
    // (RuleTiengViet exception: 3-vowel default is middle, except uyê)
    EXPECT_EQ(phon_.TonePosition(L"uyê", L"", kModern), 2u);
}

TEST_F(PhonotacticsTonePosition, ShiftedThreeVowelTypoAoi) {
    // "aoi" typo (gạo + extra i): shifted-3-vowel rule fires — first 2 vowels
    // (a,o) have a diphthong rule, so tone stays on the original diphthong's
    // FIRST vowel (a, index 0) instead of sliding to the typo's last-2 (o,i)
    // pair which would give index 1.
    EXPECT_EQ(phon_.TonePosition(L"aoi", L"", kClassic), 0u);
}

TEST_F(PhonotacticsTonePosition, ShiftedThreeVowelRepeatHoaa) {
    // "oaa" typo (hòa + extra a): shift fires onto first 2 (o,a) which is
    // rule-3 (coda-aware). Vowel-repeat at end overrides coda-aware to
    // FIRST → tone on 'o' (index 0).
    EXPECT_EQ(phon_.TonePosition(L"oaa", L"", kClassic), 0u);
}

TEST_F(PhonotacticsTonePosition, ClassicOaiHasRemainderRule) {
    // "oai" classic: shift fires onto (o,a) rule 3, but classic-mode "oai"
    // has more text (the trailing 'i') after the secondPos, so coda-aware
    // resolves to SECOND → tone on 'a' (index 1). Different from triphthong
    // path (modern) which also returns 1 but for a different reason.
    EXPECT_EQ(phon_.TonePosition(L"oai", L"", kClassic), 1u);
}

//=============================================================================
// IsValidSyllable — full syllable validity per RuleTiengViet
//=============================================================================

class PhonotacticsIsValidSyllable : public ::testing::Test {
protected:
    Phonotactics phon_;
    static constexpr bool kClassic = false;
    static constexpr bool kModern  = true;
};

TEST_F(PhonotacticsIsValidSyllable, SimpleConsonantVowelIsValid) {
    // "ba" — onset b, vowel a, no coda, no tone → valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, VowelInitialIsValid) {
    // "an" — no onset, vowel a, coda n → valid (e.g. "ăn", "an")
    EXPECT_TRUE(phon_.IsValidSyllable(L"", L"a", L"n", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, ClosedVowelRejectsCoda) {
    // "ai" is a closed vowel → cannot have coda. "ain" invalid.
    EXPECT_FALSE(phon_.IsValidSyllable(L"", L"ai", L"n", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, PendingVowelRequiresCoda) {
    // "ă" is pending (must have coda). "bă" alone invalid.
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"ă", L"", Tone::None, kModern));
    // "băn" valid.
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"ă", L"n", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, CodaCPRestrictsToneToAcuteOrDot) {
    // "bac" with sắc → bác valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"c", Tone::Acute, kModern));
    // "bac" with nặng → bạc valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"c", Tone::Dot, kModern));
    // "bac" with huyền → invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"a", L"c", Tone::Grave, kModern));
    // "bat" with hỏi → invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"a", L"t", Tone::Hook, kModern));
    // "bach" with sắc → bách valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"ch", Tone::Acute, kModern));
    // "bap" with ngã → invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"a", L"p", Tone::Tilde, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, OpenCodaAllowsAnyTone) {
    // "ban" with any tone → valid (n is not c/ch/p/t)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"n", Tone::Acute, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"n", Tone::Grave, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"n", Tone::Hook, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"n", Tone::Tilde, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"n", Tone::Dot, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, OpenSyllableAllowsAnyTone) {
    // "ba" no coda → all tones valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"", Tone::Acute, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"", Tone::Grave, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"", Tone::Hook, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"", Tone::Tilde, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a", L"", Tone::Dot, kModern));
}

//=============================================================================
// IsValidSyllable — onset / vowel front-back agreement.
// Rule and qu-exemption rationale documented at the helper definition in
// Phonotactics.cpp.
//=============================================================================

TEST_F(PhonotacticsIsValidSyllable, OnsetCAcceptsBackVowels) {
    // ca, cô, cu, cơ, cư, cân, căn — c + back vowel = valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"a",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"\x00F4",  L"",  Tone::None, kModern));   // cô
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"u",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"\x01A1",  L"",  Tone::None, kModern));   // cơ
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"\x01B0",  L"",  Tone::None, kModern));   // cư
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"\x00E2",  L"n", Tone::None, kModern));   // cân
    EXPECT_TRUE(phon_.IsValidSyllable(L"c", L"\x0103",  L"n", Tone::None, kModern));   // căn
}

TEST_F(PhonotacticsIsValidSyllable, OnsetCRejectsFrontVowels) {
    // ce, cê, ci, cy — c + front vowel = invalid (must use k)
    EXPECT_FALSE(phon_.IsValidSyllable(L"c", L"e",       L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"c", L"\x00EA",  L"", Tone::None, kModern));   // cê
    EXPECT_FALSE(phon_.IsValidSyllable(L"c", L"i",       L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"c", L"y",       L"", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, OnsetKAcceptsFrontVowels) {
    // ke, kê, ki, ky, ken, kim, kênh — k + front vowel = valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"k", L"e",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"k", L"\x00EA",  L"",  Tone::None, kModern));   // kê
    EXPECT_TRUE(phon_.IsValidSyllable(L"k", L"i",       L"m", Tone::None, kModern));   // kim
    EXPECT_TRUE(phon_.IsValidSyllable(L"k", L"y",       L"",  Tone::None, kModern));   // ky
    EXPECT_TRUE(phon_.IsValidSyllable(L"k", L"e",       L"n", Tone::None, kModern));   // ken
    EXPECT_TRUE(phon_.IsValidSyllable(L"k", L"\x00EA",  L"nh", Tone::None, kModern));  // kênh
}

TEST_F(PhonotacticsIsValidSyllable, OnsetKRejectsBackVowels) {
    // ka, kô, ku, kơ, kư, kâu — k + back vowel = invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"k", L"a",       L"",  Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"k", L"\x00F4",  L"",  Tone::None, kModern));   // kô
    EXPECT_FALSE(phon_.IsValidSyllable(L"k", L"u",       L"",  Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"k", L"\x01A1",  L"",  Tone::None, kModern));   // kơ
    EXPECT_FALSE(phon_.IsValidSyllable(L"k", L"\x01B0",  L"",  Tone::None, kModern));   // kư
    EXPECT_FALSE(phon_.IsValidSyllable(L"k", L"\x00E2",  L"u", Tone::None, kModern));   // kâu
}

TEST_F(PhonotacticsIsValidSyllable, OnsetGAcceptsBackVowels) {
    // ga, gô, gu, gơ, gan — g + back vowel = valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"g", L"a",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"g", L"\x00F4",  L"",  Tone::None, kModern));   // gô
    EXPECT_TRUE(phon_.IsValidSyllable(L"g", L"u",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"g", L"a",       L"n", Tone::None, kModern));   // gan
    EXPECT_TRUE(phon_.IsValidSyllable(L"g", L"\x01A1",  L"i", Tone::None, kModern));   // gơi-ish
}

TEST_F(PhonotacticsIsValidSyllable, OnsetGRejectsFrontVowels) {
    // ge, gê, gi — g + front vowel = invalid (must use gh, or "gi" cluster onset)
    EXPECT_FALSE(phon_.IsValidSyllable(L"g", L"e",      L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"g", L"\x00EA", L"", Tone::None, kModern));   // gê
    EXPECT_FALSE(phon_.IsValidSyllable(L"g", L"i",      L"", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, OnsetGhAcceptsFrontVowels) {
    // ghe, ghê, ghi, ghen — gh + front vowel = valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"gh", L"e",      L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"gh", L"\x00EA", L"",  Tone::None, kModern));  // ghê
    EXPECT_TRUE(phon_.IsValidSyllable(L"gh", L"i",      L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"gh", L"e",      L"n", Tone::None, kModern));  // ghen
}

TEST_F(PhonotacticsIsValidSyllable, OnsetGhRejectsBackVowels) {
    // gha, ghu, ghô — gh + back vowel = invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"gh", L"a",      L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"gh", L"u",      L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"gh", L"\x00F4", L"", Tone::None, kModern));  // ghô
}

TEST_F(PhonotacticsIsValidSyllable, OnsetNgAcceptsBackVowels) {
    // nga, ngô, ngu, ngư, ngon — ng + back = valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"ng", L"a",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"ng", L"\x00F4",  L"",  Tone::None, kModern));  // ngô
    EXPECT_TRUE(phon_.IsValidSyllable(L"ng", L"u",       L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"ng", L"\x01B0",  L"",  Tone::None, kModern));  // ngư
    EXPECT_TRUE(phon_.IsValidSyllable(L"ng", L"o",       L"n", Tone::None, kModern));  // ngon
}

TEST_F(PhonotacticsIsValidSyllable, OnsetNgRejectsFrontVowels) {
    // nge, ngê, ngi — ng + front = invalid (must use ngh)
    EXPECT_FALSE(phon_.IsValidSyllable(L"ng", L"e",      L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"ng", L"\x00EA", L"", Tone::None, kModern));   // ngê
    EXPECT_FALSE(phon_.IsValidSyllable(L"ng", L"i",      L"", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, OnsetNghAcceptsFrontVowels) {
    // nghe, nghê, nghi, nghin — ngh + front = valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"ngh", L"e",      L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"ngh", L"\x00EA", L"",  Tone::None, kModern));  // nghê
    EXPECT_TRUE(phon_.IsValidSyllable(L"ngh", L"i",      L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"ngh", L"i",      L"n", Tone::None, kModern));  // nghin
}

TEST_F(PhonotacticsIsValidSyllable, OnsetNghRejectsBackVowels) {
    // ngha, nghô, nghu — ngh + back = invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"ngh", L"a",      L"", Tone::None, kModern));
    EXPECT_FALSE(phon_.IsValidSyllable(L"ngh", L"\x00F4", L"", Tone::None, kModern));  // nghô
    EXPECT_FALSE(phon_.IsValidSyllable(L"ngh", L"u",      L"", Tone::None, kModern));
}

TEST_F(PhonotacticsIsValidSyllable, OnsetQuFreePassByDesign) {
    // qu agreement intentionally not enforced — see helper rationale comment.
    EXPECT_TRUE(phon_.IsValidSyllable(L"qu", L"a",      L"",  Tone::None, kModern));   // qua
    EXPECT_TRUE(phon_.IsValidSyllable(L"qu", L"a",      L"n", Tone::None, kModern));   // quan
    EXPECT_TRUE(phon_.IsValidSyllable(L"qu", L"\x00EA", L"",  Tone::None, kModern));   // quê
    EXPECT_TRUE(phon_.IsValidSyllable(L"qu", L"y",      L"",  Tone::None, kModern));   // quy
    EXPECT_TRUE(phon_.IsValidSyllable(L"qu", L"\x00E2", L"n", Tone::None, kModern));   // quân
    EXPECT_TRUE(phon_.IsValidSyllable(L"qu", L"\x0103", L"n", Tone::None, kModern));   // quăn
}

TEST_F(PhonotacticsIsValidSyllable, OnsetGiClusterUnaffected) {
    // "gi" is its own onset cluster (giáo, giải, giờ) — not subject to g/gh agreement.
    EXPECT_TRUE(phon_.IsValidSyllable(L"gi", L"a",      L"",  Tone::None, kModern));   // gia
    EXPECT_TRUE(phon_.IsValidSyllable(L"gi", L"ao",     L"",  Tone::None, kModern));   // giao
    EXPECT_TRUE(phon_.IsValidSyllable(L"gi", L"\x01A1", L"",  Tone::None, kModern));   // giờ-ish
}

TEST_F(PhonotacticsIsValidSyllable, OtherOnsetsNotSubjectToAgreement) {
    // b/d/h/l/m/n/p/r/s/t/v/x and ch/kh/nh/ph/th/tr accept any vowel.
    EXPECT_TRUE(phon_.IsValidSyllable(L"b",  L"e", L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"b",  L"a", L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"th", L"e", L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"th", L"a", L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"tr", L"a", L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"tr", L"i", L"",  Tone::None, kModern));
    EXPECT_TRUE(phon_.IsValidSyllable(L"",   L"a", L"n", Tone::None, kModern));   // vowel-initial
}

TEST_F(PhonotacticsIsValidSyllable, OnsetAgreementUsesFirstVowelOfDiphthong) {
    // Agreement is checked against the FIRST vowel of vowelSeq.
    EXPECT_TRUE (phon_.IsValidSyllable(L"ngh", L"i\x00EA", L"u", Tone::None, kModern));   // nghiêu (first 'i' front)
    EXPECT_FALSE(phon_.IsValidSyllable(L"ng",  L"i\x00EA", L"u", Tone::None, kModern));   // ngiêu invalid
    EXPECT_TRUE (phon_.IsValidSyllable(L"k",   L"i\x00EA", L"n", Tone::None, kModern));   // kiên
    EXPECT_FALSE(phon_.IsValidSyllable(L"c",   L"i\x00EA", L"n", Tone::None, kModern));   // ciên invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"k",   L"oa",      L"n", Tone::None, kModern));   // koan invalid (first 'o' back)
    EXPECT_FALSE(phon_.IsValidSyllable(L"gh",  L"oa",      L"",  Tone::None, kModern));   // gh + back invalid
    EXPECT_TRUE (phon_.IsValidSyllable(L"gh",  L"e",       L"o", Tone::None, kModern));   // gheo first 'e' front
}

//=============================================================================
// IsValidSyllable — N1/N2/N3 vowel/coda group compatibility (T3).
// Rule and "Other" lenient fall-through documented at the helper definition
// in Phonotactics.cpp.
//=============================================================================

TEST_F(PhonotacticsIsValidSyllable, N1VowelAcceptsC1Coda) {
    // N1 nuclei (e, o, ô, u, ư, ơ, ă, oă, uâ, uô, ươ, ...) + C1 (ng, c) — valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"",  L"e",       L"ng", Tone::None,  kModern));   // eng
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"\x00F4",  L"ng", Tone::None,  kModern));   // bông
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"u",       L"c",  Tone::Dot,   kModern));   // bục
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"\x0103",  L"ng", Tone::None,  kModern));   // băng
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"\x01B0",  L"c",  Tone::Acute, kModern));   // bức
}

TEST_F(PhonotacticsIsValidSyllable, N1VowelAcceptsC3Coda) {
    // N1 + C3 (m, n, p, t) — valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"e",       L"m", Tone::None,  kModern));   // bem
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"e",       L"n", Tone::None,  kModern));   // ben
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"o",       L"p", Tone::Acute, kModern));   // bóp
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"u",       L"t", Tone::Acute, kModern));   // bút
}

TEST_F(PhonotacticsIsValidSyllable, N1VowelRejectsC2Coda) {
    // N1 + C2 (nh, ch) — invalid (N1 forbids C2)
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"e",      L"nh", Tone::None,  kModern));   // benh wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"e",      L"ch", Tone::Acute, kModern));   // bech wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"o",      L"nh", Tone::None,  kModern));   // bonh wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"\x0103", L"nh", Tone::None,  kModern));   // bănh wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"\x00F4", L"ch", Tone::Acute, kModern));   // bôch wrong
}

TEST_F(PhonotacticsIsValidSyllable, N2VowelAcceptsC2Coda) {
    // N2 (ê, i, uê, uy, ua) + C2 (nh, ch) — valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"\x00EA", L"nh", Tone::None,  kModern));   // bênh
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"i",      L"nh", Tone::None,  kModern));   // binh
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"i",      L"ch", Tone::Acute, kModern));   // bích
    EXPECT_TRUE(phon_.IsValidSyllable(L"h", L"uy",     L"nh", Tone::Grave, kModern));   // huỳnh
}

TEST_F(PhonotacticsIsValidSyllable, N2VowelAcceptsC3Coda) {
    // N2 + C3 — valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"i",      L"m", Tone::None,  kModern));   // bim
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"\x00EA", L"n", Tone::None,  kModern));   // bên
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"i",      L"t", Tone::Acute, kModern));   // bít
}

TEST_F(PhonotacticsIsValidSyllable, N2VowelRejectsC1Coda) {
    // N2 + C1 (ng, c) — invalid (N2 forbids C1)
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"i",       L"ng", Tone::None,  kModern));  // bing wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"\x00EA",  L"ng", Tone::None,  kModern));  // bêng wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"i",       L"c",  Tone::Acute, kModern));  // bic wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"\x00EA",  L"c",  Tone::Acute, kModern));  // bêc wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"h", L"uy",      L"ng", Tone::None,  kModern));  // huyng wrong
}

TEST_F(PhonotacticsIsValidSyllable, N3VowelAcceptsAllCodaGroups) {
    // N3 (a, oa, iê, uyê) — accepts C1, C2, C3
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a",        L"ng", Tone::None,  kModern));  // bang (C1)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a",        L"nh", Tone::None,  kModern));  // banh (C2)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a",        L"n",  Tone::None,  kModern));  // ban  (C3)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a",        L"ch", Tone::Acute, kModern));  // bách (C2)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"a",        L"c",  Tone::Acute, kModern));  // bác  (C1)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"i\x00EA",  L"ng", Tone::None,  kModern));  // biêng (iê N3 + C1)
    EXPECT_TRUE(phon_.IsValidSyllable(L"b", L"i\x00EA",  L"nh", Tone::None,  kModern));  // biênh (iê N3 + C2)
}

TEST_F(PhonotacticsIsValidSyllable, MultiVowelN1RejectsC2) {
    // Multi-vowel N1: oă, uô, ươ, uâ, uơ + C2 — invalid
    EXPECT_FALSE(phon_.IsValidSyllable(L"",  L"o\x0103",       L"nh", Tone::None,  kModern));  // oănh wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"u\x00F4",       L"nh", Tone::None,  kModern));  // buônh wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"\x01B0\x01A1",  L"ch", Tone::Acute, kModern));  // bươch wrong
    EXPECT_FALSE(phon_.IsValidSyllable(L"t", L"u\x00E2",       L"nh", Tone::None,  kModern));  // tuânh wrong
}

TEST_F(PhonotacticsIsValidSyllable, MultiVowelN2AcceptsC2) {
    // Multi-vowel N2: uê, uy + C2 — valid
    EXPECT_TRUE(phon_.IsValidSyllable(L"th", L"u\x00EA", L"nh", Tone::None,  kModern));   // thuênh (rule allows)
    EXPECT_TRUE(phon_.IsValidSyllable(L"h",  L"uy",      L"nh", Tone::Grave, kModern));   // huỳnh
}

TEST_F(PhonotacticsIsValidSyllable, UnclassifiedVowelAllowsAnyCoda) {
    // Lenient fall-through for nuclei not in N1/N2/N3 lists (e.g. "oo" used in
    // loanword-style "xoong"). Closed/pending rules handle their own cases first.
    EXPECT_TRUE(phon_.IsValidSyllable(L"x", L"oo", L"ng", Tone::None, kModern));   // xoong (real word)
}

TEST_F(PhonotacticsIsValidSyllable, ClosedVowelRuleStillFiresBeforeNGroup) {
    // Closed-vowel rule rejects any coda regardless of N-group classification.
    // "ai" is closed (no coda allowed). "an" + n-coda would be N3-style if allowed,
    // but the closed-vowel rule short-circuits before reaching N-group check.
    EXPECT_FALSE(phon_.IsValidSyllable(L"b", L"ai", L"n", Tone::None, kModern));   // bain still invalid (closed)
}

//=============================================================================
// CanComplete — partial syllable extensibility (auto-exclusion gate)
//=============================================================================

class PhonotacticsCanComplete : public ::testing::Test {
protected:
    Phonotactics phon_;
};

TEST_F(PhonotacticsCanComplete, EmptyIsCompletable) {
    // Empty string is trivially extensible into any syllable.
    EXPECT_TRUE(phon_.CanComplete(L""));
}

TEST_F(PhonotacticsCanComplete, ValidSyllableIsCompletable) {
    // Already-valid syllables are by definition completable.
    EXPECT_TRUE(phon_.CanComplete(L"ba"));
    EXPECT_TRUE(phon_.CanComplete(L"ban"));
    EXPECT_TRUE(phon_.CanComplete(L"hoa"));
}

TEST_F(PhonotacticsCanComplete, ValidPrefixIsCompletable) {
    // Bare consonant — can be extended with vowels.
    EXPECT_TRUE(phon_.CanComplete(L"b"));
    EXPECT_TRUE(phon_.CanComplete(L"th"));
    EXPECT_TRUE(phon_.CanComplete(L"ng"));
}

TEST_F(PhonotacticsCanComplete, ClosedVowelPlusVowelRejected) {
    // "gach" is fully closed (a is N3 + ch coda). Adding 'a' (→"gacha") cannot
    // form a valid Vietnamese syllable — auto-exclusion case.
    EXPECT_FALSE(phon_.CanComplete(L"gacha"));
    // Sibling case from same bug: "gachw" cannot extend.
    EXPECT_FALSE(phon_.CanComplete(L"gachw"));
}

TEST_F(PhonotacticsCanComplete, NonsenseClusterRejected) {
    // "bcd" — no vowel, can't form syllable.
    EXPECT_FALSE(phon_.CanComplete(L"bcd"));
}

//=============================================================================
// TypingEngine DI plumbing — verifies G-2.1 wiring:
// TypingEngine accepts a custom IPhonotactics via ctor, default-binds to
// Phonotactics::Default() singleton, behavior unchanged from G-1 baseline.
//=============================================================================

TEST(TypingEngineDI, AcceptsCustomPhonotactics) {
    Phonotactics customPhonotactics;
    TypingConfig config;
    TypingEngine engine(config, customPhonotactics);
    // Smoke: engine constructible + functional through DI ctor.
    engine.PushChar(L'a');
    EXPECT_EQ(engine.Peek(), L"a");
}

TEST(TypingEngineDI, SingleArgCtorBindsDefaultPhonotactics) {
    // Existing single-arg ctor must still compile and behave identically;
    // it delegates to Phonotactics::Default() internally.
    TypingConfig config;
    TypingEngine engine(config);
    engine.PushChar(L'a');
    EXPECT_EQ(engine.Peek(), L"a");
}

TEST(PhonotacticsDefault, ReturnsStableSingleton) {
    // Default() must return the same instance every call (singleton lifetime
    // covers any TypingEngine that bound to it).
    const Phonotactics& a = Phonotactics::Default();
    const Phonotactics& b = Phonotactics::Default();
    EXPECT_EQ(&a, &b);
}

}  // namespace
}  // namespace Phonology
}  // namespace NextKey
