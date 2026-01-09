import Foundation

/// XPC Service for text extraction from files
public final class ExtractorService: NSObject, ExtractorServiceProtocol {
    private let extractionManager: ExtractionManager

    public init(settings: IndexingSettings = IndexingSettings()) {
        self.extractionManager = ExtractionManager(settings: settings)
        super.init()
    }

    // MARK: - ExtractorServiceProtocol

    public func extractText(from path: String, reply: @escaping (Data?, Error?) -> Void) {
        Task {
            do {
                let url = URL(fileURLWithPath: path)
                let result = try await extractionManager.extract(from: url)

                let response = ExtractTextResponse(
                    text: result.text,
                    chunks: result.chunks.map { chunk in
                        ExtractTextResponse.Chunk(
                            index: chunk.index,
                            text: chunk.text,
                            startOffset: chunk.startOffset,
                            endOffset: chunk.endOffset
                        )
                    },
                    wordCount: result.metadata.wordCount,
                    characterCount: result.metadata.characterCount
                )

                let data = try JSONEncoder().encode(response)
                reply(data, nil)
            } catch {
                reply(nil, error)
            }
        }
    }

    public func extractMetadata(from path: String, reply: @escaping (Data?, Error?) -> Void) {
        Task {
            do {
                let url = URL(fileURLWithPath: path)
                let attrs = try FileManager.default.attributesOfItem(atPath: path)

                let response = ExtractMetadataResponse(
                    path: path,
                    filename: url.lastPathComponent,
                    fileExtension: url.pathExtension,
                    size: (attrs[.size] as? Int64) ?? 0,
                    modificationDate: attrs[.modificationDate] as? Date,
                    creationDate: attrs[.creationDate] as? Date,
                    owner: attrs[.ownerAccountName] as? String,
                    isReadable: FileManager.default.isReadableFile(atPath: path),
                    isWritable: FileManager.default.isWritableFile(atPath: path),
                    isExecutable: FileManager.default.isExecutableFile(atPath: path)
                )

                let data = try JSONEncoder().encode(response)
                reply(data, nil)
            } catch {
                reply(nil, error)
            }
        }
    }

    public func performOCR(on path: String, reply: @escaping (String?, Error?) -> Void) {
        Task {
            do {
                let url = URL(fileURLWithPath: path)
                let ocrExtractor = OcrExtractor()
                let result = try await ocrExtractor.extract(from: url)
                reply(result.text, nil)
            } catch {
                reply(nil, error)
            }
        }
    }

    public func isSupported(fileExtension: String, reply: @escaping (Bool) -> Void) {
        Task {
            let supported = await extractionManager.isSupported(extension: fileExtension)
            reply(supported)
        }
    }
}

// MARK: - Response Types

private struct ExtractTextResponse: Codable {
    let text: String
    let chunks: [Chunk]
    let wordCount: Int
    let characterCount: Int

    struct Chunk: Codable {
        let index: Int
        let text: String
        let startOffset: Int
        let endOffset: Int
    }
}

private struct ExtractMetadataResponse: Codable {
    let path: String
    let filename: String
    let fileExtension: String
    let size: Int64
    let modificationDate: Date?
    let creationDate: Date?
    let owner: String?
    let isReadable: Bool
    let isWritable: Bool
    let isExecutable: Bool
}
