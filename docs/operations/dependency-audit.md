# Dependency Audit & Licensing

**Status**: Active | **Last Updated**: 2025-02 | **Ownership**: Tech Lead

This document catalogs every third-party dependency for BetterSpotlight, including version targets, licensing constraints, linking strategies, and risk assessments. It serves as both an inventory and a licensing compliance checklist.

---

## Executive Summary

BetterSpotlight's licensing flexibility is **distribution-dependent**:

- **Proprietary Distribution**: Feasible with careful dependency selection (avoid GPL2/GPL3 strong copyleft at link time)
- **GPL Distribution**: All dependencies compatible, but requires GPL 2.0 or 3.0 license for the entire project
- **Hybrid Approach** (Not Recommended): GPL components dynamically linked as separate deliverables creates legal ambiguity and user confusion

**Key Constraint**: Qt is LGPL 3.0 / GPL 2.0. For proprietary builds, dynamic linking is **mandatory** to allow user relinking.

---

## Dependency Matrix

### 1. Qt 6.10+

| Aspect | Details |
|--------|---------|
| **Version Range** | Qt 6.10 or later (LTS preferred: 6.10+) |
| **License** | LGPL 3.0 (primary) / GPL 2.0 (alternative) / Commercial (€530/year small business tier) |
| **Components Used** | Qt Core (event loop, strings, file I/O), Qt GUI/QML (UI framework), Qt Network (placeholder for future features), Qt SQL (SQLite driver) |
| **Linking Strategy** | **Dynamic (required for proprietary)** |
| **Why Linking Matters** | LGPL requires that users can relink the application with their own modified version of Qt. Static linking would require proprietary code to also be under LGPL, defeating the purpose. |
| **macOS Specifics** | Qt on macOS uses native frameworks; Qt 6.10+ includes M1/M2 support. Ship as framework bundle or via package manager (brew install qt@6). |
| **Risk Assessment** | **Low**. Qt is industry-standard, backed by Qt Company, active maintenance cycle. Community is large. |
| **Alternatives** | wxWidgets (LGPL), Gtk+ (LGPL), custom Cocoa (high development cost). None viable for rapid M1 support. |
| **Action Items** | Verify CMake FindQt6 module availability; test on M1 and Intel runners in CI; document Framework search path for packaging. |

**Licensing Implication**: Proprietary distribution requires:
```
BetterSpotlight ships UNMODIFIED Qt 6 libraries as dynamic .framework bundles.
Source must include Qt license notice and download link: https://www.qt.io/download
Users retain right to relink against modified Qt.
```

---

### 2. SQLite 3.x

| Aspect | Details |
|--------|---------|
| **Version Range** | SQLite 3.40+ (FTS5 support, modern features). Ship with pinned version in vcpkg or brew. |
| **License** | Public domain (can use freely, no restrictions) |
| **Purpose** | Metadata storage (file path, size, modified time, indexed flags) in 7 normalized tables; FTS5 lexical full-text search index for fast token-based queries. |
| **Linking Strategy** | **Direct static linking OR dynamic via Qt's driver** |
| **Why Linking Matters** | Public domain has no copyleft. Can link either way without license implications. **Recommendation**: Statically link sqlite3.a to simplify macOS distribution (no separate .dylib to ship) AND to ensure FTS5 is built in. Qt's bundled SQLite driver is often limited. |
| **macOS Specifics** | macOS ships with SQLite, but version is outdated and cannot be relied upon for FTS5. Statically link your own version. |
| **Risk Assessment** | **Very Low**. SQLite is de facto standard, well-maintained by D. Richard Hipp, been stable for 20+ years. No fork risk. |
| **Alternatives** | LevelDB (Google, but requires custom query layer), RocksDB (Facebook, C++), custom file indexing (not realistic). |
| **Action Items** | Pin SQLite version in CMakeLists.txt or vcpkg manifest; test FTS5 tokenizer behavior with Unicode filenames; add SQLite pragma for WAL mode and memory-mapped I/O. |

**Licensing Implication**: Zero restrictions. Safe for proprietary and GPL builds.

---

### 3. Tesseract OCR 5.x + Leptonica

