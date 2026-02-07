#pragma once

#include "core/ipc/service_base.h"
#include "core/index/sqlite_store.h"
#include "core/ranking/scorer.h"
#include "core/ranking/match_classifier.h"

#include <optional>

namespace bs {

class QueryService : public ServiceBase {
    Q_OBJECT
public:
    explicit QueryService(QObject* parent = nullptr);
    ~QueryService() override;

protected:
    QJsonObject handleRequest(const QJsonObject& request) override;

private:
    QJsonObject handleSearch(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetHealth(uint64_t id);
    QJsonObject handleRecordFeedback(uint64_t id, const QJsonObject& params);
    QJsonObject handleGetFrequency(uint64_t id, const QJsonObject& params);

    std::optional<SQLiteStore> m_store;
    Scorer m_scorer;

    // Opens the store if not already open. Returns true on success.
    bool ensureStoreOpen();
};

} // namespace bs
