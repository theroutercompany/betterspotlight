#include <QtTest/QtTest>

#include "core/shared/search_result.h"

class TestSearchResult : public QObject {
    Q_OBJECT

private slots:
    void testMatchTypeMappings();
    void testUnknownMatchTypeFallbacks();
};

void TestSearchResult::testMatchTypeMappings()
{
    struct Row {
        bs::MatchType type;
        int points;
        const char* token;
    };

    const Row rows[] = {
        {bs::MatchType::ExactName, 200, "exactNameMatch"},
        {bs::MatchType::PrefixName, 150, "prefixNameMatch"},
        {bs::MatchType::ContainsName, 100, "containsNameMatch"},
        {bs::MatchType::ExactPath, 90, "exactPathMatch"},
        {bs::MatchType::PrefixPath, 80, "prefixPathMatch"},
        {bs::MatchType::Content, 50, "contentMatch"},
        {bs::MatchType::Fuzzy, 30, "fuzzyMatch"},
    };

    for (const Row& row : rows) {
        QCOMPARE(bs::matchTypeBasePoints(row.type), row.points);
        QCOMPARE(bs::matchTypeToString(row.type), QString::fromUtf8(row.token));
    }
}

void TestSearchResult::testUnknownMatchTypeFallbacks()
{
    const auto unknown = static_cast<bs::MatchType>(999);
    QCOMPARE(bs::matchTypeBasePoints(unknown), 0);
    QCOMPARE(bs::matchTypeToString(unknown), QStringLiteral("unknown"));
}

QTEST_MAIN(TestSearchResult)
#include "test_search_result.moc"
