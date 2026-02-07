#include <QtTest/QtTest>
#include "core/ranking/match_classifier.h"
#include "core/shared/search_result.h"

class TestMatchClassifier : public QObject {
    Q_OBJECT

private slots:
    // ── classify() match types ───────────────────────────────────
    void testExactNameMatch();
    void testExactNameMatchCaseInsensitive();
    void testPrefixNameMatch();
    void testContainsNameMatch();
    void testExactPathMatch();
    void testPrefixPathMatch();
    void testFuzzyMatch();
    void testContentFallback();

    // ── File extension handling ──────────────────────────────────
    void testExtensionStrippedForExactName();
    void testDotfileNotStripped();

    // ── Case insensitive ─────────────────────────────────────────
    void testCaseInsensitiveContains();
    void testCaseInsensitivePrefix();

    // ── editDistance ─────────────────────────────────────────────
    void testEditDistanceIdentical();
    void testEditDistanceOneSubstitution();
    void testEditDistanceOneDeletion();
    void testEditDistanceOneInsertion();
    void testEditDistanceKittenSitting();
    void testEditDistanceEmpty();
    void testEditDistanceCaseInsensitive();

    // ── isFuzzyMatch ─────────────────────────────────────────────
    void testIsFuzzyMatchWithinThreshold();
    void testIsFuzzyMatchExceedsThreshold();
    void testIsFuzzyMatchEmptyQuery();
    void testIsFuzzyMatchEmptyFileName();
    void testIsFuzzyMatchCustomThreshold();
    void testIsFuzzyMatchStripsExtension();

    // ── Edge cases ───────────────────────────────────────────────
    void testEmptyQuery();
    void testSingleCharQuery();
    void testQueryMatchesPathNotName();
};

// ── classify() match types ───────────────────────────────────────

void TestMatchClassifier::testExactNameMatch()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("readme"),
        QStringLiteral("README.md"),
        QStringLiteral("/path/README.md"));
    QCOMPARE(result, bs::MatchType::ExactName);
}

void TestMatchClassifier::testExactNameMatchCaseInsensitive()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("README"),
        QStringLiteral("readme.txt"),
        QStringLiteral("/path/readme.txt"));
    QCOMPARE(result, bs::MatchType::ExactName);
}

void TestMatchClassifier::testPrefixNameMatch()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("read"),
        QStringLiteral("README.md"),
        QStringLiteral("/path/README.md"));
    QCOMPARE(result, bs::MatchType::PrefixName);
}

void TestMatchClassifier::testContainsNameMatch()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("adm"),
        QStringLiteral("README.md"),
        QStringLiteral("/path/README.md"));
    QCOMPARE(result, bs::MatchType::ContainsName);
}

void TestMatchClassifier::testExactPathMatch()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("/Users/me/project/config.yaml"),
        QStringLiteral("config.yaml"),
        QStringLiteral("/Users/me/project/config.yaml"));
    QCOMPARE(result, bs::MatchType::ExactPath);
}

void TestMatchClassifier::testPrefixPathMatch()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("/Users/me/project"),
        QStringLiteral("main.cpp"),
        QStringLiteral("/Users/me/project/src/main.cpp"));
    QCOMPARE(result, bs::MatchType::PrefixPath);
}

void TestMatchClassifier::testFuzzyMatch()
{
    // "raedme" is edit distance 2 from "readme" (transposed letters).
    // It does NOT appear as a prefix or substring of "readme.md", so
    // it should fall through to fuzzy matching.
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("raedme"),
        QStringLiteral("readme.md"),
        QStringLiteral("/path/readme.md"));
    QCOMPARE(result, bs::MatchType::Fuzzy);
}

void TestMatchClassifier::testContentFallback()
{
    // Query that doesn't match name or path at all and is too far for fuzzy
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("completely_different_query_string"),
        QStringLiteral("readme.md"),
        QStringLiteral("/path/readme.md"));
    QCOMPARE(result, bs::MatchType::Content);
}

// ── File extension handling ──────────────────────────────────────

void TestMatchClassifier::testExtensionStrippedForExactName()
{
    // "main" should exact-match "main.cpp" (extension stripped)
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("main"),
        QStringLiteral("main.cpp"),
        QStringLiteral("/path/main.cpp"));
    QCOMPARE(result, bs::MatchType::ExactName);
}

void TestMatchClassifier::testDotfileNotStripped()
{
    // ".bashrc" has no extension to strip (dot at index 0)
    auto result = bs::MatchClassifier::classify(
        QStringLiteral(".bashrc"),
        QStringLiteral(".bashrc"),
        QStringLiteral("/home/user/.bashrc"));
    QCOMPARE(result, bs::MatchType::ExactName);
}

// ── Case insensitive ─────────────────────────────────────────────

