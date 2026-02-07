#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include "core/fs/bsignore_parser.h"

class TestBsignoreParser : public QObject {
    Q_OBJECT

private slots:
    // ── loadFromString basics ────────────────────────────────────
    void testSimplePatternMatch();
    void testWildcardStarMatch();
    void testWildcardStarDoesNotMatchPartialExtension();
    void testDoubleStarMatchesDirectoryTraversal();
    void testQuestionMarkMatchesSingleChar();
    void testCommentLinesIgnored();
    void testEmptyLinesIgnored();
    void testTrailingSlashPattern();
    void testNegationPatternsIgnored();

    // ── No patterns -> no matches ────────────────────────────────
    void testNoPatternsMeansNoMatch();

    // ── Multiple patterns ────────────────────────────────────────
    void testMultiplePatternsMatchAny();

    // ── Path component matching ──────────────────────────────────
    void testPatternMatchesDeepComponent();
    void testPatternMatchesBasename();

    // ── loadFromFile ─────────────────────────────────────────────
    void testLoadFromFileReadsPatterns();
    void testLoadFromFileNonexistent();

    // ── patterns() accessor ──────────────────────────────────────
    void testPatternsAccessor();
    void testLoadFromStringReplacesExisting();

    // ── Whitespace handling ──────────────────────────────────────
    void testLeadingWhitespaceStripped();
    void testTrailingWhitespaceStripped();
    void testCrlfLineEndings();

    // ── Edge cases ───────────────────────────────────────────────
    void testStarAloneMatchesSingleComponent();
    void testDoubleStarAloneMatchesEverything();
};

// ── loadFromString basics ────────────────────────────────────────

void TestBsignoreParser::testSimplePatternMatch()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.o");
    QVERIFY(parser.matches("test.o"));
    QVERIFY(parser.matches("/path/to/test.o"));
}

void TestBsignoreParser::testWildcardStarMatch()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.txt");
    QVERIFY(parser.matches("file.txt"));
    QVERIFY(parser.matches("/some/path/notes.txt"));
}

void TestBsignoreParser::testWildcardStarDoesNotMatchPartialExtension()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.txt");
    // ".txta" should NOT match "*.txt" because * should consume all chars
    // but the pattern requires the suffix to be exactly ".txt"
    QVERIFY(!parser.matches("file.txta"));
}

void TestBsignoreParser::testDoubleStarMatchesDirectoryTraversal()
{
    bs::BsignoreParser parser;
    parser.loadFromString("**/build");
    QVERIFY(parser.matches("a/b/build"));
    QVERIFY(parser.matches("/project/deep/nested/build"));
    QVERIFY(parser.matches("build"));
}

void TestBsignoreParser::testQuestionMarkMatchesSingleChar()
{
    bs::BsignoreParser parser;
    parser.loadFromString("file?.txt");
    QVERIFY(parser.matches("file1.txt"));
    QVERIFY(parser.matches("fileA.txt"));
    QVERIFY(!parser.matches("file12.txt")); // ? matches only one char
}

void TestBsignoreParser::testCommentLinesIgnored()
{
    bs::BsignoreParser parser;
    parser.loadFromString("# This is a comment\n*.log");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 1);
    QCOMPARE(parser.patterns()[0], std::string("*.log"));
}

void TestBsignoreParser::testEmptyLinesIgnored()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log\n\n\n*.tmp\n\n");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 2);
}

void TestBsignoreParser::testTrailingSlashPattern()
{
    bs::BsignoreParser parser;
    parser.loadFromString("build/");
    // Trailing slash is stripped at match time, matches the dir name
    QVERIFY(parser.matches("project/build/output.o"));
    QVERIFY(parser.matches("/Users/me/project/build"));
}

void TestBsignoreParser::testNegationPatternsIgnored()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log\n!important.log");
    // Negation patterns are parsed but treated as no-ops for M1
    QCOMPARE(static_cast<int>(parser.patterns().size()), 1);
    QCOMPARE(parser.patterns()[0], std::string("*.log"));
}

// ── No patterns -> no matches ────────────────────────────────────

