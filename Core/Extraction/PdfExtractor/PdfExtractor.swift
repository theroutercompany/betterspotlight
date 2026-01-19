import Foundation
import PDFKit

/// Extracts text from PDF files using PDFKit
public struct PdfExtractor: TextExtractor {
    public let supportedExtensions: Set<String> = ["pdf"]

    private let maxFileSize: Int64
    private let chunkSize: Int
    private let maxPages: Int

    public init(
        maxFileSize: Int64 = 100 * 1024 * 1024,
        chunkSize: Int = 1000,
        maxPages: Int = 1000
    ) {
        self.maxFileSize = maxFileSize
        self.chunkSize = chunkSize
        self.maxPages = maxPages
    }

    public func extract(from url: URL) async throws -> ExtractionResult {
        // Check file size
        let attrs = try FileManager.default.attributesOfItem(atPath: url.path)
        let fileSize = (attrs[.size] as? Int64) ?? 0

        if fileSize > maxFileSize {
            throw ExtractionError.fileTooLarge(fileSize)
        }

        return try await Task.detached(priority: .utility) {
            guard let document = PDFDocument(url: url) else {
                throw ExtractionError.extractionFailed("Could not open PDF document")
            }

            var fullText = ""
            let pageCount = min(document.pageCount, maxPages)

            for i in 0..<pageCount {
                guard let page = document.page(at: i) else { continue }
                if let pageText = page.string {
                    fullText += pageText
                    fullText += "\n\n"
                }
            }

            // Clean up the text
            fullText = self.cleanText(fullText)

            let chunks = self.chunkText(fullText)

            // Extract metadata
            let metadata = self.extractMetadata(from: document, text: fullText)

            return ExtractionResult(text: fullText, metadata: metadata, chunks: chunks)
        }.value
    }

    private func extractMetadata(from document: PDFDocument, text: String) -> ExtractionMetadata {
        let attrs = document.documentAttributes

        let title = attrs?[PDFDocumentAttribute.titleAttribute] as? String
        let author = attrs?[PDFDocumentAttribute.authorAttribute] as? String
        let createdDate = attrs?[PDFDocumentAttribute.creationDateAttribute] as? Date
        let modifiedDate = attrs?[PDFDocumentAttribute.modificationDateAttribute] as? Date

        return ExtractionMetadata(
            title: title,
            author: author,
            createdDate: createdDate,
            modifiedDate: modifiedDate,
            wordCount: countWords(in: text),
            characterCount: text.count,
            mimeType: "application/pdf"
        )
    }

    private func cleanText(_ text: String) -> String {
        var cleaned = text

        // Remove excessive whitespace
        while cleaned.contains("  ") {
            cleaned = cleaned.replacingOccurrences(of: "  ", with: " ")
        }

        // Remove excessive newlines
        while cleaned.contains("\n\n\n") {
            cleaned = cleaned.replacingOccurrences(of: "\n\n\n", with: "\n\n")
        }

        // Remove form feed and other control characters
        cleaned = cleaned.components(separatedBy: .controlCharacters)
            .joined(separator: " ")

        return cleaned.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func chunkText(_ text: String) -> [TextChunk] {
        var chunks: [TextChunk] = []
        var currentIndex = text.startIndex
        var chunkIndex = 0
        var offset = 0

        while currentIndex < text.endIndex {
            let remainingDistance = text.distance(from: currentIndex, to: text.endIndex)
            let chunkLength = min(chunkSize, remainingDistance)
            let endIndex = text.index(currentIndex, offsetBy: chunkLength)

            // Try to break at a paragraph or sentence boundary
            var actualEnd = endIndex
            if endIndex < text.endIndex {
                // Look for paragraph break first
                if let paraIndex = text[currentIndex..<endIndex].range(of: "\n\n", options: .backwards)?.lowerBound {
                    actualEnd = text.index(after: text.index(after: paraIndex))
                }
                // Fall back to sentence break
                else if let periodIndex = text[currentIndex..<endIndex].lastIndex(of: ".") {
                    actualEnd = text.index(after: periodIndex)
                }
                // Fall back to word break
                else if let spaceIndex = text[currentIndex..<endIndex].lastIndex(of: " ") {
                    actualEnd = text.index(after: spaceIndex)
                }
            }

            let chunkText = String(text[currentIndex..<actualEnd])
            let chunkEndOffset = offset + text.distance(from: currentIndex, to: actualEnd)

            if !chunkText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                chunks.append(TextChunk(
                    index: chunkIndex,
                    text: chunkText,
                    startOffset: offset,
                    endOffset: chunkEndOffset
                ))
                chunkIndex += 1
            }

            offset = chunkEndOffset
            currentIndex = actualEnd
        }

        return chunks
    }

    private func countWords(in text: String) -> Int {
        text.components(separatedBy: .whitespacesAndNewlines)
            .filter { !$0.isEmpty }
            .count
    }
}
