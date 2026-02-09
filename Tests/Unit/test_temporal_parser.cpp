#include <QtTest/QtTest>

#include "core/query/temporal_parser.h"

#include <QDateTime>
#include <QTimeZone>

#include <cmath>

class TestTemporalParser : public QObject {
    Q_OBJECT

private slots:
    void testMonthExtraction();
    void testMonthYearExtraction();
    void testSeasonExtraction();
    void testSeasonYearExtraction();
    void testRelativeLastWeek();
    void testRelativeMonthsAgo();
    void testYearOnly();
    void testNoSignal();
    void testYesterday();
};

void TestTemporalParser::testMonthExtraction()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("january report"));
    QVERIFY(result.has_value());

    int currentYear = QDateTime::currentDateTimeUtc().date().year();
    QDateTime expectedStart(QDate(currentYear, 1, 1), QTime(0, 0, 0), QTimeZone::UTC);
    QDateTime expectedEnd(QDate(currentYear, 1, 31), QTime(23, 59, 59), QTimeZone::UTC);

    QCOMPARE(result->startEpoch, static_cast<double>(expectedStart.toSecsSinceEpoch()));
    QCOMPARE(result->endEpoch, static_cast<double>(expectedEnd.toSecsSinceEpoch()));
}

void TestTemporalParser::testMonthYearExtraction()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("june 2023 photos"));
    QVERIFY(result.has_value());

    QDateTime expectedStart(QDate(2023, 6, 1), QTime(0, 0, 0), QTimeZone::UTC);
    QDateTime expectedEnd(QDate(2023, 6, 30), QTime(23, 59, 59), QTimeZone::UTC);

    QCOMPARE(result->startEpoch, static_cast<double>(expectedStart.toSecsSinceEpoch()));
    QCOMPARE(result->endEpoch, static_cast<double>(expectedEnd.toSecsSinceEpoch()));
}

void TestTemporalParser::testSeasonExtraction()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("that summer"));
    QVERIFY(result.has_value());

    int currentYear = QDateTime::currentDateTimeUtc().date().year();
    QDateTime expectedStart(QDate(currentYear, 6, 1), QTime(0, 0, 0), QTimeZone::UTC);
    QDateTime expectedEnd(QDate(currentYear, 8, 31), QTime(23, 59, 59), QTimeZone::UTC);

    QCOMPARE(result->startEpoch, static_cast<double>(expectedStart.toSecsSinceEpoch()));
    QCOMPARE(result->endEpoch, static_cast<double>(expectedEnd.toSecsSinceEpoch()));
}

void TestTemporalParser::testSeasonYearExtraction()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("summer 2022"));
    QVERIFY(result.has_value());

    QDateTime expectedStart(QDate(2022, 6, 1), QTime(0, 0, 0), QTimeZone::UTC);
    QDateTime expectedEnd(QDate(2022, 8, 31), QTime(23, 59, 59), QTimeZone::UTC);

    QCOMPARE(result->startEpoch, static_cast<double>(expectedStart.toSecsSinceEpoch()));
    QCOMPARE(result->endEpoch, static_cast<double>(expectedEnd.toSecsSinceEpoch()));
}

void TestTemporalParser::testRelativeLastWeek()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("last week"));
    QVERIFY(result.has_value());

    double now = static_cast<double>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    double sevenDaysAgo = now - (7 * 24 * 3600);

    QVERIFY(std::abs(result->startEpoch - sevenDaysAgo) < 5.0);
    QVERIFY(std::abs(result->endEpoch - now) < 5.0);
}

void TestTemporalParser::testRelativeMonthsAgo()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("3 months ago"));
    QVERIFY(result.has_value());

    double now = static_cast<double>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    double ninetyDaysAgo = now - (90 * 24 * 3600);

    QVERIFY(std::abs(result->startEpoch - ninetyDaysAgo) < 5.0);
    QVERIFY(std::abs(result->endEpoch - now) < 5.0);
}

void TestTemporalParser::testYearOnly()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("2019 taxes"));
    QVERIFY(result.has_value());

    QDateTime expectedStart(QDate(2019, 1, 1), QTime(0, 0, 0), QTimeZone::UTC);
    QDateTime expectedEnd(QDate(2019, 12, 31), QTime(23, 59, 59), QTimeZone::UTC);

    QCOMPARE(result->startEpoch, static_cast<double>(expectedStart.toSecsSinceEpoch()));
    QCOMPARE(result->endEpoch, static_cast<double>(expectedEnd.toSecsSinceEpoch()));
}

void TestTemporalParser::testNoSignal()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("my resume"));
    QVERIFY(!result.has_value());
}

void TestTemporalParser::testYesterday()
{
    auto result = bs::TemporalParser::parse(QStringLiteral("yesterday"));
    QVERIFY(result.has_value());

    double now = static_cast<double>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    double oneDayAgo = now - (24 * 3600);

    QVERIFY(std::abs(result->startEpoch - oneDayAgo) < 5.0);
    QVERIFY(std::abs(result->endEpoch - now) < 5.0);
}

QTEST_MAIN(TestTemporalParser)
#include "test_temporal_parser.moc"
