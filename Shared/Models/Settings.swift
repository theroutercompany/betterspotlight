import Foundation

/// Root configuration for indexing
public struct IndexRoot: Codable, Sendable, Identifiable, Hashable {
    public var id: String { path }

    public let path: String
    public var classification: FolderClassification
    public var isUserOverride: Bool

    public init(path: String, classification: FolderClassification, isUserOverride: Bool = false) {
        self.path = path
        self.classification = classification
        self.isUserOverride = isUserOverride
    }
}

/// Exclusion pattern configuration
public struct ExclusionPattern: Codable, Sendable, Identifiable, Hashable {
    public let id: UUID
    public let pattern: String
    public let isRegex: Bool
    public let isBuiltIn: Bool
    public var isEnabled: Bool
    public let description: String?

    public init(
        id: UUID = UUID(),
        pattern: String,
        isRegex: Bool = false,
        isBuiltIn: Bool = false,
        isEnabled: Bool = true,
        description: String? = nil
    ) {
        self.id = id
        self.pattern = pattern
        self.isRegex = isRegex
        self.isBuiltIn = isBuiltIn
        self.isEnabled = isEnabled
        self.description = description
    }
}

/// Privacy settings for sensitive folders
public struct SensitiveFolderConfig: Codable, Sendable, Hashable {
    public let path: String
    public var allowContentSearch: Bool
    public var allowPreviews: Bool
    public var allowEmbeddings: Bool

    public init(
        path: String,
        allowContentSearch: Bool = false,
        allowPreviews: Bool = false,
        allowEmbeddings: Bool = false
    ) {
        self.path = path
        self.allowContentSearch = allowContentSearch
        self.allowPreviews = allowPreviews
        self.allowEmbeddings = allowEmbeddings
    }

    /// Default sensitive folder patterns
    public static let defaultPatterns: [String] = [
        ".ssh",
        ".gnupg",
        ".aws",
        ".kube",
        ".netrc",
        ".env",
    ]
}

/// Main application settings
public struct AppSettings: Codable, Sendable {
    public var indexRoots: [IndexRoot]
    public var exclusionPatterns: [ExclusionPattern]
    public var sensitiveFolders: [SensitiveFolderConfig]
    public var hotkey: HotkeyConfig
    public var indexing: IndexingSettings
    public var search: SearchSettings
    public var privacy: PrivacySettings

    public init(
        indexRoots: [IndexRoot] = [],
        exclusionPatterns: [ExclusionPattern] = ExclusionPattern.builtInPatterns,
        sensitiveFolders: [SensitiveFolderConfig] = [],
        hotkey: HotkeyConfig = HotkeyConfig(),
        indexing: IndexingSettings = IndexingSettings(),
        search: SearchSettings = SearchSettings(),
        privacy: PrivacySettings = PrivacySettings()
    ) {
        self.indexRoots = indexRoots
        self.exclusionPatterns = exclusionPatterns
        self.sensitiveFolders = sensitiveFolders
        self.hotkey = hotkey
        self.indexing = indexing
        self.search = search
        self.privacy = privacy
    }
}

/// Hotkey configuration
public struct HotkeyConfig: Codable, Sendable {
    public var keyCode: UInt16
    public var modifiers: UInt32

    public init(keyCode: UInt16 = 49, modifiers: UInt32 = 1048840) {
        // Default: Command + Space (keyCode 49 = Space, modifiers = Cmd)
        self.keyCode = keyCode
        self.modifiers = modifiers
    }
}

/// Indexing behavior settings
public struct IndexingSettings: Codable, Sendable {
    public var maxFileSizeMB: Int
    public var debounceDelayMs: Int
    public var batchSize: Int
    public var enableOCR: Bool
    public var enableSemanticIndex: Bool
    public var maxConcurrentExtractions: Int

    public init(
        maxFileSizeMB: Int = 50,
        debounceDelayMs: Int = 100,
        batchSize: Int = 100,
        enableOCR: Bool = false,
        enableSemanticIndex: Bool = false,
        maxConcurrentExtractions: Int = 4
    ) {
        self.maxFileSizeMB = maxFileSizeMB
        self.debounceDelayMs = debounceDelayMs
        self.batchSize = batchSize
        self.enableOCR = enableOCR
        self.enableSemanticIndex = enableSemanticIndex
        self.maxConcurrentExtractions = maxConcurrentExtractions
    }
}

/// Search behavior settings
public struct SearchSettings: Codable, Sendable {
    public var maxResults: Int
    public var debounceDelayMs: Int
    public var enableFuzzyMatching: Bool
    public var boostRecentFiles: Bool
    public var boostFrequentFiles: Bool

    public init(
        maxResults: Int = 50,
        debounceDelayMs: Int = 30,
        enableFuzzyMatching: Bool = true,
        boostRecentFiles: Bool = true,
        boostFrequentFiles: Bool = true
    ) {
        self.maxResults = maxResults
        self.debounceDelayMs = debounceDelayMs
        self.enableFuzzyMatching = enableFuzzyMatching
        self.boostRecentFiles = boostRecentFiles
        self.boostFrequentFiles = boostFrequentFiles
    }
}

