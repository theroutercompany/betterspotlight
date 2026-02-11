// DEPRECATED SWIFT REFERENCE
// Qt/C++ is the source of truth.
// Keep this file only as temporary migration reference while parity items are closed.
// Do not add new features or fixes here.

import Foundation
import SQLite

/// SQLite-based storage for the search index
public actor SQLiteStore {
    private let db: Connection
    private let dbPath: String

    // MARK: - Tables

    private let items = Table("items")
    private let content = Table("content")
    private let tags = Table("tags")
    private let failures = Table("failures")
    private let settings = Table("settings")
    private let feedback = Table("feedback")
    private let frequencies = Table("frequencies")

    // MARK: - Items Columns

    private let id = SQLite.Expression<Int64>("id")
    private let path = SQLite.Expression<String>("path")
    private let kind = SQLite.Expression<String>("kind")
    private let size = SQLite.Expression<Int64>("size")
    private let mtime = SQLite.Expression<Double>("mtime")
    private let ctime = SQLite.Expression<Double>("ctime")
    private let owner = SQLite.Expression<String?>("owner")
    private let flags = SQLite.Expression<Int64>("flags")
    private let contentHash = SQLite.Expression<String?>("content_hash")
    private let sensitivity = SQLite.Expression<String>("sensitivity")

    // MARK: - Content Columns

    private let itemId = SQLite.Expression<Int64>("item_id")
    private let chunkIndex = SQLite.Expression<Int>("chunk_index")
    private let textHash = SQLite.Expression<String>("text_hash")
    private let snippet = SQLite.Expression<String>("snippet")
    private let startOffset = SQLite.Expression<Int>("start_offset")
    private let endOffset = SQLite.Expression<Int>("end_offset")

    // MARK: - Tags Columns

    private let tag = SQLite.Expression<String>("tag")
    private let source = SQLite.Expression<String>("source")
    private let confidence = SQLite.Expression<Double>("confidence")

    // MARK: - Failures Columns

    private let failureItemId = SQLite.Expression<Int64?>("item_id")
    private let stage = SQLite.Expression<String>("stage")
    private let error = SQLite.Expression<String>("error")
    private let lastSeen = SQLite.Expression<Double>("last_seen")
    private let failureCount = SQLite.Expression<Int>("failure_count")

    // MARK: - Feedback Columns

    private let feedbackId = SQLite.Expression<String>("feedback_id")
    private let timestamp = SQLite.Expression<Double>("timestamp")
    private let query = SQLite.Expression<String>("query")
    private let action = SQLite.Expression<String>("action")
    private let resultPosition = SQLite.Expression<Int>("result_position")
    private let totalResults = SQLite.Expression<Int>("total_results")
    private let contextJson = SQLite.Expression<String?>("context_json")

    // MARK: - Frequencies Columns

    private let openCount = SQLite.Expression<Int>("open_count")
    private let lastOpened = SQLite.Expression<Double>("last_opened")
    private let isPinned = SQLite.Expression<Bool>("is_pinned")

    // MARK: - Settings Columns

    private let key = SQLite.Expression<String>("key")
    private let value = SQLite.Expression<String>("value")

    // MARK: - Initialization

    public init(path: String? = nil) throws {
        if let path = path {
            self.dbPath = path
        } else {
            let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            let appFolder = appSupport.appendingPathComponent("BetterSpotlight")
            try FileManager.default.createDirectory(at: appFolder, withIntermediateDirectories: true)
            self.dbPath = appFolder.appendingPathComponent("index.sqlite").path
        }

        self.db = try Connection(dbPath)
        try createTables()
    }

    private func createTables() throws {
        // Items table
        try db.run(items.create(ifNotExists: true) { t in
            t.column(id, primaryKey: .autoincrement)
            t.column(path, unique: true)
            t.column(kind)
            t.column(size)
            t.column(mtime)
            t.column(ctime)
            t.column(owner)
            t.column(flags)
            t.column(contentHash)
            t.column(sensitivity, defaultValue: "normal")
        })
        try db.run(items.createIndex(path, ifNotExists: true))
        try db.run(items.createIndex(kind, ifNotExists: true))
        try db.run(items.createIndex(mtime, ifNotExists: true))

        // Content table
        try db.run(content.create(ifNotExists: true) { t in
            t.column(id, primaryKey: .autoincrement)
            t.column(itemId)
            t.column(chunkIndex)
            t.column(textHash)
            t.column(snippet)
            t.column(startOffset)
            t.column(endOffset)
            t.foreignKey(itemId, references: items, id, delete: .cascade)
        })
        try db.run(content.createIndex(itemId, ifNotExists: true))

        // Tags table
        try db.run(tags.create(ifNotExists: true) { t in
            t.column(itemId)
            t.column(tag)
            t.column(source)
            t.column(confidence)
            t.primaryKey(itemId, tag)
            t.foreignKey(itemId, references: items, id, delete: .cascade)
        })

        // Failures table
        try db.run(failures.create(ifNotExists: true) { t in
            t.column(id, primaryKey: .autoincrement)
            t.column(path)
            t.column(itemId)
            t.column(stage)
            t.column(error)
            t.column(lastSeen)
            t.column(failureCount, defaultValue: 1)
        })
        try db.run(failures.createIndex(path, ifNotExists: true))

        // Settings table
        try db.run(settings.create(ifNotExists: true) { t in
            t.column(key, primaryKey: true)
            t.column(value)
        })

        // Feedback table
        try db.run(feedback.create(ifNotExists: true) { t in
            t.column(feedbackId, primaryKey: true)
            t.column(timestamp)
            t.column(query)
            t.column(itemId)
            t.column(path)
            t.column(action)
            t.column(resultPosition)
            t.column(totalResults)
            t.column(contextJson)
        })
        try db.run(feedback.createIndex(timestamp, ifNotExists: true))
        try db.run(feedback.createIndex(query, ifNotExists: true))

        // Frequencies table
        try db.run(frequencies.create(ifNotExists: true) { t in
            t.column(itemId, primaryKey: true)
            t.column(path)
            t.column(openCount, defaultValue: 0)
            t.column(lastOpened)
            t.column(isPinned, defaultValue: false)
            t.foreignKey(itemId, references: items, id, delete: .cascade)
        })

        // Enable foreign keys
        try db.execute("PRAGMA foreign_keys = ON")

        // Performance settings
        try db.execute("PRAGMA journal_mode = WAL")
        try db.execute("PRAGMA synchronous = NORMAL")
    }

    // MARK: - Items CRUD

    public func insertItem(_ item: IndexItem) throws -> Int64 {
        let insert = items.insert(
            path <- item.path,
            kind <- item.kind.rawValue,
            size <- item.size,
            mtime <- item.modificationDate.timeIntervalSince1970,
            ctime <- item.creationDate.timeIntervalSince1970,
            owner <- item.owner,
            flags <- Int64(item.flags),
            contentHash <- item.contentHash,
            sensitivity <- item.sensitivity.rawValue
        )
        return try db.run(insert)
    }

    public func updateItem(_ item: IndexItem) throws {
        let row = items.filter(id == item.id)
        try db.run(row.update(
            path <- item.path,
            kind <- item.kind.rawValue,
            size <- item.size,
            mtime <- item.modificationDate.timeIntervalSince1970,
            owner <- item.owner,
            flags <- Int64(item.flags),
            contentHash <- item.contentHash,
            sensitivity <- item.sensitivity.rawValue
        ))
    }

    public func deleteItem(id itemId: Int64) throws {
        let row = items.filter(id == itemId)
        try db.run(row.delete())
    }

    public func deleteItem(path itemPath: String) throws {
        let row = items.filter(path == itemPath)
        try db.run(row.delete())
    }

    public func getItem(id itemId: Int64) throws -> IndexItem? {
        guard let row = try db.pluck(items.filter(id == itemId)) else {
            return nil
        }
        return itemFromRow(row)
    }

    public func getItem(path itemPath: String) throws -> IndexItem? {
        guard let row = try db.pluck(items.filter(path == itemPath)) else {
            return nil
        }
        return itemFromRow(row)
    }

    public func itemExists(path itemPath: String) throws -> Bool {
        let count = try db.scalar(items.filter(path == itemPath).count)
        return count > 0
    }

    private func itemFromRow(_ row: Row) -> IndexItem {
        IndexItem(
            id: row[id],
            path: row[path],
            kind: ItemKind(rawValue: row[kind]) ?? .file,
            size: row[size],
            modificationDate: Date(timeIntervalSince1970: row[mtime]),
            creationDate: Date(timeIntervalSince1970: row[ctime]),
            owner: row[owner],
            flags: UInt32(row[flags]),
            contentHash: row[contentHash],
            sensitivity: SensitivityLevel(rawValue: row[sensitivity]) ?? .normal
        )
    }

    // MARK: - Content CRUD

    public func insertContent(_ chunk: ContentChunk) throws -> Int64 {
        let insert = content.insert(
            itemId <- chunk.itemId,
            chunkIndex <- chunk.chunkIndex,
            textHash <- chunk.textHash,
            snippet <- chunk.snippet,
            startOffset <- chunk.startOffset,
            endOffset <- chunk.endOffset
        )
        return try db.run(insert)
    }

    public func deleteContent(forItemId id: Int64) throws {
        let rows = content.filter(itemId == id)
        try db.run(rows.delete())
    }

    public func getContent(forItemId id: Int64) throws -> [ContentChunk] {
        var chunks: [ContentChunk] = []
        for row in try db.prepare(content.filter(itemId == id).order(chunkIndex)) {
            chunks.append(ContentChunk(
                id: row[self.id],
                itemId: row[itemId],
                chunkIndex: row[chunkIndex],
                textHash: row[textHash],
                snippet: row[snippet],
                startOffset: row[startOffset],
                endOffset: row[endOffset]
            ))
        }
        return chunks
    }

    // MARK: - Failures

    public func recordFailure(_ failure: IndexFailure) throws {
        let existing = failures.filter(path == failure.path && stage == failure.stage)
        if try db.scalar(existing.count) > 0 {
            try db.run(existing.update(
                error <- failure.error,
                lastSeen <- failure.lastSeen.timeIntervalSince1970,
                failureCount <- failureCount + 1
            ))
        } else {
            try db.run(failures.insert(
                path <- failure.path,
                failureItemId <- failure.itemId,
                stage <- failure.stage,
                error <- failure.error,
                lastSeen <- failure.lastSeen.timeIntervalSince1970,
                failureCount <- failure.failureCount
            ))
        }
    }

    public func getRecentFailures(limit: Int = 100) throws -> [IndexFailure] {
        var result: [IndexFailure] = []
        for row in try db.prepare(failures.order(lastSeen.desc).limit(limit)) {
            result.append(IndexFailure(
                path: row[path],
                itemId: row[itemId],
                stage: row[stage],
                error: row[error],
                lastSeen: Date(timeIntervalSince1970: row[lastSeen]),
                failureCount: row[failureCount]
            ))
        }
        return result
    }

    public func clearFailures(forPath failurePath: String? = nil) throws {
        if let p = failurePath {
            try db.run(failures.filter(path == p).delete())
        } else {
            try db.run(failures.delete())
        }
    }

    // MARK: - Feedback

    public func recordFeedback(_ entry: FeedbackEntry) throws {
        let contextData = entry.context.flatMap { try? JSONEncoder().encode($0) }
        let contextString = contextData.flatMap { String(data: $0, encoding: .utf8) }

        try db.run(feedback.insert(
            feedbackId <- entry.id.uuidString,
            timestamp <- entry.timestamp.timeIntervalSince1970,
            query <- entry.query,
            itemId <- entry.itemId,
            path <- entry.itemPath,
            action <- entry.action.rawValue,
            resultPosition <- entry.resultPosition,
            totalResults <- entry.totalResults,
            contextJson <- contextString
        ))
    }

    // MARK: - Frequencies

    public func incrementOpenCount(forItemId id: Int64, itemPath: String) throws {
        let row = frequencies.filter(itemId == id)
        if try db.scalar(row.count) > 0 {
            try db.run(row.update(
                openCount <- openCount + 1,
                lastOpened <- Date().timeIntervalSince1970
            ))
        } else {
            try db.run(frequencies.insert(
                itemId <- id,
                path <- itemPath,
                openCount <- 1,
                lastOpened <- Date().timeIntervalSince1970,
                isPinned <- false
            ))
        }
    }

    public func getFrequency(forItemId id: Int64) throws -> ItemFrequency? {
        guard let row = try db.pluck(frequencies.filter(itemId == id)) else {
            return nil
        }
        return ItemFrequency(
            itemId: row[itemId],
            path: row[path],
            openCount: row[openCount],
            lastOpened: Date(timeIntervalSince1970: row[lastOpened]),
            isPinned: row[isPinned]
        )
    }

    public func setPinned(_ pinned: Bool, forItemId id: Int64) throws {
        try db.run(frequencies.filter(itemId == id).update(isPinned <- pinned))
    }

    // MARK: - Statistics

    public func getTotalItemCount() throws -> Int64 {
        Int64(try db.scalar(items.count))
    }

    public func getTotalContentChunkCount() throws -> Int64 {
        Int64(try db.scalar(content.count))
    }

    public func getFailureCountByType() throws -> [String: Int] {
        var result: [String: Int] = [:]
        for row in try db.prepare(failures.select(stage, failureCount.total).group(stage)) {
            result[row[stage]] = Int(row[failureCount.total] ?? 0)
        }
        return result
    }

    public func getDatabaseSize() throws -> Int64 {
        let attrs = try FileManager.default.attributesOfItem(atPath: dbPath)
        return (attrs[.size] as? Int64) ?? 0
    }

    // MARK: - Maintenance

    public func vacuum() throws {
        try db.execute("VACUUM")
    }

    public func checkpoint() throws {
        try db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    }
}
