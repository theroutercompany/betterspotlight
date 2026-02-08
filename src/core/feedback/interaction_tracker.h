#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QString>
#include <cstdint>

struct sqlite3;

namespace bs {

class InteractionTracker {
public:
    struct Interaction {
        QString query;
        int64_t selectedItemId = 0;
        QString selectedPath;
        QString matchType;
        int resultPosition = 0;
        QString frontmostApp;
        QDateTime timestamp;
    };

    explicit InteractionTracker(sqlite3* db);

    bool recordInteraction(const Interaction& interaction);
    int getInteractionCount(const QString& query, int64_t itemId);
    int getInteractionBoost(const QString& query, int64_t itemId);
    bool cleanup(int retentionDays = 180);
    QJsonArray exportData();

    static QString normalizeQuery(const QString& query);

private:
    sqlite3* m_db = nullptr;
};

} // namespace bs
