// DEPRECATED SWIFT REFERENCE
// Qt/C++ is the source of truth.
// Keep this file only as temporary migration reference while parity items are closed.
// Do not add new features or fixes here.

import Foundation

/// User action taken on a search result
public enum FeedbackAction: String, Codable, Sendable {
    case open           // Opened the file/app
    case reveal         // Revealed in Finder
    case copyPath       // Copied absolute path
    case copyRelative   // Copied relative path
    case openWith       // Opened with specific app
    case dismiss        // Dismissed without action
}

/// Recorded feedback entry for learning
public struct FeedbackEntry: Codable, Sendable, Identifiable {
    public let id: UUID
    public let timestamp: Date
    public let query: String
    public let itemId: Int64
    public let itemPath: String
    public let action: FeedbackAction
    public let resultPosition: Int
    public let totalResults: Int
    public let context: FeedbackContext?

    public init(
        id: UUID = UUID(),
        timestamp: Date = Date(),
        query: String,
        itemId: Int64,
        itemPath: String,
        action: FeedbackAction,
        resultPosition: Int,
        totalResults: Int,
        context: FeedbackContext? = nil
    ) {
        self.id = id
        self.timestamp = timestamp
        self.query = query
        self.itemId = itemId
        self.itemPath = itemPath
        self.action = action
        self.resultPosition = resultPosition
        self.totalResults = totalResults
        self.context = context
    }
}

/// Optional context for feedback
public struct FeedbackContext: Codable, Sendable {
    public let frontmostApp: String?
    public let workingDirectory: String?

    public init(frontmostApp: String? = nil, workingDirectory: String? = nil) {
        self.frontmostApp = frontmostApp
        self.workingDirectory = workingDirectory
    }
}

/// Aggregated frequency data for ranking
public struct ItemFrequency: Codable, Sendable {
    public let itemId: Int64
    public let path: String
    public var openCount: Int
    public var lastOpened: Date
    public var isPinned: Bool

    public init(
        itemId: Int64,
        path: String,
        openCount: Int = 0,
        lastOpened: Date = Date(),
        isPinned: Bool = false
    ) {
        self.itemId = itemId
        self.path = path
        self.openCount = openCount
        self.lastOpened = lastOpened
        self.isPinned = isPinned
    }
}