| Aspect | Details |
|--------|---------|
| **Version Range** | Tesseract 5.4+, Leptonica 1.84+ |
| **License** | Tesseract: Apache 2.0 | Leptonica: BSD 2-Clause |
| **Purpose** | Optical character recognition (OCR) for image files (.png, .jpg) and screenshots. Extracts visible text for indexing. |
| **Dependency Chain** | Tesseract requires Leptonica for image manipulation (rotation, scaling, preprocessing). Both must be shipped. |
| **Linking Strategy** | **Static or dynamic, both safe.** Apache 2.0 is permissive; BSD 2-Clause is permissive. Either linking strategy is compatible with proprietary and GPL. **Recommendation**: Static link both to reduce runtime dependencies and simplify distribution. |
| **macOS Specifics** | Homebrew provides bottles for both. Precompiled binaries available; ensure M1/M2 compatible versions. |
| **Trained Data Files** | Ship English LSTM model (~15 MB) in app bundle. Download link: https://github.com/UB-Mannheim/tesseract/wiki. MUST be included; OCR will not work without this file. |
| **Performance** | OCR is CPU-intensive. Run in background process, indexed in priority order (most-modified first). Consider threading or async task queue. |
| **Risk Assessment** | **Low-to-Medium**. Tesseract is open-source and maintained, but OCR quality varies by content. Leptonica is stable. Main risk: training data license (same as Tesseract, Apache 2.0, safe). **Bus factor**: Tesseract has active maintainers but smaller community than Qt. If abandoned, source is stable; new forks exist. |
| **Alternatives** | EasyOCR (PyTorch, Python-first, integration overhead), Apple Vision framework (macOS 10.15+, proprietary but free, recommended as future alternative). |
| **Action Items** | Test OCR on sample PDFs and screenshots; measure performance impact on indexing; consider Vision.framework as fallback for M1 (lower effort, better privacy). |

**Licensing Implication**: Apache 2.0 + BSD 2-Clause are both permissive. Safe for proprietary and GPL builds.

---

### 4. PDF Extraction (Poppler vs MuPDF vs PDFium)

| Aspect | Details |
|--------|---------|
| **Status** | **ACCEPTED (ADR-006)**: Poppler for development, PDFium (Apache 2.0) for release builds. This section retains all options for reference. |

#### Option A: Poppler

| Detail | Value |
|--------|-------|
| **License** | GPL 2.0 / GPL 3.0 (depending on optional components) |
| **Version** | 24.x or later |
| **Risk** | **GPL Strong Copyleft at Link Time** — Static linking forces entire BetterSpotlight to be GPL. Dynamic linking is legally ambiguous in macOS context (FSF has not clarified for .dylib). **NOT RECOMMENDED for proprietary builds.** |
| **Purpose** | Extract text from PDF documents for full-text indexing. |
| **Integration Effort** | Moderate. Poppler API is C++ friendly. |
| **macOS Support** | Good; Homebrew provides bottles. |

#### Option B: MuPDF

| Detail | Value |
|--------|-------|
| **License** | AGPL 3.0 (free version) OR Commercial ($). AGPL is stronger than GPL — any application linking MuPDF must publish its source code. **Incompatible with proprietary distribution.** |
| **Version** | 1.24+ |
| **Risk** | **AGPL Copyleft** — Stricter than GPL. Suitable only for GPL/open-source builds. |
| **Purpose** | Extract text from PDF documents; also renders PDFs (not needed for text extraction alone). |
| **Integration Effort** | Moderate. MuPDF API is C-based but usable from C++. |
| **macOS Support** | Yes, but less common than Poppler. |

#### Option C: PDFium (Chrome's PDF Engine)

| Detail | Value |
|--------|-------|
| **License** | Apache 2.0 (permissive, safe for proprietary and GPL) |
| **Version** | Latest from chromium/pdfium (rolling release) |
| **Risk** | **Low licensing risk. Very high build complexity.** PDFium has a complex dependency tree and requires Chromium's build system (GN). Integration is non-trivial for Qt projects. |
| **Purpose** | Extract text from PDF documents. |
| **Integration Effort** | High. Requires either pre-built binaries (maintenance burden) or custom build system integration. |
| **macOS Support** | Yes, but documentation is sparse. |
| **Recommendation** | Only if Poppler/MuPDF prove inadequate and you have build infrastructure expertise. |

