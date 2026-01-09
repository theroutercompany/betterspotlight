import XCTest
@testable import Core
@testable import Shared

/// Integration tests for the indexing pipeline
final class IndexingIntegrationTests: XCTestCase {

    var tempDir: URL!
    var store: SQLiteStore!

    override func setUp() async throws {
        try await super.setUp()

        // Create temp directory for test files
        tempDir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)

        // Create test database
        let dbPath = tempDir.appendingPathComponent("test.sqlite").path
        store = try SQLiteStore(path: dbPath)
    }

    override func tearDown() async throws {
        try? FileManager.default.removeItem(at: tempDir)
        try await super.tearDown()
    }

    // MARK: - File Scanning

    func testScannerFindsFiles() async throws {
        // Create test files
        let file1 = tempDir.appendingPathComponent("test1.txt")
        let file2 = tempDir.appendingPathComponent("test2.swift")
        let subdir = tempDir.appendingPathComponent("subdir")
        let file3 = subdir.appendingPathComponent("test3.md")

        try FileManager.default.createDirectory(at: subdir, withIntermediateDirectories: true)
        try "content1".write(to: file1, atomically: true, encoding: .utf8)
        try "content2".write(to: file2, atomically: true, encoding: .utf8)
        try "content3".write(to: file3, atomically: true, encoding: .utf8)

        let rules = PathRules()
        let scanner = FileScanner(rules: rules)

        var foundPaths: Set<String> = []
        let stream = await scanner.enumerateDirectory(at: tempDir.path)

        for try await result in stream {
            foundPaths.insert(result.path)
        }

        XCTAssertTrue(foundPaths.contains(file1.path))
        XCTAssertTrue(foundPaths.contains(file2.path))
        XCTAssertTrue(foundPaths.contains(file3.path))
        XCTAssertTrue(foundPaths.contains(subdir.path))
    }

    func testScannerRespectsExclusions() async throws {
        // Create a node_modules directory
        let nodeModules = tempDir.appendingPathComponent("node_modules")
        let packageFile = nodeModules.appendingPathComponent("package/index.js")

        try FileManager.default.createDirectory(at: nodeModules.appendingPathComponent("package"), withIntermediateDirectories: true)
        try "module.exports = {}".write(to: packageFile, atomically: true, encoding: .utf8)

        // Create a normal file
        let normalFile = tempDir.appendingPathComponent("main.js")
        try "console.log('hello')".write(to: normalFile, atomically: true, encoding: .utf8)

        let rules = PathRules()
        let scanner = FileScanner(rules: rules)

        var foundPaths: Set<String> = []
        let stream = await scanner.enumerateDirectory(at: tempDir.path)

        for try await result in stream {
            foundPaths.insert(result.path)
        }

        XCTAssertTrue(foundPaths.contains(normalFile.path))
        XCTAssertFalse(foundPaths.contains(packageFile.path))
    }

    // MARK: - SQLite Store

    func testInsertAndRetrieveItem() async throws {
        let item = IndexItem(
            id: 0,
            path: "/test/file.txt",
            kind: .file,
            size: 1234,
            modificationDate: Date(),
            creationDate: Date(),
            owner: "testuser",
            flags: 0o644,
            contentHash: "abc123",
            sensitivity: .normal
        )

        let insertedId = try await store.insertItem(item)
        XCTAssertGreaterThan(insertedId, 0)

        let retrieved = try await store.getItem(id: insertedId)
        XCTAssertNotNil(retrieved)
        XCTAssertEqual(retrieved?.path, item.path)
        XCTAssertEqual(retrieved?.kind, item.kind)
        XCTAssertEqual(retrieved?.size, item.size)
    }

    func testItemExistsCheck() async throws {
        let item = IndexItem(
            id: 0,
            path: "/test/exists.txt",
            kind: .file,
            size: 100,
            modificationDate: Date(),
            creationDate: Date()
        )

        _ = try await store.insertItem(item)

        let exists = try await store.itemExists(path: "/test/exists.txt")
        let notExists = try await store.itemExists(path: "/test/notexists.txt")

        XCTAssertTrue(exists)
        XCTAssertFalse(notExists)
    }

    func testDeleteItem() async throws {
        let item = IndexItem(
            id: 0,
            path: "/test/todelete.txt",
            kind: .file,
            size: 100,
            modificationDate: Date(),
            creationDate: Date()
        )

        let itemId = try await store.insertItem(item)

        try await store.deleteItem(id: itemId)

        let retrieved = try await store.getItem(id: itemId)
        XCTAssertNil(retrieved)
    }

    func testContentChunks() async throws {
        let item = IndexItem(
            id: 0,
            path: "/test/content.txt",
            kind: .file,
            size: 1000,
            modificationDate: Date(),
            creationDate: Date()
        )

        let itemId = try await store.insertItem(item)

        // Insert chunks
        for i in 0..<3 {
            let chunk = ContentChunk(
                id: 0,
                itemId: itemId,
                chunkIndex: i,
                textHash: "hash\(i)",
                snippet: "Chunk \(i) content...",
                startOffset: i * 100,
                endOffset: (i + 1) * 100
            )
            _ = try await store.insertContent(chunk)
        }

        // Retrieve chunks
        let chunks = try await store.getContent(forItemId: itemId)
        XCTAssertEqual(chunks.count, 3)
        XCTAssertEqual(chunks[0].chunkIndex, 0)
        XCTAssertEqual(chunks[1].chunkIndex, 1)
        XCTAssertEqual(chunks[2].chunkIndex, 2)
    }

    // MARK: - Frequency Tracking

    func testFrequencyTracking() async throws {
        let item = IndexItem(
            id: 0,
            path: "/test/frequent.txt",
            kind: .file,
            size: 100,
            modificationDate: Date(),
            creationDate: Date()
        )

        let itemId = try await store.insertItem(item)

        // Increment open count multiple times
        try await store.incrementOpenCount(forItemId: itemId, itemPath: item.path)
        try await store.incrementOpenCount(forItemId: itemId, itemPath: item.path)
        try await store.incrementOpenCount(forItemId: itemId, itemPath: item.path)

        let frequency = try await store.getFrequency(forItemId: itemId)
        XCTAssertNotNil(frequency)
        XCTAssertEqual(frequency?.openCount, 3)
    }

    // MARK: - Failure Recording

    func testFailureRecording() async throws {
        let failure = IndexFailure(
            path: "/test/failed.bin",
            stage: "extraction",
            error: "Unsupported file type"
        )

        try await store.recordFailure(failure)

        let failures = try await store.getRecentFailures()
        XCTAssertEqual(failures.count, 1)
        XCTAssertEqual(failures[0].path, failure.path)
        XCTAssertEqual(failures[0].stage, failure.stage)
    }

    func testFailureCountIncrementsOnRepeat() async throws {
        let failure = IndexFailure(
            path: "/test/repeatfail.bin",
            stage: "extraction",
            error: "Failed"
        )

        try await store.recordFailure(failure)
        try await store.recordFailure(failure)
        try await store.recordFailure(failure)

        let failures = try await store.getRecentFailures()
        let matchingFailure = failures.first { $0.path == failure.path }

        XCTAssertNotNil(matchingFailure)
        XCTAssertEqual(matchingFailure?.failureCount, 3)
    }

    // MARK: - Statistics

    func testStatistics() async throws {
        // Insert some items
        for i in 0..<10 {
            let item = IndexItem(
                id: 0,
                path: "/test/item\(i).txt",
                kind: .file,
                size: Int64(i * 100),
                modificationDate: Date(),
                creationDate: Date()
            )
            _ = try await store.insertItem(item)
        }

        let count = try await store.getTotalItemCount()
        XCTAssertEqual(count, 10)
    }
}
