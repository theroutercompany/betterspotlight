#include "core/extraction/text_cleaner.h"

namespace bs {

QString TextCleaner::clean(const QString& raw)
{
    if (raw.isEmpty()) {
        return raw;
    }

    QString result;
    result.reserve(raw.size());

    // Pass 1: Strip control chars and normalize line endings
    for (int i = 0; i < raw.size(); ++i) {
        QChar ch = raw[i];
        ushort code = ch.unicode();

        // Normalize \r\n to \n
        if (code == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == QLatin1Char('\n')) {
                ++i; // skip \r, the \n will be added next iteration
            }
            result.append(QLatin1Char('\n'));
            continue;
        }

        // Strip control characters except tab (0x09) and newline (0x0A)
        if (code < 0x20 && code != 0x09 && code != 0x0A) {
            continue;
        }
        // Also strip DEL (0x7F)
        if (code == 0x7F) {
            continue;
        }

        result.append(ch);
    }

    // Pass 2: Collapse runs of 3+ newlines to 2 (preserve paragraph breaks)
    // and collapse runs of 2+ spaces/tabs to single space
    QString collapsed;
    collapsed.reserve(result.size());

    int i = 0;
    while (i < result.size()) {
        QChar ch = result[i];

        if (ch == QLatin1Char('\n')) {
            // Count consecutive newlines
            int count = 0;
            while (i < result.size() && result[i] == QLatin1Char('\n')) {
                ++count;
                ++i;
            }
            // Emit at most 2 newlines
            int emitCount = qMin(count, 2);
            for (int j = 0; j < emitCount; ++j) {
                collapsed.append(QLatin1Char('\n'));
            }
        } else if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t')) {
            // Collapse horizontal whitespace
            while (i < result.size() &&
                   (result[i] == QLatin1Char(' ') || result[i] == QLatin1Char('\t'))) {
                ++i;
            }
            collapsed.append(QLatin1Char(' '));
        } else {
            collapsed.append(ch);
            ++i;
        }
    }

    return collapsed.trimmed();
}

} // namespace bs
