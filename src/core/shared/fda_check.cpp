#include "core/shared/fda_check.h"
#include "core/shared/logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QSet>
#include <QStringList>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace bs {

namespace {

enum class ProbeKind {
    Directory,
    File,
};

enum class ProbeResult {
    Accessible,
    Denied,
    Missing,
    OtherError,
};

enum class GrantResult {
    Granted,
    Denied,
    NotFound,
    Unknown,
};

enum class SchemaReadResult {
    Ready,
    NotFound,
    Unknown,
};

enum class AuthorizationColumn {
    AuthValue,
    Allowed,
};

struct ProbeSpec {
    QString relativePath;
    ProbeKind kind;
};

struct TccClientCandidate {
    QString value;
    int clientType = -1;
};

struct AccessTableSchema {
    AuthorizationColumn authorizationColumn = AuthorizationColumn::AuthValue;
    bool hasClientType = false;
    bool hasLastModified = false;
};

constexpr int kBundleIdentifierClientType = 0;
constexpr int kPathClientType = 1;

bool containsNulByte(const QString& value)
{
    return value.contains(QChar(0));
}

ProbeResult classifyErrno(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
        return ProbeResult::Denied;
    case ENOENT:
    case ENOTDIR:
        return ProbeResult::Missing;
    default:
        return ProbeResult::OtherError;
    }
}

ProbeResult probeDirectory(const QString& path)
{
    if (path.isEmpty() || containsNulByte(path)) {
        return ProbeResult::OtherError;
    }

    const QByteArray nativePath = QFile::encodeName(path);
    errno = 0;
    DIR* dir = ::opendir(nativePath.constData());
    if (dir != nullptr) {
        ::closedir(dir);
        return ProbeResult::Accessible;
    }
    return classifyErrno(errno);
}

ProbeResult probeFile(const QString& path)
{
    if (path.isEmpty() || containsNulByte(path)) {
        return ProbeResult::OtherError;
    }

    const QByteArray nativePath = QFile::encodeName(path);
    errno = 0;
    const int fd = ::open(nativePath.constData(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::close(fd);
        return ProbeResult::Accessible;
    }
    return classifyErrno(errno);
}

ProbeResult probePath(const QString& path, ProbeKind kind)
{
    return kind == ProbeKind::Directory ? probeDirectory(path) : probeFile(path);
}

QString sqlString(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

QString appBundlePath()
{
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.dirName() != QLatin1String("MacOS") || !dir.cdUp()) {
        return {};
    }
    if (dir.dirName() != QLatin1String("Contents") || !dir.cdUp()) {
        return {};
    }
    if (!dir.dirName().endsWith(QStringLiteral(".app"))) {
        return {};
    }
    return QFileInfo(dir.path()).canonicalFilePath();
}

void addTccClientCandidate(QList<TccClientCandidate>& clients, const QString& value, int clientType)
{
    if (value.isEmpty()) {
        return;
    }

    for (const TccClientCandidate& client : clients) {
        if (client.value == value && client.clientType == clientType) {
            return;
        }
    }

    clients.push_back(TccClientCandidate{value, clientType});
}

QList<TccClientCandidate> tccClientCandidates()
{
    QList<TccClientCandidate> clients;
    addTccClientCandidate(
        clients,
        QStringLiteral("com.betterspotlight.app"),
        kBundleIdentifierClientType);

    const QString executablePath = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    if (!executablePath.isEmpty()) {
        addTccClientCandidate(clients, executablePath, kPathClientType);
    }

    const QString bundlePath = appBundlePath();
    if (!bundlePath.isEmpty()) {
        addTccClientCandidate(clients, bundlePath, kPathClientType);
    }

    return clients;
}

GrantResult mergeGrantResult(GrantResult current, GrantResult next)
{
    if (next == GrantResult::Granted || current == GrantResult::Granted) {
        return GrantResult::Granted;
    }
    if (next == GrantResult::Denied || current == GrantResult::Denied) {
        return GrantResult::Denied;
    }
    if (next == GrantResult::Unknown || current == GrantResult::Unknown) {
        return GrantResult::Unknown;
    }
    return GrantResult::NotFound;
}

QString sqliteSeparator()
{
    return QString(QChar(0x1f));
}

bool runSqliteQuery(const QString& dbPath, const QString& query, QString* output)
{
    QProcess sqlite;
    sqlite.setProgram(QStringLiteral("/usr/bin/sqlite3"));
    sqlite.setArguments({
        QStringLiteral("-batch"),
        QStringLiteral("-noheader"),
        QStringLiteral("-separator"),
        sqliteSeparator(),
        dbPath,
        query,
    });
    sqlite.start();
    if (!sqlite.waitForFinished(1500)) {
        sqlite.kill();
        sqlite.waitForFinished(200);
        return false;
    }
    if (sqlite.exitStatus() != QProcess::NormalExit || sqlite.exitCode() != 0) {
        return false;
    }
    if (output != nullptr) {
        *output = QString::fromUtf8(sqlite.readAllStandardOutput());
    }
    return true;
}

SchemaReadResult readAccessTableSchema(const QString& dbPath, AccessTableSchema* schema)
{
    QString output;
    if (!runSqliteQuery(dbPath, QStringLiteral("pragma table_info(access);"), &output)) {
        return SchemaReadResult::Unknown;
    }

    const QString trimmedOutput = output.trimmed();
    if (trimmedOutput.isEmpty()) {
        return SchemaReadResult::NotFound;
    }

    QSet<QString> columns;
    const QString separator = sqliteSeparator();
    const QStringList rows = trimmedOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& row : rows) {
        const QStringList fields = row.split(separator, Qt::KeepEmptyParts);
        if (fields.size() < 2) {
            return SchemaReadResult::Unknown;
        }
        columns.insert(fields.at(1).trimmed().toLower());
    }

    if (!columns.contains(QStringLiteral("service")) || !columns.contains(QStringLiteral("client"))) {
        return SchemaReadResult::Unknown;
    }

    AccessTableSchema parsedSchema;
    if (columns.contains(QStringLiteral("auth_value"))) {
        parsedSchema.authorizationColumn = AuthorizationColumn::AuthValue;
    } else if (columns.contains(QStringLiteral("allowed"))) {
        parsedSchema.authorizationColumn = AuthorizationColumn::Allowed;
    } else {
        return SchemaReadResult::Unknown;
    }
    parsedSchema.hasClientType = columns.contains(QStringLiteral("client_type"));
    parsedSchema.hasLastModified = columns.contains(QStringLiteral("last_modified"));

    if (schema != nullptr) {
        *schema = parsedSchema;
    }
    return SchemaReadResult::Ready;
}