#### Option D: Apple PDFKit (Proprietary macOS Solution)

| Detail | Value |
|--------|-------|
| **License** | macOS system framework (included, no licensing concerns) |
| **Version** | Available on macOS 10.4+ (all BetterSpotlight targets) |
| **Risk** | **Zero licensing risk. Best performance on macOS.** PDFKit is native, optimized, and requires zero shipping effort. |
| **Purpose** | Extract text from PDF documents using native macOS APIs. |
| **Integration Effort** | Low-to-Moderate. Requires Objective-C++ bridge to C++/Qt code, but straightforward. |
| **Recommendation** | **Preferred for proprietary builds.** Reduces dependencies, improves performance, simplifies distribution. Example: Use ObjC++ wrapper around PDFKit, export C++ interface. |

**Accepted Decision (ADR-006)**:
1. **Development**: Poppler (GPL) for fast iteration. Easy setup via `brew install poppler`.
2. **Release builds**: PDFium (Apache 2.0) for proprietary distribution. Complex build, but license-clean.
3. **PdfExtractor interface** abstracts the backend, making the swap low-cost before release.

| Licensing Compatibility | Proprietary | GPL | Hybrid |
|--------------------------|-----------|-----|--------|
| **Poppler (GPL)** | ❌ No (unless dynamic link, ambiguous) | ✅ Yes | ⚠️ Ambiguous |
| **MuPDF (AGPL)** | ❌ No | ❌ No (stricter) | ❌ No |
| **PDFium (Apache)** | ✅ Yes | ✅ Yes | ✅ Yes (complex build) |
| **PDFKit (macOS)** | ✅ Yes | ✅ Yes | ✅ Yes (proprietary framework) |

---

### 5. ONNX Runtime (Apple Silicon Semantic Search)

| Aspect | Details |
|--------|---------|
| **Version Range** | ONNX Runtime 1.17+ |
| **License** | MIT (permissive, safe for proprietary and GPL) |
| **Purpose** | Local inference engine for embedding models. Enables semantic search via embedding-based similarity (e.g., "find files about project X" without exact keyword matching). Uses Apple Silicon's Neural Engine for efficient computation. |
| **Target Platforms** | M1 / M2 / M3+ only. Intel builds will use CPU execution provider (slower but functional). |
| **Dependency Chain** | None critical; ONNX Runtime is self-contained. Optional: ship a small embedding model (~50 MB MiniLM or similar). |
| **Linking Strategy** | **Dynamic recommended.** ONNX Runtime is large (~100+ MB unstripped). Dynamic linking reduces app bundle size. Linking strategy is transparent to licensing (MIT is permissive). |
| **macOS Specifics** | ONNX Runtime provides pre-built wheels for Python; C++ binary available. Ship as framework or via Homebrew. Requires CoreML execution provider for Neural Engine access (built into ONNX Runtime). |
| **Performance** | Embedding inference is fast on Apple Silicon (Neural Engine: ~50-100 ms per file batch). CPU-only mode on Intel is viable but slower. |
| **Risk Assessment** | **Low-to-Medium**. ONNX Runtime is backed by Microsoft, actively maintained. Embedding models are separate from runtime licensing. Main risk: model selection (ensure model license permits use; recommend open-source models like MiniLM under MIT/Apache). |
| **Alternatives** | CoreML models (macOS-native, but limited model variety and no direct equivalent), TensorFlow Lite (larger, less mature on macOS), PyTorch (Python-first, integration complexity). |
| **Action Items** | Embedding model: BGE-small-en-v1.5 or E5-small-v2 class (384-dim, MIT/Apache licensed); int8 quantized, first-chunk-only strategy; benchmark inference on M1/M2; design UI for semantic search; document as M2+ feature with Intel fallback. |

**Licensing Implication**: MIT is fully permissive. Safe for proprietary and GPL builds. Model licensing must be checked separately.

---

### 6. CoreServices Framework (macOS System)

