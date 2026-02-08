#include "core/fs/file_monitor_macos.h"
#include "core/shared/logging.h"

#include <QFileInfo>
#include <sys/stat.h>

namespace bs {

FileMonitorMacOS::FileMonitorMacOS(double latencySeconds)
    : m_latency(latencySeconds)
{
}

FileMonitorMacOS::~FileMonitorMacOS()
{
    stop();
}

bool FileMonitorMacOS::start(const std::vector<std::string>& roots,
                             ChangeCallback callback)
{
    if (m_running.load()) {
        LOG_WARN(bsFs, "FileMonitorMacOS::start called while already running");
        return false;
    }

    if (roots.empty()) {
        LOG_ERROR(bsFs, "FileMonitorMacOS::start called with empty roots");
        return false;
    }

    if (!callback) {
        LOG_ERROR(bsFs, "FileMonitorMacOS::start called with null callback");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_callback = std::move(callback);
    m_roots = roots;

    // Build CFArray of paths to watch
    CFMutableArrayRef pathsToWatch = CFArrayCreateMutable(
        kCFAllocatorDefault,
        static_cast<CFIndex>(roots.size()),
        &kCFTypeArrayCallBacks);

    if (!pathsToWatch) {
        LOG_ERROR(bsFs, "Failed to allocate CFArray for watch paths");
        return false;
    }

    for (const auto& root : roots) {
        CFStringRef cfPath = CFStringCreateWithCString(
            kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
        if (cfPath) {
            CFArrayAppendValue(pathsToWatch, cfPath);
            CFRelease(cfPath);
        }
    }

    // Create the FSEvents context, passing 'this' as the info pointer.
    FSEventStreamContext context{};
    context.version = 0;
    context.info = this;
    context.retain = nullptr;
    context.release = nullptr;
    context.copyDescription = nullptr;

    constexpr FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagWatchRoot |
        kFSEventStreamCreateFlagFileEvents;

    m_stream = FSEventStreamCreate(
        kCFAllocatorDefault,
        &FileMonitorMacOS::fsEventsCallback,
        &context,
        pathsToWatch,
        m_lastEventId,
        m_latency,
        flags);

    CFRelease(pathsToWatch);

    if (!m_stream) {
        LOG_ERROR(bsFs, "FSEventStreamCreate failed");
        m_callback = nullptr;
        return false;
    }

    // Create a dedicated serial dispatch queue for event delivery.
    m_queue = dispatch_queue_create("com.betterspotlight.fsevents",
                                   DISPATCH_QUEUE_SERIAL);
    if (!m_queue) {
        LOG_ERROR(bsFs, "Failed to create dispatch queue for FSEvents");
        FSEventStreamInvalidate(m_stream);
        FSEventStreamRelease(m_stream);
        m_stream = nullptr;
        m_callback = nullptr;
        return false;
    }

    FSEventStreamSetDispatchQueue(m_stream, m_queue);

    if (!FSEventStreamStart(m_stream)) {
        LOG_ERROR(bsFs, "FSEventStreamStart failed");
        FSEventStreamInvalidate(m_stream);
        FSEventStreamRelease(m_stream);
        m_stream = nullptr;
        dispatch_release(m_queue);
        m_queue = nullptr;
        m_callback = nullptr;
        return false;
    }

    m_running.store(true);

    LOG_INFO(bsFs, "FileMonitorMacOS started watching %zu root(s), "
                   "latency=%.2fs", roots.size(), m_latency);
    for (const auto& root : roots) {
        LOG_DEBUG(bsFs, "  watching: %s", root.c_str());
    }

    return true;
}

void FileMonitorMacOS::stop()
{
    if (!m_running.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_stream) {
        FSEventStreamStop(m_stream);
        FSEventStreamInvalidate(m_stream);
        FSEventStreamRelease(m_stream);
        m_stream = nullptr;
    }

    if (m_queue) {
        dispatch_release(m_queue);
        m_queue = nullptr;
    }

    // Flush any remaining buffered events before clearing the callback.
    flushPendingEvents();

    m_callback = nullptr;
    m_running.store(false);

    LOG_INFO(bsFs, "FileMonitorMacOS stopped");
}

bool FileMonitorMacOS::isRunning() const
{
    return m_running.load();
}

void FileMonitorMacOS::fsEventsCallback(
    ConstFSEventStreamRef /*streamRef*/,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[])
{
    auto* self = static_cast<FileMonitorMacOS*>(clientCallBackInfo);
    auto** paths = static_cast<char**>(eventPaths);
    self->handleEvents(numEvents, paths, eventFlags, eventIds);
}

void FileMonitorMacOS::handleEvents(
    size_t numEvents,
    char** paths,
    const FSEventStreamEventFlags flags[],
    const FSEventStreamEventId eventIds[])
{
    std::vector<WorkItem> items;
    items.reserve(numEvents);

    for (size_t i = 0; i < numEvents; ++i) {
        const FSEventStreamEventFlags eventFlags = flags[i];

        if (eventFlags & kFSEventStreamEventFlagMustScanSubDirs) {
            if (m_errorCallback) {
                m_errorCallback(QStringLiteral("FSEvents: must rescan subdirs at ") +
                                QString::fromUtf8(paths[i]));
            }
        }
        if (eventFlags & kFSEventStreamEventFlagKernelDropped) {
            if (m_errorCallback) {
                m_errorCallback(QStringLiteral("FSEvents: kernel dropped events"));
            }
        }
        if (eventFlags & kFSEventStreamEventFlagUserDropped) {
            if (m_errorCallback) {
                m_errorCallback(QStringLiteral("FSEvents: user dropped events"));
            }
        }

        // Skip history-done and root-changed sentinel events.
        if (eventFlags & kFSEventStreamEventFlagHistoryDone) {
            continue;
        }

        // If the root itself changed (e.g., renamed/deleted), emit a rescan.
        if (eventFlags & kFSEventStreamEventFlagRootChanged) {
            LOG_WARN(bsFs, "Watched root changed: %s", paths[i]);
            WorkItem item;
            item.type = WorkItem::Type::RescanDirectory;
            item.filePath = paths[i];
            items.push_back(std::move(item));
            continue;
        }

        // Skip mount/unmount events.
        if (eventFlags & (kFSEventStreamEventFlagMount |
                          kFSEventStreamEventFlagUnmount)) {
            continue;
        }

        WorkItem item;
        item.type = classifyEvent(eventFlags);
        item.filePath = paths[i];

        // For non-delete events, try to stat the file for size/mtime.
        if (item.type != WorkItem::Type::Delete) {
            struct stat st{};
            if (stat(paths[i], &st) == 0) {
                item.knownModTime = static_cast<uint64_t>(st.st_mtime);
                item.knownSize = static_cast<uint64_t>(st.st_size);

                // If it's a directory, emit as RescanDirectory.
                if (S_ISDIR(st.st_mode)) {
                    item.type = WorkItem::Type::RescanDirectory;
                }
            } else {
                // stat failed — file was probably deleted between event
                // and callback. Treat as delete.
                item.type = WorkItem::Type::Delete;
            }
        }

        items.push_back(std::move(item));
    }

    // Track the latest event ID for persistence
    // (the caller stores this in SQLite settings for restart recovery)
    if (numEvents > 0) {
        m_lastEventId = eventIds[numEvents - 1];
    }

    if (items.empty()) {
        return;
    }

    // Buffer events and schedule a debounced delivery.
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_pendingEvents.insert(m_pendingEvents.end(),
                               std::make_move_iterator(items.begin()),
                               std::make_move_iterator(items.end()));

        if (!m_deliveryScheduled) {
            m_deliveryScheduled = true;

            dispatch_after(
                dispatch_time(DISPATCH_TIME_NOW,
                              static_cast<int64_t>(kDebounceMs) * NSEC_PER_MSEC),
                m_queue,
                ^{ flushPendingEvents(); });
        }
    }
}

void FileMonitorMacOS::flushPendingEvents()
{
    std::vector<WorkItem> batch;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        batch = std::move(m_pendingEvents);
        m_pendingEvents.clear();
        m_deliveryScheduled = false;
    }

    if (batch.empty()) {
        return;
    }

    ChangeCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callback;
    }

