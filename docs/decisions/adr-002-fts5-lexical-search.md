# ADR-002: FTS5 for Lexical Search

**Date:** 2026-01-19 (carried from original design)
**Status:** Accepted

## Context

The application requires full-text search across extracted file content with the following requirements:
- Sub-100ms response time for typical user queries
- Support for prefix matching, phrase queries, and relevance ranking
- Local, persistent, embeddable index (no external server dependency)
- Indexes text extracted from plain text files, source code, markdown, PDF text layers, and OCR output
- Typical corpus size: 100K to 1M files on desktop systems

## Decision

Use SQLite FTS5 (Full-Text Search 5) with Porter stemmer and Unicode61 tokenizer for all lexical search operations.

## Alternatives Considered

**Xapian**
- Rejected. Faster for very large corpora, but overkill for desktop-scale search; significantly larger index sizes; more complex API; higher learning curve.

**Tantivy (Rust)**
- Rejected. Excellent performance characteristics, but introduces Rust dependency; requires FFI complexity in a C++ project; adds build system friction.

**Lucene/CLucene**
- Rejected. Mature ecosystem, but CLucene is unmaintained; larger memory footprint; unnecessary complexity for desktop search scale.

**Custom inverted index**
- Rejected. Engineering effort is not justified; FTS5 covers all requirements with proven reliability.

## Consequences

### Positive

- Zero additional dependencies beyond SQLite, which is already a project dependency.
- Single-file database format simplifies backup, versioning, and distribution.
- Built-in BM25 ranking with configurable per-column weights enables relevance tuning.
- Native support for prefix queries and phrase matching without post-processing.
- Snippet extraction enables preview of context in search results.
- Well-documented, battle-tested in production systems; large community.

### Negative

- Limited to lexical matching; no semantic or neural similarity search (addressed separately by planned VectorIndex in M2).
- Porter stemmer is English-centric, acceptable for v1 targeting English-speaking power users.
- FTS5 index overhead adds approximately 30-50% size on top of raw extracted text.
- Configuration tuning (tokenizer, stemmer, column weights) requires testing and iteration per domain (e.g., code vs. prose).

## Implementation Notes

- Wrap FTS5 operations in a SearchIndex class with defined interface for potential future backend swapping.
- Configure Porter stemmer for English; evaluate Unicode61 tokenizer customization for code identifiers.
- Implement relevance weighting: higher weights for filename/title, medium for document content, lower for extracted metadata.
- Store document vector offsets for efficient snippet extraction.
