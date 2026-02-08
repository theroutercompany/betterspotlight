#include <QtTest/QtTest>
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"
#include "core/vector/search_merger.h"
#include "core/embedding/embedding_manager.h"

#include <sqlite3.h>

#include <algorithm>
#include <vector>

class TestSemanticSearch : public QObject {
    Q_OBJECT

private slots:
    void testSemanticSearchFallback();
    void testVectorStoreIntegration();
    void testSearchMergerWithVectorStore();

private:
    static std::vector<float> makeVector(int seed);
};

std::vector<float> TestSemanticSearch::makeVector(int seed)
{
    std::vector<float> vector(static_cast<size_t>(bs::VectorIndex::kDimensions), 0.0F);
    vector[static_cast<size_t>(seed % bs::VectorIndex::kDimensions)] = 1.0F;
    return vector;
}

void TestSemanticSearch::testSemanticSearchFallback()
{
    bs::EmbeddingManager manager(QStringLiteral("missing.onnx"),
                                 QStringLiteral("missing_vocab.txt"));
    QVERIFY(!manager.isAvailable());

    const std::vector<float> embedding = manager.embed(QStringLiteral("test query"));
    QVERIFY(embedding.empty());

    std::vector<bs::SearchResult> lexical;
    bs::SearchResult result;
    result.itemId = 1;
    result.path = QStringLiteral("/tmp/lexical.txt");
    result.name = QStringLiteral("lexical.txt");
    result.score = 100.0;
    result.matchType = bs::MatchType::Content;
    lexical.push_back(result);

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, {});
    QCOMPARE(static_cast<int>(merged.size()), 1);
    QCOMPARE(merged[0].itemId, static_cast<int64_t>(1));
}

void TestSemanticSearch::testVectorStoreIntegration()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            extension TEXT,
            kind TEXT NOT NULL,
            size INTEGER NOT NULL DEFAULT 0,
            created_at REAL NOT NULL,
            modified_at REAL NOT NULL,
            indexed_at REAL NOT NULL,
            content_hash TEXT,
            classification TEXT,
            sensitivity TEXT NOT NULL DEFAULT 'normal',
            is_pinned INTEGER NOT NULL DEFAULT 0,
            parent_path TEXT
        );
        CREATE TABLE IF NOT EXISTS vector_map (
            item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
            hnsw_label INTEGER NOT NULL UNIQUE,
            model_version TEXT NOT NULL,
            embedded_at REAL NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_vector_map_label ON vector_map(hnsw_label);
        INSERT INTO items (id, path, name, kind, size, created_at, modified_at, indexed_at)
            VALUES (42, '/tmp/test.cpp', 'test.cpp', 'file', 1024, 0.0, 0.0, 0.0);
    )";
    QCOMPARE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK);

    bs::VectorStore store(db);
    QVERIFY(store.addMapping(42, 7, "v1"));

    const auto label = store.getLabel(42);
    QVERIFY(label.has_value());
    QCOMPARE(label.value(), static_cast<uint64_t>(7));

    const auto itemId = store.getItemId(7);
    QVERIFY(itemId.has_value());
    QCOMPARE(itemId.value(), static_cast<int64_t>(42));

    sqlite3_close(db);
}

void TestSemanticSearch::testSearchMergerWithVectorStore()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            extension TEXT,
            kind TEXT NOT NULL,
            size INTEGER NOT NULL DEFAULT 0,
            created_at REAL NOT NULL,
            modified_at REAL NOT NULL,
            indexed_at REAL NOT NULL,
            content_hash TEXT,
            classification TEXT,
            sensitivity TEXT NOT NULL DEFAULT 'normal',
            is_pinned INTEGER NOT NULL DEFAULT 0,
            parent_path TEXT
        );
        CREATE TABLE IF NOT EXISTS vector_map (
            item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
            hnsw_label INTEGER NOT NULL UNIQUE,
            model_version TEXT NOT NULL,
            embedded_at REAL NOT NULL
        );
        INSERT INTO items (id, path, name, kind, size, created_at, modified_at, indexed_at)
            VALUES (10, '/src/main.cpp', 'main.cpp', 'file', 512, 0.0, 0.0, 0.0);
        INSERT INTO items (id, path, name, kind, size, created_at, modified_at, indexed_at)
            VALUES (20, '/src/utils.cpp', 'utils.cpp', 'file', 256, 0.0, 0.0, 0.0);
    )";
    QCOMPARE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK);

    bs::VectorStore store(db);
    QVERIFY(store.addMapping(10, 0, "v1"));
    QVERIFY(store.addMapping(20, 1, "v1"));

    std::vector<bs::SearchResult> lexical;
    bs::SearchResult lex;
    lex.itemId = 10;
    lex.path = QStringLiteral("/src/main.cpp");
    lex.name = QStringLiteral("main.cpp");
    lex.score = 150.0;
    lex.matchType = bs::MatchType::ContainsName;
    lexical.push_back(lex);

    std::vector<bs::SemanticResult> semantic;
    const auto itemIdForLabel1 = store.getItemId(1);
    QVERIFY(itemIdForLabel1.has_value());
    semantic.push_back({itemIdForLabel1.value(), 0.92F});

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QVERIFY(!merged.empty());

    const bool hasLexical = std::any_of(merged.begin(), merged.end(),
        [](const bs::SearchResult& r) { return r.itemId == 10; });
    const bool hasSemantic = std::any_of(merged.begin(), merged.end(),
        [](const bs::SearchResult& r) { return r.itemId == 20; });
    QVERIFY(hasLexical);
    QVERIFY(hasSemantic);

    sqlite3_close(db);
}

QTEST_MAIN(TestSemanticSearch)
#include "test_semantic_search.moc"
