#include <QtTest/QtTest>
#include "core/embedding/tokenizer.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

class TestTokenizerPair : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void testBasicPairEncoding();
    void testPairTruncation();
    void testPairBatchPadding();
    void testEmptySegment();

private:
    QTemporaryDir m_tempDir;
    QString m_vocabPath;
};

void TestTokenizerPair::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    m_vocabPath = m_tempDir.path() + QStringLiteral("/vocab.txt");

    QFile vocab(m_vocabPath);
    QVERIFY(vocab.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&vocab);
    // indices: 0=[PAD], 1=[UNK], 2=[CLS] (but kClsTokenId=101), etc.
    // Write enough entries to match expected special token IDs
    // Standard BERT vocab has [PAD]=0, [UNK]=100, [CLS]=101, [SEP]=102
    for (int i = 0; i < 100; ++i) {
        out << QStringLiteral("unused_%1\n").arg(i);
    }
    out << "[UNK]\n";   // 100
    out << "[CLS]\n";   // 101
    out << "[SEP]\n";   // 102
    out << "hello\n";   // 103
    out << "world\n";   // 104
    out << "foo\n";     // 105
    out << "bar\n";     // 106
    out << "test\n";    // 107
    out << "a\n";       // 108
    out << "quick\n";   // 109
    out << "brown\n";   // 110
    out << "fox\n";     // 111
}

void TestTokenizerPair::testBasicPairEncoding()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    const auto pair = tokenizer.tokenizePair(
        QStringLiteral("hello world"), QStringLiteral("foo bar"));

    // Structure: [CLS] hello world [SEP] foo bar [SEP]
    QVERIFY(!pair.inputIds.empty());
    QCOMPARE(pair.inputIds.front(), static_cast<int64_t>(101)); // [CLS]
    QCOMPARE(pair.inputIds.back(), static_cast<int64_t>(102));  // [SEP]

    // Verify two [SEP] tokens exist
    int sepCount = 0;
    for (const int64_t id : pair.inputIds) {
        if (id == 102) ++sepCount;
    }
    QCOMPARE(sepCount, 2);

    // tokenTypeIds: segment A = 0, segment B = 1
    // [CLS](0) hello(0) world(0) [SEP](0) foo(1) bar(1) [SEP](1)
    QCOMPARE(pair.tokenTypeIds.size(), pair.inputIds.size());
    QCOMPARE(pair.tokenTypeIds[0], static_cast<int64_t>(0)); // [CLS]

    // Find the first [SEP] position (end of segment A)
    size_t firstSepPos = 0;
    for (size_t i = 1; i < pair.inputIds.size(); ++i) {
        if (pair.inputIds[i] == 102) {
            firstSepPos = i;
            break;
        }
    }
    QVERIFY(firstSepPos > 0);
    QCOMPARE(pair.tokenTypeIds[firstSepPos], static_cast<int64_t>(0)); // first [SEP] is segment A
    if (firstSepPos + 1 < pair.tokenTypeIds.size()) {
        QCOMPARE(pair.tokenTypeIds[firstSepPos + 1], static_cast<int64_t>(1)); // segment B starts
    }

    // attentionMask: all 1s (no padding)
    for (const int64_t mask : pair.attentionMask) {
        QCOMPARE(mask, static_cast<int64_t>(1));
    }
}

void TestTokenizerPair::testPairTruncation()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    // Build a very long text for segment B
    QString longB;
    for (int i = 0; i < 2000; ++i) {
        longB += QStringLiteral("hello ");
    }

    const auto pair = tokenizer.tokenizePair(QStringLiteral("test"), longB);

    // Total length must be <= 512
    QVERIFY(static_cast<int>(pair.inputIds.size()) <= 512);

    // First token is [CLS], last is [SEP]
    QCOMPARE(pair.inputIds.front(), static_cast<int64_t>(101));
    QCOMPARE(pair.inputIds.back(), static_cast<int64_t>(102));

    // "test" token should still be present (A is short, shouldn't be truncated)
    bool foundTest = false;
    for (const int64_t id : pair.inputIds) {
        if (id == 107) { // "test" token ID
            foundTest = true;
            break;
        }
    }
    QVERIFY(foundTest);
}

void TestTokenizerPair::testPairBatchPadding()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    std::vector<std::pair<QString, QString>> pairs = {
        {QStringLiteral("hello"), QStringLiteral("world")},
        {QStringLiteral("a quick brown fox"), QStringLiteral("test")},
    };

    const auto batch = tokenizer.tokenizePairBatch(pairs);

    QCOMPARE(batch.batchSize, 2);
    QVERIFY(batch.sequenceLength > 0);

    // All arrays should be flattened: batchSize * sequenceLength
    const size_t expected = static_cast<size_t>(batch.batchSize) * static_cast<size_t>(batch.sequenceLength);
    QCOMPARE(batch.inputIds.size(), expected);
    QCOMPARE(batch.attentionMask.size(), expected);
    QCOMPARE(batch.tokenTypeIds.size(), expected);

    // Both sequences start with [CLS]
    QCOMPARE(batch.inputIds[0], static_cast<int64_t>(101));
    QCOMPARE(batch.inputIds[static_cast<size_t>(batch.sequenceLength)], static_cast<int64_t>(101));

    // Shorter sequence should have padding (attention mask = 0 at end)
    // Find which is shorter and verify padding
    const size_t row0Start = 0;
    const size_t row1Start = static_cast<size_t>(batch.sequenceLength);
    int row0RealTokens = 0;
    int row1RealTokens = 0;
    for (int i = 0; i < batch.sequenceLength; ++i) {
        if (batch.attentionMask[row0Start + static_cast<size_t>(i)] == 1) ++row0RealTokens;
        if (batch.attentionMask[row1Start + static_cast<size_t>(i)] == 1) ++row1RealTokens;
    }
    // The batch max length should equal the longer sequence
    QCOMPARE(batch.sequenceLength, std::max(row0RealTokens, row1RealTokens));
}

void TestTokenizerPair::testEmptySegment()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    // Empty B: should produce [CLS] A [SEP] [SEP]
    const auto pair = tokenizer.tokenizePair(QStringLiteral("hello"), QString());

    QVERIFY(!pair.inputIds.empty());
    QCOMPARE(pair.inputIds.front(), static_cast<int64_t>(101)); // [CLS]
    QCOMPARE(pair.inputIds.back(), static_cast<int64_t>(102));  // [SEP]

    // Should have two [SEP] tokens (even with empty B)
    int sepCount = 0;
    for (const int64_t id : pair.inputIds) {
        if (id == 102) ++sepCount;
    }
    QCOMPARE(sepCount, 2);

    // tokenTypeIds segment B should exist (just the last [SEP])
    QCOMPARE(pair.tokenTypeIds.back(), static_cast<int64_t>(1));
}

QTEST_MAIN(TestTokenizerPair)
#include "test_tokenizer_pair.moc"
