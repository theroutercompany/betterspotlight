#include "core/indexing/path_state_actor.h"

namespace bs {

PathStateActor::PathStateActor(QObject* parent)
    : QObject(parent)
{
}

PathStateActor::~PathStateActor() = default;

std::optional<PathStateActor::DispatchTask> PathStateActor::onIngress(const WorkItem& item)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    PathState& state = m_paths[item.filePath];
    state.latestGeneration += 1;

    if (state.inPrep) {
        if (state.pendingMergedType.has_value()) {
            state.pendingMergedType =
                mergeWorkTypes(state.pendingMergedType.value(), item.type);
        } else {
            state.pendingMergedType = item.type;
        }
        state.pendingRebuildLane = state.pendingRebuildLane || item.rebuildLane;
        return std::nullopt;
    }

    state.inPrep = true;
    DispatchTask task;
    task.item = item;
    task.generation = state.latestGeneration;
    return task;
}

std::optional<PathStateActor::DispatchTask> PathStateActor::onPrepCompleted(const PreparedWork& prepared)
{
    const std::string path = prepared.path.toStdString();

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_paths.find(path);
    if (it == m_paths.end()) {
        return std::nullopt;
    }

    PathState& state = it->second;
    if (state.pendingMergedType.has_value()) {
        DispatchTask task;
        task.item.type = state.pendingMergedType.value();
        task.item.filePath = path;
        task.item.rebuildLane = state.pendingRebuildLane;
        task.generation = state.latestGeneration;

        state.pendingMergedType.reset();
        state.pendingRebuildLane = false;
        state.inPrep = true;
        return task;
    }

    state.inPrep = false;
    return std::nullopt;
}

bool PathStateActor::isStalePrepared(const PreparedWork& prepared) const
{
    const std::string path = prepared.path.toStdString();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_paths.find(path);
    if (it == m_paths.end()) {
        return false;
    }
    return prepared.generation < it->second.latestGeneration;
}

size_t PathStateActor::pendingMergedCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& [_, state] : m_paths) {
        if (state.pendingMergedType.has_value()) {
            ++count;
        }
    }
    return count;
}

void PathStateActor::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paths.clear();
}

WorkItem::Type PathStateActor::mergeWorkTypes(WorkItem::Type lhs, WorkItem::Type rhs)
{
    auto rank = [](WorkItem::Type t) -> int {
        switch (t) {
        case WorkItem::Type::Delete:
            return 0;
        case WorkItem::Type::ModifiedContent:
            return 1;
        case WorkItem::Type::NewFile:
            return 2;
        case WorkItem::Type::RescanDirectory:
            return 3;
        }
        return 3;
    };

    return (rank(lhs) <= rank(rhs)) ? lhs : rhs;
}

} // namespace bs
