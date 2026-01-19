import Foundation

/// Result of scanning a file or directory
public struct ScanResult: Sendable {
    public let path: String
    public let kind: ItemKind
    public let size: Int64
    public let modificationDate: Date
    public let creationDate: Date
    public let owner: String?
    public let flags: UInt32
    public let isHidden: Bool
    public let isReadable: Bool

    public init(
        path: String,
        kind: ItemKind,
        size: Int64,
        modificationDate: Date,
        creationDate: Date,
        owner: String?,
        flags: UInt32,
        isHidden: Bool,
        isReadable: Bool
    ) {
        self.path = path
        self.kind = kind
        self.size = size
        self.modificationDate = modificationDate
        self.creationDate = creationDate
        self.owner = owner
        self.flags = flags
        self.isHidden = isHidden
        self.isReadable = isReadable
    }
}

/// Scans the file system and produces metadata for indexing
public actor FileScanner {
    private let rules: PathRules
    private let fileManager: FileManager

    public init(rules: PathRules) {
        self.rules = rules
        self.fileManager = FileManager.default
    }

    /// Scan a single path and return its metadata
    public func scan(path: String) async throws -> ScanResult? {
        guard rules.classification(for: path) != .exclude else {
            return nil
        }

        return try await Task.detached(priority: .utility) { [fileManager] in
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: path, isDirectory: &isDirectory) else {
                return nil
            }

            let attributes = try fileManager.attributesOfItem(atPath: path)

            let kind: ItemKind
            if isDirectory.boolValue {
                if path.hasSuffix(".app") {
                    kind = .application
                } else if FileScanner.isBundle(at: path) {
                    kind = .bundle
                } else {
                    kind = .folder
                }
            } else {
                let fileType = attributes[.type] as? FileAttributeType
                if fileType == .typeSymbolicLink {
                    kind = .symlink
                } else {
                    kind = .file
                }
            }

            let size = (attributes[.size] as? Int64) ?? 0
            let modDate = (attributes[.modificationDate] as? Date) ?? Date()
            let creationDate = (attributes[.creationDate] as? Date) ?? modDate
            let owner = attributes[.ownerAccountName] as? String

            // Get POSIX permissions as flags
            let posixPermissions = (attributes[.posixPermissions] as? UInt32) ?? 0

            // Check if hidden
            let url = URL(fileURLWithPath: path)
            let resourceValues = try? url.resourceValues(forKeys: [.isHiddenKey])
            let isHidden = resourceValues?.isHidden ?? path.split(separator: "/").last?.hasPrefix(".") ?? false

            // Check if readable
            let isReadable = fileManager.isReadableFile(atPath: path)

            return ScanResult(
                path: path,
                kind: kind,
                size: size,
                modificationDate: modDate,
                creationDate: creationDate,
                owner: owner,
                flags: posixPermissions,
                isHidden: isHidden,
                isReadable: isReadable
            )
        }.value
    }

    /// Enumerate contents of a directory
    public func enumerateDirectory(
        at path: String,
        recursive: Bool = true,
        maxDepth: Int = 50
    ) -> AsyncThrowingStream<ScanResult, Error> {
        AsyncThrowingStream { continuation in
            Task.detached(priority: .utility) { [fileManager, rules] in
                do {
                    let url = URL(fileURLWithPath: path)
                    let keys: [URLResourceKey] = [
                        .isDirectoryKey,
                        .isRegularFileKey,
                        .isSymbolicLinkKey,
                        .fileSizeKey,
                        .contentModificationDateKey,
                        .creationDateKey,
                        .isHiddenKey,
                        .isReadableKey,
                    ]

                    let options: FileManager.DirectoryEnumerationOptions = [
                        .skipsPackageDescendants,
                    ]

                    guard let enumerator = fileManager.enumerator(
                        at: url,
                        includingPropertiesForKeys: keys,
                        options: options
                    ) else {
                        continuation.finish()
                        return
                    }

                    for case let itemURL as URL in enumerator {
                        let itemPath = itemURL.path

                        // Check depth
                        let depth = itemPath.dropFirst(path.count).filter { $0 == "/" }.count
                        if depth > maxDepth {
                            enumerator.skipDescendants()
                            continue
                        }

                        // Apply exclusion rules
                        if rules.shouldExclude(itemPath) {
                            if (try? itemURL.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                                enumerator.skipDescendants()
                            }
                            continue
                        }

                        // Get resource values
                        guard let values = try? itemURL.resourceValues(forKeys: Set(keys)) else {
                            continue
                        }

                        let kind: ItemKind
                        if values.isSymbolicLink == true {
                            kind = .symlink
                        } else if values.isDirectory == true {
                            if itemPath.hasSuffix(".app") {
                                kind = .application
                            } else {
                                kind = .folder
                            }
                        } else {
                            kind = .file
                        }

                        let result = ScanResult(
                            path: itemPath,
                            kind: kind,
                            size: Int64(values.fileSize ?? 0),
                            modificationDate: values.contentModificationDate ?? Date(),
                            creationDate: values.creationDate ?? Date(),
                            owner: nil,
                            flags: 0,
                            isHidden: values.isHidden ?? false,
                            isReadable: values.isReadable ?? true
                        )

                        continuation.yield(result)
                    }

                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
        }
    }

    /// List top-level contents of home directory for initial setup
    public func listHomeDirectory() async throws -> [ScanResult] {
        let home = fileManager.homeDirectoryForCurrentUser.path
        var results: [ScanResult] = []

        let contents = try fileManager.contentsOfDirectory(atPath: home)

        for name in contents.sorted() {
            let path = (home as NSString).appendingPathComponent(name)
            if let result = try await scan(path: path) {
                results.append(result)
            }
        }

        return results
    }

    private static func isBundle(at path: String) -> Bool {
        let bundleExtensions = ["app", "framework", "bundle", "plugin", "kext", "xpc"]
        let ext = (path as NSString).pathExtension.lowercased()
        return bundleExtensions.contains(ext)
    }
}
