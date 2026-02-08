#include <QtTest/QtTest>
#include "core/embedding/tokenizer.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

class TestTokenizer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void testLoadVocabNotFound();
    void testEmptyInputReturnsEmpty();
    void testBasicTokenization();
    void testBatchTokenization();
    void testPaddingAligns();
    void testSpecialCharsHandled();
    void testLongTextTruncated();

private:
    QTemporaryDir m_tempDir;
    QString m_vocabPath;
};

void TestTokenizer::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    m_vocabPath = m_tempDir.path() + QStringLiteral("/vocab.txt");

    QFile vocab(m_vocabPath);
    QVERIFY(vocab.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&vocab);
    out << "[PAD]\n";
    out << "[UNK]\n";
    out << "[CLS]\n";
    out << "[SEP]\n";
    out << "hello\n";
    out << "world\n";
    out << "it\n";
    out << "test\n";
    out << "a\n";
    out << "!\n";
}

void TestTokenizer::testLoadVocabNotFound()
{
    bs::WordPieceTokenizer tokenizer(QStringLiteral("/definitely/missing/vocab.txt"));
    QVERIFY(!tokenizer.isLoaded());
}

void TestTokenizer::testEmptyInputReturnsEmpty()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    const bs::TokenizerOutput output = tokenizer.tokenize(QString());
    QVERIFY(output.seqLength <= 2);
    QVERIFY(output.inputIds.size() <= 2);
}

void TestTokenizer::testBasicTokenization()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    const bs::TokenizerOutput output = tokenizer.tokenize(QStringLiteral("hello world"));
    QVERIFY(!output.inputIds.empty());
    QCOMPARE(output.inputIds.front(), static_cast<int64_t>(101));
    QCOMPARE(output.inputIds.back(), static_cast<int64_t>(102));
}

void TestTokenizer::testBatchTokenization()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    const bs::BatchTokenizerOutput batch = tokenizer.tokenizeBatch(
        {QStringLiteral("hello"), QStringLiteral("world")});
    QCOMPARE(batch.batchSize, 2);
    QVERIFY(batch.seqLength > 0);
}

void TestTokenizer::testPaddingAligns()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    const bs::TokenizerOutput output = tokenizer.tokenize(QStringLiteral("hi"), 10);
    QCOMPARE(output.seqLength, 10);
    QCOMPARE(static_cast<int>(output.attentionMask.size()), 10);
    QVERIFY(output.attentionMask.back() == 0);
}

void TestTokenizer::testSpecialCharsHandled()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    const bs::TokenizerOutput output = tokenizer.tokenize(QStringLiteral("it's a test!"));
    QVERIFY(output.seqLength > 0);
    QVERIFY(!output.inputIds.empty());
}

void TestTokenizer::testLongTextTruncated()
{
    bs::WordPieceTokenizer tokenizer(m_vocabPath);
    QVERIFY(tokenizer.isLoaded());

    QString longText;
    for (int i = 0; i < 2000; ++i) {
        longText += QStringLiteral("hello ");
    }

    const bs::TokenizerOutput output = tokenizer.tokenize(longText);
    QVERIFY(output.seqLength <= 512);
}

QTEST_MAIN(TestTokenizer)
#include "test_tokenizer.moc"
