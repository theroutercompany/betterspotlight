import Foundation

/// XPC Service for background indexing
public final class IndexerService: NSObject, IndexerServiceProtocol {
    private let store: SQLiteStore
    private let scanner: FileScanner
    private let watcher: FSEventsWatcher?
    private let extractionManager: ExtractionManager

    private var isIndexing = false
    private var isPaused = false
    private var workQueue: [IndexWorkItem] = []
    private let workQueueLock = NSLock()
    private var processingTask: Task<Void, Never>?

    private let settings: AppSettings

    public init(settings: AppSettings) throws {
        self.settings = settings
        self.store = try SQLiteStore()

        let rules = PathRules(
            exclusionPatterns: settings.exclusionPatterns,
            sensitivePatterns: SensitiveFolderConfig.defaultPatterns,
            overrides: Dictionary(uniqueKeysWithValues: settings.indexRoots.map { ($0.path, $0.classification) })
        )

        self.scanner = FileScanner(rules: rules)
        self.extractionManager = ExtractionManager(settings: settings.indexing)

        // Set up FSEvents watcher for configured roots
        let watchPaths = settings.indexRoots
            .filter { $0.classification != .exclude }
            .map { $0.path }

        if !watchPaths.isEmpty {
            self.watcher = FSEventsWatcher(
                paths: watchPaths,
                latency: TimeInterval(settings.indexing.debounceDelayMs) / 1000.0
            )
        } else {
            self.watcher = nil
        }

        super.init()

        watcher?.delegate = self
    }

    // MARK: - IndexerServiceProtocol

    public func startIndexing(reply: @escaping (Bool, Error?) -> Void) {
        guard !isIndexing else {
            reply(true, nil)
            return
        }

        isIndexing = true
        isPaused = false

        do {
            try watcher?.start()
            startProcessingQueue()

            // Queue initial scan of all roots
            Task {
                await queueInitialScan()
            }

            reply(true, nil)
        } catch {
            isIndexing = false
            reply(false, error)
        }
    }

    public func stopIndexing(reply: @escaping (Bool, Error?) -> Void) {
        isIndexing = false
        isPaused = false
        watcher?.stop()
        processingTask?.cancel()
        processingTask = nil

        workQueueLock.lock()
        workQueue.removeAll()
        workQueueLock.unlock()

        reply(true, nil)
    }

    public func pauseIndexing(reply: @escaping (Bool, Error?) -> Void) {
        isPaused = true
        reply(true, nil)
    }

    public func resumeIndexing(reply: @escaping (Bool, Error?) -> Void) {
        isPaused = false
        reply(true, nil)
    }

    public func reindexFolder(at path: String, reply: @escaping (Bool, Error?) -> Void) {
        Task {
            do {
                // Remove existing items under this path
                // (Would need to add a method to SQLiteStore for this)

                // Queue scan of the folder
                await queueScan(of: path)
                reply(true, nil)
            } catch {
                reply(false, error)
            }
        }
    }

    public func rebuildIndex(reply: @escaping (Bool, Error?) -> Void) {
        Task {
            do {
                // Stop current indexing
                stopIndexing { _, _ in }

                // Clear all data
                // (Would need to add a vacuum/clear method to SQLiteStore)

                // Restart
                startIndexing(reply: reply)
            }
        }
    }

    public func getQueueLength(reply: @escaping (Int, Error?) -> Void) {
        workQueueLock.lock()
        let length = workQueue.count
        workQueueLock.unlock()
        reply(length, nil)
    }

    public func getStatistics(reply: @escaping (Data?, Error?) -> Void) {
        Task {
            do {
                let itemCount = try await store.getTotalItemCount()
                let contentCount = try await store.getTotalContentChunkCount()
                let failuresByType = try await store.getFailureCountByType()

                let stats = IndexingStats(
                    itemsIndexed: itemCount,
                    itemsFailed: Int64(failuresByType.values.reduce(0, +)),
                    bytesProcessed: 0,
                    averageExtractionTimeMs: 0,
                    extractionTimesByType: [:],
                    queueLengthHistory: []
                )

                let data = try JSONEncoder().encode(stats)
                reply(data, nil)
            } catch {
                reply(nil, error)
            }
        }
    }

    // MARK: - Private Methods

    private func queueInitialScan() async {
        for root in settings.indexRoots where root.classification != .exclude {
            await queueScan(of: root.path)
        }
    }

    private func queueScan(of path: String) async {
        let stream = await scanner.enumerateDirectory(at: path)

        do {
            for try await result in stream {
                enqueueWork(.index(result))
            }
        } catch {
            // Log error
            print("Scan error for \(path): \(error)")
        }
    }

    private func enqueueWork(_ item: IndexWorkItem) {
        workQueueLock.lock()

        // Priority ordering: prefer recent files and user-visible paths
        switch item {
        case .index(let result):
            // Insert at position based on modification time
            let insertIndex = workQueue.firstIndex { existing in
                if case .index(let existingResult) = existing {
                    return result.modificationDate > existingResult.modificationDate
                }
                return false
            } ?? workQueue.endIndex
            workQueue.insert(item, at: insertIndex)

        case .delete:
            // Deletes go to front
            workQueue.insert(item, at: 0)

        case .update(let result):
            // Updates based on modification time
            let insertIndex = workQueue.firstIndex { existing in
                if case .index(let existingResult) = existing {
                    return result.modificationDate > existingResult.modificationDate
                }
                return false
            } ?? workQueue.endIndex
            workQueue.insert(item, at: insertIndex)
        }

        workQueueLock.unlock()
    }