QStringList tccClientValues(const QList<TccClientCandidate>& clients)
{
    QStringList values;
    values.reserve(clients.size());
    for (const TccClientCandidate& client : clients) {
        if (!values.contains(client.value)) {
            values << client.value;
        }
    }
    return values;
}

bool rowMatchesExpectedClient(
    const QList<TccClientCandidate>& clients,
    const QString& rowClient,
    int rowClientType,
    bool hasClientType)
{
    for (const TccClientCandidate& client : clients) {
        if (client.value != rowClient) {
            continue;
        }
        if (!hasClientType) {
            return true;
        }
        if (client.clientType == rowClientType) {
            return true;
        }
    }
    return false;
}

GrantResult classifyAuthorizationValue(const QString& value, AuthorizationColumn column)
{
    bool ok = false;
    const int parsed = value.trimmed().toInt(&ok);
    if (!ok) {
        return GrantResult::Unknown;
    }

    if (column == AuthorizationColumn::Allowed) {
        if (parsed == 1) {
            return GrantResult::Granted;
        }
        if (parsed == 0) {
            return GrantResult::Denied;
        }
        return GrantResult::Unknown;
    }

    if (parsed == 2) {
        return GrantResult::Granted;
    }
    if (parsed == 0) {
        return GrantResult::Denied;
    }
    return GrantResult::Unknown;
}

GrantResult queryTccDatabase(const QString& dbPath, const QList<TccClientCandidate>& clients)
{
    if (!QFileInfo::exists(dbPath) || clients.isEmpty()) {
        return GrantResult::NotFound;
    }

    AccessTableSchema schema;
    const SchemaReadResult schemaResult = readAccessTableSchema(dbPath, &schema);
    if (schemaResult == SchemaReadResult::NotFound) {
        return GrantResult::NotFound;
    }
    if (schemaResult != SchemaReadResult::Ready) {
        return GrantResult::Unknown;
    }

    QStringList quotedClients;
    const QStringList clientValues = tccClientValues(clients);
    quotedClients.reserve(clientValues.size());
    for (const QString& client : clientValues) {
        quotedClients << sqlString(client);
    }

    const QString clientTypeExpression =
        schema.hasClientType ? QStringLiteral("client_type") : QStringLiteral("-1");
    const QString authColumn = schema.authorizationColumn == AuthorizationColumn::AuthValue
        ? QStringLiteral("auth_value")
        : QStringLiteral("allowed");
    const QString orderClause = schema.hasLastModified
        ? QStringLiteral(" order by last_modified desc")
        : QString();
    const QString query = QStringLiteral(
        "select client, %1, %2 from access "
        "where service='kTCCServiceSystemPolicyAllFiles' "
        "and client in (%3)%4;")
                              .arg(
                                  clientTypeExpression,
                                  authColumn,
                                  quotedClients.join(QLatin1Char(',')),
                                  orderClause);

    QString output;
    if (!runSqliteQuery(dbPath, query, &output)) {
        return GrantResult::Unknown;
    }

    bool sawDenied = false;
    bool sawUnknown = false;
    const QString separator = sqliteSeparator();
    const QStringList rows = output.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& row : rows) {
        const QStringList fields = row.split(separator, Qt::KeepEmptyParts);
        if (fields.size() != 3) {
            sawUnknown = true;
            continue;
        }

        bool clientTypeOk = true;
        const int rowClientType = schema.hasClientType ? fields.at(1).trimmed().toInt(&clientTypeOk) : -1;
        if (!clientTypeOk) {
            sawUnknown = true;
            continue;
        }
        if (!rowMatchesExpectedClient(clients, fields.at(0), rowClientType, schema.hasClientType)) {
            continue;
        }

        const GrantResult grant = classifyAuthorizationValue(fields.at(2), schema.authorizationColumn);
        if (grant == GrantResult::Granted) {
            return GrantResult::Granted;
        }
        if (grant == GrantResult::Denied) {
            sawDenied = true;
            continue;
        }
        sawUnknown = true;
    }
    if (sawDenied) {
        return GrantResult::Denied;
    }
    if (sawUnknown) {
        return GrantResult::Unknown;
    }
    return GrantResult::NotFound;
}

