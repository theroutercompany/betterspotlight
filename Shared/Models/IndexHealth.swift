import Foundation

/// Overall health status of the index
public enum IndexHealthStatus: String, Codable, Sendable {
    case healthy        // All systems normal
    case degraded       // Some issues but functional
    case unhealthy      // Significant problems
    case rebuilding     // Full rebuild in progress
}

/// Snapshot of index health for display
public struct IndexHealthSnapshot: Codable, Sendable {
    public let status: IndexHealthStatus
    public let totalItems: Int64
    public let totalContentChunks: Int64
    public let indexSizeBytes: Int64
    public let previewCacheSizeBytes: Int64
    public let queueLength: Int
    public let lastEventProcessed: Date?
    public let failuresByType: [String: Int]
    public let roots: [IndexRootStatus]
    public let recentErrors: [IndexFailure]

    public init(
        status: IndexHealthStatus,
        totalItems: Int64,
        totalContentChunks: Int64,
        indexSizeBytes: Int64,
        previewCacheSizeBytes: Int64,
        queueLength: Int,
        lastEventProcessed: Date?,
        failuresByType: [String: Int],
        roots: [IndexRootStatus],
        recentErrors: [IndexFailure]
    ) {
        self.status = status
        self.totalItems = totalItems
        self.totalContentChunks = totalContentChunks
        self.indexSizeBytes = indexSizeBytes
        self.previewCacheSizeBytes = previewCacheSizeBytes
        self.queueLength = queueLength
        self.lastEventProcessed = lastEventProcessed
        self.failuresByType = failuresByType
        self.roots = roots
        self.recentErrors = recentErrors
    }

    /// Human-readable index size
    public var formattedIndexSize: String {
        ByteCountFormatter.string(fromByteCount: indexSizeBytes, countStyle: .file)
    }

    /// Human-readable preview cache size
    public var formattedPreviewCacheSize: String {
        ByteCountFormatter.string(fromByteCount: previewCacheSizeBytes, countStyle: .file)
    }
}

/// Status of a single index root
public struct IndexRootStatus: Codable, Sendable, Identifiable {
    public var id: String { path }

    public let path: String
    public let classification: FolderClassification
    public let itemCount: Int64
    public let lastScanned: Date?
    public let pendingUpdates: Int
    public let errors: Int

    public init(
        path: String,
        classification: FolderClassification,
        itemCount: Int64,
        lastScanned: Date?,
        pendingUpdates: Int,
        errors: Int
    ) {
        self.path = path
        self.classification = classification
        self.itemCount = itemCount
        self.lastScanned = lastScanned
        self.pendingUpdates = pendingUpdates
        self.errors = errors
    }
}

/// Actions available for index management
public enum IndexManagementAction: Sendable {
    case reindexFolder(path: String)
    case rebuildAll
    case clearPreviewCache
    case clearFailures(path: String?)
    case pauseIndexing
    case resumeIndexing
}

/// Statistics for observability
public struct IndexingStats: Codable, Sendable {
    public var itemsIndexed: Int64
    public var itemsFailed: Int64
    public var bytesProcessed: Int64
    public var averageExtractionTimeMs: Double
    public var extractionTimesByType: [String: Double]
    public var queueLengthHistory: [QueueLengthSample]

    public init(
        itemsIndexed: Int64 = 0,
        itemsFailed: Int64 = 0,
        bytesProcessed: Int64 = 0,
        averageExtractionTimeMs: Double = 0,
        extractionTimesByType: [String: Double] = [:],
        queueLengthHistory: [QueueLengthSample] = []
    ) {
        self.itemsIndexed = itemsIndexed
        self.itemsFailed = itemsFailed
        self.bytesProcessed = bytesProcessed
        self.averageExtractionTimeMs = averageExtractionTimeMs
        self.extractionTimesByType = extractionTimesByType
        self.queueLengthHistory = queueLengthHistory
    }
}

/// Sample of queue length over time
public struct QueueLengthSample: Codable, Sendable {
    public let timestamp: Date
    public let length: Int

    public init(timestamp: Date, length: Int) {
        self.timestamp = timestamp
        self.length = length
    }
}
