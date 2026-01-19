import Foundation

/// Client for communicating with XPC services
public actor ServiceClient {
    private var indexerConnection: NSXPCConnection?
    private var extractorConnection: NSXPCConnection?
    private var queryConnection: NSXPCConnection?

    public init() {}

    // MARK: - Connection Management

    private func getIndexerConnection() throws -> NSXPCConnection {
        if let connection = indexerConnection {
            return connection
        }

        let connection = NSXPCConnection(serviceName: XPCServiceIdentifier.indexer.rawValue)
        connection.remoteObjectInterface = NSXPCInterface(with: IndexerServiceProtocol.self)
        connection.interruptionHandler = { [weak self] in
            print("ServiceClient: Indexer connection interrupted")
            Task { await self?.handleIndexerInvalidation() }
        }
        connection.invalidationHandler = { [weak self] in
            print("ServiceClient: Indexer connection invalidated")
            Task { await self?.handleIndexerInvalidation() }
        }
        connection.resume()
        indexerConnection = connection
        return connection
    }

    private func getExtractorConnection() throws -> NSXPCConnection {
        if let connection = extractorConnection {
            return connection
        }

        let connection = NSXPCConnection(serviceName: XPCServiceIdentifier.extractor.rawValue)
        connection.remoteObjectInterface = NSXPCInterface(with: ExtractorServiceProtocol.self)
        connection.interruptionHandler = { [weak self] in
            Task { await self?.handleExtractorInvalidation() }
        }
        connection.invalidationHandler = { [weak self] in
            Task { await self?.handleExtractorInvalidation() }
        }
        connection.resume()
        extractorConnection = connection
        return connection
    }

    private func getQueryConnection() throws -> NSXPCConnection {
        if let connection = queryConnection {
            return connection
        }

        let connection = NSXPCConnection(serviceName: XPCServiceIdentifier.query.rawValue)
        connection.remoteObjectInterface = NSXPCInterface(with: QueryServiceProtocol.self)
        connection.interruptionHandler = { [weak self] in
            Task { await self?.handleQueryInvalidation() }
        }
        connection.invalidationHandler = { [weak self] in
            Task { await self?.handleQueryInvalidation() }
        }
        connection.resume()
        queryConnection = connection
        return connection
    }

    private func handleIndexerInvalidation() {
        indexerConnection = nil
    }

    private func handleExtractorInvalidation() {
        extractorConnection = nil
    }

    private func handleQueryInvalidation() {
        queryConnection = nil
    }

    // MARK: - Indexer Operations

    public func startIndexing() async throws {
        let connection = try getIndexerConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Indexer proxy error: \(error)")
        }) as? IndexerServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            proxy.startIndexing { success, error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else if success {
                    continuation.resume()
                } else {
                    continuation.resume(throwing: XPCError.operationFailed("Failed to start indexing"))
                }
            }
        }
    }

    public func stopIndexing() async throws {
        let connection = try getIndexerConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Indexer proxy error: \(error)")
        }) as? IndexerServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            proxy.stopIndexing { success, error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else if success {
                    continuation.resume()
                } else {
                    continuation.resume(throwing: XPCError.operationFailed("Failed to stop indexing"))
                }
            }
        }
    }

    public func reindexFolder(at path: String) async throws {
        let connection = try getIndexerConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Indexer proxy error: \(error)")
        }) as? IndexerServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            proxy.reindexFolder(at: path) { success, error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else if success {
                    continuation.resume()
                } else {
                    continuation.resume(throwing: XPCError.operationFailed("Failed to reindex folder"))
                }
            }
        }
    }

    public func getQueueLength() async throws -> Int {
        let connection = try getIndexerConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Indexer proxy error: \(error)")
        }) as? IndexerServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        return try await withCheckedThrowingContinuation { continuation in
            proxy.getQueueLength { length, error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume(returning: length)
                }
            }
        }
    }

    // MARK: - Query Operations

    public func search(query: SearchQuery) async throws -> [SearchResult] {
        let connection = try getQueryConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Query proxy error: \(error)")
        }) as? QueryServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        let encoder = JSONEncoder()
        guard let queryData = try? encoder.encode(query) else {
            throw XPCError.encodingFailed
        }

        return try await withCheckedThrowingContinuation { continuation in
            proxy.search(queryData: queryData) { resultData, error in
                if let error = error {
                    continuation.resume(throwing: error)
                    return
                }

                guard let data = resultData else {
                    continuation.resume(returning: [])
                    return
                }

                let decoder = JSONDecoder()
                do {
                    let results = try decoder.decode([SearchResult].self, from: data)
                    continuation.resume(returning: results)
                } catch {
                    continuation.resume(throwing: XPCError.decodingFailed)
                }
            }
        }
    }

    public func getIndexHealth() async throws -> IndexHealthSnapshot {
        let connection = try getQueryConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Query proxy error: \(error)")
        }) as? QueryServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        return try await withCheckedThrowingContinuation { continuation in
            proxy.getIndexHealth { data, error in
                if let error = error {
                    continuation.resume(throwing: error)
                    return
                }

                guard let data = data else {
                    continuation.resume(throwing: XPCError.decodingFailed)
                    return
                }

                let decoder = JSONDecoder()
                do {
                    let health = try decoder.decode(IndexHealthSnapshot.self, from: data)
                    continuation.resume(returning: health)
                } catch {
                    continuation.resume(throwing: XPCError.decodingFailed)
                }
            }
        }
    }

    public func recordFeedback(_ feedback: FeedbackEntry) async throws {
        let connection = try getQueryConnection()

        guard let proxy = connection.remoteObjectProxyWithErrorHandler({ error in
            print("ServiceClient: Query proxy error: \(error)")
        }) as? QueryServiceProtocol else {
            throw XPCError.serviceUnavailable
        }

        let encoder = JSONEncoder()
        guard let feedbackData = try? encoder.encode(feedback) else {
            throw XPCError.encodingFailed
        }

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            proxy.recordFeedback(feedbackData: feedbackData) { success, error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else if success {
                    continuation.resume()
                } else {
                    continuation.resume(throwing: XPCError.operationFailed("Failed to record feedback"))
                }
            }
        }
    }

    // MARK: - Cleanup

    public func invalidateAll() {
        indexerConnection?.invalidate()
        extractorConnection?.invalidate()
        queryConnection?.invalidate()
        indexerConnection = nil
        extractorConnection = nil
        queryConnection = nil
    }
}