/// Privacy-related settings
public struct PrivacySettings: Codable, Sendable {
    public var enableFeedbackLogging: Bool
    public var feedbackRetentionDays: Int
    public var maskSensitivePreviews: Bool

    public init(
        enableFeedbackLogging: Bool = true,
        feedbackRetentionDays: Int = 30,
        maskSensitivePreviews: Bool = true
    ) {
        self.enableFeedbackLogging = enableFeedbackLogging
        self.feedbackRetentionDays = feedbackRetentionDays
        self.maskSensitivePreviews = maskSensitivePreviews
    }
}

// MARK: - Built-in Patterns

extension ExclusionPattern {
    /// Built-in exclusion patterns for common noise sources
    public static let builtInPatterns: [ExclusionPattern] = [
        // Cloud storage folders
        ExclusionPattern(pattern: "Library/Mobile Documents", isBuiltIn: true, description: "iCloud Drive"),
        ExclusionPattern(pattern: "Library/CloudStorage", isBuiltIn: true, description: "Cloud storage mount points"),
        ExclusionPattern(pattern: "Dropbox", isBuiltIn: true, description: "Dropbox folder"),
        ExclusionPattern(pattern: "Google Drive", isBuiltIn: true, description: "Google Drive folder"),
        ExclusionPattern(pattern: "OneDrive", isBuiltIn: true, description: "OneDrive folder"),

        // System and library
        ExclusionPattern(pattern: "Library/Caches", isBuiltIn: true, description: "System caches"),
        ExclusionPattern(pattern: "Library/Logs", isBuiltIn: true, description: "System logs"),
        ExclusionPattern(pattern: ".Trash", isBuiltIn: true, description: "Trash folder"),

        // Package managers and build tools
        ExclusionPattern(pattern: "node_modules", isBuiltIn: true, description: "Node.js modules"),
        ExclusionPattern(pattern: ".npm", isBuiltIn: true, description: "npm cache"),
        ExclusionPattern(pattern: ".yarn", isBuiltIn: true, description: "Yarn cache"),
        ExclusionPattern(pattern: ".pnpm-store", isBuiltIn: true, description: "pnpm store"),
        ExclusionPattern(pattern: "target", isBuiltIn: true, description: "Rust/Maven build output"),
        ExclusionPattern(pattern: "build", isBuiltIn: true, description: "Build output"),
        ExclusionPattern(pattern: "dist", isBuiltIn: true, description: "Distribution output"),
        ExclusionPattern(pattern: ".next", isBuiltIn: true, description: "Next.js build"),
        ExclusionPattern(pattern: ".nuxt", isBuiltIn: true, description: "Nuxt.js build"),
        ExclusionPattern(pattern: ".svelte-kit", isBuiltIn: true, description: "SvelteKit build"),
        ExclusionPattern(pattern: ".turbo", isBuiltIn: true, description: "Turborepo cache"),
        ExclusionPattern(pattern: ".nx", isBuiltIn: true, description: "Nx cache"),
        ExclusionPattern(pattern: ".direnv", isBuiltIn: true, description: "direnv cache"),
        ExclusionPattern(pattern: "coverage", isBuiltIn: true, description: "Test coverage"),
        ExclusionPattern(pattern: "__pycache__", isBuiltIn: true, description: "Python cache"),
        ExclusionPattern(pattern: ".pytest_cache", isBuiltIn: true, description: "pytest cache"),
        ExclusionPattern(pattern: "venv", isBuiltIn: true, description: "Python virtual environment"),
        ExclusionPattern(pattern: ".venv", isBuiltIn: true, description: "Python virtual environment"),
        ExclusionPattern(pattern: "Pods", isBuiltIn: true, description: "CocoaPods"),
        ExclusionPattern(pattern: ".gradle", isBuiltIn: true, description: "Gradle cache"),
        ExclusionPattern(pattern: ".m2", isBuiltIn: true, description: "Maven cache"),
        ExclusionPattern(pattern: ".cargo", isBuiltIn: true, description: "Cargo cache"),

        // IDE and editor
        ExclusionPattern(pattern: ".idea", isBuiltIn: true, description: "JetBrains IDE"),
        ExclusionPattern(pattern: ".vscode", isBuiltIn: true, description: "VS Code settings"),
        ExclusionPattern(pattern: "*.xcodeproj", isRegex: false, isBuiltIn: true, description: "Xcode projects"),
        ExclusionPattern(pattern: "DerivedData", isBuiltIn: true, description: "Xcode derived data"),

        // Version control
        ExclusionPattern(pattern: ".git/objects", isBuiltIn: true, description: "Git objects"),

        // General caches
        ExclusionPattern(pattern: ".cache", isBuiltIn: true, description: "Generic cache folder"),
    ]
}
