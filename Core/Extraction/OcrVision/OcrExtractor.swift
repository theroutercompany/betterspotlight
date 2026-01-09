import Foundation
import Vision

/// Extracts text from images using Apple's Vision framework OCR
public struct OcrExtractor: TextExtractor {
    public let supportedExtensions: Set<String> = [
        "png", "jpg", "jpeg", "heic", "heif", "tiff", "tif", "bmp", "gif", "webp"
    ]

    private let maxFileSize: Int64
    private let recognitionLevel: VNRequestTextRecognitionLevel
    private let recognitionLanguages: [String]

    public init(
        maxFileSize: Int64 = 50 * 1024 * 1024,
        recognitionLevel: VNRequestTextRecognitionLevel = .accurate,
        recognitionLanguages: [String] = ["en-US"]
    ) {
        self.maxFileSize = maxFileSize
        self.recognitionLevel = recognitionLevel
        self.recognitionLanguages = recognitionLanguages
    }

    public func extract(from url: URL) async throws -> ExtractionResult {
        // Check file size
        let attrs = try FileManager.default.attributesOfItem(atPath: url.path)
        let fileSize = (attrs[.size] as? Int64) ?? 0

        if fileSize > maxFileSize {
            throw ExtractionError.fileTooLarge(fileSize)
        }

        return try await withCheckedThrowingContinuation { continuation in
            performOCR(on: url) { result in
                switch result {
                case .success(let extractionResult):
                    continuation.resume(returning: extractionResult)
                case .failure(let error):
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private func performOCR(on url: URL, completion: @escaping (Result<ExtractionResult, Error>) -> Void) {
        guard let imageSource = CGImageSourceCreateWithURL(url as CFURL, nil),
              let cgImage = CGImageSourceCreateImageAtIndex(imageSource, 0, nil) else {
            completion(.failure(ExtractionError.fileNotReadable(url.path)))
            return
        }

        let request = VNRecognizeTextRequest { request, error in
            if let error = error {
                completion(.failure(ExtractionError.extractionFailed(error.localizedDescription)))
                return
            }

            guard let observations = request.results as? [VNRecognizedTextObservation] else {
                completion(.success(ExtractionResult(
                    text: "",
                    metadata: ExtractionMetadata(wordCount: 0, characterCount: 0, mimeType: "image/*"),
                    chunks: []
                )))
                return
            }

            // Sort observations by position (top to bottom, left to right)
            let sortedObservations = observations.sorted { a, b in
                // Vision coordinates have origin at bottom-left, so we flip Y
                let aY = 1 - a.boundingBox.midY
                let bY = 1 - b.boundingBox.midY

                // If on roughly the same line (within 5% of image height)
                if abs(aY - bY) < 0.05 {
                    return a.boundingBox.minX < b.boundingBox.minX
                }
                return aY < bY
            }

            var fullText = ""
            var lastY: CGFloat = -1

            for observation in sortedObservations {
                guard let candidate = observation.topCandidates(1).first else { continue }

                let currentY = 1 - observation.boundingBox.midY

                // Add newline if we've moved to a new line
                if lastY >= 0 && abs(currentY - lastY) > 0.02 {
                    fullText += "\n"
                } else if !fullText.isEmpty {
                    fullText += " "
                }

                fullText += candidate.string
                lastY = currentY
            }

            let chunks = self.chunkText(fullText)

            let metadata = ExtractionMetadata(
                wordCount: self.countWords(in: fullText),
                characterCount: fullText.count,
                mimeType: self.mimeType(for: url.pathExtension)
            )

            completion(.success(ExtractionResult(text: fullText, metadata: metadata, chunks: chunks)))
        }

        request.recognitionLevel = recognitionLevel
        request.recognitionLanguages = recognitionLanguages
        request.usesLanguageCorrection = true

        let handler = VNImageRequestHandler(cgImage: cgImage, options: [:])

        DispatchQueue.global(qos: .utility).async {
            do {
                try handler.perform([request])
            } catch {
                completion(.failure(ExtractionError.extractionFailed(error.localizedDescription)))
            }
        }
    }

    private func chunkText(_ text: String) -> [TextChunk] {
        guard !text.isEmpty else { return [] }

        var chunks: [TextChunk] = []
        let chunkSize = 500
        var currentIndex = text.startIndex
        var chunkIndex = 0
        var offset = 0

        while currentIndex < text.endIndex {
            let remainingDistance = text.distance(from: currentIndex, to: text.endIndex)
            let chunkLength = min(chunkSize, remainingDistance)
            let endIndex = text.index(currentIndex, offsetBy: chunkLength)

            var actualEnd = endIndex
            if endIndex < text.endIndex {
                if let newlineIndex = text[currentIndex..<endIndex].lastIndex(of: "\n") {
                    actualEnd = text.index(after: newlineIndex)
                } else if let spaceIndex = text[currentIndex..<endIndex].lastIndex(of: " ") {
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

    private func mimeType(for ext: String) -> String {
        switch ext.lowercased() {
        case "png": return "image/png"
        case "jpg", "jpeg": return "image/jpeg"
        case "heic", "heif": return "image/heic"
        case "tiff", "tif": return "image/tiff"
        case "bmp": return "image/bmp"
        case "gif": return "image/gif"
        case "webp": return "image/webp"
        default: return "image/*"
        }
    }
}
