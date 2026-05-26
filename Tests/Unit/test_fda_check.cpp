#include <QtTest/QtTest>

#include "core/shared/fda_check.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <sqlite3.h>

namespace {

class EnvVarGuard {
public:
    explicit EnvVarGuard(const char* variableName)
        : name(variableName),
          hadValue(qEnvironmentVariableIsSet(variableName)),
          previousValue(qgetenv(variableName))
    {
    }

    ~EnvVarGuard()
    {
        if (hadValue) {
            qputenv(name.constData(), previousValue);
        } else {
            qunsetenv(name.constData());
        }
    }

private:
    QByteArray name;
    bool hadValue = false;
    QByteArray previousValue;
};

using DbPtr = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

constexpr int kBundleIdentifierClientType = 0;
constexpr int kPathClientType = 1;

QString sqliteError(sqlite3* db)
{
    return QString::fromUtf8(sqlite3_errmsg(db));
}

DbPtr openDb(const QString& path, QString* error)
{
    sqlite3* raw = nullptr;
    const QByteArray nativePath = QFile::encodeName(path);
    const int rc = sqlite3_open(nativePath.constData(), &raw);
    if (rc != SQLITE_OK) {
        if (error != nullptr) {
            *error = raw != nullptr ? sqliteError(raw) : QStringLiteral("sqlite3_open returned null db");
        }
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        return DbPtr(nullptr, sqlite3_close);
    }
    return DbPtr(raw, sqlite3_close);
}

bool execSql(sqlite3* db, const char* sql, QString* error)
{
    char* rawError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &rawError);
    if (rc == SQLITE_OK) {
        return true;
    }
    if (error != nullptr) {
        *error = rawError != nullptr ? QString::fromUtf8(rawError) : sqliteError(db);
    }
    sqlite3_free(rawError);
    return false;
}

bool createCurrentAccessTable(sqlite3* db, QString* error, bool includeClientType = false)
{
    const char* sql = includeClientType
        ? "create table access("
          "service text not null,"
          "client text not null,"
          "client_type integer not null,"
          "auth_value integer not null,"
          "last_modified integer not null)"
        : "create table access("
          "service text not null,"
          "client text not null,"
          "auth_value integer not null,"
          "last_modified integer not null)";
    return execSql(db, sql, error);
}

bool createLegacyAccessTable(sqlite3* db, QString* error)
{
    return execSql(db,
                   "create table access("
                   "service text not null,"
                   "client text not null,"
                   "client_type integer not null,"
                   "allowed integer not null,"
                   "last_modified integer not null)",
                   error);
}

bool insertAccess(
    sqlite3* db,
    const QString& client,
    int authValue,
    int lastModified,
    QString* error,
    std::optional<int> clientType = std::nullopt)
{
    sqlite3_stmt* rawStmt = nullptr;
    const char* sql = clientType.has_value()
        ? "insert into access(service, client, client_type, auth_value, last_modified) "
          "values(?, ?, ?, ?, ?)"
        : "insert into access(service, client, auth_value, last_modified) values(?, ?, ?, ?)";
    const int prepareRc = sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr);
    if (prepareRc != SQLITE_OK) {
        if (error != nullptr) {
            *error = sqliteError(db);
        }
        return false;
    }

    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    const QByteArray service = QByteArrayLiteral("kTCCServiceSystemPolicyAllFiles");
    const QByteArray clientUtf8 = client.toUtf8();
    sqlite3_bind_text(stmt.get(), 1, service.constData(), service.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, clientUtf8.constData(), clientUtf8.size(), SQLITE_TRANSIENT);
    int bindIndex = 3;
    if (clientType.has_value()) {
        sqlite3_bind_int(stmt.get(), bindIndex++, *clientType);
    }
    sqlite3_bind_int(stmt.get(), bindIndex++, authValue);
    sqlite3_bind_int(stmt.get(), bindIndex, lastModified);

    const int stepRc = sqlite3_step(stmt.get());
    if (stepRc == SQLITE_DONE) {
        return true;
    }
    if (error != nullptr) {
        *error = sqliteError(db);
    }
    return false;
}

bool insertLegacyAccess(
    sqlite3* db,
    const QString& client,
    int allowed,
    int lastModified,
    QString* error,
    int clientType = kBundleIdentifierClientType)
{
    sqlite3_stmt* rawStmt = nullptr;
    const int prepareRc = sqlite3_prepare_v2(
        db,
        "insert into access(service, client, client_type, allowed, last_modified) values(?, ?, ?, ?, ?)",
        -1,
        &rawStmt,
        nullptr);
    if (prepareRc != SQLITE_OK) {
        if (error != nullptr) {
            *error = sqliteError(db);
        }
        return false;
    }

    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    const QByteArray service = QByteArrayLiteral("kTCCServiceSystemPolicyAllFiles");
    const QByteArray clientUtf8 = client.toUtf8();
    sqlite3_bind_text(stmt.get(), 1, service.constData(), service.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, clientUtf8.constData(), clientUtf8.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 3, clientType);
    sqlite3_bind_int(stmt.get(), 4, allowed);
    sqlite3_bind_int(stmt.get(), 5, lastModified);

    const int stepRc = sqlite3_step(stmt.get());
    if (stepRc == SQLITE_DONE) {
        return true;
    }
    if (error != nullptr) {
        *error = sqliteError(db);
    }
    return false;
}

QString createFakeTccDb(const QString& homePath)
{
    const QString dbPath = QDir(homePath).filePath(
        QStringLiteral("Library/Application Support/com.apple.TCC/TCC.db"));
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    return dbPath;
}

