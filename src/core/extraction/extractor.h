#pragma once

#include <QString>
#include <optional>

namespace bs {

// Result of a content extraction attempt.
// Every extraction produces a status; content is present only on Success.
struct ExtractionResult {
    enum class Status {
        Success,
        Timeout,
        CorruptedFile,
        UnsupportedFormat,
        SizeExceeded,
        Inaccessible,
        Unknown,
        Cancelled,
    };

    Status status = Status::Unknown;
    std::optional<QString> content;
    std::optional<QString> errorMessage;
    int durationMs = 0;
};

// FileExtractor — abstract interface for content extraction backends.
//
// Each implementation handles a family of file types (plain text, PDF, OCR).
// The ExtractionManager selects the appropriate extractor based on ItemKind.
class FileExtractor {
public:
    virtual ~FileExtractor() = default;

    // Extract textual content from the file at filePath.
    // Returns an ExtractionResult with status and (on success) content.
    virtual ExtractionResult extract(const QString& filePath) = 0;

    // Returns true if this extractor can handle files with the given extension.
    // The extension should be lowercase without a leading dot (e.g. "py", "txt").
    virtual bool supports(const QString& extension) const = 0;
};

} // namespace bs