void TestBsignoreParser::testNoPatternsMeansNoMatch()
{
    bs::BsignoreParser parser;
    QVERIFY(!parser.matches("anything.txt"));
    QVERIFY(!parser.matches("/any/path/at/all"));
}

// ── Multiple patterns ────────────────────────────────────────────

void TestBsignoreParser::testMultiplePatternsMatchAny()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log\n*.tmp\n*.cache");

    QVERIFY(parser.matches("debug.log"));
    QVERIFY(parser.matches("session.tmp"));
    QVERIFY(parser.matches("data.cache"));
    QVERIFY(!parser.matches("report.txt"));
}

// ── Path component matching ──────────────────────────────────────

void TestBsignoreParser::testPatternMatchesDeepComponent()
{
    bs::BsignoreParser parser;
    parser.loadFromString("node_modules");
    // Should match the node_modules directory component at any level
    QVERIFY(parser.matches("project/node_modules/express/index.js"));
    QVERIFY(parser.matches("/Users/me/project/node_modules"));
}

void TestBsignoreParser::testPatternMatchesBasename()
{
    bs::BsignoreParser parser;
    parser.loadFromString(".DS_Store");
    QVERIFY(parser.matches("/Users/me/Documents/.DS_Store"));
    QVERIFY(parser.matches(".DS_Store"));
}

// ── loadFromFile ─────────────────────────────────────────────────

void TestBsignoreParser::testLoadFromFileReadsPatterns()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString filePath = dir.path() + "/.bsignore";
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "# Comment\n";
    out << "*.log\n";
    out << "temp/\n";
    out << "\n";
    out << "*.bak\n";
    f.close();

    bs::BsignoreParser parser;
    QVERIFY(parser.loadFromFile(filePath.toStdString()));
    QCOMPARE(static_cast<int>(parser.patterns().size()), 3);
    QVERIFY(parser.matches("debug.log"));
    QVERIFY(parser.matches("backup.bak"));
}

void TestBsignoreParser::testLoadFromFileNonexistent()
{
    bs::BsignoreParser parser;
    QVERIFY(!parser.loadFromFile("/nonexistent/path/.bsignore"));
}

// ── patterns() accessor ──────────────────────────────────────────

void TestBsignoreParser::testPatternsAccessor()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log\n*.tmp");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 2);
    QCOMPARE(parser.patterns()[0], std::string("*.log"));
    QCOMPARE(parser.patterns()[1], std::string("*.tmp"));
}

void TestBsignoreParser::testLoadFromStringReplacesExisting()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 1);

    // Loading new patterns should replace existing ones
    parser.loadFromString("*.tmp\n*.bak");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 2);
    QCOMPARE(parser.patterns()[0], std::string("*.tmp"));
}

// ── Whitespace handling ──────────────────────────────────────────

void TestBsignoreParser::testLeadingWhitespaceStripped()
{
    bs::BsignoreParser parser;
    parser.loadFromString("  *.log");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 1);
    QCOMPARE(parser.patterns()[0], std::string("*.log"));
}

void TestBsignoreParser::testTrailingWhitespaceStripped()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log   ");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 1);
    QCOMPARE(parser.patterns()[0], std::string("*.log"));
}

void TestBsignoreParser::testCrlfLineEndings()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*.log\r\n*.tmp\r\n");
    QCOMPARE(static_cast<int>(parser.patterns().size()), 2);
    QCOMPARE(parser.patterns()[0], std::string("*.log"));
    QCOMPARE(parser.patterns()[1], std::string("*.tmp"));
}

// ── Edge cases ───────────────────────────────────────────────────

void TestBsignoreParser::testStarAloneMatchesSingleComponent()
{
    bs::BsignoreParser parser;
    parser.loadFromString("*");
    // * matches any single component (no /)
    QVERIFY(parser.matches("file.txt"));
    QVERIFY(parser.matches("anything"));
}

void TestBsignoreParser::testDoubleStarAloneMatchesEverything()
{
    bs::BsignoreParser parser;
    parser.loadFromString("**");
    QVERIFY(parser.matches("file.txt"));
    QVERIFY(parser.matches("/deep/nested/path/file.txt"));
}

QTEST_MAIN(TestBsignoreParser)
#include "test_bsignore_parser.moc"
