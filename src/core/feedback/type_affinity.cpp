#include <QDateTime>
#include <QFileInfo>
#include <QSet>

#include <sqlite3.h>

#include "core/feedback/type_affinity.h"

namespace bs {

namespace {

constexpr qint64 kRefreshIntervalMs = 10 * 60 * 1000;

const QSet<QString>& codeExtensions()
{
    static const QSet<QString> kCode = {
        QStringLiteral(".py"), QStringLiteral(".js"), QStringLiteral(".ts"),
        QStringLiteral(".tsx"), QStringLiteral(".jsx"), QStringLiteral(".cpp"),
        QStringLiteral(".c"), QStringLiteral(".h"), QStringLiteral(".hpp"),
        QStringLiteral(".go"), QStringLiteral(".rs"), QStringLiteral(".java"),
        QStringLiteral(".rb"), QStringLiteral(".php"), QStringLiteral(".swift"),
        QStringLiteral(".kt"), QStringLiteral(".scala"), QStringLiteral(".sh"),
        QStringLiteral(".bash"), QStringLiteral(".zsh")
    };
    return kCode;
}

const QSet<QString>& documentExtensions()
{
    static const QSet<QString> kDocument = {
        QStringLiteral(".md"), QStringLiteral(".txt"), QStringLiteral(".pdf"),
        QStringLiteral(".docx"), QStringLiteral(".doc"), QStringLiteral(".rtf"),
        QStringLiteral(".tex"), QStringLiteral(".org"), QStringLiteral(".rst"),
        QStringLiteral(".csv")
    };
    return kDocument;
}

const QSet<QString>& mediaExtensions()
{
    static const QSet<QString> kMedia = {
        QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
        QStringLiteral(".gif"), QStringLiteral(".svg"), QStringLiteral(".mp3"),
        QStringLiteral(".mp4"), QStringLiteral(".mov"), QStringLiteral(".wav"),
        QStringLiteral(".webp"), QStringLiteral(".ico")
    };
    return kMedia;
}

} // anonymous namespace

TypeAffinity::TypeAffinity(sqlite3* db)
    : m_db(db)
{
}

QString TypeAffinity::fileExtension(const QString& filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix.isEmpty()) {
        return {};
    }
    return QStringLiteral(".") + suffix;
}

bool TypeAffinity::extensionMatchesCategory(const QString& extension, const QString& category)
{
    if (category == QStringLiteral("code")) {
        return codeExtensions().contains(extension);
    }
    if (category == QStringLiteral("document")) {
        return documentExtensions().contains(extension);
    }
    if (category == QStringLiteral("media")) {
        return mediaExtensions().contains(extension);
    }
    return !codeExtensions().contains(extension)
        && !documentExtensions().contains(extension)
        && !mediaExtensions().contains(extension);
}

bool TypeAffinity::shouldRefreshCache() const
{
    if (!m_cacheValid || !m_lastRefresh.isValid()) {
        return true;
    }
    return m_lastRefresh.msecsTo(QDateTime::currentDateTimeUtc()) >= kRefreshIntervalMs;
}

void TypeAffinity::refreshCacheIfNeeded()
{
    if (!shouldRefreshCache()) {
        return;
    }

    m_cachedStats = {};

    if (!m_db) {
        m_lastRefresh = QDateTime::currentDateTimeUtc();
        m_cacheValid = true;
        return;
    }

    static constexpr const char* kSql = R"(
        SELECT path, COUNT(*)
        FROM interactions
        WHERE timestamp >= datetime('now', '-30 days')
        GROUP BY path
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastRefresh = QDateTime::currentDateTimeUtc();
        m_cacheValid = true;
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (rawPath == nullptr) {
            continue;
        }

        const int count = sqlite3_column_int(stmt, 1);
        if (count <= 0) {
            continue;
        }

        const QString extension = fileExtension(QString::fromUtf8(rawPath));
        if (codeExtensions().contains(extension)) {
            m_cachedStats.codeOpens += count;
        } else if (documentExtensions().contains(extension)) {
            m_cachedStats.documentOpens += count;
        } else if (mediaExtensions().contains(extension)) {
            m_cachedStats.mediaOpens += count;
        } else {
            m_cachedStats.otherOpens += count;
        }
    }
    sqlite3_finalize(stmt);

    int bestCount = m_cachedStats.otherOpens;
    m_cachedStats.primaryAffinity = QStringLiteral("other");

    if (m_cachedStats.codeOpens > bestCount) {
        bestCount = m_cachedStats.codeOpens;
        m_cachedStats.primaryAffinity = QStringLiteral("code");
    }
    if (m_cachedStats.documentOpens > bestCount) {
        bestCount = m_cachedStats.documentOpens;
        m_cachedStats.primaryAffinity = QStringLiteral("document");
    }
    if (m_cachedStats.mediaOpens > bestCount) {
        m_cachedStats.primaryAffinity = QStringLiteral("media");
    }

    m_lastRefresh = QDateTime::currentDateTimeUtc();
    m_cacheValid = true;
}

TypeAffinity::AffinityStats TypeAffinity::getAffinityStats()
{
    refreshCacheIfNeeded();
    return m_cachedStats;
}

double TypeAffinity::getBoost(const QString& filePath)
{
    refreshCacheIfNeeded();
    if (m_cachedStats.primaryAffinity.isEmpty()) {
        return 0.0;
    }

    const QString extension = fileExtension(filePath);
    if (extensionMatchesCategory(extension, m_cachedStats.primaryAffinity)) {
        return 5.0;
    }
    return 0.0;
}

void TypeAffinity::invalidateCache()
{
    m_cacheValid = false;
}

} // namespace bs
