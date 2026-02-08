#pragma once

#include <QDateTime>

struct sqlite3;

namespace bs {

class FeedbackAggregator {
public:
    explicit FeedbackAggregator(sqlite3* db);

    bool runAggregation();
    bool cleanup(int feedbackRetentionDays = 90, int interactionRetentionDays = 180);
    QDateTime lastAggregationTime();

private:
    sqlite3* m_db = nullptr;
};

} // namespace bs
