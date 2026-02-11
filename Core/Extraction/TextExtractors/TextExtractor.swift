// DEPRECATED SWIFT REFERENCE
// Qt/C++ is the source of truth.
// Keep this file only as temporary migration reference while parity items are closed.
// Do not add new features or fixes here.

import Foundation

/// Protocol for text extraction from files
public protocol TextExtractor: Sendable {
    /// File extensions this extractor handles
    var supportedExtensions: Set<String> { get }

    /// Extract text content from a file
    func extract(from url: URL) async throws -> ExtractionResult
}

/// Result of text extraction
public struct ExtractionResult: Sendable {
    public let text: String
    public let metadata: ExtractionMetadata
    public let chunks: [TextChunk]

    public init(text: String, metadata: ExtractionMetadata, chunks: [TextChunk]) {
        self.text = text
        self.metadata = metadata
        self.chunks = chunks
    }
}

/// Metadata extracted from a file
public struct ExtractionMetadata: Sendable {
    public let title: String?
    public let author: String?
    public let createdDate: Date?
    public let modifiedDate: Date?
    public let wordCount: Int
    public let characterCount: Int
    public let language: String?
    public let encoding: String?
    public let mimeType: String?

    public init(
        title: String? = nil,
        author: String? = nil,
        createdDate: Date? = nil,
        modifiedDate: Date? = nil,
        wordCount: Int = 0,
        characterCount: Int = 0,
        language: String? = nil,
        encoding: String? = nil,
        mimeType: String? = nil
    ) {
        self.title = title
        self.author = author
        self.createdDate = createdDate
        self.modifiedDate = modifiedDate
        self.wordCount = wordCount
        self.characterCount = characterCount
        self.language = language
        self.encoding = encoding
        self.mimeType = mimeType
    }
}

/// A chunk of extracted text for indexing
public struct TextChunk: Sendable {
    public let index: Int
    public let text: String
    public let startOffset: Int
    public let endOffset: Int

    public init(index: Int, text: String, startOffset: Int, endOffset: Int) {
        self.index = index
        self.text = text
        self.startOffset = startOffset
        self.endOffset = endOffset
    }
}

/// Errors from extraction
public enum ExtractionError: Error, LocalizedError {
    case unsupportedFileType(String)
    case fileNotReadable(String)
    case encodingError(String)
    case extractionFailed(String)
    case fileTooLarge(Int64)

    public var errorDescription: String? {
        switch self {
        case .unsupportedFileType(let ext):
            return "Unsupported file type: \(ext)"
        case .fileNotReadable(let path):
            return "File not readable: \(path)"
        case .encodingError(let message):
            return "Encoding error: \(message)"
        case .extractionFailed(let message):
            return "Extraction failed: \(message)"
        case .fileTooLarge(let size):
            return "File too large: \(ByteCountFormatter.string(fromByteCount: size, countStyle: .file))"
        }
    }
}

/// Plain text extractor for txt, md, code files
public struct PlainTextExtractor: TextExtractor {
    public let supportedExtensions: Set<String> = [
        // Plain text
        "txt", "text", "md", "markdown", "rst", "asciidoc", "adoc",
        // Code files
        "swift", "m", "h", "c", "cpp", "cc", "cxx", "hpp", "hxx",
        "py", "pyw", "pyx", "pxd", "pxi",
        "js", "jsx", "ts", "tsx", "mjs", "cjs",
        "java", "kt", "kts", "scala", "groovy", "gradle",
        "rb", "rake", "gemspec",
        "go", "rs", "zig",
        "php", "phtml",
        "cs", "fs", "fsx", "vb",
        "lua", "r", "R", "jl", "pl", "pm",
        "sh", "bash", "zsh", "fish", "ps1", "psm1",
        "sql", "graphql", "gql",
        "html", "htm", "xhtml", "xml", "xsl", "xslt",
        "css", "scss", "sass", "less", "styl",
        "json", "jsonc", "json5", "yaml", "yml", "toml", "ini", "cfg", "conf",
        "dockerfile", "containerfile",
        "makefile", "cmake",
        "vim", "vimrc", "el", "lisp", "clj", "cljs", "edn",
        "erl", "hrl", "ex", "exs",
        "hs", "lhs", "ml", "mli", "mll", "mly",
        "tf", "tfvars", "hcl",
        "proto", "thrift", "avsc",
        "vue", "svelte", "astro",
        // Config files (no extension but common names)
        "gitignore", "gitattributes", "editorconfig", "prettierrc", "eslintrc",
    ]

    private let maxFileSize: Int64
    private let chunkSize: Int

    public init(maxFileSize: Int64 = 50 * 1024 * 1024, chunkSize: Int = 1000) {
        self.maxFileSize = maxFileSize
        self.chunkSize = chunkSize
    }

    public func extract(from url: URL) async throws -> ExtractionResult {
        // Check file size
        let attrs = try FileManager.default.attributesOfItem(atPath: url.path)
        let fileSize = (attrs[.size] as? Int64) ?? 0

        if fileSize > maxFileSize {
            throw ExtractionError.fileTooLarge(fileSize)
        }

        // Try to read with detected encoding
        let text: String
        do {
            text = try String(contentsOf: url, encoding: .utf8)
        } catch {
            // Try other encodings
            if let contents = try? String(contentsOf: url, encoding: .isoLatin1) {
                text = contents
            } else if let contents = try? String(contentsOf: url, encoding: .macOSRoman) {
                text = contents
            } else {
                throw ExtractionError.encodingError("Could not decode file with any supported encoding")
            }
        }

        let chunks = chunkText(text)

        let metadata = ExtractionMetadata(
            wordCount: countWords(in: text),
            characterCount: text.count,
            encoding: "utf-8",
            mimeType: mimeType(for: url.pathExtension)
        )

        return ExtractionResult(text: text, metadata: metadata, chunks: chunks)
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

            // Try to break at a word boundary
            var actualEnd = endIndex
            if endIndex < text.endIndex {
                if let spaceIndex = text[currentIndex..<endIndex].lastIndex(of: " ") {
                    actualEnd = text.index(after: spaceIndex)
                } else if let newlineIndex = text[currentIndex..<endIndex].lastIndex(of: "\n") {
                    actualEnd = text.index(after: newlineIndex)
                }
            }

            let chunkText = String(text[currentIndex..<actualEnd])
            let chunkEndOffset = offset + text.distance(from: currentIndex, to: actualEnd)

            chunks.append(TextChunk(
                index: chunkIndex,
                text: chunkText,
                startOffset: offset,
                endOffset: chunkEndOffset
            ))

            offset = chunkEndOffset
            currentIndex = actualEnd
            chunkIndex += 1
        }

        return chunks
    }

    private func countWords(in text: String) -> Int {
        let words = text.components(separatedBy: .whitespacesAndNewlines)
            .filter { !$0.isEmpty }
        return words.count
    }

    private func mimeType(for ext: String) -> String {
        switch ext.lowercased() {
        case "txt", "text": return "text/plain"
        case "md", "markdown": return "text/markdown"
        case "html", "htm": return "text/html"
        case "css": return "text/css"
        case "js", "mjs": return "text/javascript"
        case "json": return "application/json"
        case "xml": return "application/xml"
        case "yaml", "yml": return "application/x-yaml"
        default: return "text/plain"
        }
    }
}
