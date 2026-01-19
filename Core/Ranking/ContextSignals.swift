import Foundation
import AppKit

/// Gathers context signals for ranking adjustments
public actor ContextSignalProvider {
    private var recentPaths: [String] = []
    private let maxRecentPaths = 50

    public init() {}

    /// Get the current query context
    public func getCurrentContext() async -> QueryContext {
        let frontmostApp = await getFrontmostApp()
        let cwd = getCurrentWorkingDirectory()

        return QueryContext(
            frontmostAppBundleId: frontmostApp?.bundleIdentifier,
            frontmostAppName: frontmostApp?.localizedName,
            currentWorkingDirectory: cwd,
            recentPaths: recentPaths
        )
    }

    /// Record a path access
    public func recordPathAccess(_ path: String) {
        // Remove if already present
        recentPaths.removeAll { $0 == path }

        // Add to front
        recentPaths.insert(path, at: 0)

        // Trim to max size
        if recentPaths.count > maxRecentPaths {
            recentPaths = Array(recentPaths.prefix(maxRecentPaths))
        }
    }

    /// Clear recent paths
    public func clearRecentPaths() {
        recentPaths.removeAll()
    }

    // MARK: - Private Helpers

    private func getFrontmostApp() async -> NSRunningApplication? {
        await MainActor.run {
            NSWorkspace.shared.frontmostApplication
        }
    }

    private func getCurrentWorkingDirectory() -> String? {
        // Try to get CWD from environment
        if let pwd = ProcessInfo.processInfo.environment["PWD"] {
            return pwd
        }

        // Fall back to current directory
        return FileManager.default.currentDirectoryPath
    }
}

/// Detects context from the frontmost application
public struct AppContextDetector: Sendable {
    public init() {}

    /// Get context hints based on the frontmost app
    public func getContextHints(bundleId: String?, appName: String?) -> AppContextHints {
        guard let bundleId = bundleId?.lowercased() ?? appName?.lowercased() else {
            return AppContextHints()
        }

        // IDE/Editor detection
        if isIDE(bundleId) {
            return AppContextHints(
                preferredFileTypes: ["swift", "py", "js", "ts", "go", "rs", "java", "cpp", "c", "h", "rb"],
                preferredPaths: ["~/Developer", "~/Projects", "~/Code", "~/src"],
                isDevContext: true
            )
        }

        // Terminal detection
        if isTerminal(bundleId) {
            return AppContextHints(
                preferredFileTypes: ["sh", "bash", "zsh", "fish", "yaml", "json", "toml"],
                preferredPaths: [],
                isDevContext: true,
                isTerminalContext: true
            )
        }

        // Document apps
        if isDocumentApp(bundleId) {
            return AppContextHints(
                preferredFileTypes: ["md", "txt", "doc", "docx", "pdf", "rtf", "pages"],
                preferredPaths: ["~/Documents", "~/Desktop"],
                isDocumentContext: true
            )
        }

        // Design apps
        if isDesignApp(bundleId) {
            return AppContextHints(
                preferredFileTypes: ["sketch", "fig", "psd", "ai", "svg", "png", "jpg"],
                preferredPaths: ["~/Documents", "~/Desktop", "~/Pictures"],
                isDesignContext: true
            )
        }

        return AppContextHints()
    }

    private func isIDE(_ id: String) -> Bool {
        let idePatterns = [
            "xcode", "com.apple.dt.xcode",
            "vscode", "visual studio code", "com.microsoft.vscode",
            "intellij", "webstorm", "pycharm", "rubymine", "goland", "rider",
            "sublime", "com.sublimetext",
            "atom", "com.github.atom",
            "nova", "com.panic.nova",
            "bbedit", "com.barebones.bbedit",
            "textmate", "com.macromates.textmate",
            "cursor", "com.todesktop.cursor"
        ]
        return idePatterns.contains { id.contains($0) }
    }

    private func isTerminal(_ id: String) -> Bool {
        let terminalPatterns = [
            "terminal", "com.apple.terminal",
            "iterm", "com.googlecode.iterm2",
            "alacritty", "io.alacritty",
            "kitty", "net.kovidgoyal.kitty",
            "warp", "dev.warp.warp-stable",
            "hyper", "co.zeit.hyper"
        ]
        return terminalPatterns.contains { id.contains($0) }
    }

