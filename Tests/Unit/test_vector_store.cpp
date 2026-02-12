#include <QtTest/QtTest>

#include "core/vector/vector_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <limits>

class TestVectorStore : public QObject {
    Q_OBJECT

private slots:
    void testMappingLifecycleAndGenerations();
    void testSetActiveGenerationCreatesDefaultState();
    void testLegacySchemaMigrationPath();
    void testRejectsInvalidMappingArguments();
    void testGetAllMappingsAndClearAll();
};

void TestVectorStore::testMappingLifecycleAndGenerations()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);
    QVERIFY(store.addMapping(1, 100, "model-a", "v1", 384, "cpu", 0, "active"));
    QCOMPARE(store.countMappings(), 1);
    QCOMPARE(store.countMappingsForGeneration("v1"), 1);

    const auto label = store.getLabel(1);
    QVERIFY(label.has_value());
    QCOMPARE(label.value(), static_cast<uint64_t>(100));

    const auto itemId = store.getItemId(100);
    QVERIFY(itemId.has_value());
    QCOMPARE(itemId.value(), static_cast<int64_t>(1));

    bs::VectorStore::GenerationState nextState;
    nextState.generationId = "v2";
    nextState.modelId = "model-b";
    nextState.dimensions = 768;
    nextState.provider = "cpu";
    nextState.state = "building";
    nextState.progressPct = 10.0;
    nextState.active = true;
    QVERIFY(store.upsertGenerationState(nextState));
    QCOMPARE(store.activeGenerationId(), std::string("v2"));

    QVERIFY(store.addMapping(1, 200, "model-b", "v2", 768, "cpu", 0, "building"));
    QCOMPARE(store.countMappingsForGeneration("v2"), 1);
    QVERIFY(store.removeMapping(1));
    QCOMPARE(store.countMappingsForGeneration("v2"), 0);
    QCOMPARE(store.countMappingsForGeneration("v1"), 1);

    QVERIFY(store.removeGeneration("v1"));
    QCOMPARE(store.countMappings(), 0);

    sqlite3_close(db);
}

void TestVectorStore::testSetActiveGenerationCreatesDefaultState()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);
    QVERIFY(store.setActiveGeneration("v9"));
    QCOMPARE(store.activeGenerationId(), std::string("v9"));

    const auto activeState = store.activeGenerationState();
    QVERIFY(activeState.has_value());
    QCOMPARE(activeState->generationId, std::string("v9"));
    QVERIFY(activeState->active);

    sqlite3_close(db);
}

void TestVectorStore::testLegacySchemaMigrationPath()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    QCOMPARE(sqlite3_exec(db,
                          "CREATE TABLE vector_map ("
                          "item_id INTEGER PRIMARY KEY,"
                          "hnsw_label INTEGER NOT NULL,"
                          "model_version TEXT,"
                          "embedded_at REAL NOT NULL"
                          ");"
                          "INSERT INTO vector_map (item_id, hnsw_label, model_version, embedded_at) "
                          "VALUES (7, 77, 'legacy-model', strftime('%s','now'));",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    bs::VectorStore store(db);
    QCOMPARE(store.countMappings(), 1);
    QCOMPARE(store.countMappingsForGeneration("v1"), 1);
    const auto label = store.getLabel(7, "v1");
    QVERIFY(label.has_value());
    QCOMPARE(label.value(), static_cast<uint64_t>(77));

    sqlite3_close(db);
}

void TestVectorStore::testRejectsInvalidMappingArguments()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);
    QVERIFY(!store.addMapping(1,
                              std::numeric_limits<uint64_t>::max(),
                              "model-a",
                              "v1",
                              384,
                              "cpu",
                              0,
                              "active"));
    QVERIFY(!store.addMapping(1, 1, "model-a", "v1", 384, "cpu", -1, "active"));

    sqlite3_close(db);
}

void TestVectorStore::testGetAllMappingsAndClearAll()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);
    QVERIFY(store.addMapping(10, 110, "model-a", "v1", 384, "cpu", 0, "active"));
    QVERIFY(store.addMapping(11, 111, "model-b", "v2", 768, "cpu", 0, "building"));
    QVERIFY(store.addMapping(12, 112, "model-b", "v2", 768, "cpu", 1, "building"));

    const auto allMappings = store.getAllMappings();
    QCOMPARE(static_cast<int>(allMappings.size()), 3);

    const auto v2Mappings = store.getAllMappings("v2");
    QCOMPARE(static_cast<int>(v2Mappings.size()), 2);
    QVERIFY(std::find_if(v2Mappings.begin(), v2Mappings.end(), [](const auto& pair) {
        return pair.first == 11 && pair.second == 111;
    }) != v2Mappings.end());

    QVERIFY(store.clearAll());
    QCOMPARE(store.countMappings(), 0);
    QVERIFY(store.getAllMappings().empty());

    sqlite3_close(db);
}

QTEST_MAIN(TestVectorStore)
#include "test_vector_store.moc"
