// DEPRECATED SWIFT REFERENCE
// Qt/C++ is the source of truth.
// Keep this file only as temporary migration reference while parity items are closed.
// Do not add new features or fixes here.

import Foundation

/// Evaluates paths against configured rules to determine indexing behavior
public struct PathRules: Sendable {
    private let exclusionPatterns: [ExclusionPattern]
    private let sensitivePatterns: [String]
    private let overrides: [String: FolderClassification]
    private let compiledPatterns: [CompiledPattern]

    public init(
        exclusionPatterns: [ExclusionPattern] = ExclusionPattern.builtInPatterns,
        sensitivePatterns: [String] = SensitiveFolderConfig.defaultPatterns,
        overrides: [String: FolderClassification] = [:]
    ) {
        self.exclusionPatterns = exclusionPatterns.filter { $0.isEnabled }
        self.sensitivePatterns = sensitivePatterns
        self.overrides = overrides
        self.compiledPatterns = self.exclusionPatterns.compactMap { pattern in
            CompiledPattern(from: pattern)
        }
    }

    /// Check if a path should be excluded from indexing
    public func shouldExclude(_ path: String) -> Bool {
        // Check overrides first (user overrides beat all rules)
        if let override = findOverride(for: path) {
            return override == .exclude
        }

        // Check against exclusion patterns
        for compiled in compiledPatterns {
            if compiled.matches(path) {
                return true
            }
        }

        return false
    }

    /// Get the classification for a path
    public func classification(for path: String) -> FolderClassification {
        // Check overrides first
        if let override = findOverride(for: path) {
            return override
        }

        // Check exclusions
        if shouldExclude(path) {
            return .exclude
        }

        // Default to index
        return .index
    }

    /// Check if a path is sensitive
    public func isSensitive(_ path: String) -> Bool {
        let filename = (path as NSString).lastPathComponent

        for pattern in sensitivePatterns {
            if pattern.hasPrefix(".") {
                // Match dotfiles
                if filename == pattern || filename.hasPrefix(pattern) {
                    return true
                }
            } else if filename.contains(pattern) {
                return true
            }
        }

        return false
    }

    /// Get sensitivity level for a path
    public func sensitivityLevel(for path: String) -> SensitivityLevel {
        if isSensitive(path) {
            return .sensitive
        }
        return .normal
    }

    private func findOverride(for path: String) -> FolderClassification? {
        // Find the most specific override that applies
        var bestMatch: (path: String, classification: FolderClassification)?

        for (overridePath, classification) in overrides {
            if path.hasPrefix(overridePath) || path == overridePath {
                if bestMatch == nil || overridePath.count > bestMatch!.path.count {
                    bestMatch = (overridePath, classification)
                }
            }
        }

        return bestMatch?.classification
    }
}

/// Pre-compiled pattern for efficient matching
private struct CompiledPattern: Sendable {
    let original: ExclusionPattern
    let regex: NSRegularExpression?
    let simplePattern: String?

    init?(from pattern: ExclusionPattern) {
        self.original = pattern

        if pattern.isRegex {
            do {
                self.regex = try NSRegularExpression(pattern: pattern.pattern, options: [])
                self.simplePattern = nil
            } catch {
                return nil
            }
        } else {
            self.regex = nil
            self.simplePattern = pattern.pattern
        }
    }

    func matches(_ path: String) -> Bool {
        if let regex = regex {
            let range = NSRange(path.startIndex..., in: path)
            return regex.firstMatch(in: path, options: [], range: range) != nil
        }

        if let simple = simplePattern {
            // Simple substring/component matching
            let components = path.split(separator: "/").map(String.init)

            // Check if any path component matches the pattern
            for component in components {
                if component == simple {
                    return true
                }
                // Handle glob patterns like "*.xcodeproj"
                if simple.hasPrefix("*") {
                    let suffix = String(simple.dropFirst())
                    if component.hasSuffix(suffix) {
                        return true
                    }
                }
            }

            // Also check if the pattern appears as a path segment
            if path.contains("/\(simple)/") || path.hasSuffix("/\(simple)") {
                return true
            }
        }

        return false
    }
}

// MARK: - Cloud Folder Detection

extension PathRules {
    /// Known cloud storage folder patterns
    public static let cloudFolderPatterns: [String] = [
        "Library/Mobile Documents",     // iCloud Drive
        "Library/CloudStorage",         // macOS cloud storage mount
        "Dropbox",
        "Google Drive",
        "OneDrive",
        "Box",
        "pCloud",
        "MEGA",
        "Sync",                          // Sync.com
    ]

    /// Check if a path is within a cloud storage folder
    public static func isCloudFolder(_ path: String) -> Bool {
        let home = FileManager.default.homeDirectoryForCurrentUser.path

        for pattern in cloudFolderPatterns {
            let cloudPath = (home as NSString).appendingPathComponent(pattern)
            if path.hasPrefix(cloudPath) || path == cloudPath {
                return true
            }
        }

        return false
    }
}

// MARK: - Repo Detection

extension PathRules {
    /// Git repository markers
    private static let repoMarkers = [".git", ".hg", ".svn"]

    /// Detect if a path is within a repository and find the root
    public static func findRepoRoot(for path: String) -> String? {
        var currentPath = path
        let fileManager = FileManager.default

        while currentPath != "/" && !currentPath.isEmpty {
            for marker in repoMarkers {
                let markerPath = (currentPath as NSString).appendingPathComponent(marker)
                var isDirectory: ObjCBool = false
                if fileManager.fileExists(atPath: markerPath, isDirectory: &isDirectory) {
                    return currentPath
                }
            }
            currentPath = (currentPath as NSString).deletingLastPathComponent
        }

        return nil
    }

    /// Check if a path is a repo root
    public static func isRepoRoot(_ path: String) -> Bool {
        let fileManager = FileManager.default
        for marker in repoMarkers {
            let markerPath = (path as NSString).appendingPathComponent(marker)
            if fileManager.fileExists(atPath: markerPath) {
                return true
            }
        }
        return false
    }
}

// MARK: - Home Directory Classification

extension PathRules {
    /// Suggested classification for common home directory folders
    public static func suggestedClassification(for folderName: String) -> FolderClassification {
        switch folderName {
        // Definitely index
        case "Documents", "Desktop", "Downloads", "Projects", "Developer", "Code", "src", "work":
            return .index

        // Exclude by default
        case "Library", "Applications", "Public", ".Trash":
            return .exclude

        // Cloud folders - exclude
        case "Dropbox", "Google Drive", "OneDrive", "iCloud Drive":
            return .exclude

        // Common developer folders - index
        case ".config", ".local":
            return .metadataOnly

        // Dotfolders that might have useful content
        case ".zshrc", ".bashrc", ".gitconfig", ".vimrc":
            return .index

        // Large cache folders - exclude
        case ".npm", ".yarn", ".cargo", ".m2", ".gradle", ".cache":
            return .exclude

        // Sensitive folders - metadata only
        case ".ssh", ".gnupg", ".aws", ".kube":
            return .metadataOnly

        default:
            // For unknown dotfolders, default to metadata only
            if folderName.hasPrefix(".") {
                return .metadataOnly
            }
            // For unknown regular folders, default to index
            return .index
        }
    }
}
