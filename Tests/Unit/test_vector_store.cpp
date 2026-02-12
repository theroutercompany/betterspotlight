#include <QtTest/QtTest>

#include "core/vector/vector_store.h"

#include <sqlite3.h>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <utility>

class TestVectorStore : public QObject {
    Q_OBJECT

private slots:
    void testNullDatabaseGuardClauses();
    void testMappingLifecycleAndGenerations();
    void testSetActiveGenerationCreatesDefaultState();
    void testLegacySchemaMigrationPath();
    void testRejectsInvalidMappingArguments();
    void testGetAllMappingsAndClearAll();
    void testCorruptNegativeLabelRowsAreIgnored();
    void testGenerationStateActivationFlow();
    void testReadOnlyDatabaseRejectsMutationsGracefully();
    void testListGenerationStatesHandlesMissingTable();
};

void TestVectorStore::testNullDatabaseGuardClauses()
{
    bs::VectorStore store(nullptr);

    QVERIFY(!store.addMapping(1, 1, "m", "v1", 1, "cpu", 0, "active"));
    QVERIFY(!store.removeMapping(1));
    QVERIFY(!store.removeGeneration("v1"));
    QCOMPARE(store.countMappings(), 0);
    QCOMPARE(store.countMappingsForGeneration("v1"), 0);
    QVERIFY(!store.getLabel(1).has_value());
    QVERIFY(!store.getItemId(1).has_value());
    QVERIFY(store.getAllMappings().empty());
    QVERIFY(store.listGenerationStates().empty());
    QVERIFY(!store.activeGenerationState().has_value());
    QCOMPARE(store.activeGenerationId(), std::string("v1"));
    QVERIFY(!store.setActiveGeneration("v2"));
    QVERIFY(!store.clearAll());

    bs::VectorStore::GenerationState invalid;
    invalid.generationId = "";
    QVERIFY(!store.upsertGenerationState(invalid));
}

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

void TestVectorStore::testCorruptNegativeLabelRowsAreIgnored()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);
    QVERIFY(store.addMapping(10, 110, "model-a", "v1", 384, "cpu", 0, "active"));

    // Inject corrupt legacy row directly; production code should defensively ignore it.
    QCOMPARE(sqlite3_exec(
                 db,
                 "INSERT INTO vector_map ("
                 "item_id, hnsw_label, generation_id, model_id, dimensions, provider, "
                 "passage_ordinal, embedded_at, migration_state"
                 ") VALUES (999, -7, 'v1', 'legacy', 384, 'cpu', 0, 0, 'active');",
                 nullptr, nullptr, nullptr),
             SQLITE_OK);

    QVERIFY(!store.getLabel(999, "v1").has_value());
    const auto mappings = store.getAllMappings("v1");
    const auto it = std::find_if(mappings.begin(), mappings.end(), [](const auto& pair) {
        return pair.first == 999;
    });
    QVERIFY(it == mappings.end());

    sqlite3_close(db);
}

void TestVectorStore::testGenerationStateActivationFlow()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);

    bs::VectorStore::GenerationState v2;
    v2.generationId = "v2";
    v2.modelId = "model-v2";
    v2.dimensions = 768;
    v2.provider = "cpu";
    v2.state = "building";
    v2.progressPct = 25.0;
    v2.active = true;
    QVERIFY(store.upsertGenerationState(v2));
    QCOMPARE(store.activeGenerationId(), std::string("v2"));

    bs::VectorStore::GenerationState v3;
    v3.generationId = "v3";
    v3.modelId = "model-v3";
    v3.dimensions = 1024;
    v3.provider = "cpu";
    v3.state = "building";
    v3.progressPct = 10.0;
    v3.active = false;
    QVERIFY(store.upsertGenerationState(v3));

    const auto states = store.listGenerationStates();
    QVERIFY(states.size() >= 3); // includes default v1
    const auto activeState = store.activeGenerationState();
    QVERIFY(activeState.has_value());
    QCOMPARE(activeState->generationId, std::string("v2"));

    QVERIFY(store.setActiveGeneration("v3"));
    QCOMPARE(store.activeGenerationId(), std::string("v3"));
    const auto activeStateAfterSwitch = store.activeGenerationState();
    QVERIFY(activeStateAfterSwitch.has_value());
    QCOMPARE(activeStateAfterSwitch->generationId, std::string("v3"));

    sqlite3_close(db);
}

void TestVectorStore::testReadOnlyDatabaseRejectsMutationsGracefully()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("vector.db"));

    sqlite3* rwDb = nullptr;
    QCOMPARE(sqlite3_open(dbPath.toUtf8().constData(), &rwDb), SQLITE_OK);
    QVERIFY(rwDb != nullptr);
    {
        bs::VectorStore seeded(rwDb);
        QVERIFY(seeded.addMapping(1, 11, "model", "v1", 384, "cpu", 0, "active"));
    }
    sqlite3_close(rwDb);

    sqlite3* roDb = nullptr;
    QCOMPARE(sqlite3_open_v2(dbPath.toUtf8().constData(),
                             &roDb,
                             SQLITE_OPEN_READONLY,
                             nullptr),
             SQLITE_OK);
    QVERIFY(roDb != nullptr);

    bs::VectorStore store(roDb);
    bs::VectorStore::GenerationState update;
    update.generationId = "v2";
    update.modelId = "model-v2";
    update.dimensions = 768;
    update.provider = "cpu";
    update.state = "building";
    update.progressPct = 10.0;
    update.active = true;

    QVERIFY(!store.upsertGenerationState(update));
    QVERIFY(!store.setActiveGeneration("v2"));

    sqlite3_close(roDb);
}

void TestVectorStore::testListGenerationStatesHandlesMissingTable()
{
    sqlite3* db = nullptr;
    QCOMPARE(sqlite3_open(":memory:", &db), SQLITE_OK);
    QVERIFY(db != nullptr);

    bs::VectorStore store(db);
    QCOMPARE(sqlite3_exec(db,
                          "DROP TABLE vector_generation_state;",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);

    const auto states = store.listGenerationStates();
    QVERIFY(states.empty());
    QVERIFY(!store.activeGenerationState().has_value());

    sqlite3_close(db);
}

QTEST_MAIN(TestVectorStore)
#include "test_vector_store.moc"
