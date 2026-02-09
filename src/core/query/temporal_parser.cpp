#include "core/query/temporal_parser.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>

namespace bs {

namespace {

struct MonthEntry {
    const char* name;
    int month;
};

constexpr MonthEntry kMonths[] = {
    {"january", 1},   {"february", 2},  {"march", 3},
    {"april", 4},     {"may", 5},       {"june", 6},
    {"july", 7},      {"august", 8},    {"september", 9},
    {"october", 10},  {"november", 11}, {"december", 12},
};

struct SeasonEntry {
    const char* name;
    int startMonth;
    int endMonth;
};

constexpr SeasonEntry kSeasons[] = {
    {"summer", 6, 8},
    {"winter", 12, 2},
    {"spring", 3, 5},
    {"fall", 9, 11},
    {"autumn", 9, 11},
};

QDateTime makeUtcDateTime(QDate date, QTime time)
{
    return QDateTime(date, time, QTimeZone::UTC);
}

std::optional<int> extractAdjacentYear(const QString& lower, int keywordStart, int keywordEnd)
{
    static const QRegularExpression yearPattern(QStringLiteral(R"(\b(19|20)\d{2}\b)"));
    QRegularExpressionMatchIterator it = yearPattern.globalMatch(lower);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        int matchStart = m.capturedStart();
        int matchEnd = m.capturedEnd();
        if (matchEnd + 1 == keywordStart || matchStart == keywordEnd + 1
            || matchEnd == keywordStart || matchStart - 1 == keywordEnd) {
            return m.captured(0).toInt();
        }
    }
    return std::nullopt;
}

TemporalRange monthRange(int month, int year)
{
    QDateTime start = makeUtcDateTime(QDate(year, month, 1), QTime(0, 0, 0));
    QDate endDate = QDate(year, month, 1).addMonths(1).addDays(-1);
    QDateTime end = makeUtcDateTime(endDate, QTime(23, 59, 59));
    return {static_cast<double>(start.toSecsSinceEpoch()),
            static_cast<double>(end.toSecsSinceEpoch())};
}

TemporalRange seasonRange(const SeasonEntry& season, int year)
{
    if (season.startMonth > season.endMonth) {
        QDateTime start = makeUtcDateTime(
            QDate(year, season.startMonth, 1), QTime(0, 0, 0));
        int febEnd = QDate(year + 1, 2, 1).daysInMonth();
        QDateTime end = makeUtcDateTime(
            QDate(year + 1, 2, febEnd), QTime(23, 59, 59));
        return {static_cast<double>(start.toSecsSinceEpoch()),
                static_cast<double>(end.toSecsSinceEpoch())};
    }

    QDateTime start = makeUtcDateTime(
        QDate(year, season.startMonth, 1), QTime(0, 0, 0));
    QDate endDate(year, season.endMonth, 1);
    endDate = QDate(year, season.endMonth, endDate.daysInMonth());
    QDateTime end = makeUtcDateTime(endDate, QTime(23, 59, 59));
    return {static_cast<double>(start.toSecsSinceEpoch()),
            static_cast<double>(end.toSecsSinceEpoch())};
}

TemporalRange yearRange(int year)
{
    QDateTime start = makeUtcDateTime(QDate(year, 1, 1), QTime(0, 0, 0));
    QDateTime end = makeUtcDateTime(QDate(year, 12, 31), QTime(23, 59, 59));
    return {static_cast<double>(start.toSecsSinceEpoch()),
            static_cast<double>(end.toSecsSinceEpoch())};
}

} // namespace

std::optional<TemporalRange> TemporalParser::parse(const QString& query)
{
    const QString lower = query.toLower().trimmed();
    if (lower.isEmpty()) {
        return std::nullopt;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const int currentYear = now.date().year();

    // --- Priority 1: Relative time expressions ---

    if (lower.contains(QStringLiteral("yesterday"))) {
        QDateTime start = now.addDays(-1);
        return TemporalRange{static_cast<double>(start.toSecsSinceEpoch()),
                             static_cast<double>(now.toSecsSinceEpoch())};
    }

    if (lower.contains(QStringLiteral("last week"))) {
        QDateTime start = now.addDays(-7);
        return TemporalRange{static_cast<double>(start.toSecsSinceEpoch()),
                             static_cast<double>(now.toSecsSinceEpoch())};
    }

    if (lower.contains(QStringLiteral("last month"))) {
        QDateTime start = now.addDays(-30);
        return TemporalRange{static_cast<double>(start.toSecsSinceEpoch()),
                             static_cast<double>(now.toSecsSinceEpoch())};
    }

    if (lower.contains(QStringLiteral("recent"))
        || lower.contains(QStringLiteral("recently"))) {
        QDateTime start = now.addDays(-14);
        return TemporalRange{static_cast<double>(start.toSecsSinceEpoch()),
                             static_cast<double>(now.toSecsSinceEpoch())};
    }

    // "N months/weeks/days ago"
    {
        static const QRegularExpression agoPattern(
            QStringLiteral(R"((\d+)\s+(months?|weeks?|days?)\s+ago)"));
        QRegularExpressionMatch agoMatch = agoPattern.match(lower);
        if (agoMatch.hasMatch()) {
            int n = agoMatch.captured(1).toInt();
            const QString unit = agoMatch.captured(2);
            int days = n;
            if (unit.startsWith(QStringLiteral("month"))) {
                days = n * 30;
            } else if (unit.startsWith(QStringLiteral("week"))) {
                days = n * 7;
            }
            QDateTime start = now.addDays(-days);
            return TemporalRange{static_cast<double>(start.toSecsSinceEpoch()),
                                 static_cast<double>(now.toSecsSinceEpoch())};
        }
    }

    // --- Priority 2: Month (optionally with year) ---

    for (const auto& entry : kMonths) {
        const QString monthName = QString::fromLatin1(entry.name);
        int idx = lower.indexOf(monthName);
        if (idx < 0) {
            continue;
        }

        std::optional<int> adjacentYear =
            extractAdjacentYear(lower, idx, idx + monthName.size());
        int year = adjacentYear.value_or(currentYear);
        return monthRange(entry.month, year);
    }

    // --- Priority 3: Season (optionally with year) ---

    for (const auto& season : kSeasons) {
        const QString seasonName = QString::fromLatin1(season.name);
        int idx = lower.indexOf(seasonName);
        if (idx < 0) {
            continue;
        }

        std::optional<int> adjacentYear =
            extractAdjacentYear(lower, idx, idx + seasonName.size());
        int year = adjacentYear.value_or(currentYear);
        return seasonRange(season, year);
    }

    // --- Priority 4: Year only ---

    {
        static const QRegularExpression yearOnlyPattern(
            QStringLiteral(R"(\b(19|20)\d{2}\b)"));
        QRegularExpressionMatch yearMatch = yearOnlyPattern.match(lower);
        if (yearMatch.hasMatch()) {
            int year = yearMatch.captured(0).toInt();
            return yearRange(year);
        }
    }

    return std::nullopt;
}

} // namespace bs
