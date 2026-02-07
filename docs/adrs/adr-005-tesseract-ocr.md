# ADR-005: Tesseract for OCR

**Date:** 2026-02-06
**Status:** Accepted

## Context

The application optionally extracts text from images and screenshots via Optical Character Recognition (OCR) to make image content searchable. The original Swift scaffold used Apple's Vision framework, which is native, high-quality, and deep-integrated with macOS.

With the Qt/C++ rewrite, OCR must be accomplished via a cross-platform library to maintain architectural consistency and enable future non-macOS ports.

## Decision

Use Tesseract OCR via the libtesseract C API for all OCR text extraction operations.

## Alternatives Considered

**Apple Vision framework**
- Rejected. Excellent quality on macOS, particularly for Retina displays and system-generated content. Requires Objective-C++ bridging from C++, adding complexity. Strictly macOS-only; no cross-platform path.

**EasyOCR (Python)**
- Rejected. Good accuracy and ease of use, but requires shipping a Python runtime environment; inappropriate for a native C++ desktop application. Adds complexity and runtime dependency management.

**PaddleOCR**
- Rejected. Strong multi-language support, but heavy dependency footprint; complex build integration; overkill for desktop application OCR needs.

**Windows OCR API**
- Rejected. Platform-specific; not relevant to macOS-first project. Blocks future cross-platform development.

## Consequences

### Positive

- Mature, widely-used OCR library; decades of development and refinement.
- Apache 2.0 licensed; no licensing restrictions or compliance burden.
- C API callable directly from C++ without complex bridging.
- Supports 100+ languages; enables localization for international users.
- Cross-platform availability (macOS, Linux, Windows); clear path to future platform support.
- Large community; abundant documentation, tutorials, and example code.

### Negative

- Requires shipping trained language model files (~25-30 MB for English LSTM best model, larger for additional languages); increases application distribution size.
- Accuracy lower than Vision framework on macOS-specific content (Retina screenshots, system fonts, handwriting).
- Requires image preprocessing (binarization, deskew, contrast enhancement) for reliable results; raw image OCR quality is poor.
- Build system complexity increases; Tesseract depends on Leptonica library (image processing); CMake must manage both dependencies.
- OCR is computationally expensive; must be offloaded to Extractor service to avoid UI blocking.
- Slower inference than Vision framework; typical 50-500ms per image depending on size and language model.

## Implementation Notes

- Locate trained language data files in application resources; load dynamically at runtime based on user language preferences.
- Implement image preprocessing pipeline in Extractor service: convert to grayscale, detect text regions (via Leptonica), apply binarization.
- Cache OCR results keyed by image content hash to avoid re-processing identical images.
- Run OCR as lower-priority task in Extractor service to prevent UI responsiveness issues.
- Implement progress reporting for long-running OCR jobs (e.g., scanning document set); allow user cancellation.
- Consider optional integration with Vision framework on macOS for improved quality, with Tesseract as fallback.