| Aspect | Details |
|--------|---------|
| **Version** | macOS 10.13+ (available on all targets) |
| **License** | macOS system framework. No distribution concerns or licensing restrictions. |
| **Purpose** | FSEvents C API for file system change detection. Notifies the app when files are created, modified, or deleted so the index can be updated in real-time (low latency, <1s typically). |
| **Why This Approach** | FSEvents is the only production-grade FS monitor on macOS. Alternatives (polling with stat, manual scanning) are inefficient and introduce 5-30s delays. |
| **Linking Strategy** | Framework link. No licensing implications. |
| **macOS Specifics** | CoreServices is part of the macOS SDK. No separate shipping required. Available via Xcode's frameworks. |
| **Integration** | C API with Qt wrapper. Low-level but straightforward. Example: `FSEventStreamCreate()` → monitor directory, schedule on runloop, handle events in callback. |
| **Risk Assessment** | **Negligible**. System framework, guaranteed available, Apple maintains. Zero bus factor risk. |
| **Alternatives** | Directory Monitoring via Qt (slower), kqueue (BSD API, works but less efficient for recursive directory trees), polling (unacceptable latency). |
| **Action Items** | Implement FSEvents wrapper in C++; test with large directory trees (>100k files); ensure runloop integration with Qt event loop. |

**Licensing Implication**: System framework. No licensing considerations. Zero distribution burden.

---

### 7. CMake 3.21+

| Aspect | Details |
|--------|---------|
| **Version Range** | CMake 3.21 or later (Qt 6 requires 3.21+ for modern find_package semantics) |
| **License** | BSD 3-Clause |
| **Purpose** | Build system. Configures compilation, links libraries, manages dependencies. Not shipped with app. |
| **Linking Strategy** | Build-time only. Not distributed. |
| **Developer Requirement** | All developers must have CMake 3.21+ installed locally. Recommend: brew install cmake or download from cmake.org. |
| **macOS Specifics** | Homebrew provides pre-built CMake for Intel and M1. No issues. |
| **Risk Assessment** | **Negligible**. CMake is de facto standard for C++ projects, actively maintained by Kitware, large community. |
| **Alternatives** | Bazel (overkill for this project), Meson (less mature for Qt), raw Makefiles (unmanageable). |
| **Action Items** | Document CMake minimum version in README; add pre-build version check in CI; ensure developers run `brew upgrade cmake` if needed. |

**Licensing Implication**: Build-time tool, not distributed. No licensing constraints.

---

### 8. hnswlib (Vector Similarity Index)

| Aspect | Details |
|--------|---------|
| **Version Range** | Latest (header-only, no version constraints) |
| **License** | Apache 2.0 (permissive, safe for proprietary and GPL) |
| **Purpose** | HNSW approximate nearest neighbor index for vector similarity search. Enables semantic search via embedding-based similarity (e.g., "find files about project X" without exact keyword matching). |
| **Linking Strategy** | **Header-only, compiled into the project** (no separate library). Adds negligible binary size impact. |
| **macOS Specifics** | No platform-specific code; C++ header library works on all platforms. |
| **Dependency Chain** | Zero external dependencies; self-contained header-only library. |
| **Performance** | Fast KNN search: O(log N) average case for 500K vectors. Memory footprint: ~100-150 MB for 384-dim int8 quantized vectors at 500K files. |
| **Risk Assessment** | **Low**. Actively maintained, widely used (basis for Faiss, Weaviate, and other vector databases), no external dependencies, header-only means no ABI compatibility concerns. |
| **Alternatives** | Faiss (Facebook, heavier, requires build system), Annoy (Spotify, older), HNSW from other sources (hnswlib is the standard reference implementation). |
| **Action Items** | Include hnswlib header-only library in codebase or via FetchContent; ensure Apache 2.0 license is preserved in distribution; test vector index performance at 500K+ file counts. |

**Licensing Implication**: Apache 2.0 is fully permissive. Safe for proprietary and GPL builds. Header-only inclusion requires no special distribution handling.

---

### 9. Test Framework (Catch2 or Google Test)

| Aspect | Details |
|--------|---------|
| **Version Range** | Catch2 v3.5+ OR Google Test v1.14+ |
| **License** | Catch2: BSL-1.0 (Boost Software License) | Google Test: BSD 3-Clause |
| **Purpose** | Unit and integration testing. Tests are NOT shipped with the app. |
| **Linking Strategy** | Test-only dependency, static linking acceptable. Not distributed. |
| **macOS Specifics** | Both available via Homebrew or Conan. No platform-specific issues. |
| **Recommendation** | **Catch2** for rapid iteration and readable test output. Light integration with CMake (header-only or single-header option available). Google Test if you want more advanced fixture patterns. |
| **Risk Assessment** | **Negligible**. Both are mature, widely used, well-maintained. |
| **Alternatives** | Doctest (lightweight), Boost.Test (heavyweight), custom test harness (not recommended). |
| **Action Items** | Choose Catch2 or Google Test early in dev; add CI step to run test suite on every commit; aim for >70% code coverage. |