    private func startProcessingQueue() {
        processingTask = Task.detached(priority: .utility) { [weak self] in
            while !Task.isCancelled {
                guard let self = self else { break }

                // Check if paused
                if self.isPaused {
                    try? await Task.sleep(nanoseconds: 100_000_000) // 100ms
                    continue
                }

                // Get next work item
                self.workQueueLock.lock()
                let item = self.workQueue.isEmpty ? nil : self.workQueue.removeFirst()
                self.workQueueLock.unlock()

                guard let workItem = item else {
                    // No work, sleep briefly
                    try? await Task.sleep(nanoseconds: 50_000_000) // 50ms
                    continue
                }

                // Process the item
                await self.processWorkItem(workItem)
            }
        }
    }

    private func processWorkItem(_ item: IndexWorkItem) async {
        switch item {
        case .index(let result), .update(let result):
            await indexFile(result)

        case .delete(let path):
            do {
                try await store.deleteItem(path: path)
            } catch {
                print("Failed to delete \(path): \(error)")
            }
        }
    }

    private func indexFile(_ result: ScanResult) async {
        do {
            // Create or update the item
            let sensitivity = PathRules().sensitivityLevel(for: result.path)

            let existingItem = try await store.getItem(path: result.path)

            if let existing = existingItem {
                // Check if we need to update
                if existing.modificationDate >= result.modificationDate {
                    return // Already up to date
                }

                // Update item
                let updated = IndexItem(
                    id: existing.id,
                    path: result.path,
                    kind: result.kind,
                    size: result.size,
                    modificationDate: result.modificationDate,
                    creationDate: result.creationDate,
                    owner: result.owner,
                    flags: result.flags,
                    contentHash: nil,
                    sensitivity: sensitivity
                )
                try await store.updateItem(updated)

                // Re-extract content if needed
                if result.kind == .file && result.isReadable {
                    await extractAndIndexContent(itemId: existing.id, path: result.path)
                }
            } else {
                // Insert new item
                let newItem = IndexItem(
                    id: 0, // Will be auto-assigned
                    path: result.path,
                    kind: result.kind,
                    size: result.size,
                    modificationDate: result.modificationDate,
                    creationDate: result.creationDate,
                    owner: result.owner,
                    flags: result.flags,
                    contentHash: nil,
                    sensitivity: sensitivity
                )
                let itemId = try await store.insertItem(newItem)

                // Extract content for files
                if result.kind == .file && result.isReadable {
                    await extractAndIndexContent(itemId: itemId, path: result.path)
                }
            }
        } catch {
            // Record failure
            let failure = IndexFailure(
                path: result.path,
                stage: "index",
                error: error.localizedDescription
            )
            try? await store.recordFailure(failure)
        }
    }

    private func extractAndIndexContent(itemId: Int64, path: String) async {
        let url = URL(fileURLWithPath: path)

        do {
            guard let extractionResult = await extractionManager.extractIfSupported(from: url) else {
                return // Unsupported file type
            }

            // Delete old content
            try await store.deleteContent(forItemId: itemId)

            // Insert new chunks
            for chunk in extractionResult.chunks {
                let contentChunk = ContentChunk(
                    id: 0,
                    itemId: itemId,
                    chunkIndex: chunk.index,
                    textHash: chunk.text.hashValue.description,
                    snippet: String(chunk.text.prefix(200)),
                    startOffset: chunk.startOffset,
                    endOffset: chunk.endOffset
                )
                _ = try await store.insertContent(contentChunk)
            }

            // TODO: Index in FTS5 lexical index
            // TODO: Optionally compute embeddings for vector index

        } catch {
            let failure = IndexFailure(
                path: path,
                itemId: itemId,
                stage: "extraction",
                error: error.localizedDescription
            )
            try? await store.recordFailure(failure)
        }
    }
}

// MARK: - FSEventsWatcherDelegate

extension IndexerService: FSEventsWatcherDelegate {
    public func watcher(_ watcher: FSEventsWatcher, didReceiveEvents events: [FSEvent]) {
        for event in events {
            if event.isRemoved {
                enqueueWork(.delete(event.path))
            } else if event.isCreated || event.isModified || event.isRenamed {
                // Need to scan the path to get full metadata
                Task {
                    if let result = try? await scanner.scan(path: event.path) {
                        if event.isCreated {
                            enqueueWork(.index(result))
                        } else {
                            enqueueWork(.update(result))
                        }
                    }
                }
            }
        }
    }

    public func watcher(_ watcher: FSEventsWatcher, didFailWithError error: Error) {
        print("FSEvents watcher error: \(error)")
    }
}

// MARK: - Work Item Types

private enum IndexWorkItem {
    case index(ScanResult)
    case update(ScanResult)
    case delete(String)
}