    private func isDocumentApp(_ id: String) -> Bool {
        let docPatterns = [
            "pages", "com.apple.iwork.pages",
            "word", "com.microsoft.word",
            "notion", "notion.id",
            "obsidian", "md.obsidian",
            "bear", "net.shinyfrog.bear",
            "ulysses", "com.ulyssesapp",
            "ia writer", "pro.writer.mac"
        ]
        return docPatterns.contains { id.contains($0) }
    }

    private func isDesignApp(_ id: String) -> Bool {
        let designPatterns = [
            "sketch", "com.bohemiancoding.sketch",
            "figma", "com.figma.desktop",
            "photoshop", "com.adobe.photoshop",
            "illustrator", "com.adobe.illustrator",
            "affinity", "com.seriflabs",
            "pixelmator", "com.pixelmatorteam"
        ]
        return designPatterns.contains { id.contains($0) }
    }
}

/// Context hints based on the current app
public struct AppContextHints: Sendable {
    public let preferredFileTypes: [String]
    public let preferredPaths: [String]
    public let isDevContext: Bool
    public let isTerminalContext: Bool
    public let isDocumentContext: Bool
    public let isDesignContext: Bool

    public init(
        preferredFileTypes: [String] = [],
        preferredPaths: [String] = [],
        isDevContext: Bool = false,
        isTerminalContext: Bool = false,
        isDocumentContext: Bool = false,
        isDesignContext: Bool = false
    ) {
        self.preferredFileTypes = preferredFileTypes
        self.preferredPaths = preferredPaths
        self.isDevContext = isDevContext
        self.isTerminalContext = isTerminalContext
        self.isDocumentContext = isDocumentContext
        self.isDesignContext = isDesignContext
    }
}

/// Tracks user interaction patterns for smarter ranking
public actor InteractionTracker {
    private var queryPatterns: [String: QueryPattern] = [:]
    private var fileTypePreferences: [String: Int] = [:]
    private var pathPreferences: [String: Int] = [:]

    public init() {}

    /// Record a query and the selected result
    public func recordSelection(query: String, selectedPath: String, position: Int) {
        // Update query pattern
        let queryLower = query.lowercased()
        if var pattern = queryPatterns[queryLower] {
            pattern.selectedPaths.append(selectedPath)
            pattern.averagePosition = (pattern.averagePosition * Double(pattern.count) + Double(position)) / Double(pattern.count + 1)
            pattern.count += 1
            queryPatterns[queryLower] = pattern
        } else {
            queryPatterns[queryLower] = QueryPattern(
                query: queryLower,
                selectedPaths: [selectedPath],
                averagePosition: Double(position),
                count: 1
            )
        }

        // Update file type preference
        let ext = (selectedPath as NSString).pathExtension.lowercased()
        if !ext.isEmpty {
            fileTypePreferences[ext, default: 0] += 1
        }

        // Update path preference (use parent directory)
        let parentPath = (selectedPath as NSString).deletingLastPathComponent
        pathPreferences[parentPath, default: 0] += 1
    }

    /// Get boost for a file type based on user history
    public func fileTypeBoost(for ext: String) -> Double {
        let count = fileTypePreferences[ext.lowercased(), default: 0]
        if count == 0 { return 0 }
        if count < 5 { return 5 }
        if count < 20 { return 10 }
        return 15
    }

    /// Get boost for a path based on user history
    public func pathBoost(for path: String) -> Double {
        let parentPath = (path as NSString).deletingLastPathComponent
        let count = pathPreferences[parentPath, default: 0]
        if count == 0 { return 0 }
        if count < 5 { return 5 }
        if count < 20 { return 10 }
        return 15
    }

    /// Check if a specific result was previously selected for a query
    public func wasSelectedFor(query: String, path: String) -> Bool {
        queryPatterns[query.lowercased()]?.selectedPaths.contains(path) ?? false
    }
}

/// Pattern tracking for queries
private struct QueryPattern {
    let query: String
    var selectedPaths: [String]
    var averagePosition: Double
    var count: Int
}
