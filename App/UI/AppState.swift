import Foundation
import Shared
import SwiftUI
import Shared

/// Central application state
@MainActor
public final class AppState: ObservableObject {
    // MARK: - Published State

    @Published var settings: AppSettings
    @Published var indexHealth: IndexHealthSnapshot?
    @Published var isIndexing = false
    @Published var hasCompletedOnboarding = false
    @Published var hasFullDiskAccess = false

    // MARK: - Services

    private var serviceClient: ServiceClient?

    // MARK: - Persistence

    private let settingsURL: URL

    // MARK: - Initialization

    public init() {
        // Set up settings persistence
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let appFolder = appSupport.appendingPathComponent("BetterSpotlight")
        try? FileManager.default.createDirectory(at: appFolder, withIntermediateDirectories: true)
        settingsURL = appFolder.appendingPathComponent("settings.json")

        // Load settings
        if let data = try? Data(contentsOf: settingsURL),
           let loaded = try? JSONDecoder().decode(AppSettings.self, from: data) {
            settings = loaded
        } else {
            settings = AppSettings.defaultSettings()
        }

        // Load onboarding state
        hasCompletedOnboarding = UserDefaults.standard.bool(forKey: "hasCompletedOnboarding")

        // Check Full Disk Access
        checkFullDiskAccess()

        // Initialize service client
        serviceClient = ServiceClient()
    }

    // MARK: - Settings Persistence

    public func saveSettings() {
        do {
            let data = try JSONEncoder().encode(settings)
            try data.write(to: settingsURL)
        } catch {
            print("Failed to save settings: \(error)")
        }
    }

    // MARK: - Indexing Control

    public func startIndexing() async {
        guard hasFullDiskAccess else { return }

        do {
            try await serviceClient?.startIndexing()
            isIndexing = true
        } catch {
            print("Failed to start indexing: \(error)")
        }
    }

    public func stopIndexing() async {
        do {
            try await serviceClient?.stopIndexing()
            isIndexing = false
        } catch {
            print("Failed to stop indexing: \(error)")
        }
    }

    public func refreshIndexHealth() async {
        do {
            indexHealth = try await serviceClient?.getIndexHealth()
        } catch {
            print("Failed to get index health: \(error)")
        }
    }

    // MARK: - Search

    public func search(query: String, context: QueryContext = QueryContext()) async -> [SearchResult] {
        guard !query.isEmpty else { return [] }

        let searchQuery = SearchQuery(
            text: query,
            context: context,
            options: SearchOptions(
                maxResults: settings.search.maxResults,
                includeContent: true,
                includeSemantic: settings.indexing.enableSemanticIndex
            )
        )

        do {
            return try await serviceClient?.search(query: searchQuery) ?? []
        } catch {
            print("Search failed: \(error)")
            return []
        }
    }

    public func recordFeedback(_ feedback: FeedbackEntry) async {
        do {
            try await serviceClient?.recordFeedback(feedback)
        } catch {
            print("Failed to record feedback: \(error)")
        }
    }

    // MARK: - Permissions

    public func checkFullDiskAccess() {
        // Check by trying to read a protected path
        let testPath = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Safari/Bookmarks.plist")

        hasFullDiskAccess = FileManager.default.isReadableFile(atPath: testPath.path)
    }

    public func openFullDiskAccessSettings() {
        let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!
        NSWorkspace.shared.open(url)
    }

    // MARK: - Onboarding

    public func completeOnboarding(with roots: [IndexRoot]) {
        settings.indexRoots = roots
        saveSettings()

        hasCompletedOnboarding = true
        UserDefaults.standard.set(true, forKey: "hasCompletedOnboarding")

        Task {
            await startIndexing()
        }
    }
}

// MARK: - Default Settings

extension AppSettings {
    static func defaultSettings() -> AppSettings {
        let home = FileManager.default.homeDirectoryForCurrentUser.path

        // Build default index roots
        let defaultRoots: [IndexRoot] = [
            IndexRoot(path: "\(home)/Documents", classification: .index),
            IndexRoot(path: "\(home)/Desktop", classification: .index),
            IndexRoot(path: "\(home)/Downloads", classification: .index),
            IndexRoot(path: "\(home)/Developer", classification: .index),
            IndexRoot(path: "\(home)/Projects", classification: .index),
        ].filter { FileManager.default.fileExists(atPath: $0.path) }

        return AppSettings(
            indexRoots: defaultRoots,
            exclusionPatterns: ExclusionPattern.builtInPatterns,
            sensitiveFolders: SensitiveFolderConfig.defaultPatterns.map {
                SensitiveFolderConfig(path: "\(home)/\($0)")
            },
            hotkey: HotkeyConfig(),
            indexing: IndexingSettings(),
            search: SearchSettings(),
            privacy: PrivacySettings()
        )
    }
}
