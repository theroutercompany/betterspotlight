#pragma once

#include "core/shared/types.h"
#include <functional>
#include <string>
#include <vector>

namespace bs {

// FileMonitor — platform-agnostic interface for filesystem change detection.
//
// Implementations watch one or more root directories for file system events
// (creates, modifies, deletes, renames) and deliver batched WorkItems via
// the registered callback. The callback is invoked on an unspecified thread;
// callers must handle synchronisation.
class FileMonitor {
public:
    virtual ~FileMonitor() = default;

    // Non-copyable, non-movable (implementations own OS resources)
    FileMonitor(const FileMonitor&) = delete;
    FileMonitor& operator=(const FileMonitor&) = delete;
    FileMonitor(FileMonitor&&) = delete;
    FileMonitor& operator=(FileMonitor&&) = delete;

    // Callback type: receives a batch of WorkItems for changed paths.
    using ChangeCallback = std::function<void(const std::vector<WorkItem>&)>;

    // Start monitoring the given root directories.
    // Returns true on success. The callback will be invoked on a background
    // thread whenever changes are detected.  Calling start() while already
    // running is an error (returns false).
    virtual bool start(const std::vector<std::string>& roots,
                       ChangeCallback callback) = 0;

    // Stop monitoring. Blocks until any in-flight callback has completed.
    // Safe to call when not running (no-op).
    virtual void stop() = 0;

    // Returns true if the monitor is currently watching for events.
    virtual bool isRunning() const = 0;

protected:
    FileMonitor() = default;
};

} // namespace bs
