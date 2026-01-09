import Foundation

/// Protocol for the Indexer XPC Service
@objc public protocol IndexerServiceProtocol {
    /// Start indexing configured roots
    func startIndexing(reply: @escaping (Bool, Error?) -> Void)

    /// Stop indexing
    func stopIndexing(reply: @escaping (Bool, Error?) -> Void)

    /// Pause indexing temporarily
    func pauseIndexing(reply: @escaping (Bool, Error?) -> Void)

    /// Resume paused indexing
    func resumeIndexing(reply: @escaping (Bool, Error?) -> Void)

    /// Reindex a specific folder
    func reindexFolder(at path: String, reply: @escaping (Bool, Error?) -> Void)

    /// Rebuild the entire index
    func rebuildIndex(reply: @escaping (Bool, Error?) -> Void)

    /// Get current queue length
    func getQueueLength(reply: @escaping (Int, Error?) -> Void)

    /// Get indexing statistics as JSON data
    func getStatistics(reply: @escaping (Data?, Error?) -> Void)
}

/// Protocol for the Extractor XPC Service
@objc public protocol ExtractorServiceProtocol {
    /// Extract text content from a file
    func extractText(
        from path: String,
        reply: @escaping (Data?, Error?) -> Void
    )

    /// Extract metadata from a file
    func extractMetadata(
        from path: String,
        reply: @escaping (Data?, Error?) -> Void
    )

    /// Perform OCR on an image file
    func performOCR(
        on path: String,
        reply: @escaping (String?, Error?) -> Void
    )

    /// Check if a file type is supported for extraction
    func isSupported(
        fileExtension: String,
        reply: @escaping (Bool) -> Void
    )
}

/// Protocol for the Query XPC Service
@objc public protocol QueryServiceProtocol {
    /// Execute a search query
    func search(
        queryData: Data,
        reply: @escaping (Data?, Error?) -> Void
    )

    /// Get item by ID
    func getItem(
        id: Int64,
        reply: @escaping (Data?, Error?) -> Void
    )

    /// Get items by path prefix
    func getItems(
        underPath: String,
        limit: Int,
        reply: @escaping (Data?, Error?) -> Void
    )

    /// Record feedback for a search result
    func recordFeedback(
        feedbackData: Data,
        reply: @escaping (Bool, Error?) -> Void
    )

    /// Get index health snapshot
    func getIndexHealth(
        reply: @escaping (Data?, Error?) -> Void
    )
}

// MARK: - XPC Connection Helpers

/// Service identifiers for XPC connections
public enum XPCServiceIdentifier: String {
    case indexer = "com.betterspotlight.indexer"
    case extractor = "com.betterspotlight.extractor"
    case query = "com.betterspotlight.query"
}

/// Error types for XPC communication
public enum XPCError: Error, LocalizedError {
    case connectionFailed
    case serviceUnavailable
    case encodingFailed
    case decodingFailed
    case operationFailed(String)

    public var errorDescription: String? {
        switch self {
        case .connectionFailed:
            return "Failed to establish XPC connection"
        case .serviceUnavailable:
            return "XPC service is not available"
        case .encodingFailed:
            return "Failed to encode data for XPC"
        case .decodingFailed:
            return "Failed to decode data from XPC"
        case .operationFailed(let message):
            return "Operation failed: \(message)"
        }
    }
}
