import XCTest
@testable import Core
@testable import Shared

final class TextExtractorTests: XCTestCase {

    var extractor: PlainTextExtractor!
    var tempDir: URL!

    override func setUp() async throws {
        try await super.setUp()
        extractor = PlainTextExtractor()
        tempDir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDown() async throws {
        try? FileManager.default.removeItem(at: tempDir)
        try await super.tearDown()
    }

    // MARK: - Supported Extensions

    func testSupportsCommonTextFiles() {
        XCTAssertTrue(extractor.supportedExtensions.contains("txt"))
        XCTAssertTrue(extractor.supportedExtensions.contains("md"))
        XCTAssertTrue(extractor.supportedExtensions.contains("markdown"))
    }

    func testSupportsCodeFiles() {
        XCTAssertTrue(extractor.supportedExtensions.contains("swift"))
        XCTAssertTrue(extractor.supportedExtensions.contains("py"))
        XCTAssertTrue(extractor.supportedExtensions.contains("js"))
        XCTAssertTrue(extractor.supportedExtensions.contains("ts"))
        XCTAssertTrue(extractor.supportedExtensions.contains("go"))
        XCTAssertTrue(extractor.supportedExtensions.contains("rs"))
        XCTAssertTrue(extractor.supportedExtensions.contains("java"))
        XCTAssertTrue(extractor.supportedExtensions.contains("rb"))
    }

    func testSupportsConfigFiles() {
        XCTAssertTrue(extractor.supportedExtensions.contains("json"))
        XCTAssertTrue(extractor.supportedExtensions.contains("yaml"))
        XCTAssertTrue(extractor.supportedExtensions.contains("yml"))
        XCTAssertTrue(extractor.supportedExtensions.contains("toml"))
        XCTAssertTrue(extractor.supportedExtensions.contains("xml"))
    }

    // MARK: - Text Extraction

    func testExtractsPlainText() async throws {
        let content = "Hello, World!\nThis is a test file."
        let file = tempDir.appendingPathComponent("test.txt")
        try content.write(to: file, atomically: true, encoding: .utf8)

        let result = try await extractor.extract(from: file)

        XCTAssertEqual(result.text, content)
    }

    func testExtractsWithCorrectMetadata() async throws {
        let content = "Word one two three four five."
        let file = tempDir.appendingPathComponent("test.txt")
        try content.write(to: file, atomically: true, encoding: .utf8)

        let result = try await extractor.extract(from: file)

        XCTAssertEqual(result.metadata.wordCount, 6)
        XCTAssertEqual(result.metadata.characterCount, content.count)
    }

    func testChunksLongText() async throws {
        // Create text longer than default chunk size
        let longText = String(repeating: "word ", count: 500)
        let file = tempDir.appendingPathComponent("long.txt")
        try longText.write(to: file, atomically: true, encoding: .utf8)

        let result = try await extractor.extract(from: file)

        XCTAssertGreaterThan(result.chunks.count, 1)

        // Verify chunks cover the whole text
        let reconstructed = result.chunks.map { $0.text }.joined()
        XCTAssertEqual(reconstructed.count, longText.count)
    }

    func testChunkOffsets() async throws {
        let content = "First chunk of text. Second chunk of text. Third chunk of text."
        let file = tempDir.appendingPathComponent("offsets.txt")
        try content.write(to: file, atomically: true, encoding: .utf8)

        let smallChunkExtractor = PlainTextExtractor(chunkSize: 25)
        let result = try await smallChunkExtractor.extract(from: file)

        // Verify offsets are correct
        for chunk in result.chunks {
            XCTAssertLessThanOrEqual(chunk.startOffset, chunk.endOffset)
            if chunk.index > 0 {
                let prevChunk = result.chunks[chunk.index - 1]
                XCTAssertEqual(chunk.startOffset, prevChunk.endOffset)
            }
        }
    }

    // MARK: - Error Cases

    func testRejectsOversizedFiles() async {
        let smallExtractor = PlainTextExtractor(maxFileSize: 100)
        let content = String(repeating: "x", count: 200)
        let file = tempDir.appendingPathComponent("large.txt")
        try? content.write(to: file, atomically: true, encoding: .utf8)

        do {
            _ = try await smallExtractor.extract(from: file)
            XCTFail("Should have thrown for oversized file")
        } catch let error as ExtractionError {
            if case .fileTooLarge = error {
                // Expected
            } else {
                XCTFail("Wrong error type: \(error)")
            }
        } catch {
            XCTFail("Wrong error type: \(error)")
        }
    }

    func testHandlesNonexistentFile() async {
        let file = tempDir.appendingPathComponent("nonexistent.txt")

        do {
            _ = try await extractor.extract(from: file)
            XCTFail("Should have thrown for nonexistent file")
        } catch {
            // Expected - any error is acceptable
        }
    }
}

// MARK: - Extraction Manager Tests

final class ExtractionManagerTests: XCTestCase {

    func testIdentifiesSupportedExtensions() async {
        let manager = ExtractionManager()

        let txtSupported = await manager.isSupported(extension: "txt")
        let swiftSupported = await manager.isSupported(extension: "swift")
        let pdfSupported = await manager.isSupported(extension: "pdf")
        let exeSupported = await manager.isSupported(extension: "exe")
        let dmgSupported = await manager.isSupported(extension: "dmg")

        XCTAssertTrue(txtSupported)
        XCTAssertTrue(swiftSupported)
        XCTAssertTrue(pdfSupported)
        XCTAssertFalse(exeSupported)
        XCTAssertFalse(dmgSupported)
    }

    func testOcrExtractorDisabledByDefault() async {
        let settings = IndexingSettings(enableOCR: false)
        let manager = ExtractionManager(settings: settings)

        // PNG should not be supported when OCR is disabled
        let pngSupported = await manager.isSupported(extension: "png")
        XCTAssertFalse(pngSupported)
    }

    func testOcrExtractorEnabledWhenConfigured() async {
        let settings = IndexingSettings(enableOCR: true)
        let manager = ExtractionManager(settings: settings)

        // PNG should be supported when OCR is enabled
        let pngSupported = await manager.isSupported(extension: "png")
        XCTAssertTrue(pngSupported)
    }
}
