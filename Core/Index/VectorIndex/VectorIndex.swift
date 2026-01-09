import Foundation

/// Vector similarity search index for semantic search (optional feature)
/// This is a placeholder implementation - in production, you'd use a proper vector DB
/// or an embedded library like FAISS, Annoy, or sqlite-vss
public actor VectorIndex {
    private var vectors: [Int64: [Float]] = [:]
    private let dimensions: Int

    public init(dimensions: Int = 384) {
        self.dimensions = dimensions
    }

    /// Add or update a vector for an item
    public func upsert(itemId: Int64, vector: [Float]) throws {
        guard vector.count == dimensions else {
            throw VectorIndexError.dimensionMismatch(expected: dimensions, got: vector.count)
        }
        vectors[itemId] = normalize(vector)
    }

    /// Remove a vector
    public func delete(itemId: Int64) {
        vectors.removeValue(forKey: itemId)
    }

    /// Find similar items using cosine similarity
    public func search(query: [Float], limit: Int = 20) throws -> [VectorSearchResult] {
        guard query.count == dimensions else {
            throw VectorIndexError.dimensionMismatch(expected: dimensions, got: query.count)
        }

        let normalizedQuery = normalize(query)

        var results: [(itemId: Int64, similarity: Float)] = []

        for (itemId, vector) in vectors {
            let similarity = cosineSimilarity(normalizedQuery, vector)
            results.append((itemId, similarity))
        }

        // Sort by similarity (descending) and take top results
        results.sort { $0.similarity > $1.similarity }

        return results.prefix(limit).map { result in
            VectorSearchResult(
                itemId: result.itemId,
                similarity: result.similarity
            )
        }
    }

    /// Get the number of indexed vectors
    public var count: Int {
        vectors.count
    }

    /// Clear all vectors
    public func clear() {
        vectors.removeAll()
    }

    // MARK: - Persistence

    /// Save index to disk
    public func save(to url: URL) throws {
        let data = try JSONEncoder().encode(SerializableIndex(vectors: vectors, dimensions: dimensions))
        try data.write(to: url)
    }

    /// Load index from disk
    public func load(from url: URL) throws {
        let data = try Data(contentsOf: url)
        let index = try JSONDecoder().decode(SerializableIndex.self, from: data)
        guard index.dimensions == dimensions else {
            throw VectorIndexError.dimensionMismatch(expected: dimensions, got: index.dimensions)
        }
        vectors = index.vectors
    }

    // MARK: - Private Helpers

    private func normalize(_ vector: [Float]) -> [Float] {
        let magnitude = sqrt(vector.reduce(0) { $0 + $1 * $1 })
        guard magnitude > 0 else { return vector }
        return vector.map { $0 / magnitude }
    }

    private func cosineSimilarity(_ a: [Float], _ b: [Float]) -> Float {
        // Vectors are already normalized, so dot product = cosine similarity
        zip(a, b).reduce(0) { $0 + $1.0 * $1.1 }
    }
}

/// Result from vector search
public struct VectorSearchResult: Sendable {
    public let itemId: Int64
    public let similarity: Float

    public init(itemId: Int64, similarity: Float) {
        self.itemId = itemId
        self.similarity = similarity
    }
}

/// Errors from vector index operations
public enum VectorIndexError: Error, LocalizedError {
    case dimensionMismatch(expected: Int, got: Int)
    case indexNotLoaded
    case persistenceFailed(String)

    public var errorDescription: String? {
        switch self {
        case .dimensionMismatch(let expected, let got):
            return "Vector dimension mismatch: expected \(expected), got \(got)"
        case .indexNotLoaded:
            return "Vector index not loaded"
        case .persistenceFailed(let message):
            return "Failed to persist vector index: \(message)"
        }
    }
}

/// Serializable format for persistence
private struct SerializableIndex: Codable {
    let vectors: [Int64: [Float]]
    let dimensions: Int
}
