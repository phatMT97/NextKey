# NexusKey Refactor Status — Living Inventory

> **Last refresh:** 2026-05-08 (post T2 + T3, both PR-pending stacked on Main)
> **Scope:** All architectural / cleanup refactor work. Excludes user-facing features (G-5/G-6 Sciter UI + keymap files, TSF Phase 2/3, etc.) — those track separately.
> **Sequencing rule (anh decision 2026-05-07):** Complete HookEngine refactor backlog BEFORE picking up TypingEngine TODO items. **Post-H1c the rule is satisfied; T5 done; T2/T3 done.** Remaining: T6 retest (anh confirmed test ổn 2026-05-08, can close).

---

## Codebase vital signs (snapshot 2026-05-07)

| Metric | Value |
|---|---|
| Total production LOC (`src/`) | 37,865 |
| App layer LOC | 25,496 (67%) |
| Engine layer LOC | 5,419 |
| TypingEngine.cpp | 1,662 LOC |
| HookEngine.cpp | **3,563 LOC** (+135 vs post-H5 from verbose H1 helper headers; +178 vs original peak) |
| `ProcessKeyDown` (HookEngine) | **79 LOC** orchestrator (down from pre-H1 561; -86%) — cache GetKeyState + 4 switch-on-outcome + final return |
| `RunTopGuards` (H1b) | ~50 LOC body — TSF / modifier / toggle / excluded-app guards |
| `HandleCommitUndo` (H1a) | ~190 LOC body — Idle/Ready/Primed FSM for backspace-into-committed-word |
| `HandlePreDispatch` (H1c) | ~107 LOC body — English mode + auto-caps + macro tracking |
| `DispatchKeyAction` (H1c) | ~158 LOC body — alpha / Telex bracket / VNI digit / BS / commit trigger / fallthrough |
| `core/MacroCase.{h,cpp}` (new H5) | 171 LOC — pure free helpers (`Macro::Plan` / `BuildSegments` / `ExpandEscapesForClipboard`) |
| `app/system/Win32CaseMapper.h` (new H5) | 28 LOC — production `CaseMapper` impl wrapping `CharUpperBuffW`/`CharLowerBuffW` |
| GTest count | 1,515 / 1,515 PASS Linux (+38 H5 tests; H1 added zero — pure refactor) |
| TODO/FIXME density | 3 markers total in src/ (very low) |
| Open GitHub issues | 11 (6 bugs / 4 enhancements / 1 wontfix) |
| Layering violations | **None** (engine/config has zero Win32 deps) |
| Dependency state | Sciter 6.0.3.17, toml++ 3.4.0, GoogleTest local-tagged |

---

## Section A — Refactor work DONE

