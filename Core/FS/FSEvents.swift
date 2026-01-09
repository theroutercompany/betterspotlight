import Foundation
import Shared

/// Delegate for receiving file system events
public protocol FSEventsWatcherDelegate: AnyObject {
    func watcher(_ watcher: FSEventsWatcher, didReceiveEvents events: [FSEvent])
    func watcher(_ watcher: FSEventsWatcher, didFailWithError error: Error)
}

/// Represents a single file system event
public struct FSEvent: Sendable {
    public let path: String
    public let flags: FSEventStreamEventFlags
    public let eventId: FSEventStreamEventId

    public init(path: String, flags: FSEventStreamEventFlags, eventId: FSEventStreamEventId) {
        self.path = path
        self.flags = flags
        self.eventId = eventId
    }

    /// Whether the item was created
    public var isCreated: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemCreated) != 0
    }

    /// Whether the item was removed
    public var isRemoved: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemRemoved) != 0
    }

    /// Whether the item was renamed
    public var isRenamed: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemRenamed) != 0
    }

    /// Whether the item was modified
    public var isModified: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemModified) != 0
    }

    /// Whether metadata changed (permissions, owner, etc.)
    public var isMetadataModified: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemInodeMetaMod) != 0
    }

    /// Whether this is a directory
    public var isDirectory: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemIsDir) != 0
    }

    /// Whether this is a file
    public var isFile: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemIsFile) != 0
    }

    /// Whether this is a symlink
    public var isSymlink: Bool {
        flags & UInt32(kFSEventStreamEventFlagItemIsSymlink) != 0
    }

    /// Whether history was lost and we need to rescan
    public var mustRescanDirectory: Bool {
        flags & UInt32(kFSEventStreamEventFlagMustScanSubDirs) != 0
    }
}

/// Watches file system paths for changes using FSEvents
public final class FSEventsWatcher: @unchecked Sendable {
    private var stream: FSEventStreamRef?
    private let queue: DispatchQueue
    private let paths: [String]
    private let latency: TimeInterval
    public weak var delegate: FSEventsWatcherDelegate?

    /// Initialize a watcher for the given paths
    /// - Parameters:
    ///   - paths: Paths to watch
    ///   - latency: Coalescing latency in seconds (default 0.1)
    public init(paths: [String], latency: TimeInterval = 0.1) {
        self.paths = paths
        self.latency = latency
        self.queue = DispatchQueue(label: "com.betterspotlight.fsevents", qos: .utility)
    }

    deinit {
        stop()
    }

    /// Start watching for events
    public func start() throws {
        guard stream == nil else { return }

        var context = FSEventStreamContext(
            version: 0,
            info: Unmanaged.passUnretained(self).toOpaque(),
            retain: nil,
            release: nil,
            copyDescription: nil
        )

        let pathsToWatch = paths as CFArray

        guard let stream = FSEventStreamCreate(
            nil,
            { (streamRef, clientCallBackInfo, numEvents, eventPaths, eventFlags, eventIds) in
                guard let info = clientCallBackInfo else { return }
                let watcher = Unmanaged<FSEventsWatcher>.fromOpaque(info).takeUnretainedValue()
                watcher.handleEvents(
                    numEvents: numEvents,
                    eventPaths: eventPaths,
                    eventFlags: eventFlags,
                    eventIds: eventIds
                )
            },
            &context,
            pathsToWatch,
            FSEventStreamEventId(kFSEventStreamEventIdSinceNow),
            latency,
            FSEventStreamCreateFlags(
                kFSEventStreamCreateFlagFileEvents |
                kFSEventStreamCreateFlagUseCFTypes |
                kFSEventStreamCreateFlagNoDefer
            )
        ) else {
            throw FSEventsError.failedToCreateStream
        }

        self.stream = stream
        FSEventStreamSetDispatchQueue(stream, queue)

        guard FSEventStreamStart(stream) else {
            FSEventStreamInvalidate(stream)
            FSEventStreamRelease(stream)
            self.stream = nil
            throw FSEventsError.failedToStartStream
        }
    }

    /// Stop watching for events
    public func stop() {
        guard let stream = stream else { return }
        FSEventStreamStop(stream)
        FSEventStreamInvalidate(stream)
        FSEventStreamRelease(stream)
        self.stream = nil
    }

    /// Flush pending events synchronously
    public func flush() {
        guard let stream = stream else { return }
        FSEventStreamFlushSync(stream)
    }

    private func handleEvents(
        numEvents: Int,
        eventPaths: UnsafeMutableRawPointer,
        eventFlags: UnsafePointer<FSEventStreamEventFlags>,
        eventIds: UnsafePointer<FSEventStreamEventId>
    ) {
        guard let pathArray = unsafeBitCast(eventPaths, to: NSArray.self) as? [String] else {
            return
        }

        var events: [FSEvent] = []
        events.reserveCapacity(numEvents)

        for i in 0..<numEvents {
            let event = FSEvent(
                path: pathArray[i],
                flags: eventFlags[i],
                eventId: eventIds[i]
            )
            events.append(event)
        }

        delegate?.watcher(self, didReceiveEvents: events)
    }
}

/// Errors from FSEvents operations
public enum FSEventsError: Error, LocalizedError {
    case failedToCreateStream
    case failedToStartStream

    public var errorDescription: String? {
        switch self {
        case .failedToCreateStream:
            return "Failed to create FSEvents stream"
        case .failedToStartStream:
            return "Failed to start FSEvents stream"
        }
    }
}