GrantResult directFullDiskAccessGrantState()
{
    const QList<TccClientCandidate> clients = tccClientCandidates();
    const QString overrideDatabases =
        qEnvironmentVariable("BETTERSPOTLIGHT_TEST_FDA_TCC_DB_PATHS").trimmed();
    const QStringList rawDatabases = overrideDatabases.isEmpty()
        ? QStringList{
              QStringLiteral("/Library/Application Support/com.apple.TCC/TCC.db"),
              QDir::homePath() + QStringLiteral("/Library/Application Support/com.apple.TCC/TCC.db"),
          }
        : overrideDatabases.split(QDir::listSeparator(), Qt::SkipEmptyParts);

    GrantResult result = GrantResult::NotFound;
    for (const QString& rawDbPath : rawDatabases) {
        const QString dbPath = rawDbPath.trimmed();
        if (dbPath.isEmpty()) {
            continue;
        }
        result = mergeGrantResult(result, queryTccDatabase(dbPath, clients));
    }
    return result;
}

} // namespace

bool FdaCheck::hasFullDiskAccess()
{
    const QString homePath = QDir::homePath();
    const ProbeSpec probes[] = {
        {QStringLiteral("Library/Messages/chat.db"), ProbeKind::File},
        {QStringLiteral("Library/Safari/History.db"), ProbeKind::File},
        {QStringLiteral("Library/Mail"), ProbeKind::Directory},
        {QStringLiteral("Library/Calendars"), ProbeKind::Directory},
        {QStringLiteral("Library/Application Support/AddressBook"), ProbeKind::Directory},
    };

    bool sawDenied = false;
    bool sawProbeCandidate = false;
    bool effectiveAccess = false;
    for (const ProbeSpec& probe : probes) {
        const QString path = homePath + QLatin1Char('/') + probe.relativePath;
        const ProbeResult result = probePath(path, probe.kind);
        if (result == ProbeResult::Accessible) {
            effectiveAccess = true;
            break;
        }
        if (result == ProbeResult::Denied) {
            sawDenied = true;
            sawProbeCandidate = true;
            continue;
        }
        if (result == ProbeResult::OtherError) {
            sawProbeCandidate = true;
        }
    }

    const GrantResult directGrant = directFullDiskAccessGrantState();
    if (directGrant != GrantResult::Granted) {
        LOG_WARN(bsFs, "Full Disk Access: NOT GRANTED (no current BetterSpotlight FDA grant)");
        return false;
    }

    if (effectiveAccess) {
        LOG_INFO(bsFs, "Full Disk Access: GRANTED");
        return true;
    }
    if (sawDenied) {
        LOG_WARN(bsFs, "Full Disk Access: NOT GRANTED (protected sentinel denied)");
        return false;
    }
    if (sawProbeCandidate) {
        LOG_WARN(bsFs, "Full Disk Access: NOT VERIFIED (protected sentinel unreadable)");
        return false;
    }

    LOG_WARN(bsFs, "Full Disk Access: NOT VERIFIED (direct grant present but no protected sentinel found)");
    return false;
}

QString FdaCheck::instructionMessage()
{
    return QStringLiteral(
        "BetterSpotlight requires Full Disk Access to index all files.\n\n"
        "To grant access:\n"
        "1. Open System Settings > Privacy & Security > Full Disk Access\n"
        "2. Turn on BetterSpotlight if it is already listed\n"
        "3. Use '+' only if macOS does not show BetterSpotlight\n"
        "4. Return here and click Verify Access");
}

} // namespace bs