| Sprint / Plan | Merge | Date | Scope |
|---|---|---|---|
| Phase 0a TestRunner | #114 `56a09d2` | 2026-05-04 | Chaos harness, edit_distance verdict, JUnit/CSV reporters |
| **Sprint 1** Single-Owner Hook | #115 `c34497a` | 2026-05-05 | Atomic flags (D5), RCU `shared_ptr<TypingConfig>` (D6), audit CI (D7), MainThreadWorker (D8-D10), `recursive_mutex` drop (D11), chaos baselines (D12), RichEditD2DPT routing |
| **Sprint 2 T3** IOutputInjector | #120 `61639b9` (+ #121, #123) | 2026-05-05 | D0-D6 trọn bộ: Win32SendInput / RichEditEm / SplitDispatch injectors, factory, ChannelTraits cleanup, SettleBudget (D5), HookEngine 3593 → ~3300 LOC |
| FSM codegen tool | #132 `4a52399` | 2026-05-06 | Standalone Python NFA→DFA→Hopcroft tool — salvage on Main, FSM rewrite cancelled |
| EscapeState + QuickConsonantState refactor | `cc3ac21` | 2026-04-11 | TelexEngine field consolidation |
| TypingEngine unification | `e5b21fc` | 2026-04-16 | TelexEngine → TypingEngine; VNI routed through one engine; `InputMethod::Combined` |
| TSF DLL hybrid update | `c1e9ce2` | 2026-04-22 | SharedState ABI gate + deferred DLL swap |
| **Sprint 3 Path G G-1..G-4** | #134-#138 | 2026-05-07 | Phonotactics class, DI, TypingAction enum, unified ProcessModifier dispatch, customKeyMap engine hook |
| **Post-Path-G cleanup** (H2 + H7 + T1 + T4) | #139 `64fb80f` | 2026-05-07 | Delete `HookEngine::CheckConfigEvent` dead code (H2); strip Sprint 2 D4/T3 history comments (H7); rename `SpellChecker → PhonotacticsValidator` (T1); verify Hot-path Fix 3 shipped (T4). H4 attempted then reverted (wontfix — member is logging adapter, not dup) |
| **H3** atomic migration `cachedFocusedHwnd_` | #140 `6ebc603` | 2026-05-07 | Closes Pre-T3 Minor 1: `HWND` field → `std::atomic<HWND>` with `relaxed` memory order. Tuple race with `cachedFocusedClass_` (wstring) documented as benign in field comment. 4 stores + 2 loads updated. |
| **H5** Macro extract to pure functions | #141 `a141548` | 2026-05-07 | `TryExpandMacro` 217-LOC body → ~50-LOC orchestrator delegating to `Macro::Plan` + `ExpandEscapesForClipboard` + `BuildSegments` (3 free helpers in new `core/MacroCase.{h,cpp}`, Linux-portable). DI seam via `Macro::CaseMapper` abstract — production wraps Win32 `CharUpperBuffW`/`CharLowerBuffW` (`Win32CaseMapper.h`); tests use `AsciiCaseMapper`. HookEngine.cpp 3582 → 3428 LOC (-154). +38 gtests. Chaos 55/55 PASS across notepad/notepad++/chrome/discord/gpt. Behavior byte-identical to Main `3257758` per spec NF2. |
| **H1a** `HandleCommitUndo` extract | #143 `e820876` | 2026-05-07 | Step 2d (commit-undo Idle/Ready/Primed FSM, 187 LOC) extracted to private method with `KeyOutcome { Eat / Pass / Fallthrough }` enum. ProcessKeyDown 561 → 380 LOC. Chaos 55/55 PASS. Pre-merge naming fix: `kEat/kPass/kFallthrough` → `Eat/Pass/Fallthrough` to match project convention (CommitUndoState/MacroResult/AutoCapState/etc. all PascalCase no prefix). |
| **H1b** `RunTopGuards` extract | #144 `3c06499` | 2026-05-07 | Steps 0/0b/1/1b/1c (TSF early-out / modifier track / toggle keys / excluded-app PID verification, 54 LOC) extracted. Bookkeeping (`otherKeyPressed_/altTapCount_/synth-watchdog`) stays in ProcessKeyDown post-switch — runs only on Fallthrough; excluded-app's same-PID/still-excluded paths set `otherKeyPressed_` themselves before returning Pass. ProcessKeyDown 380 → 327 LOC. `CommitUndoOutcome` renamed to generic `KeyOutcome` for sharing across H1a/H1b/H1c. Chaos 55/55 PASS. |
| **H1c** `HandlePreDispatch` + `DispatchKeyAction` extract | #145 `659b910` | 2026-05-07 | Steps 3 / 3a-3d (English mode + auto-caps + macro tracking, ~107 LOC) → `HandlePreDispatch`. Steps 4b-10 (tempEngineOff / Ctrl-Alt-Win / alpha / bracket / VNI digit / BS / commit-trigger / fallthrough, ~158 LOC) → `DispatchKeyAction`. Cache shifted from after-step-3 to before HandlePreDispatch — byte-identical because GetKeyState within hook callback is stable. ProcessKeyDown 327 → **79 LOC** orchestrator (-86% cumulative across H1). Chaos 55/55 PASS. **HookEngine refactor backlog effectively closed.** |
| **T5** Tone replacement on invalid buffer recovers validity | #146 `1ade8dc` | 2026-05-07 | New helper `TypingEngine::IsToneStopCodaMismatch()` + 3 gate-relaxations (tone gate, modifier outer gate, `WouldBeValidSyllable`). Engine no longer treats tone/modifier as literal when current buffer is Invalid solely because of huyền/hỏi/ngã + stop coda — the user is mid-correction; next tone keystroke recovers. Generic across vowels (a/e/o/ô) + Telex/VNI modifier paths. Linux 1524/1524 PASS (+9 T5 tests). Windows MSVC clean. Hot-path cost ~hundreds of ns on rare `spellCheckDisabled_` path, within Rule #11.1 1 ms budget. |
| **T2** Phonotactics onset/vowel agreement | PR #147 | 2026-05-08 | `Phonotactics::IsValidSyllable` no longer ignores `onset` arg. New helper `IsOnsetVowelAgreementValid` enforces c/k, g/gh, ng/ngh front-back agreement against first base vowel of `vowelSeq`; qu intentionally exempted (qua/quan/quát outside the canonical labial-diphthong list). Production zero-touch — interface had no production caller; this closes the contract for future consumers (spell-check overlay, dictionary lookups). Linux 1540/1540 PASS (+16 T2 tests). |
| **T3** Phonotactics N1/N2/N3 vowel-coda groups | PR pending (stacked on T2) | 2026-05-08 | New helpers `ClassifyCoda` (C1/C2/C3 partition) + `ClassifyVowelGroup` (N1/N2/N3 lookup) + `IsCodaCompatibleWithVowelGroup` wired into `IsValidSyllable` after closed/pending checks. Lenient fall-through for nuclei not in N1/N2/N3 lists. Production zero-touch (same path-2 layer as T2). Linux 1551/1551 PASS (+11 T3 tests). **Note**: coarser than `PhonotacticsValidator.cpp:159-204 kVCPairRules` per-nucleus bitmask (Path 1 production validator). VCPair is stricter (e.g. ơ → only m/n/p/t; iê → no ch/nh) — when Path 2 ever gains a production caller, port VCPair rather than relying on the N-group approximation. See T2.1. |

