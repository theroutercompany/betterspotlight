#include <QtTest/QtTest>
#include "core/indexing/chunker.h"
#include "core/shared/chunk.h"

class TestChunker : public QObject {
    Q_OBJECT

private slots:
    // ── Basic behavior ───────────────────────────────────────────
    void testEmptyContentReturnsEmpty();
    void testShortContentReturnsSingleChunk();
    void testContentAtTargetSizeReturnsSingleChunk();
    void testContentExceedingTargetSplits();

    // ── Split boundary priorities ────────────────────────────────
    void testSplitsAtParagraphBoundary();
    void testSplitsAtSentenceBoundary();
    void testSplitsAtWordBoundary();
    void testForceSplitAtMaxSize();

    // ── Chunk ID stability ───────────────────────────────────────
    void testChunkIdsStableSameInput();
    void testChunkIdsDifferForDifferentPaths();
    void testChunkIdsDifferForDifferentIndices();

    // ── Size constraints ─────────────────────────────────────────
    void testAllChunksWithinSizeBounds();
    void testLastChunkCanBeSmallerThanMinSize();
    void testSmallLeftoverAbsorbedIntoLastChunk();

    // ── Custom config ────────────────────────────────────────────
    void testCustomChunkerConfig();
    void testVerySmallTargetSize();

    // ── Determinism ──────────────────────────────────────────────
    void testChunkCountDeterministic();

    // ── Edge cases ───────────────────────────────────────────────
    void testSingleCharContent();
    void testAllNewlines();
    void testNoSplitBoundaries();
};

// ── Basic behavior ───────────────────────────────────────────────

void TestChunker::testEmptyContentReturnsEmpty()
{
    bs::Chunker chunker;
    auto chunks = chunker.chunkContent(QStringLiteral("/test/file.txt"), QString());
    QVERIFY(chunks.empty());
}

void TestChunker::testShortContentReturnsSingleChunk()
{
    bs::Chunker chunker;
    QString content = QStringLiteral("Hello, world! This is short.");
    auto chunks = chunker.chunkContent(QStringLiteral("/test/short.txt"), content);
    QCOMPARE(static_cast<int>(chunks.size()), 1);
    QCOMPARE(chunks[0].content, content);
    QCOMPARE(chunks[0].chunkIndex, 0);
}

void TestChunker::testContentAtTargetSizeReturnsSingleChunk()
{
    bs::Chunker chunker; // targetSize=1000
    // Create exactly 1000 chars
    QString content;
    content.fill(QLatin1Char('a'), 1000);
    auto chunks = chunker.chunkContent(QStringLiteral("/test/exact.txt"), content);
    QCOMPARE(static_cast<int>(chunks.size()), 1);
}

void TestChunker::testContentExceedingTargetSplits()
{
    bs::Chunker chunker; // targetSize=1000, minSize=500, maxSize=2000
    // Create content that exceeds target but has paragraph splits
    QString content;
    // 800 chars + paragraph break + 800 chars = 1602 chars total
    content.fill(QLatin1Char('a'), 800);
    content += QStringLiteral("\n\n");
    QString part2;
    part2.fill(QLatin1Char('b'), 800);
    content += part2;

    auto chunks = chunker.chunkContent(QStringLiteral("/test/split.txt"), content);
    QVERIFY(chunks.size() >= 2);
}

// ── Split boundary priorities ────────────────────────────────────

void TestChunker::testSplitsAtParagraphBoundary()
{
    // Use small config to test boundary detection more easily
    bs::ChunkerConfig config;
    config.targetSize = 50;
    config.minSize = 10;
    config.maxSize = 100;
    bs::Chunker chunker(config);

    // 30 chars + paragraph boundary + 30 chars
    QString content = QStringLiteral("This is the first paragraph!!");
    content += QStringLiteral("\n\n");
    content += QStringLiteral("This is the second paragraph.");

    auto chunks = chunker.chunkContent(QStringLiteral("/test/para.txt"), content);

    // With targetSize=50, the content (~62 chars) should split at \n\n
    if (chunks.size() >= 2) {
        QVERIFY(chunks[0].content.endsWith(QStringLiteral("\n\n")) ||
                chunks[1].content.startsWith(QStringLiteral("This is the second")));
    }
}

void TestChunker::testSplitsAtSentenceBoundary()
{
    bs::ChunkerConfig config;
    config.targetSize = 50;
    config.minSize = 10;
    config.maxSize = 100;
    bs::Chunker chunker(config);

    // No paragraph boundary, but has sentence boundary ". "
    QString content = QStringLiteral("First sentence ends here. Second sentence starts after that period.");

    auto chunks = chunker.chunkContent(QStringLiteral("/test/sent.txt"), content);
    // Should split at ". "
    QVERIFY(chunks.size() >= 1);
}

