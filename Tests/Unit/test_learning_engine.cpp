#include <QtTest/QtTest>

#include "core/index/sqlite_store.h"
#include "core/learning/learning_engine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <sqlite3.h>

namespace {

std::optional<int64_t> seedItem(bs::SQLiteStore& store,
                                const QString& path,
                                const QString& name)
{
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    return store.upsertItem(path,
                            name,
                            QFileInfo(path).suffix(),
                            bs::ItemKind::Markdown,
                            1024,
                            now,
                            now,
                            QString(),
                            QStringLiteral("normal"),
                            QFileInfo(path).absolutePath());
}

void insertTrainingRow(sqlite3* db,
                       const QString& sampleId,
                       int64_t itemId,
                       int label,
                       double f0,
                       double f1)
{
    const QString featuresJson = QStringLiteral("[%1,%2,0,0,0,0,0,0,1,0,1,1,0]")
        .arg(f0, 0, 'f', 6)
        .arg(f1, 0, 'f', 6);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        INSERT INTO training_examples_v1 (
            sample_id,
            created_at,
            query,
            query_normalized,
            item_id,
            path,
            label,
            weight,
            features_json,
            consumed
        ) VALUES (?1, ?2, 'report', 'report', ?3, '/tmp/report.md', ?4, 1.0, ?5, 0)
    )";

    QCOMPARE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    const QByteArray sampleUtf8 = sampleId.toUtf8();
    const QByteArray featuresUtf8 = featuresJson.toUtf8();
    sqlite3_bind_text(stmt, 1, sampleUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    sqlite3_bind_int64(stmt, 3, itemId);
    sqlite3_bind_int(stmt, 4, label);
    sqlite3_bind_text(stmt, 5, featuresUtf8.constData(), -1, SQLITE_TRANSIENT);
    QCOMPARE(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

} // namespace

class TestLearningEngine : public QObject {
    Q_OBJECT

private slots:
    void testRecordBehaviorEventWithConsent();
    void testRecordBehaviorEventDenylistFilter();
    void testExposureAndPositiveAttribution();
    void testTriggerLearningCyclePromotesModel();
    void testCoreMlBootstrapSeededFromEnvOverride();
};

void TestLearningEngine::testRecordBehaviorEventWithConsent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    auto itemIdOpt = seedItem(store,
                              QDir(tempDir.path()).filePath(QStringLiteral("notes.md")),
                              QStringLiteral("notes.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::BehaviorEvent event;
    event.eventId = QStringLiteral("evt-1");
    event.source = QStringLiteral("betterspotlight");
    event.eventType = QStringLiteral("query_submitted");
    event.itemId = itemIdOpt.value();
    event.itemPath = QDir(tempDir.path()).filePath(QStringLiteral("notes.md"));
    event.timestamp = QDateTime::currentDateTimeUtc();

    QVERIFY(engine.recordBehaviorEvent(event));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testRecordBehaviorEventDenylistFilter()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {QStringLiteral("com.example.secret")}));

    bs::BehaviorEvent event;
    event.eventId = QStringLiteral("evt-deny-1");
    event.source = QStringLiteral("betterspotlight");
    event.eventType = QStringLiteral("query_submitted");
    event.appBundleId = QStringLiteral("com.example.secret");
    event.timestamp = QDateTime::currentDateTimeUtc();
    QVERIFY(engine.recordBehaviorEvent(event));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM behavior_events_v1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testExposureAndPositiveAttribution()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString path = QDir(tempDir.path()).filePath(QStringLiteral("report.md"));
    auto itemIdOpt = seedItem(store, path, QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    bs::SearchResult result;
    result.itemId = itemIdOpt.value();
    result.path = path;
    result.name = QStringLiteral("report.md");
    result.score = 120.0;
    result.semanticNormalized = 0.64;
    result.crossEncoderScore = 0.55f;

    bs::QueryContext context;
    context.contextEventId = QStringLiteral("ctx-1");
    context.activityDigest = QStringLiteral("digest-1");

    QVERIFY(engine.recordExposure(QStringLiteral("report"),
                                  result,
                                  context,
                                  bs::QueryClass::NaturalLanguage,
                                  0.8f,
                                  0.6f,
                                  0));

    sqlite3_stmt* stmt = nullptr;
    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM training_examples_v1 WHERE label IS NULL",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    QVERIFY(engine.recordPositiveInteraction(QStringLiteral("report"),
                                             itemIdOpt.value(),
                                             path,
                                             QStringLiteral("com.apple.finder"),
                                             QDateTime::currentDateTimeUtc()));

