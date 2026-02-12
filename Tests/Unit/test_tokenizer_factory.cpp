#include <QtTest/QtTest>

#include "core/models/model_manifest.h"
#include "core/models/tokenizer_factory.h"

#include <QFile>
#include <QTemporaryDir>

class TestTokenizerFactory : public QObject {
    Q_OBJECT

private slots:
    void testUnsupportedTokenizerTypeRejected();
    void testMissingVocabRejected();
    void testMissingVocabFileRejected();
    void testEmptyVocabFileRejected();
    void testValidWordpieceTokenizerLoads();
};

void TestTokenizerFactory::testUnsupportedTokenizerTypeRejected()
{
    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("invalid");
    entry.tokenizer = QStringLiteral("sentencepiece");
    entry.vocab = QStringLiteral("vocab.txt");

    const auto tokenizer = bs::TokenizerFactory::create(entry, QStringLiteral("/tmp"));
    QVERIFY(tokenizer == nullptr);
}

void TestTokenizerFactory::testMissingVocabRejected()
{
    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("missing-vocab");
    entry.tokenizer = QStringLiteral("wordpiece");

    const auto tokenizer = bs::TokenizerFactory::create(entry, QStringLiteral("/tmp"));
    QVERIFY(tokenizer == nullptr);
}

void TestTokenizerFactory::testMissingVocabFileRejected()
{
    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("missing-vocab-file");
    entry.tokenizer = QStringLiteral("wordpiece");
    entry.vocab = QStringLiteral("nope-vocab.txt");

    const auto tokenizer = bs::TokenizerFactory::create(entry, QStringLiteral("/tmp"));
    QVERIFY(tokenizer == nullptr);
}

void TestTokenizerFactory::testEmptyVocabFileRejected()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString vocabPath = tempDir.path() + QStringLiteral("/empty-vocab.txt");
    QFile vocabFile(vocabPath);
    QVERIFY(vocabFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    vocabFile.write("\n\n");
    vocabFile.close();

    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("empty-vocab");
    entry.tokenizer = QStringLiteral("wordpiece");
    entry.vocab = QStringLiteral("empty-vocab.txt");

    const auto tokenizer = bs::TokenizerFactory::create(entry, tempDir.path());
    QVERIFY(tokenizer == nullptr);
}

void TestTokenizerFactory::testValidWordpieceTokenizerLoads()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString vocabPath = tempDir.path() + QStringLiteral("/vocab.txt");
    QFile vocabFile(vocabPath);
    QVERIFY(vocabFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    vocabFile.write("[PAD]\n[UNK]\nhello\nworld\n##ly\n");
    vocabFile.close();

    bs::ModelManifestEntry entry;
    entry.name = QStringLiteral("wordpiece-ok");
    entry.tokenizer = QStringLiteral("wordpiece");
    entry.vocab = QStringLiteral("vocab.txt");

    auto tokenizer = bs::TokenizerFactory::create(entry, tempDir.path());
    QVERIFY(tokenizer != nullptr);
    QVERIFY(tokenizer->isLoaded());
}

QTEST_MAIN(TestTokenizerFactory)
#include "test_tokenizer_factory.moc"