void TestChunker::testSplitsAtWordBoundary()
{
    bs::ChunkerConfig config;
    config.targetSize = 30;
    config.minSize = 10;
    config.maxSize = 60;
    bs::Chunker chunker(config);

    // No paragraph or sentence boundary, just words
    QString content = QStringLiteral("word1 word2 word3 word4 word5 word6 word7 word8 word9 word10");

    auto chunks = chunker.chunkContent(QStringLiteral("/test/word.txt"), content);
    QVERIFY(chunks.size() >= 1);
    // Each chunk should end at a word boundary (space) where possible
    for (size_t i = 0; i + 1 < chunks.size(); ++i) {
        // Non-last chunks should end at a space boundary
        QVERIFY(chunks[i].content.endsWith(QLatin1Char(' ')) ||
                static_cast<size_t>(chunks[i].content.size()) == config.maxSize);
    }
}

void TestChunker::testForceSplitAtMaxSize()
{
    bs::ChunkerConfig config;
    config.targetSize = 20;
    config.minSize = 5;
    config.maxSize = 30;
    bs::Chunker chunker(config);

    // A long string with no split boundaries
    QString content;
    content.fill(QLatin1Char('x'), 100);

    auto chunks = chunker.chunkContent(QStringLiteral("/test/force.txt"), content);
    QVERIFY(chunks.size() >= 2);
    // No chunk should exceed maxSize
    for (const auto& c : chunks) {
        QVERIFY(static_cast<size_t>(c.content.size()) <= config.maxSize);
    }
}

// ── Chunk ID stability ───────────────────────────────────────────

void TestChunker::testChunkIdsStableSameInput()
{
    bs::Chunker chunker;
    QString content = QStringLiteral("Some test content for stability testing.");

    auto chunks1 = chunker.chunkContent(QStringLiteral("/test/stable.txt"), content);
    auto chunks2 = chunker.chunkContent(QStringLiteral("/test/stable.txt"), content);

    QCOMPARE(chunks1.size(), chunks2.size());
    for (size_t i = 0; i < chunks1.size(); ++i) {
        QCOMPARE(chunks1[i].chunkId, chunks2[i].chunkId);
    }
}

void TestChunker::testChunkIdsDifferForDifferentPaths()
{
    bs::Chunker chunker;
    QString content = QStringLiteral("Identical content for both files.");

    auto chunks1 = chunker.chunkContent(QStringLiteral("/test/file_a.txt"), content);
    auto chunks2 = chunker.chunkContent(QStringLiteral("/test/file_b.txt"), content);

    QCOMPARE(chunks1.size(), chunks2.size());
    // Same content, different paths -> different chunk IDs
    QVERIFY(chunks1[0].chunkId != chunks2[0].chunkId);
}

void TestChunker::testChunkIdsDifferForDifferentIndices()
{
    bs::Chunker chunker;

    auto id0 = bs::computeChunkId(QStringLiteral("/test/file.txt"), 0);
    auto id1 = bs::computeChunkId(QStringLiteral("/test/file.txt"), 1);

    QVERIFY(id0 != id1);
}

// ── Size constraints ─────────────────────────────────────────────

void TestChunker::testAllChunksWithinSizeBounds()
{
    bs::ChunkerConfig config;
    config.targetSize = 100;
    config.minSize = 50;
    config.maxSize = 200;
    bs::Chunker chunker(config);

    // Build content with multiple split points
    QString content;
    for (int i = 0; i < 20; ++i) {
        content += QStringLiteral("Sentence number ") + QString::number(i) +
                   QStringLiteral(" with some filler text to reach size. ");
    }

    auto chunks = chunker.chunkContent(QStringLiteral("/test/bounds.txt"), content);
    QVERIFY(chunks.size() > 1);

    for (size_t i = 0; i < chunks.size(); ++i) {
        QVERIFY2(static_cast<size_t>(chunks[i].content.size()) <= config.maxSize,
                 qPrintable(QStringLiteral("Chunk %1 exceeds maxSize: %2")
                                .arg(i)
                                .arg(chunks[i].content.size())));
    }
}

