# BetterSpotlight Retrieval Remediation Memo
**Date:** February 9, 2026  
**Audience:** Senior Engineering Executive  
**Scope:** Current system behavior, shipped remediation, KPI delta, residual risk, and 2-week plan

## 1) Executive Summary
- We shipped a focused non-ML remediation batch (query planning, root policy, extraction-state surfacing, telemetry parity, and operational reset to curated roots).
- The product moved from unusable user-facing relevance in this environment to passing the current quality gate target.
- Post-remediation live simulation (through `query.search`) is now **88.37% pass rate** with **p95 143.83 ms**.
- Remaining gap is concentrated in semantic-probe and typo edge cases, plus extraction availability for offline placeholders.

## 2) Problem Statement (Observed Behavior)
Before remediation, retrieval quality was dominated by corpus noise and root misconfiguration:
- Large high-fanout indexing roots introduced lexical pollution (developer/cache/config artifacts overpowering consumer files).
- Query planner/path constraints were inconsistently applied across runtime scenarios.
- Health and queue telemetry were previously inconsistent, reducing operational trust.
- Some user-critical files were present but unavailable for extraction (offline placeholders), creating misleading relevance outcomes.

## 3) What Was Shipped
## Code-level improvements
- Query planner and filter wiring in `/Users/rexliu/betterspotlight/src/services/query/query_service.cpp`
  - Consumer prefilter to curated roots for natural-language intent.
  - Path/code-like queries still allowed broad behavior.
  - Typo auto guardrails preserved deterministic behavior.
  - Semantic candidate path/filter enforcement aligned with lexical filter scope.
- Search options/store updates:
  - `/Users/rexliu/betterspotlight/src/core/shared/search_options.h`
  - `/Users/rexliu/betterspotlight/src/core/index/sqlite_store.cpp`
- Hidden directory policy updated:
  - `/Users/rexliu/betterspotlight/src/core/fs/path_rules.cpp`
  - `/Users/rexliu/betterspotlight/src/core/fs/path_rules.h`
  - Hidden paths excluded by default unless explicitly included as roots.
- Root derivation and onboarding/settings consistency:
  - `/Users/rexliu/betterspotlight/src/app/service_manager.cpp`
  - `/Users/rexliu/betterspotlight/src/app/settings_controller.cpp`
  - `/Users/rexliu/betterspotlight/src/app/onboarding_controller.cpp`
- Queue/health observability parity in `query.getHealth` plus fanout advisory.

## Operational remediation executed
- Enforced curated active roots: `Documents`, `Desktop`, `Downloads`.
- Cleared stale high-noise corpus and re-indexed curated scope.
- Completed vector rebuild for refreshed corpus.

## 4) KPI Delta (Live, Same Environment)
## Query suite (UI-like simulation via `query.search`)
- Baseline (pre-op remediation state): `/tmp/bs_queryservice_ui_sim_report.json`
  - Pass rate: **0.00%** (0/43)
  - Latency: **avg 181.21 ms**, **p95 259.66 ms**
- Current (post-op remediation): `/tmp/bs_queryservice_ui_sim_report_after_ops.json`
  - Pass rate: **88.37%** (38/43)
  - Latency: **avg 54.23 ms**, **p95 143.83 ms**

## Category pass rates (current)
- Exact: **100%** (12/12)
- Phrase: **100%** (8/8)
- Noise: **100%** (5/5)
- Typo auto: **80%** (8/10)
- Semantic probe: **62.5%** (5/8)

## Current health snapshot
- Source: `/tmp/bs_current_state_snapshot.json`
- `queueSource=indexer_rpc`, queue drained (`pending=0`, `processing=0`)
- `indexRoots` now exactly curated 3 roots
- Vector rebuild succeeded (`processed=2017`, `embedded=2017`, `failed=0`)

## 5) Residual Risks / Gaps
- `overallStatus=degraded` remains due extraction/content coverage signals, not queue backlog.
- Coverage is not expected to be 100% because:
  - offline placeholders / inaccessible docs
  - unsupported or low-value extract targets
  - non-content file classes intentionally indexed for path/name recall
- Remaining miss patterns are concentrated in:
  - semantic probe disambiguation
  - typo rewrite edge conditions
  - offline placeholder scenarios where filename fallback is correct but content evidence is absent

## 6) Decision / Recommendation
- **Proceed** with current architecture and incremental retrieval improvements; no immediate need for heavy reranker integration.
- Keep cross-encoder reranker deferred until post-Phase-2 metadata/filter enhancements are complete and re-measured.
- Continue to prioritize extraction reliability and metadata-aware retrieval before adding new model-serving complexity.

## 7) Two-Week Roadmap (Execution-Ready)
## Week 1: Quality hardening on current stack
- Improve typo-auto policy for known failure pairs (confidence + token-context checks).
- Strengthen semantic probe routing with better lexical/metadata gating for ambiguous broad terms.
- Add explicit regression fixtures for the 5 current failing cases.
- Deliverables:
  - Updated tests and suite report with target **>=90% effective pass**.
  - p95 maintained **<160 ms** under active indexing.

## Week 2: Metadata/filter retrieval uplift (no heavy new ML)
- Implement richer metadata predicates (kind/path/date/availability) in first-pass candidate generation.
- Improve fallback ordering using availability and query intent (consumer vs code intent split).
- Surface clearer user-facing reason labels for unavailable docs to reduce “wrong top result” perception.
- Deliverables:
  - semantic_probe pass rate improvement from **62.5% -> >=80%**.
  - typo_auto pass rate improvement from **80% -> >=90%**.

## 8) Artifacts / Evidence
- Pre-op report: `/tmp/bs_queryservice_ui_sim_report.json`
- Post-op report: `/tmp/bs_queryservice_ui_sim_report_after_ops.json`
- Current state snapshot: `/tmp/bs_current_state_snapshot.json`
- Relevant shipped commits:
  - `9839d95`
  - `de2b2cb`
  - `f9f964c`

