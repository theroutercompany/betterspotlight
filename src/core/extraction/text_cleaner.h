#pragma once

#include <QString>

namespace bs {

// TextCleaner — normalizes raw extractor output for FTS5 indexing.
//
// Operations performed:
// 1. Strip ASCII control characters (0x00-0x08, 0x0B, 0x0C, 0x0E-0x1F) except tab and newline
// 2. Normalize line endings: \r\n and \r to \n
// 3. Collapse runs of 3+ newlines to 2 newlines (preserve paragraph breaks)
// 4. Collapse runs of 2+ spaces/tabs to single space
// 5. Trim leading/trailing whitespace
class TextCleaner {
public:
    static QString clean(const QString& raw);
};

} // namespace bs
