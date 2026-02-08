# ADR-006: PDF Extraction Library Selection

**Date:** 2026-02-06
**Status:** Accepted

## Context

The application extracts text and metadata from PDF files to enable searching document content. The original Swift scaffold used Apple's PDFKit, which is native and well-integrated but macOS-only.

With the Qt/C++ rewrite, a cross-platform PDF library is required. The primary need is text extraction and basic metadata reading; full rendering support is not required.

The choice hinges on the licensing model adopted by the project (open-source vs. proprietary/commercial).

## Decision

**Development Strategy**: Use Poppler (GPL) for development and internal testing. Easy setup via Homebrew.

**Release Strategy**: Use PDFium (Apache 2.0) or MuPDF (commercial license) for release builds. PDFium preferred for license cleanliness despite build complexity.

**Rationale**: The project's distribution model is proprietary dual-license (free personal use with limited features, commercial license for full functionality). GPL-licensed Poppler is incompatible with proprietary distribution. The extraction interface abstracts the PDF backend, so switching from Poppler to PDFium before release is low-cost.

The process isolation architecture (ExtractorService as separate binary) provides an additional boundary, but relying on the "separate process" GPL argument is legally contested and should not be the primary strategy.

## Distribution Model Context

The project's licensing model determines PDF library selection:

**Free tier**: Personal use, feature-limited (specific features TBD)

**Commercial tier**: Full functionality, not open source

This model requires all dependencies to be compatible with proprietary distribution. GPL libraries must not be linked in release builds.

## Alternatives Considered

**PDFium (Google/Chromium)**
- Apache 2.0 license (best option for proprietary distribution). Build system is complex; inherits Chromium build tooling and dependency graph. C API exists but is poorly documented. Recommended for release builds despite integration overhead.

**Apache PDFBox**
- Rejected. Java-based; unsuitable for native C++ application. Would require JNI bridging.

**pdf.js**
- Rejected. JavaScript library; unsuitable for native C++ application.

## Consequences

**Development Phase**:
- Fast iteration during prototyping with Poppler (GPL acceptable in development)
- Easy setup: `brew install poppler`
- Good text extraction quality for testing

**Release Phase**:
- Switch to PDFium (Apache 2.0) for final production builds
- Complex build system requires dedicated infrastructure
- Clean licensing for proprietary dual-license model
- PdfExtractor interface abstraction makes backend swappable

**Implementation**:
- Define PdfExtractor interface with methods: extractText(), extractMetadata(), getPageCount()
- Poppler implementation for development, PDFium implementation for release
- Decoupling allows backend replacement with minimal code changes
- Both libraries available on macOS, Linux, and Windows

## Implementation Notes

- Define PdfExtractor interface with methods: extractText(), extractMetadata(), getPageCount().
- Implement platform-specific loading of PDF library (optional at build time).
- Embed Poppler language data files (minimal for text extraction) in application resources.
- Implement error handling for corrupted or malformed PDFs; failures should not crash Extractor service.
- Cache extracted text keyed by PDF content hash to avoid re-extraction.
