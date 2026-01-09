import Foundation
import SQLite

/// XPC Service for handling search queries
public final class QueryService: NSObject, QueryServiceProtocol {
    private let store: SQLiteStore
    private let lexicalIndex: LexicalIndex
    private let vectorIndex: VectorIndex?
    private let scorer: ResultScorer
    private let contextProvider: ContextSignalProvider

    private let settings: AppSettings

    public init(settings: AppSettings) throws {
        self.settings = settings
        self.store = try SQLiteStore()

        // Initialize lexical index with same database connection
        // Note: In production, you'd want to share the connection properly
        let db = try Connection(
            FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
                .appendingPathComponent("BetterSpotlight/index.sqlite").path
        )
        self.lexicalIndex = try LexicalIndex(db: db)

        // Initialize vector index if semantic search is enabled
        if settings.indexing.enableSemanticIndex {
            self.vectorIndex = VectorIndex()
        } else {
            self.vectorIndex = nil
        }

        self.scorer = ResultScorer(settings: settings.search)
        self.contextProvider = ContextSignalProvider()

        super.init()
    }

    // MARK: - QueryServiceProtocol

    public func search(queryData: Data, reply: @escaping (Data?, Error?) -> Void) {
        Task {
            do {
                let query = try JSONDecoder().decode(SearchQuery.self, from: queryData)
                let results = try await performSearch(query)
                let data = try JSONEncoder().encode(results)
                reply(data, nil)
            } catch {
                reply(nil, error)
            }
        }
    }

    public func getItem(id: Int64, reply: @escaping (Data?, Error?) -> Void) {
        Task {
            do {
                if let item = try await store.getItem(id: id) {
                    let data = try JSONEncoder().encode(item)
                    reply(data, nil)
                } else {
                    reply(nil, nil)
                }
            } catch {
                reply(nil, error)
            }
        }
    }

    public func getItems(underPath: String, limit: Int, reply: @escaping (Data?, Error?) -> Void) {
        // This would need a new method in SQLiteStore
        // For now, return empty
        reply(try? JSONEncoder().encode([IndexItem]()), nil)
    }

    public func recordFeedback(feedbackData: Data, reply: @escaping (Bool, Error?) -> Void) {
        Task {
            do {
                let feedback = try JSONDecoder().decode(FeedbackEntry.self, from: feedbackData)
                try await store.recordFeedback(feedback)

                // Update frequency tracking
                if feedback.action == .open {
                    try await store.incrementOpenCount(forItemId: feedback.itemId, itemPath: feedback.itemPath)
                }

                // Record path access for context
                await contextProvider.recordPathAccess(feedback.itemPath)

                reply(true, nil)
            } catch {
                reply(false, error)
            }
        }
    }

    public func getIndexHealth(reply: @escaping (Data?, Error?) -> Void) {
        Task {
            do {
                let health = try await buildHealthSnapshot()
                let data = try JSONEncoder().encode(health)
                reply(data, nil)
            } catch {
                reply(nil, error)
            }
        }
    }

    // MARK: - Private Methods

    private func performSearch(_ query: SearchQuery) async throws -> [SearchResult] {
        let searchText = query.text.trimmingCharacters(in: .whitespacesAndNewlines)

        guard !searchText.isEmpty else {
            return []
        }

        // Get current context
        let context = await contextProvider.getCurrentContext()

        // Collect candidates from different sources
        var candidates: [(item: IndexItem, matchType: MatchType, baseScore: Double)] = []

        // 1. Filename/path search (always on)
        let lexicalResults = try await lexicalIndex.search(query: searchText, limit: query.options.maxResults * 2)

        for result in lexicalResults {
            if let item = try await store.getItem(id: result.itemId) {
                candidates.append((item, result.matchType, result.score))
            }
        }

        // 2. Semantic search (if enabled)
        if query.options.includeSemantic, let vectorIndex = vectorIndex {
            // Would need to embed the query text first
            // For now, skip semantic search
        }

        // 3. Get frequency data for candidates
        var frequencies: [Int64: ItemFrequency] = [:]
        for (item, _, _) in candidates {
            if let freq = try await store.getFrequency(forItemId: item.id) {
                frequencies[item.id] = freq
            }
        }

        // 4. Score and rank
        let scored = scorer.rank(
            results: candidates,
            context: context,
            frequencies: frequencies
        )

        // 5. Build final results
        var results: [SearchResult] = []

        for (index, scored) in scored.prefix(query.options.maxResults).enumerated() {
            // Get snippet if this was a content match
            var snippet: String? = nil
            if scored.matchType == .contentExact || scored.matchType == .contentFuzzy {
                let chunks = try await store.getContent(forItemId: scored.item.id)
                snippet = chunks.first?.snippet
            }

            results.append(SearchResult(
                item: scored.item,
                score: scored.score,
                matchType: scored.matchType,
                highlights: [], // Would come from lexical index
                snippet: snippet
            ))
        }

        return results
    }

    private func buildHealthSnapshot() async throws -> IndexHealthSnapshot {
        let totalItems = try await store.getTotalItemCount()
        let totalChunks = try await store.getTotalContentChunkCount()
        let dbSize = try await store.getDatabaseSize()
        let failuresByType = try await store.getFailureCountByType()
        let recentErrors = try await store.getRecentFailures(limit: 10)

        // Build root status
        var roots: [IndexRootStatus] = []
        for root in settings.indexRoots {
            roots.append(IndexRootStatus(
                path: root.path,
                classification: root.classification,
                itemCount: 0, // Would need to count items under path
                lastScanned: nil,
                pendingUpdates: 0,
                errors: failuresByType.values.reduce(0, +)
            ))
        }

        // Determine overall status
        let totalFailures = failuresByType.values.reduce(0, +)
        let status: IndexHealthStatus
        if totalFailures == 0 {
            status = .healthy
        } else if totalFailures < 100 {
            status = .degraded
        } else {
            status = .unhealthy
        }

        return IndexHealthSnapshot(
            status: status,
            totalItems: totalItems,
            totalContentChunks: totalChunks,
            indexSizeBytes: dbSize,
            previewCacheSizeBytes: 0, // Would need to calculate
            queueLength: 0, // Would get from indexer service
            lastEventProcessed: nil,
            failuresByType: failuresByType,
            roots: roots,
            recentErrors: recentErrors
        )
    }
}