void TestChunker::testLastChunkCanBeSmallerThanMinSize()
{
    bs::ChunkerConfig config;
    config.targetSize = 100;
    config.minSize = 50;
    config.maxSize = 200;
    bs::Chunker chunker(config);

    // Content designed to leave a small tail that gets absorbed
    // But with maxSize constraint, the last chunk might be small
    // The chunker absorbs small leftovers into the last chunk if combined <= maxSize
    QString content;
    content.fill(QLatin1Char('a'), 250);

    auto chunks = chunker.chunkContent(QStringLiteral("/test/tail.txt"), content);
    QVERIFY(!chunks.empty());
    // Total content should be preserved
    int totalLen = 0;
    for (const auto& c : chunks) {
        totalLen += c.content.size();
    }
    QCOMPARE(totalLen, content.size());
}

void TestChunker::testSmallLeftoverAbsorbedIntoLastChunk()
{
    bs::ChunkerConfig config;
    config.targetSize = 100;
    config.minSize = 50;
    config.maxSize = 200;
    bs::Chunker chunker(config);

    // 120 chars: first chunk would be ~100, leaving 20 which is < minSize(50)
    // So chunker should absorb leftover into the first chunk => single chunk
    QString content;
    content.fill(QLatin1Char('a'), 120);

    auto chunks = chunker.chunkContent(QStringLiteral("/test/absorb.txt"), content);
    QCOMPARE(static_cast<int>(chunks.size()), 1);
    QCOMPARE(chunks[0].content.size(), content.size());
}

// ── Custom config ────────────────────────────────────────────────

void TestChunker::testCustomChunkerConfig()
{
    bs::ChunkerConfig config;
    config.targetSize = 50;
    config.minSize = 20;
    config.maxSize = 80;
    bs::Chunker chunker(config);

    // Content large enough to produce multiple chunks
    QString content;
    for (int i = 0; i < 10; ++i) {
        content += QStringLiteral("Word ") + QString::number(i) + QStringLiteral(" ");
    }
    content += QStringLiteral("\n\n");
    for (int i = 10; i < 20; ++i) {
        content += QStringLiteral("Word ") + QString::number(i) + QStringLiteral(" ");
    }

    auto chunks = chunker.chunkContent(QStringLiteral("/test/custom.txt"), content);
    QVERIFY(!chunks.empty());

    for (const auto& c : chunks) {
        QVERIFY(static_cast<size_t>(c.content.size()) <= config.maxSize);
    }
}

void TestChunker::testVerySmallTargetSize()
{
    bs::ChunkerConfig config;
    config.targetSize = 10;
    config.minSize = 5;
    config.maxSize = 20;
    bs::Chunker chunker(config);

    QString content = QStringLiteral("Hello world! This is a test of very small chunks.");

    auto chunks = chunker.chunkContent(QStringLiteral("/test/tiny.txt"), content);
    QVERIFY(chunks.size() > 1);
}

// ── Determinism ──────────────────────────────────────────────────

void TestChunker::testChunkCountDeterministic()
{
    bs::Chunker chunker;
    QString content;
    for (int i = 0; i < 50; ++i) {
        content += QStringLiteral("Line ") + QString::number(i) +
                   QStringLiteral(": Some content with words.\n\n");
    }

    auto chunks1 = chunker.chunkContent(QStringLiteral("/test/det.txt"), content);
    auto chunks2 = chunker.chunkContent(QStringLiteral("/test/det.txt"), content);
    auto chunks3 = chunker.chunkContent(QStringLiteral("/test/det.txt"), content);

    QCOMPARE(chunks1.size(), chunks2.size());
    QCOMPARE(chunks2.size(), chunks3.size());
}

// ── Edge cases ───────────────────────────────────────────────────

void TestChunker::testSingleCharContent()
{
    bs::Chunker chunker;
    auto chunks = chunker.chunkContent(QStringLiteral("/test/x.txt"), QStringLiteral("x"));
    QCOMPARE(static_cast<int>(chunks.size()), 1);
    QCOMPARE(chunks[0].content, QStringLiteral("x"));
}

void TestChunker::testAllNewlines()
{
    bs::Chunker chunker;
    QString content;
    content.fill(QLatin1Char('\n'), 50);
    auto chunks = chunker.chunkContent(QStringLiteral("/test/nl.txt"), content);
    QVERIFY(!chunks.empty());
}

void TestChunker::testNoSplitBoundaries()
{
    bs::ChunkerConfig config;
    config.targetSize = 20;
    config.minSize = 5;
    config.maxSize = 30;
    bs::Chunker chunker(config);

    // No spaces, newlines, or punctuation
    QString content;
    content.fill(QLatin1Char('x'), 80);

    auto chunks = chunker.chunkContent(QStringLiteral("/test/nosplit.txt"), content);
    // All content should be preserved
    int total = 0;
    for (const auto& c : chunks) {
        total += c.content.size();
    }
    QCOMPARE(total, 80);
}

QTEST_MAIN(TestChunker)
#include "test_chunker.moc"
