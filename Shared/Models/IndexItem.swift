import Foundation

/// Represents the kind of indexed item
public enum ItemKind: String, Codable, Sendable {
    case file
    case folder
    case application
    case bundle
    case symlink
}

/// Classification for how a folder should be treated during indexing
public enum FolderClassification: String, Codable, Sendable {
    case index          // Full content indexing
    case metadataOnly   // Only filename, path, dates
    case exclude        // Skip entirely
}

/// Sensitivity level for content handling
public enum SensitivityLevel: String, Codable, Sendable {
    case normal         // Full indexing, previews, embeddings
    case sensitive      // Searchable, masked previews, no embeddings
    case hidden         // Metadata only, no content access
}

/// Source of a tag assignment
public enum TagSource: String, Codable, Sendable {
    case rule           // Assigned by path rules
    case user           // User-assigned
    case inferred       // ML-inferred (future)
}

/// Represents an indexed item in the database
public struct IndexItem: Identifiable, Codable, Sendable, Hashable {
    public let id: Int64
    public let path: String
    public let kind: ItemKind
    public let size: Int64
    public let modificationDate: Date
    public let creationDate: Date
    public let owner: String?
    public let flags: UInt32
    public let contentHash: String?
    public let sensitivity: SensitivityLevel

    public init(
        id: Int64,
        path: String,
        kind: ItemKind,
        size: Int64,
        modificationDate: Date,
        creationDate: Date,
        owner: String? = nil,
        flags: UInt32 = 0,
        contentHash: String? = nil,
        sensitivity: SensitivityLevel = .normal
    ) {
        self.id = id
        self.path = path
        self.kind = kind
        self.size = size
        self.modificationDate = modificationDate
        self.creationDate = creationDate
        self.owner = owner
        self.flags = flags
        self.contentHash = contentHash
        self.sensitivity = sensitivity
    }

    /// Returns the filename component of the path
    public var filename: String {
        (path as NSString).lastPathComponent
    }

    /// Returns the parent directory path
    public var parentPath: String {
        (path as NSString).deletingLastPathComponent
    }

    /// Returns the file extension (lowercase)
    public var fileExtension: String {
        (path as NSString).pathExtension.lowercased()
    }
}

/// Represents a chunk of content from an indexed item
public struct ContentChunk: Identifiable, Codable, Sendable {
    public let id: Int64
    public let itemId: Int64
    public let chunkIndex: Int
    public let textHash: String
    public let snippet: String
    public let startOffset: Int
    public let endOffset: Int

    public init(
        id: Int64,
        itemId: Int64,
        chunkIndex: Int,
        textHash: String,
        snippet: String,
        startOffset: Int,
        endOffset: Int
    ) {
        self.id = id
        self.itemId = itemId
        self.chunkIndex = chunkIndex
        self.textHash = textHash
        self.snippet = snippet
        self.startOffset = startOffset
        self.endOffset = endOffset
    }
}

/// Represents a tag applied to an item
public struct ItemTag: Codable, Sendable {
    public let itemId: Int64
    public let tag: String
    public let source: TagSource
    public let confidence: Float

    public init(itemId: Int64, tag: String, source: TagSource, confidence: Float = 1.0) {
        self.itemId = itemId
        self.tag = tag
        self.source = source
        self.confidence = confidence
    }
}

/// Represents an extraction or indexing failure
public struct IndexFailure: Codable, Sendable {
    public let path: String
    public let itemId: Int64?
    public let stage: String
    public let error: String
    public let lastSeen: Date
    public let failureCount: Int

    public init(
        path: String,
        itemId: Int64? = nil,
        stage: String,
        error: String,
        lastSeen: Date = Date(),
        failureCount: Int = 1
    ) {
        self.path = path
        self.itemId = itemId
        self.stage = stage
        self.error = error
        self.lastSeen = lastSeen
        self.failureCount = failureCount
    }
}