**Licensing Implication**: Test-only dependency. Both licenses permit proprietary and GPL use. Not distributed, so no end-user licensing burden.

---

## Licensing Compatibility Matrix

### Proprietary Distribution (Closed Source)

| Dependency | License | Static Link? | Dynamic Link? | Notes |
|------------|---------|--------------|---------------|-------|
| Qt 6 | LGPL 3.0 | ❌ No | ✅ **Yes (Required)** | LGPL requires dynamic linking for proprietary. Must ship Qt frameworks. |
| SQLite | Public Domain | ✅ Yes | ✅ Yes | No restrictions. |
| Tesseract | Apache 2.0 | ✅ Yes | ✅ Yes | Permissive, no conflicts. |
| Leptonica | BSD 2-Clause | ✅ Yes | ✅ Yes | Permissive, no conflicts. |
| Poppler | GPL 2.0/3.0 | ❌ No | ⚠️ Ambiguous | Do not use for proprietary. |
| MuPDF | AGPL 3.0 | ❌ No | ❌ No | Too restrictive for proprietary. |
| PDFium | Apache 2.0 | ✅ Yes | ✅ Yes | Permissive, but build complexity high. |
| PDFKit | macOS System | ✅ Yes | ✅ Yes | System framework, zero licensing friction. **Recommended.** |
| ONNX Runtime | MIT | ✅ Yes | ✅ Yes | Permissive, no conflicts. |
| hnswlib | Apache 2.0 | ✅ Yes (Header-only) | N/A | Permissive, header-only, compiled in. |
| CoreServices | System | N/A | N/A | System framework, no concerns. |
| CMake | BSD 3-Clause | N/A | N/A | Build-time tool, not distributed. |
| Catch2 / GTest | BSL-1.0 / BSD 3-Clause | N/A | N/A | Test-only, not distributed. |

**Proprietary Build Configuration**:
```
Qt: Dynamic link (mandatory)
SQLite: Static link (simplifies distribution)
Tesseract + Leptonica: Static link (reduces runtime deps)
PDF: Use PDFKit (native, zero licensing friction) OR PDFium (if PDFKit insufficient)
ONNX Runtime: Dynamic link (large binary, reduces app size)
Others: System frameworks or test-only (no concerns)
```

### GPL Distribution (Open Source)

| Dependency | License | Static Link? | Dynamic Link? | Notes |
|------------|---------|--------------|---------------|-------|
| Qt 6 | LGPL 3.0 / GPL 2.0 | ✅ Yes (via GPL) | ✅ Yes | GPL-compatible. All linking strategies work. |
| SQLite | Public Domain | ✅ Yes | ✅ Yes | No restrictions. |
| Tesseract | Apache 2.0 | ✅ Yes | ✅ Yes | Apache 2.0 is GPL-compatible. |
| Leptonica | BSD 2-Clause | ✅ Yes | ✅ Yes | BSD is GPL-compatible. |
| Poppler | GPL 2.0/3.0 | ✅ Yes | ✅ Yes | GPL-compatible. **Recommended for GPL builds.** |
| MuPDF | AGPL 3.0 | ❌ No | ❌ No | AGPL is stricter than GPL; incompatible. Avoid. |
| PDFium | Apache 2.0 | ✅ Yes | ✅ Yes | Apache 2.0 is GPL-compatible. |
| PDFKit | macOS System | ✅ Yes | ✅ Yes | Permitted under GPL (system libraries exception). |
| ONNX Runtime | MIT | ✅ Yes | ✅ Yes | MIT is GPL-compatible. |
| hnswlib | Apache 2.0 | ✅ Yes (Header-only) | N/A | Apache 2.0 is GPL-compatible. |
| CoreServices | System | N/A | N/A | System framework, GPL-compatible (system exception). |
| CMake | BSD 3-Clause | N/A | N/A | Build-time tool. |
| Catch2 / GTest | BSL-1.0 / BSD 3-Clause | N/A | N/A | Test-only. |

