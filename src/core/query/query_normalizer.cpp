#include "core/query/query_normalizer.h"

namespace bs {

namespace {

bool isNoisePunctuation(QChar ch)
{
    switch (ch.unicode()) {
    case '!':
    case '?':
    case '$':
    case '@':
    case '#':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '{':
    case '}':
    case '[':
    case ']':
    case '~':
    case '`':
    case '"':
    case '\'':
        return true;
    default:
        return false;
    }
}

} // namespace

NormalizedQuery QueryNormalizer::normalize(const QString& raw)
{
    NormalizedQuery result;
    result.original = raw;

    QString working = raw.trimmed();
    if (working.size() >= 2) {
        const QChar first = working.front();
        const QChar last = working.back();
        const bool hasDoubleQuotes = first == QLatin1Char('"') && last == QLatin1Char('"');
        const bool hasSingleQuotes = first == QLatin1Char('\'') && last == QLatin1Char('\'');
        if (hasDoubleQuotes || hasSingleQuotes) {
            working = working.mid(1, working.size() - 2);
        }
    }

    QString normalized;
    normalized.reserve(working.size());

    for (QChar ch : working) {
        if (isNoisePunctuation(ch)) {
            continue;
        }

        if (ch.unicode() == 0x2013 || ch.unicode() == 0x2014) {
            ch = QLatin1Char('-');
        }

        if (ch.isSpace()) {
            if (normalized.isEmpty()) {
                continue;
            }

            const QChar previous = normalized.back();
            if (previous.isSpace() || previous == QLatin1Char('-')) {
                continue;
            }

            normalized.append(QLatin1Char(' '));
            continue;
        }

        if (ch == QLatin1Char('-')) {
            if (!normalized.isEmpty()) {
                const QChar previous = normalized.back();
                if (previous == QLatin1Char('-')) {
                    continue;
                }

                if (previous.isSpace()) {
                    normalized.chop(1);
                    if (!normalized.isEmpty() && normalized.back() == QLatin1Char('-')) {
                        continue;
                    }
                }
            }

            normalized.append(QLatin1Char('-'));
            continue;
        }

        normalized.append(ch.toLower());
    }

    result.normalized = normalized.trimmed();
    return result;
}

} // namespace bs
