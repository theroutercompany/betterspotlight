import AppKit
import SwiftUI

/// Controls the status bar menu
@MainActor
public final class StatusBarController {
    private var statusItem: NSStatusItem?
    private let appState: AppState

    public var onShowSettings: (() -> Void)?
    public var onQuit: (() -> Void)?

    public init(appState: AppState) {
        self.appState = appState
        setupStatusItem()
    }

    private func setupStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        if let button = statusItem?.button {
            // Try SF Symbol first, fall back to text if not available
            if let image = NSImage(systemSymbolName: "magnifyingglass", accessibilityDescription: "BetterSpotlight") {
                button.image = image
                button.image?.isTemplate = true
            } else {
                // Fallback to text title
                button.title = "🔍"
            }
        }

        updateMenu()
    }

    private func updateMenu() {
        let menu = NSMenu()

        // Status section
        let statusItem = NSMenuItem(title: statusTitle, action: nil, keyEquivalent: "")
        statusItem.isEnabled = false
        menu.addItem(statusItem)

        menu.addItem(NSMenuItem.separator())

        // Actions
        let searchItem = NSMenuItem(
            title: "Search...",
            action: #selector(handleSearch),
            keyEquivalent: " "
        )
        searchItem.keyEquivalentModifierMask = .command
        searchItem.target = self
        menu.addItem(searchItem)

        menu.addItem(NSMenuItem.separator())

        // Indexing controls
        if appState.isIndexing {
            let pauseItem = NSMenuItem(
                title: "Pause Indexing",
                action: #selector(handlePauseIndexing),
                keyEquivalent: ""
            )
            pauseItem.target = self
            menu.addItem(pauseItem)
        } else {
            let resumeItem = NSMenuItem(
                title: "Resume Indexing",
                action: #selector(handleResumeIndexing),
                keyEquivalent: ""
            )
            resumeItem.target = self
            menu.addItem(resumeItem)
        }

        menu.addItem(NSMenuItem.separator())

        // Settings
        let settingsItem = NSMenuItem(
            title: "Settings...",
            action: #selector(handleSettings),
            keyEquivalent: ","
        )
        settingsItem.keyEquivalentModifierMask = .command
        settingsItem.target = self
        menu.addItem(settingsItem)

        menu.addItem(NSMenuItem.separator())

        // Quit
        let quitItem = NSMenuItem(
            title: "Quit BetterSpotlight",
            action: #selector(handleQuit),
            keyEquivalent: "q"
        )
        quitItem.keyEquivalentModifierMask = .command
        quitItem.target = self
        menu.addItem(quitItem)

        self.statusItem?.menu = menu
    }

    private var statusTitle: String {
        if let health = appState.indexHealth {
            let formatter = NumberFormatter()
            formatter.numberStyle = .decimal
            let itemCount = formatter.string(from: NSNumber(value: health.totalItems)) ?? "\(health.totalItems)"

            switch health.status {
            case .healthy:
                return "✓ \(itemCount) items indexed"
            case .degraded:
                return "⚠ \(itemCount) items (\(health.queueLength) pending)"
            case .unhealthy:
                return "✗ Index has issues"
            case .rebuilding:
                return "↻ Rebuilding index..."
            }
        } else {
            return "Loading..."
        }
    }

    @objc private func handleSearch() {
        // Trigger hotkey action
        NotificationCenter.default.post(name: .showSearchPanel, object: nil)
    }

    @objc private func handlePauseIndexing() {
        Task {
            await appState.stopIndexing()
            updateMenu()
        }
    }

    @objc private func handleResumeIndexing() {
        Task {
            await appState.startIndexing()
            updateMenu()
        }
    }

    @objc private func handleSettings() {
        onShowSettings?()
    }

    @objc private func handleQuit() {
        onQuit?()
    }
}

// MARK: - Notifications

extension Notification.Name {
    static let showSearchPanel = Notification.Name("com.betterspotlight.showSearchPanel")
}
