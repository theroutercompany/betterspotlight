#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

#include "core/index/sqlite_store.h"
#include "core/indexing/indexer.h"
#include "core/indexing/chunker.h"
#include "core/extraction/extraction_manager.h"
#include "core/fs/path_rules.h"
#include "core/shared/types.h"
#include "core/shared/chunk.h"

class TestFullPipeline : public QObject {
    Q_OBJECT

private slots:
    void testCreateFileIndexAndSearch();
    void testIndexedFileHasCorrectMetadata();
    void testSearchByFilenameMatch();
    void testExcludedFileNotIndexed();
};

void TestFullPipeline::testCreateFileIndexAndSearch()
{
    // ── Setup temp directory with a text file ────────────────────
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/test_document.txt";
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Quantum entanglement is a phenomenon in quantum mechanics "
            << "where particles become interconnected. This has implications "
            << "for quantum computing and quantum teleportation research.";
    }

    // ── Open database ────────────────────────────────────────────
    const QString dbPath = tempDir.path() + "/index.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    // ── Create pipeline components ───────────────────────────────
    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    // ── Process the file as a NewFile work item ──────────────────
    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();

    bs::IndexResult result = indexer.processWorkItem(item);
    QCOMPARE(result.status, bs::IndexResult::Status::Indexed);
    QVERIFY(result.chunksInserted > 0);

    // ── Verify: searchFts5() finds the file by content ───────────
    auto hits = store->searchFts5(QStringLiteral("quantum entanglement"));
    QVERIFY(!hits.empty());
    QVERIFY(hits[0].snippet.contains(QStringLiteral("quantum"),
                                      Qt::CaseInsensitive));

    // ── Verify: getItemByPath() returns correct metadata ─────────
    auto item_row = store->getItemByPath(filePath);
    QVERIFY(item_row.has_value());
    QCOMPARE(item_row->name, QStringLiteral("test_document.txt"));
    QVERIFY(item_row->size > 0);
}

void TestFullPipeline::testIndexedFileHasCorrectMetadata()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/report.md";
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "# Monthly Report\n\nSales increased by 15% this quarter.\n";
    }

    const QString dbPath = tempDir.path() + "/index.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();
    indexer.processWorkItem(item);

    auto row = store->getItemByPath(filePath);
    QVERIFY(row.has_value());
    QCOMPARE(row->name, QStringLiteral("report.md"));
    QVERIFY(row->modifiedAt > 0.0);
    QVERIFY(row->id > 0);
}

void TestFullPipeline::testSearchByFilenameMatch()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/unique_searchable_filename.txt";
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "Some generic content inside.\n";
    }

    const QString dbPath = tempDir.path() + "/index.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();
    indexer.processWorkItem(item);

    // FTS5 indexes the filename too; search for it
    auto hits = store->searchFts5(QStringLiteral("unique_searchable_filename"));
    QVERIFY(!hits.empty());
}

void TestFullPipeline::testExcludedFileNotIndexed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Create a file inside a node_modules directory
    QDir(tempDir.path()).mkpath("node_modules/express");
    const QString filePath = tempDir.path() + "/node_modules/express/index.js";
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&f);
        out << "module.exports = function() {};\n";
    }

    const QString dbPath = tempDir.path() + "/index.db";
    auto store = bs::SQLiteStore::open(dbPath);
    QVERIFY(store.has_value());

    bs::ExtractionManager extractor;
    bs::PathRules pathRules;
    bs::Chunker chunker;
    bs::Indexer indexer(*store, extractor, pathRules, chunker);

    bs::WorkItem item;
    item.type = bs::WorkItem::Type::NewFile;
    item.filePath = filePath.toStdString();

    bs::IndexResult result = indexer.processWorkItem(item);
    QCOMPARE(result.status, bs::IndexResult::Status::Excluded);

    // Should not be in the database
    auto row = store->getItemByPath(filePath);
    QVERIFY(!row.has_value());
}

QTEST_MAIN(TestFullPipeline)
#include "test_full_pipeline.moc"
