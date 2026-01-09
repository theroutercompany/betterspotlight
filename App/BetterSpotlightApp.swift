import SwiftUI
import Shared

@main
struct BetterSpotlightApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        Settings {
            SettingsView()
                .environmentObject(appDelegate.appState)
        }
    }
}

/// Main application delegate
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let appState = AppState()
    private var hotkeyManager: HotkeyManager?
    private var searchPanelController: SearchPanelController?
    private var statusBarController: StatusBarController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Set up as accessory app (no dock icon)
        NSApp.setActivationPolicy(.accessory)

        // Initialize hotkey manager
        hotkeyManager = HotkeyManager()
        hotkeyManager?.onHotkey = { [weak self] in
            self?.toggleSearchPanel()
        }

        // Register default hotkey (Cmd+Space)
        hotkeyManager?.registerHotkey(
            keyCode: appState.settings.hotkey.keyCode,
            modifiers: appState.settings.hotkey.modifiers
        )

        // Set up search panel
        searchPanelController = SearchPanelController(appState: appState)

        // Set up status bar
        statusBarController = StatusBarController(appState: appState)
        statusBarController?.onShowSettings = { [weak self] in
            self?.showSettings()
        }
        statusBarController?.onQuit = {
            NSApp.terminate(nil)
        }

        // Check permissions on first run
        if !appState.hasCompletedOnboarding {
            showOnboarding()
        } else {
            // Start indexing
            Task {
                await appState.startIndexing()
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        hotkeyManager?.unregisterHotkey()
    }

    private func toggleSearchPanel() {
        if searchPanelController?.isVisible == true {
            searchPanelController?.hide()
        } else {
            searchPanelController?.show()
        }
    }

    private func showSettings() {
        NSApp.activate(ignoringOtherApps: true)
        if #available(macOS 14.0, *) {
            NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
        } else {
            NSApp.sendAction(Selector(("showPreferencesWindow:")), to: nil, from: nil)
        }
    }

    private func showOnboarding() {
        // Would show onboarding window
        // For now, just mark as completed
        appState.hasCompletedOnboarding = true
    }
}
