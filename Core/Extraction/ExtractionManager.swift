// DEPRECATED SWIFT REFERENCE
// Qt/C++ is the source of truth.
// Keep this file only as temporary migration reference while parity items are closed.
// Do not add new features or fixes here.

import Foundation

/// Manages text extraction from files using appropriate extractors
public actor ExtractionManager {
    private let extractors: [TextExtractor]
    private let extensionToExtractor: [String: TextExtractor]
    private let settings: IndexingSettings

    public init(settings: IndexingSettings = IndexingSettings()) {
        self.settings = settings

        // Initialize extractors
        var extractorList: [TextExtractor] = [
            PlainTextExtractor(maxFileSize: Int64(settings.maxFileSizeMB) * 1024 * 1024),
            PdfExtractor(maxFileSize: Int64(settings.maxFileSizeMB) * 1024 * 1024),
        ]

        // Add OCR extractor if enabled
        if settings.enableOCR {
            extractorList.append(OcrExtractor(maxFileSize: Int64(settings.maxFileSizeMB) * 1024 * 1024))
        }

        self.extractors = extractorList

        // Build extension lookup table
        var lookup: [String: TextExtractor] = [:]
        for extractor in extractorList {
            for ext in extractor.supportedExtensions {
                lookup[ext.lowercased()] = extractor
            }
        }
        self.extensionToExtractor = lookup
    }

    /// Check if a file extension is supported for extraction
    public func isSupported(extension ext: String) -> Bool {
        extensionToExtractor[ext.lowercased()] != nil
    }

    /// Get all supported extensions
    public var supportedExtensions: Set<String> {
        Set(extensionToExtractor.keys)
    }

    /// Extract content from a file
    public func extract(from url: URL) async throws -> ExtractionResult {
        let ext = url.pathExtension.lowercased()

        guard let extractor = extensionToExtractor[ext] else {
            throw ExtractionError.unsupportedFileType(ext)
        }

        return try await extractor.extract(from: url)
    }

    /// Extract content from a file, returning nil if unsupported or failed
    public func extractIfSupported(from url: URL) async -> ExtractionResult? {
        do {
            return try await extract(from: url)
        } catch {
            return nil
        }
    }

    /// Batch extract from multiple files with concurrency control
    public func extractBatch(
        from urls: [URL],
        progress: ((Int, Int) -> Void)? = nil
    ) async -> [URL: Result<ExtractionResult, Error>] {
        var results: [URL: Result<ExtractionResult, Error>] = [:]
        let total = urls.count
        var completed = 0

        await withTaskGroup(of: (URL, Result<ExtractionResult, Error>).self) { group in
            // Limit concurrency
            let maxConcurrent = settings.maxConcurrentExtractions
            var pending = urls.makeIterator()
            var inFlight = 0

            // Start initial batch
            for _ in 0..<maxConcurrent {
                guard let url = pending.next() else { break }
                inFlight += 1
                group.addTask {
                    do {
                        let result = try await self.extract(from: url)
                        return (url, .success(result))
                    } catch {
                        return (url, .failure(error))
                    }
                }
            }

            // Process results and add new tasks
            for await (url, result) in group {
                results[url] = result
                completed += 1
                progress?(completed, total)
                inFlight -= 1

                // Add next URL if available
                if let nextUrl = pending.next() {
                    inFlight += 1
                    group.addTask {
                        do {
                            let result = try await self.extract(from: nextUrl)
                            return (nextUrl, .success(result))
                        } catch {
                            return (nextUrl, .failure(error))
                        }
                    }
                }
            }
        }

        return results
    }
}

/// Extension for handling special file types
extension ExtractionManager {
    /// Get a snippet preview for a file (first N characters)
    public func getSnippet(from url: URL, maxLength: Int = 500) async -> String? {
        guard let result = await extractIfSupported(from: url) else {
            return nil
        }

        let text = result.text.trimmingCharacters(in: .whitespacesAndNewlines)

        if text.count <= maxLength {
            return text
        }

        // Find a good break point
        let endIndex = text.index(text.startIndex, offsetBy: maxLength)
        if let lastSpace = text[..<endIndex].lastIndex(of: " ") {
            return String(text[..<lastSpace]) + "..."
        }

        return String(text.prefix(maxLength)) + "..."
    }

    /// Get word count for a file
    public func getWordCount(from url: URL) async -> Int? {
        guard let result = await extractIfSupported(from: url) else {
            return nil
        }
        return result.metadata.wordCount
    }
}