    if (cb) {
        cb(batch);
    }
}

WorkItem::Type FileMonitorMacOS::classifyEvent(FSEventStreamEventFlags flags)
{
    // Removal (file removed, renamed away, or directory removed).
    if (flags & (kFSEventStreamEventFlagItemRemoved |
                 kFSEventStreamEventFlagItemRenamed)) {
        // For renames, the old path appears as Renamed. We treat it as a
        // delete; the new path will appear as a separate Created event.
        // However, if Created is also set, it's the new-name side of the
        // rename, so treat as NewFile.
        if (flags & kFSEventStreamEventFlagItemCreated) {
            return WorkItem::Type::NewFile;
        }
        // If the item still exists (stat succeeds), it's the destination
        // of a rename. The caller (handleEvents) will stat and correct.
        return WorkItem::Type::Delete;
    }

    // Creation.
    if (flags & kFSEventStreamEventFlagItemCreated) {
        return WorkItem::Type::NewFile;
    }

    // Modification (content change, xattr change, metadata change).
    if (flags & (kFSEventStreamEventFlagItemModified |
                 kFSEventStreamEventFlagItemInodeMetaMod |
                 kFSEventStreamEventFlagItemXattrMod |
                 kFSEventStreamEventFlagItemFinderInfoMod |
                 kFSEventStreamEventFlagItemChangeOwner)) {
        return WorkItem::Type::ModifiedContent;
    }

    // Fallback — treat unknown flags as modification.
    return WorkItem::Type::ModifiedContent;
}

} // namespace bs
