#include <QtTest/QtTest>

#include "core/ranking/personalized_ltr.h"

#include <sqlite3.h>

#include <QDir>
#include <QTemporaryDir>

class TestPersonalizedLtr : public QObject {
    Q_OBJECT

private slots:
    void testRetrainThreshold();
    void testApplyAdjustsScores();

private:
    sqlite3* createInteractionDb() const;
};

sqlite3* TestPersonalizedLtr::createInteractionDb() const
{
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        return nullptr;
    }
    sqlite3_exec(
        db,
        "CREATE TABLE interactions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "query TEXT,"
        "query_normalized TEXT,"
        "item_id INTEGER,"
        "path TEXT,"
        "match_type TEXT,"
        "result_position INTEGER,"
        "app_context TEXT,"
        "timestamp TEXT"
        ")",
        nullptr, nullptr, nullptr);
    return db;
}

void TestPersonalizedLtr::testRetrainThreshold()
{
    sqlite3* db = createInteractionDb();
    QVERIFY(db != nullptr);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    bs::PersonalizedLtr ltr(tempDir.path() + QStringLiteral("/ltr_model.json"));

    // Too few interactions.
    sqlite3_exec(db,
                 "INSERT INTO interactions (query, query_normalized, item_id, path, match_type, result_position) "
                 "VALUES ('q', 'q', 1, '/tmp/a', 'Content', 5)",
                 nullptr, nullptr, nullptr);
    QVERIFY(!ltr.maybeRetrain(db, 200));

    // Add enough interactions to train.
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (int i = 0; i < 240; ++i) {
        const QString sql = QStringLiteral(
            "INSERT INTO interactions (query, query_normalized, item_id, path, match_type, result_position) "
            "VALUES ('q%1', 'q%1', %2, '/tmp/a', 'Content', %3)")
            .arg(i)
            .arg(i + 1)
            .arg((i % 5) + 1);
        sqlite3_exec(db, sql.toUtf8().constData(), nullptr, nullptr, nullptr);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    QVERIFY(ltr.maybeRetrain(db, 200));
    QVERIFY(ltr.isAvailable());

    sqlite3_close(db);
}

void TestPersonalizedLtr::testApplyAdjustsScores()
{
    sqlite3* db = createInteractionDb();
    QVERIFY(db != nullptr);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString modelPath = tempDir.path() + QStringLiteral("/ltr_model.json");

    bs::PersonalizedLtr trainer(modelPath);
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (int i = 0; i < 220; ++i) {
        const QString sql = QStringLiteral(
            "INSERT INTO interactions (query, query_normalized, item_id, path, match_type, result_position) "
            "VALUES ('k%1', 'k%1', %2, '/tmp/a', 'Content', %3)")
            .arg(i)
            .arg(i + 1)
            .arg((i % 3) + 1);
        sqlite3_exec(db, sql.toUtf8().constData(), nullptr, nullptr, nullptr);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    QVERIFY(trainer.maybeRetrain(db, 200));

    bs::PersonalizedLtr ltr(modelPath);
    QVERIFY(ltr.initialize(db));

    std::vector<bs::SearchResult> results(2);
    results[0].itemId = 1;
    results[0].score = 90.0;
    results[0].semanticNormalized = 0.2;
    results[0].crossEncoderScore = 0.1f;
    results[0].scoreBreakdown.feedbackBoost = 1.0;
    results[0].scoreBreakdown.frequencyBoost = 2.0;
    results[0].matchType = bs::MatchType::Content;

    results[1].itemId = 2;
    results[1].score = 89.0;
    results[1].semanticNormalized = 0.9;
    results[1].crossEncoderScore = 0.8f;
    results[1].scoreBreakdown.feedbackBoost = 8.0;
    results[1].scoreBreakdown.frequencyBoost = 10.0;
    results[1].matchType = bs::MatchType::ExactName;

    bs::LtrContext context;
    context.queryClass = bs::QueryClass::NaturalLanguage;
    context.routerConfidence = 0.8f;
    context.semanticNeedScore = 0.7f;

    const double deltaTop10 = ltr.apply(results, context, 100);
    QVERIFY(deltaTop10 != 0.0);
    QVERIFY(results[0].score != 90.0 || results[1].score != 89.0);

    sqlite3_close(db);
}

QTEST_MAIN(TestPersonalizedLtr)
#include "test_personalized_ltr.moc"