    QCOMPARE(sqlite3_prepare_v2(store.rawDb(),
                                "SELECT COUNT(*) FROM training_examples_v1 WHERE label = 1",
                                -1,
                                &stmt,
                                nullptr),
             SQLITE_OK);
    QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
    QCOMPARE(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}

void TestLearningEngine::testTriggerLearningCyclePromotesModel()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    auto itemIdOpt = seedItem(store,
                              QDir(tempDir.path()).filePath(QStringLiteral("report.md")),
                              QStringLiteral("report.md"));
    QVERIFY(itemIdOpt.has_value());

    bs::LearningEngine engine(store.rawDb(), tempDir.path());
    QVERIFY(engine.initialize());
    QVERIFY(engine.setConsent(true, true, true, {}));

    for (int i = 0; i < 180; ++i) {
        const int label = (i % 2 == 0) ? 1 : 0;
        const double f0 = label > 0 ? 0.85 : 0.15;
        const double f1 = label > 0 ? 0.75 : 0.25;
        insertTrainingRow(store.rawDb(),
                          QStringLiteral("seed-%1").arg(i),
                          itemIdOpt.value(),
                          label,
                          f0,
                          f1);
    }

    QString reason;
    const bool promoted = engine.triggerLearningCycle(true, &reason);
    QVERIFY2(promoted, qPrintable(reason));
    QVERIFY(engine.modelAvailable());

    const QJsonObject health = engine.healthSnapshot();
    QCOMPARE(health.value(QStringLiteral("lastCycleStatus")).toString(),
             QStringLiteral("succeeded"));
}

void TestLearningEngine::testCoreMlBootstrapSeededFromEnvOverride()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("index.db"));
    auto storeOpt = bs::SQLiteStore::open(dbPath);
    QVERIFY(storeOpt.has_value());
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QString sourceBootstrapDir =
        QDir(tempDir.path()).filePath(QStringLiteral("bootstrap-source"));
    const QString sourceModelDir = QDir(sourceBootstrapDir).filePath(
        QStringLiteral("online_ranker_v1.mlmodelc"));
    QVERIFY(QDir().mkpath(sourceModelDir));

    QFile dummyModelFile(QDir(sourceModelDir).filePath(QStringLiteral("dummy.bin")));
    QVERIFY(dummyModelFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    dummyModelFile.write("seed");
    dummyModelFile.close();

    QFile metadataFile(QDir(sourceBootstrapDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    metadataFile.write("{\"version\":\"seed\"}");
    metadataFile.close();

    const QString runtimeDataDir = QDir(tempDir.path()).filePath(QStringLiteral("runtime-data"));
    const QByteArray originalEnv = qgetenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR");
    qputenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR", sourceBootstrapDir.toUtf8());

    bs::LearningEngine engine(store.rawDb(), runtimeDataDir);
    const bool initialized = engine.initialize();

    if (originalEnv.isEmpty()) {
        qunsetenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR");
    } else {
        qputenv("BETTERSPOTLIGHT_ONLINE_RANKER_BOOTSTRAP_DIR", originalEnv);
    }
    QVERIFY(initialized);

    const QString seededModelDir = QDir(runtimeDataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/online_ranker_v1.mlmodelc"));
    const QString seededMetadataPath = QDir(runtimeDataDir).filePath(
        QStringLiteral("models/online-ranker-v1/bootstrap/metadata.json"));

    QVERIFY(QFileInfo::exists(QDir(seededModelDir).filePath(QStringLiteral("dummy.bin"))));
    QVERIFY(QFileInfo::exists(seededMetadataPath));
}

QTEST_MAIN(TestLearningEngine)
#include "test_learning_engine.moc"