QString createReadableMessagesSentinel(const QString& homePath)
{
    const QString sentinelPath = QDir(homePath).filePath(QStringLiteral("Library/Messages/chat.db"));
    QDir().mkpath(QFileInfo(sentinelPath).absolutePath());
    QFile file(sentinelPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write("sqlite placeholder");
    }
    return sentinelPath;
}

} // namespace

class TestFdaCheck : public QObject {
    Q_OBJECT

private slots:
    void testGrantedBundleEntryWinsOverNewerDeniedExecutableEntry();
    void testDeniedGrantDoesNotTreatReadableSentinelAsAccess();
    void testGrantRequiresReadableProtectedSentinel();
    void testReadableSentinelWithoutGrantIsNotAccess();
    void testGrantRequiresExistingProtectedSentinel();
    void testGrantedRowWithWrongClientTypeIsIgnored();
    void testLegacyAllowedSchemaGrantIsAccepted();
    void testMalformedAccessTableDoesNotGrantReadableSentinel();
    void testOverrideDatabasePathsAreTrimmed();
};

void TestFdaCheck::testGrantedBundleEntryWinsOverNewerDeniedExecutableEntry()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createCurrentAccessTable(db.get(), &error), qPrintable(error));

    const QString executablePath = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    QVERIFY(!executablePath.isEmpty());

    QVERIFY2(insertAccess(db.get(), executablePath, 0, 200, &error), qPrintable(error));
    QVERIFY2(insertAccess(db.get(), QStringLiteral("com.betterspotlight.app"), 2, 100, &error),
             qPrintable(error));

    QVERIFY(bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testDeniedGrantDoesNotTreatReadableSentinelAsAccess()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createCurrentAccessTable(db.get(), &error), qPrintable(error));
    QVERIFY2(insertAccess(db.get(), QStringLiteral("com.betterspotlight.app"), 0, 100, &error),
             qPrintable(error));

    QVERIFY(!bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testGrantRequiresReadableProtectedSentinel()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());
    const QString sentinelPath = createReadableMessagesSentinel(fakeHome.path());
    QVERIFY(QFile::setPermissions(sentinelPath, QFileDevice::Permissions()));

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createCurrentAccessTable(db.get(), &error), qPrintable(error));
    QVERIFY2(insertAccess(db.get(), QStringLiteral("com.betterspotlight.app"), 2, 100, &error),
             qPrintable(error));

    const bool hasAccess = bs::FdaCheck::hasFullDiskAccess();
    QVERIFY(QFile::setPermissions(sentinelPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(!hasAccess);
}

void TestFdaCheck::testReadableSentinelWithoutGrantIsNotAccess()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString missingDbPath = createFakeTccDb(fakeHome.path());
    QVERIFY(!QFileInfo::exists(missingDbPath));
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", missingDbPath.toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QVERIFY(!bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testGrantRequiresExistingProtectedSentinel()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createCurrentAccessTable(db.get(), &error), qPrintable(error));
    QVERIFY2(insertAccess(db.get(), QStringLiteral("com.betterspotlight.app"), 2, 100, &error),
             qPrintable(error));

    QVERIFY(!bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testGrantedRowWithWrongClientTypeIsIgnored()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createCurrentAccessTable(db.get(), &error, true), qPrintable(error));

    const QString executablePath = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    QVERIFY(!executablePath.isEmpty());

    QVERIFY2(insertAccess(
                 db.get(),
                 QStringLiteral("com.betterspotlight.app"),
                 2,
                 100,
                 &error,
                 kPathClientType),
             qPrintable(error));
    QVERIFY2(insertAccess(db.get(), executablePath, 2, 100, &error, kBundleIdentifierClientType),
             qPrintable(error));

    QVERIFY(!bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testLegacyAllowedSchemaGrantIsAccepted()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createLegacyAccessTable(db.get(), &error), qPrintable(error));
    QVERIFY2(insertLegacyAccess(
                 db.get(),
                 QStringLiteral("com.betterspotlight.app"),
                 1,
                 100,
                 &error,
                 kBundleIdentifierClientType),
             qPrintable(error));

    QVERIFY(bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testMalformedAccessTableDoesNotGrantReadableSentinel()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS", dbPath.toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(execSql(db.get(),
                     "create table access("
                     "service text not null,"
                     "client text not null,"
                     "last_modified integer not null)",
                     &error),
             qPrintable(error));

    QVERIFY(!bs::FdaCheck::hasFullDiskAccess());
}

void TestFdaCheck::testOverrideDatabasePathsAreTrimmed()
{
    EnvVarGuard homeGuard("HOME");
    EnvVarGuard dbOverrideGuard("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS");
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    qputenv("HOME", fakeHome.path().toUtf8());

    const QString dbPath = createFakeTccDb(fakeHome.path());
    qputenv(
        "BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS",
        QStringLiteral("  %1  ").arg(dbPath).toUtf8());
    createReadableMessagesSentinel(fakeHome.path());

    QString error;
    DbPtr db = openDb(dbPath, &error);
    QVERIFY2(db != nullptr, qPrintable(error));
    QVERIFY2(createCurrentAccessTable(db.get(), &error), qPrintable(error));
    QVERIFY2(insertAccess(db.get(), QStringLiteral("com.betterspotlight.app"), 2, 100, &error),
             qPrintable(error));

    QVERIFY(bs::FdaCheck::hasFullDiskAccess());
}

QTEST_MAIN(TestFdaCheck)
#include "test_fda_check.moc"