void TestMatchClassifier::testCaseInsensitiveContains()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("EAD"),
        QStringLiteral("readme.md"),
        QStringLiteral("/path/readme.md"));
    QCOMPARE(result, bs::MatchType::ContainsName);
}

void TestMatchClassifier::testCaseInsensitivePrefix()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("READ"),
        QStringLiteral("readme.md"),
        QStringLiteral("/path/readme.md"));
    QCOMPARE(result, bs::MatchType::PrefixName);
}

// ── editDistance ─────────────────────────────────────────────────

void TestMatchClassifier::testEditDistanceIdentical()
{
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("hello"), QStringLiteral("hello")), 0);
}

void TestMatchClassifier::testEditDistanceOneSubstitution()
{
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("hello"), QStringLiteral("hallo")), 1);
}

void TestMatchClassifier::testEditDistanceOneDeletion()
{
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("hello"), QStringLiteral("hllo")), 1);
}

void TestMatchClassifier::testEditDistanceOneInsertion()
{
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("hello"), QStringLiteral("helloo")), 1);
}

void TestMatchClassifier::testEditDistanceKittenSitting()
{
    // Classic example: kitten -> sitting = 3
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("kitten"), QStringLiteral("sitting")), 3);
}

void TestMatchClassifier::testEditDistanceEmpty()
{
    QCOMPARE(bs::MatchClassifier::editDistance(
        QString(), QStringLiteral("hello")), 5);
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("hello"), QString()), 5);
    QCOMPARE(bs::MatchClassifier::editDistance(
        QString(), QString()), 0);
}

void TestMatchClassifier::testEditDistanceCaseInsensitive()
{
    // editDistance is case-insensitive
    QCOMPARE(bs::MatchClassifier::editDistance(
        QStringLiteral("Hello"), QStringLiteral("HELLO")), 0);
}

// ── isFuzzyMatch ─────────────────────────────────────────────────

void TestMatchClassifier::testIsFuzzyMatchWithinThreshold()
{
    // "readne" vs "readme" = edit distance 1 (within default maxDistance=2)
    QVERIFY(bs::MatchClassifier::isFuzzyMatch(
        QStringLiteral("readne"), QStringLiteral("readme.md")));
}

void TestMatchClassifier::testIsFuzzyMatchExceedsThreshold()
{
    // "xyz" vs "readme" = edit distance 5 (exceeds default maxDistance=2)
    QVERIFY(!bs::MatchClassifier::isFuzzyMatch(
        QStringLiteral("xyz"), QStringLiteral("readme.md")));
}

void TestMatchClassifier::testIsFuzzyMatchEmptyQuery()
{
    QVERIFY(!bs::MatchClassifier::isFuzzyMatch(
        QString(), QStringLiteral("readme.md")));
}

void TestMatchClassifier::testIsFuzzyMatchEmptyFileName()
{
    QVERIFY(!bs::MatchClassifier::isFuzzyMatch(
        QStringLiteral("test"), QString()));
}

void TestMatchClassifier::testIsFuzzyMatchCustomThreshold()
{
    // "kitten" vs "readme" = edit distance > 3
    QVERIFY(!bs::MatchClassifier::isFuzzyMatch(
        QStringLiteral("kitten"), QStringLiteral("readme.md"), 3));

    // "readm" vs "readme" (stripped) = edit distance 1, within threshold 1
    QVERIFY(bs::MatchClassifier::isFuzzyMatch(
        QStringLiteral("readm"), QStringLiteral("readme.md"), 1));
}

void TestMatchClassifier::testIsFuzzyMatchStripsExtension()
{
    // "main" vs "main.cpp" -> stripped to "main" -> distance 0
    QVERIFY(bs::MatchClassifier::isFuzzyMatch(
        QStringLiteral("main"), QStringLiteral("main.cpp")));
}

// ── Edge cases ───────────────────────────────────────────────────

void TestMatchClassifier::testEmptyQuery()
{
    auto result = bs::MatchClassifier::classify(
        QString(), QStringLiteral("readme.md"), QStringLiteral("/path/readme.md"));
    QCOMPARE(result, bs::MatchType::Content);
}

void TestMatchClassifier::testSingleCharQuery()
{
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("r"),
        QStringLiteral("readme.md"),
        QStringLiteral("/path/readme.md"));
    QCOMPARE(result, bs::MatchType::PrefixName);
}

void TestMatchClassifier::testQueryMatchesPathNotName()
{
    // Query matches a path component but not the filename
    auto result = bs::MatchClassifier::classify(
        QStringLiteral("/specific/path"),
        QStringLiteral("file.txt"),
        QStringLiteral("/specific/path/file.txt"));
    QCOMPARE(result, bs::MatchType::PrefixPath);
}

QTEST_MAIN(TestMatchClassifier)
#include "test_match_classifier.moc"