**GPL Build Configuration**:
```
Qt: Static or dynamic (both compatible under GPL)
SQLite: Static (typical pattern)
Tesseract + Leptonica: Static
PDF: Poppler (native GPL support) OR PDFKit
ONNX Runtime: Static or dynamic (both compatible)
Others: Free to choose (all GPL-compatible)

Project License: GPL 2.0 or GPL 3.0 (must declare in LICENSE file and code headers)
```

### Hybrid Distribution (Not Recommended)

A "hybrid" approach attempts to distribute proprietary code alongside GPL-licensed components. This is **legally ambiguous and operationally confusing**:

- **Scenario**: Ship proprietary BetterSpotlight with dynamically-linked GPL Poppler
- **Problem 1**: FSF interprets dynamic linking of GPL code as "derivative work," requiring proprietary code to also be GPL
- **Problem 2**: macOS .dylib linking is not clearly addressed by GPL FAQ; courts have not ruled on this edge case
- **Problem 3**: Users may redistribute both components together, creating licensing confusion
- **Recommendation**: Do not attempt hybrid. Choose:
  1. **Proprietary** (use PDFKit or PDFium for PDF extraction)
  2. **GPL** (use Poppler, release source code)

---

## Distribution Checklist

### For Proprietary Binary Releases

- [ ] Qt 6 frameworks are dynamically linked and shipped in app bundle
- [ ] Verify Qt license notice is included in app (e.g., About dialog or Help > Licenses)
- [ ] Qt source code download link is provided to users: https://www.qt.io/download
- [ ] SQLite, Tesseract, Leptonica are statically linked (no separate .dylib files needed)
- [ ] PDFKit is used for PDF extraction (no Poppler binary)
- [ ] ONNX Runtime (if included) is dynamically linked; verify it's in app bundle
- [ ] No GPL-licensed binaries are shipped
- [ ] Dependency audit is reviewed by legal counsel (optional but recommended)
- [ ] README includes: "BetterSpotlight uses Qt 6, which is licensed under LGPL 3.0. Users may relink against modified Qt versions. See LICENSE file for third-party notices."

### For GPL Release

- [ ] Project is licensed under GPL 2.0 or GPL 3.0 (declare in LICENSE and code headers)
- [ ] Source code for all dependencies is available or referenced (e.g., github.com/tesseract-ocr/tesseract)
- [ ] Poppler is used for PDF extraction (if any)
- [ ] Build instructions are clear so users can reproduce the binary
- [ ] COPYING file includes full GPL text
- [ ] README includes: "BetterSpotlight is licensed under GPL 2.0 / 3.0 and includes dependencies licensed under Apache 2.0, MIT, BSD, and Public Domain licenses. See LICENSE for details."

---

## Maintenance & Updates

### Dependency Version Pinning

- Use CMake `FetchContent` or Conan to pin exact versions in CMakeLists.txt or conanfile.txt
- Example for SQLite:
  ```cmake
  FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2024/sqlite-autoconf-3450000.tar.gz
    URL_HASH SHA256=...
  )
  ```
- Review dependency updates quarterly; test before upgrading

### Sunset Policy

If a dependency is abandoned (e.g., no commits in 2+ years, critical security issue unfixed):

1. **Low-Risk Dependencies** (SQLite, ONNX Runtime): Monitor but don't rush to replace
2. **High-Risk Dependencies** (Tesseract, Leptonica): Plan alternative (e.g., Vision.framework)
3. **Critical Dependencies** (Qt): Switch to commercial license or evaluate alternatives (wxWidgets) before sunsetttting

### License Compliance Audit

- Run `scancode` or `fossology` on final build artifact before release
- Verify no unexpected GPL-licensed binaries are shipped
- Add automated check to CI: flag any new GPL dependencies

---

## References

- Qt Licensing: https://www.qt.io/licensing
- LGPL FAQ (FSF): https://www.gnu.org/licenses/lgpl-faq.html
- GPL Linking Exception: https://www.gnu.org/software/classpath/license.html
- SPDX License List: https://spdx.org/licenses/
- macOS System Frameworks (GPL Exception): https://opensource.apple.com/source/