---

## Section B — Refactor work IN FLIGHT (paused)

### B-1 Sprint 3 FSM rewrite (CANCELLED, branch lingers)

| Field | Value |
|---|---|
| Branch | `sprint-3/fsm-engine` (local only, never pushed) |
| Last commit | `b7d0202` (handoff note) |
| Status | Brainstorm 2026-05-07 confirmed ≥19 MB DFA table required → cancelled. Path G replaces. |
| **Action** | Delete local branch `git branch -D sprint-3/fsm-engine` to avoid future confusion. The standalone codegen tool is on Main via PR #132 — keep that. |

### B-2 Sprint 2 T3 D6 `--host-class` matrix harness (DEFERRED)

| Field | Value |
|---|---|
| Status | Explicitly deferred per `docs/TODO.md` |
| Rationale | "Marginal value given existing 132-case natural coverage; reopen if QA needs forced-cell testing" |
| **Action** | Keep deferred. Reopen as focused task only if specific QA need surfaces. |

---

## Section C — HookEngine refactor backlog (DO FIRST per sequencing rule)

| ID | Item | Source | Effort | Priority |
|---|---|---|---|---|
| ~~H1~~ | ~~**`ProcessKeyDown` 561-LOC god-method decompose**~~ — shipped as 3 PRs: H1a (`HandleCommitUndo` #143) + H1b (`RunTopGuards` #144) + H1c (`HandlePreDispatch` + `DispatchKeyAction` #145). ProcessKeyDown 561 → 79 LOC (-86%). | — | DONE | ✅ |
| ~~H2~~ | ~~**Delete dead code** `HookEngine::CheckConfigEvent()` + `configEvent_` member~~ | — | DONE | ✅ PR #139 `5f080ca` |
| ~~H3~~ | ~~**`LowLevelMouseProc` race on `cachedFocusedHwnd_`**~~ | — | DONE | ✅ PR #140 `58d8f88` |
| ~~H4~~ | ~~**Dual-route `TrackedSendInput` consolidate**~~ — REJECTED 2026-05-07: `HookEngine::TrackedSendInput` is NOT a duplicate — it's a logging adapter that wraps `Internal::TrackedSendInput` and emits `HOOK_LOG` on partial sends (renderer-drop diagnostic). Consolidation attempt at commit `794f38f` lost this observability and broke MSVC `/WX` (`[[nodiscard]]` warning C4834 at 6 call sites). Reverted at `66ae1dc`. The dual-route is justified — member adds value. | docs/TODO.md M3 | N/A | ❌ Wontfix |
| ~~H5~~ | ~~**Macro extract to pure functions**~~ | — | DONE | ✅ PR #141 `a141548` |
| H6 | **Sprint 4 §3 SPSC ring + watchdog** — Rule #11 next-stage compliance | CODE_GOVERNANCE.md §3 | Sprint scale | LOW (roadmap) |
| ~~H7~~ | ~~**Strip Sprint 2 D4/T3 history comments**~~ | — | DONE | ✅ PR #139 `4042dcc` |
| H8 | **Sprint 1 deferred** — `WaitOnAddress` for configEpoch, ETW tracing, hook fast-path foreground detection | sprint-1-single-owner-refactor.md "Open items" | Sprint scale | LOW (roadmap) |

---

## Section D — TypingEngine refactor backlog (DO AFTER HookEngine per sequencing rule)

| ID | Item | Source | Effort | Priority |
|---|---|---|---|---|
| ~~T1~~ | ~~**`SpellChecker.{h,cpp}` → `PhonotacticsValidator` rename**~~ | — | DONE | ✅ PR #139 `b115f75` |
| ~~T2~~ | ~~**Phonotactics onset agreement** — c/k, g/gh, ng/ngh enforcement (qu exempted)~~ | — | DONE | ✅ PR #147 |
| T2.1 | **Phonotactics rule duplication between `Phonotactics.cpp` (Path 2) and `PhonotacticsValidator.cpp` (Path 1)** — both encode the same Vietnamese phonotactic rules: (a) c/k/g/gh/ng/ngh onset agreement (T2 / PV:684-715), (b) per-nucleus allowed-coda restrictions (T3 N1/N2/N3 / PV:159-204 `kVCPairRules` bitmask). PV bitmask is granular per-nucleus and **stricter** than T3's coarse 3-group bucketing; the wstring_view layer's N-group is an approximation. Lift to shared rule tables (e.g. `VietnameseTables.h`) when a third validator wants the rule, and prefer the bitmask granularity in any unified table. Pre-existing; surfaced during T2/T3 reviews. | T2 + T3 simplify reuse agents | 1-2h | LOW (cleanup) |
| ~~T3~~ | ~~**Phonotactics N1/N2/N3 vowel-coda compatibility**~~ | — | DONE | ✅ PR pending |
| ~~T4~~ | ~~**Verify Hot-path Fix 3 ComposeAll buffer reuse**~~ — VERIFIED 2026-05-07: shipped at `TypingEngine.h:218` (`mutable std::wstring composeBuf_`) + `TypingEngine.cpp:1236-1241` | hot-path-optimization-plan.md | DONE | ✅ |
| ~~T5~~ | ~~**Bug `cafcs → các`**~~ | — | DONE | ✅ PR #146 `1ade8dc` |
| T6 | **Bug Issue #117 `Lỗi → Lôĩ`** — fast-typing chaos timing. **Anh decision 2026-05-07 post-H1:** retest first before scheduling fix work — H1's hot-path restructure may have shifted timing enough that the race no longer reproduces. If reproducible post-H1, schedule deep work; if not, monitor + close as auto-resolved. | HANDOFF.md + Issue #117 | Hard — chaos timing | Bug (LOWER, retest gate) |

> **Note on T5/T6:** Bugs strictly speaking, not refactor. Listed here because they touch engine internals. Anh quyết định fix cùng Path G T-batch hay tách bug-fix branch riêng.

---

## Section E — Dead code confirmed (verify before delete)

Verified by grep on Main `3ded489` (2026-05-07):

| Item | Location | Status |
|---|---|---|
| `HookEngine::CheckConfigEvent()` body | HookEngine.cpp:538 | DEAD — zero callers in src/app + src/core. Tracked as H2. |
| `HookEngine::configEvent_` member | HookEngine.h:446 | CASCADE-DEAD via H2. Initialize/Wait calls in HookEngine.cpp:173, 541-544 only feed CheckConfigEvent. |
| 4 atomic flag stores (`useEditMsgPath_`, `isConsoleApp_`, `isElectronApp_`, `needBaitChar_`) | HookEngine.cpp/h | Already deleted (Sprint 2 D4 + ChannelTraits cleanup). Only history comments remain. Optional cull as H7. |
| `EngineHelpers::FindToneTargetImpl` | (deleted) | Already cleaned during Path G G-2.4 |
| `TelexEngine.{cpp,h}` / `VniEngine.{cpp,h}` | (deleted) | Already cleaned post TypingEngine unification |
| `DispatchSendInput()` body | (deleted) | Already cleaned during Sprint 2 T3 D3 |
| Hot-path Fix 3 ComposeAll buffer reuse | TypingEngine.h:218 + TypingEngine.cpp:1236-1241 | Verified shipped 2026-05-07 |

**Misleading filenames (NOT dead, rename pending):**
- `src/core/engine/SpellChecker.cpp/h` (812 LOC) — content under `NextKey::Phonology::`, files keep old name. Tracked as T1.

**File reconcile (verified clean):**
- All `.cpp` on disk are referenced in CMakeLists.txt
- CMakeLists references no missing files
- Zero empty folders in `src/` and `tests/`

**Dead docs/plans:** `docs/plans/sprint-3-fsm-engine-plan.md` (cancelled) and `docs/plans/2026-03-28-sendinput-universal-output.md` (superseded by T3) are HISTORICAL records — keep, don't delete.

---

## Section F — Sequencing rule (anh decision 2026-05-07)

**Rule:** Complete the HookEngine refactor backlog (Section C, items H1-H6 except H8 which is roadmap) BEFORE picking up TypingEngine TODO items (Section D, items T1-T4).

**Rationale:**
- HookEngine is the highest-risk file (was 3615 LOC, now 3428 post-H5 — still hottest in codebase). Architectural decomposition is the priority.
- TypingEngine is in good shape post Path G G-1..G-4. Its remaining items are correctness-quality rather than architecture.
- Sequencing prevents context-switching cost and ensures HookEngine refactor stays a focused effort.

**Exception:** Quick wins from BOTH layers may bundle into a single cleanup PR if grouped (e.g. H2 + H4 + H7 + T1 + T4 = single "post-Path-G cleanup" PR ≤ 1 day).

**Bug fixes** (T5, T6, and any new bug surfaced by users) are NOT bound by this rule — they ship when ready.

---

## Section G — Recommended next steps

**Already shipped today (2026-05-07):** PR #138 (Path G G-4 customKeyMap), PR #139 (cleanup H2 + H7 + T1 + T4), PR #140 (H3 atomic migration), PR #141 (H5 Macro extract), PR #142 (H5 follow-ups), PR #143 (H1a HandleCommitUndo), PR #144 (H1b RunTopGuards), PR #145 (H1c HandlePreDispatch + DispatchKeyAction), PR #146 (T5 tone-replace recovery). Plus tooling `2e30f82` (chaos graceful shutdown + lôĩ stress) + 3 handoff/doc commits. Total ~10 PRs / ~55 commits / 1 active day.

**Immediate (next session — start here):**
1. **T6 retest** — try to reproduce Issue #117 `Lỗi → Lôĩ` post-H1. May have been incidentally resolved by hot-path restructure (anh hypothesis 2026-05-07). 15 min to verify; if reproducible → deep timing work, if not → close auto-resolved.

**Short-term:**
2. **T2/T3 — Phonotactics deepening** — onset agreement (c/k/qu, g/gh, ng/ngh) + N-group vowel-coda compatibility. Builds on Path G groundwork. 2-3h each, MEDIUM correctness.

**User-facing alternatives** (out of refactor scope, but anh may pivot):
- **G-5/G-6** — keymap UI (Sciter dialog + per-user TOML files).
- Triage open GitHub Issues — close `wontfix` duplicates, re-classify bugs vs enhancements.

**Roadmap (defer until current backlog clears):**
4. **H6 — Sprint 4 §3 SPSC ring + watchdog** — multi-week effort. Captured in CODE_GOVERNANCE.md §3.
5. **H8 — Sprint 1 deferred** — WaitOnAddress / ETW / fast-path foreground detection.

**General checklist:**
- Open GitHub issues triage — close any wontfix duplicates, re-triage bugs vs enhancements (Issue #92 marked `wontfix` may be closable).
- Cleanup local `sprint-3/fsm-engine` branch (cancelled) — `git branch -D sprint-3/fsm-engine`.

---

## How to update this doc

This is a living inventory — update as items ship or scope changes:

- **Item shipped:** move from C/D to A with merge SHA + date.
- **New refactor item discovered:** add to C (HookEngine) or D (TypingEngine) with effort estimate.
- **Sequencing decision changed:** update Section F.
- **Vital signs drift:** refresh top table at next major checkpoint.

When this doc is stale (>1 month since last refresh), re-run the survey via:
```
Agent (Explore, very thorough): codebase health survey + refactor inventory cross-reference vs latest Main
```
