import Foundation

/// Represents a search result with scoring information
public struct SearchResult: Identifiable, Sendable {
    public let id: Int64
    public let item: IndexItem
    public let score: Double
    public let matchType: MatchType
    public let highlights: [HighlightRange]
    public let snippet: String?

    public init(
        item: IndexItem,
        score: Double,
        matchType: MatchType,
        highlights: [HighlightRange] = [],
        snippet: String? = nil
    ) {
        self.id = item.id
        self.item = item
        self.score = score
        self.matchType = matchType
        self.highlights = highlights
        self.snippet = snippet
    }
}

/// Type of match that produced the result
public enum MatchType: String, Codable, Sendable {
    case exactName          // Exact filename match
    case prefixName         // Filename prefix match
    case substringName      // Filename substring match
    case pathToken          // Match in path components
    case contentExact       // Exact phrase in content
    case contentFuzzy       // Fuzzy content match
    case semantic           // Semantic/embedding match
}

/// A range within text that should be highlighted
public struct HighlightRange: Codable, Sendable {
    public let start: Int
    public let end: Int

    public init(start: Int, end: Int) {
        self.start = start
        self.end = end
    }
}

/// Query context for ranking adjustments
public struct QueryContext: Codable, Sendable {
    public let frontmostAppBundleId: String?
    public let frontmostAppName: String?
    public let currentWorkingDirectory: String?
    public let recentPaths: [String]

    public init(
        frontmostAppBundleId: String? = nil,
        frontmostAppName: String? = nil,
        currentWorkingDirectory: String? = nil,
        recentPaths: [String] = []
    ) {
        self.frontmostAppBundleId = frontmostAppBundleId
        self.frontmostAppName = frontmostAppName
        self.currentWorkingDirectory = currentWorkingDirectory
        self.recentPaths = recentPaths
    }
}

/// Search query with options
public struct SearchQuery: Sendable {
    public let text: String
    public let context: QueryContext
    public let options: SearchOptions

    public init(
        text: String,
        context: QueryContext = QueryContext(),
        options: SearchOptions = SearchOptions()
    ) {
        self.text = text
        self.context = context
        self.options = options
    }
}

/// Options for search behavior
public struct SearchOptions: Codable, Sendable {
    public let maxResults: Int
    public let includeContent: Bool
    public let includeSemantic: Bool
    public let fileTypes: Set<String>?
    public let excludePaths: [String]

    public init(
        maxResults: Int = 50,
        includeContent: Bool = true,
        includeSemantic: Bool = false,
        fileTypes: Set<String>? = nil,
        excludePaths: [String] = []
    ) {
        self.maxResults = maxResults
        self.includeContent = includeContent
        self.includeSemantic = includeSemantic
        self.fileTypes = fileTypes
        self.excludePaths = excludePaths
    }
}
