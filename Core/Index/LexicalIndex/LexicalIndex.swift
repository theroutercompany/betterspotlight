import Foundation
import Shared
import SQLite

/// Full-text search index using SQLite FTS5
public actor LexicalIndex {
    private let db: Connection

    // FTS5 virtual table
    private let ftsTable = VirtualTable("items_fts")

    // Columns (FTS5 uses special syntax)
    private let rowid = SQLite.Expression<Int64>("rowid")
    private let filename = SQLite.Expression<String>("filename")
    private let pathTokens = SQLite.Expression<String>("path_tokens")
    private let contentText = SQLite.Expression<String>("content")

    public init(db: Connection) throws {
        self.db = db
        try createFTSTable()
    }

    private func createFTSTable() throws {
        // Create FTS5 table for full-text search
        try db.execute("""
            CREATE VIRTUAL TABLE IF NOT EXISTS items_fts USING fts5(
                filename,
                path_tokens,
                content,
                content='',
                tokenize='porter unicode61'
            )
        """)
    }

    /// Index an item's text content
    public func index(itemId: Int64, filename: String, pathTokens: String, content: String) throws {
        try db.run("""
            INSERT INTO items_fts(rowid, filename, path_tokens, content)
            VALUES (?, ?, ?, ?)
        """, itemId, filename, pathTokens, content)
    }

    /// Update an existing item's content
    public func update(itemId: Int64, filename: String, pathTokens: String, content: String) throws {
        // FTS5 requires delete then insert for updates
        try delete(itemId: itemId)
        try index(itemId: itemId, filename: filename, pathTokens: pathTokens, content: content)
    }

    /// Remove an item from the index
    public func delete(itemId: Int64) throws {
        try db.run("DELETE FROM items_fts WHERE rowid = ?", itemId)
    }

    /// Search for items matching a query
    public func search(query: String, limit: Int = 50) throws -> [LexicalSearchResult] {
        // Sanitize and prepare the query for FTS5
        let sanitizedQuery = sanitizeQuery(query)

        guard !sanitizedQuery.isEmpty else {
            return []
        }

        var results: [LexicalSearchResult] = []

        // Use FTS5 MATCH with BM25 ranking
        let stmt = try db.prepare("""
            SELECT rowid, filename, path_tokens,
                   snippet(items_fts, 2, '<mark>', '</mark>', '...', 32) as snippet,
                   bm25(items_fts, 10.0, 5.0, 1.0) as rank
            FROM items_fts
            WHERE items_fts MATCH ?
            ORDER BY rank
            LIMIT ?
        """, sanitizedQuery, limit)

        for row in stmt {
            guard let rowId = row[0] as? Int64,
                  let fname = row[1] as? String,
                  let snippet = row[3] as? String,
                  let rank = row[4] as? Double else {
                continue
            }

            let pathToks = (row[2] as? String) ?? ""

            // Determine match type based on which column matched
            let matchType = determineMatchType(query: query, filename: fname, pathTokens: pathToks)

            results.append(LexicalSearchResult(
                itemId: rowId,
                score: -rank, // BM25 returns negative scores, lower is better
                matchType: matchType,
                snippet: snippet,
                highlights: parseHighlights(from: snippet)
            ))
        }

        return results
    }

    /// Search only in filenames (fast path for prefix matching)
    public func searchFilenames(prefix: String, limit: Int = 50) throws -> [LexicalSearchResult] {
        let sanitizedPrefix = prefix.lowercased()
            .filter { $0.isLetter || $0.isNumber || $0 == " " || $0 == "_" || $0 == "-" || $0 == "." }

        guard !sanitizedPrefix.isEmpty else {
            return []
        }

        var results: [LexicalSearchResult] = []

        // Prefix search on filename column
        let stmt = try db.prepare("""
            SELECT rowid, filename, bm25(items_fts, 10.0, 1.0, 0.1) as rank
            FROM items_fts
            WHERE filename MATCH ?
            ORDER BY rank
            LIMIT ?
        """, "\(sanitizedPrefix)*", limit)

        for row in stmt {
            guard let rowId = row[0] as? Int64,
                  let fname = row[1] as? String,
                  let rank = row[2] as? Double else {
                continue
            }

            let matchType: MatchType = fname.lowercased().hasPrefix(sanitizedPrefix)
                ? .prefixName
                : .substringName

            results.append(LexicalSearchResult(
                itemId: rowId,
                score: -rank,
                matchType: matchType,
                snippet: nil,
                highlights: []
            ))
        }

        return results
    }

    /// Optimize the FTS index
    public func optimize() throws {
        try db.execute("INSERT INTO items_fts(items_fts) VALUES('optimize')")
    }

    /// Rebuild the FTS index
    public func rebuild() throws {
        try db.execute("INSERT INTO items_fts(items_fts) VALUES('rebuild')")
    }

    // MARK: - Private Helpers

    private func sanitizeQuery(_ query: String) -> String {
        // Remove special FTS5 characters that could cause syntax errors
        let dangerous = CharacterSet(charactersIn: "\"'*():^~")
        var sanitized = query.components(separatedBy: dangerous).joined(separator: " ")

        // Collapse multiple spaces
        while sanitized.contains("  ") {
            sanitized = sanitized.replacingOccurrences(of: "  ", with: " ")
        }

        sanitized = sanitized.trimmingCharacters(in: .whitespaces)

        // For single words, add prefix matching
        if !sanitized.contains(" ") && !sanitized.isEmpty {
            return "\(sanitized)*"
        }

        // For multiple words, wrap in quotes for phrase matching with OR fallback
        let words = sanitized.split(separator: " ").map(String.init)
        if words.count > 1 {
            // Try phrase match first, then individual words
            let phraseMatch = "\"\(sanitized)\""
            let wordMatches = words.map { "\($0)*" }.joined(separator: " OR ")
            return "(\(phraseMatch)) OR (\(wordMatches))"
        }

        return sanitized
    }

    private func determineMatchType(query: String, filename: String, pathTokens: String) -> MatchType {
        let queryLower = query.lowercased()
        let filenameLower = filename.lowercased()

        if filenameLower == queryLower {
            return .exactName
        } else if filenameLower.hasPrefix(queryLower) {
            return .prefixName
        } else if filenameLower.contains(queryLower) {
            return .substringName
        } else if pathTokens.lowercased().contains(queryLower) {
            return .pathToken
        } else {
            return .contentFuzzy
        }
    }

    private func parseHighlights(from snippet: String) -> [HighlightRange] {
        var highlights: [HighlightRange] = []
        var currentIndex = snippet.startIndex

        while let startRange = snippet.range(of: "<mark>", range: currentIndex..<snippet.endIndex) {
            let contentStart = startRange.upperBound
            if let endRange = snippet.range(of: "</mark>", range: contentStart..<snippet.endIndex) {
                let start = snippet.distance(from: snippet.startIndex, to: startRange.lowerBound)
                let end = snippet.distance(from: snippet.startIndex, to: endRange.lowerBound)
                highlights.append(HighlightRange(start: start, end: end))
                currentIndex = endRange.upperBound
            } else {
                break
            }
        }

        return highlights
    }
}

/// Result from lexical search
public struct LexicalSearchResult: Sendable {
    public let itemId: Int64
    public let score: Double
    public let matchType: MatchType
    public let snippet: String?
    public let highlights: [HighlightRange]

    public init(
        itemId: Int64,
        score: Double,
        matchType: MatchType,
        snippet: String?,
        highlights: [HighlightRange]
    ) {
        self.itemId = itemId
        self.score = score
        self.matchType = matchType
        self.snippet = snippet
        self.highlights = highlights
    }
}
