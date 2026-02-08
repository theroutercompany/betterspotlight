# BetterSpotlight Documentation

macOS desktop file search application (Spotlight replacement) for power users. Qt 6/C++.

---

## Foundation

Cross-cutting technical specifications that apply to all milestones.

| Document | Description |
|----------|-------------|
| [Architecture Overview](foundation/architecture-overview.md) | System design: layers, components, data flow |
| [Storage Schema](foundation/storage-schema.md) | SQLite schema, FTS5 definition, table relationships |
| [Indexing Pipeline](foundation/indexing-pipeline.md) | FSEvents → validation → extraction → chunking → FTS5 |
| [IPC & Service Boundaries](foundation/ipc-service-boundaries.md) | Unix socket protocol, message format, service isolation |
| [Ranking & Scoring](foundation/ranking-scoring.md) | Composite scoring formula, match types, boost algorithms |
| [Security & Data Handling](foundation/security-data-handling.md) | Privacy, sensitive paths, data retention |
| [Build System](foundation/build-system.md) | CMake configuration, dependencies, packaging |

## Milestones

| Document | Description |
|----------|-------------|
| [Acceptance Criteria](milestones/acceptance-criteria.md) | Testable exit criteria for M1, M2, M3 |

### M1: Basic Spotlight Replacement

| Document | Description |
|----------|-------------|
| [Kickstart Prompt](milestones/m1/kickstart-prompt.md) | Development work streams for parallel sub-agent execution |
| [Completion Roadmap](milestones/m1/completion-roadmap.md) | Gap analysis and execution plan to M1 sign-off |
| [Audit Gaps](milestones/m1/audit-gaps.md) | Implementation vs. spec discrepancies |

### M2: ML + Semantic Search + 80% Reliability

| Document | Description |
|----------|-------------|
| [Embedding Pipeline](milestones/m2/embedding-pipeline.md) | ONNX Runtime, BGE-small-en-v1.5, tokenization, batching, quantization |
| [Vector Search](milestones/m2/vector-search.md) | hnswlib index, KNN search, semantic-lexical merge algorithm |
| [Feedback System](milestones/m2/feedback-system.md) | Interaction tracking, path preferences, file type affinity, aggregation |
| [Onboarding & Settings](milestones/m2/onboarding-settings-spec.md) | 3-step first-run flow, 5-tab Settings UI, Index Health dashboard |
| [Relevance Testing](milestones/m2/relevance-testing.md) | 50-query test corpus, fixture definition, scoring methodology |
| [Kickstart Prompt](milestones/m2/kickstart-prompt.md) | 12 parallel dev work streams across 4 waves |

### M3: Polished App + 95% Reliability (future)

Planned: error messaging, 100-query corpus, 48-hour stress test, Sparkle auto-update.

## Decisions

Architecture Decision Records documenting key technical choices.

| ADR | Decision |
|-----|----------|
| [ADR-001](decisions/adr-001-qt-cpp-over-swift.md) | Qt 6/C++ over Swift/SwiftUI |
| [ADR-002](decisions/adr-002-fts5-lexical-search.md) | FTS5 for lexical search |
| [ADR-003](decisions/adr-003-fsevents-file-watching.md) | FSEvents C API for file watching |
| [ADR-004](decisions/adr-004-process-isolation.md) | Process isolation via Unix sockets |
| [ADR-005](decisions/adr-005-tesseract-ocr.md) | Tesseract for OCR |
| [ADR-006](decisions/adr-006-pdf-library-selection.md) | Poppler (dev) / PDFium (release) for PDF |

## Operations

Project management and migration tracking.

| Document | Description |
|----------|-------------|
| [Dependency Audit](operations/dependency-audit.md) | Third-party library evaluation and status |
| [Migration Mapping](operations/migration-mapping.md) | Swift → C++ component mapping |
| [Swift Deprecation Audit](operations/swift-deprecation-audit.md) | File-by-file classification: delete, port, keep |
