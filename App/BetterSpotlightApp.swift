import SwiftUI

@main
struct BetterSpotlightApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        Settings {
            SettingsView()
                .environmentObject(appDelegate.appState)
        }

        WindowGroup("Onboarding", id: "onboarding") {
            OnboardingView()
                .environmentObject(appDelegate.appState)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
    }
}

/// Main application delegate
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let appState = AppState()
    private var hotkeyManager: HotkeyManager?
    private var searchPanelController: SearchPanelController?
    private var statusBarController: StatusBarController?
    private var onboardingWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Show confirmation that app launched
        print("BetterSpotlight: Application launched successfully")

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
        print("BetterSpotlight: Status bar controller created")
        statusBarController?.onShowSettings = { [weak self] in
            self?.showSettings()
        }
        statusBarController?.onQuit = {
            NSApp.terminate(nil)
        }

        // Check permissions on first run
        print("BetterSpotlight: hasCompletedOnboarding = \(appState.hasCompletedOnboarding)")
        if !appState.hasCompletedOnboarding {
            print("BetterSpotlight: Showing onboarding...")
            showOnboarding()
        } else {
            print("BetterSpotlight: Skipping onboarding, starting indexing...")
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
        print("BetterSpotlight: Creating onboarding window...")

        let onboardingView = OnboardingView()
            .environmentObject(appState)

        let hostingController = NSHostingController(rootView: onboardingView)

        onboardingWindow = NSWindow(contentViewController: hostingController)
        onboardingWindow?.title = "Welcome to BetterSpotlight"
        onboardingWindow?.styleMask = [.titled, .closable]
        onboardingWindow?.setContentSize(NSSize(width: 600, height: 500))
        onboardingWindow?.center()

        // Need to change activation policy to show window (we set it to .accessory earlier)
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        onboardingWindow?.makeKeyAndOrderFront(nil)

        print("BetterSpotlight: Onboarding window should be visible now")
    }
}
