#pragma once

#include "core/indexing/indexer.h"

#include <QObject>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace bs {

class PathStateActor : public QObject {
    Q_OBJECT
public:
    struct DispatchTask {
        WorkItem item;
        uint64_t generation = 0;
    };

    explicit PathStateActor(QObject* parent = nullptr);
    ~PathStateActor() override;

    std::optional<DispatchTask> onIngress(const WorkItem& item);
    std::optional<DispatchTask> onPrepCompleted(const PreparedWork& prepared);
    bool isStalePrepared(const PreparedWork& prepared) const;
    size_t pendingMergedCount() const;
    void reset();

private:
    struct PathState {
        uint64_t latestGeneration = 0;
        bool inPrep = false;
        std::optional<WorkItem::Type> pendingMergedType;
        bool pendingRebuildLane = false;
    };

    static WorkItem::Type mergeWorkTypes(WorkItem::Type lhs, WorkItem::Type rhs);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, PathState> m_paths;
};

} // namespace bs
