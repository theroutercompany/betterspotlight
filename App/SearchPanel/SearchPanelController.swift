import AppKit
import SwiftUI
import Shared

/// Controls the floating search panel window
@MainActor
public final class SearchPanelController {
    private var window: NSPanel?
    private let appState: AppState
    private let viewModel: SearchPanelViewModel

    public var isVisible: Bool {
        window?.isVisible ?? false
    }

    public init(appState: AppState) {
        self.appState = appState
        self.viewModel = SearchPanelViewModel(appState: appState)
    }

    public func show() {
        if window == nil {
            createWindow()
        }

        guard let window = window else { return }

        // Position window at top center of main screen
        if let screen = NSScreen.main {
            let screenFrame = screen.visibleFrame
            let windowWidth: CGFloat = 680
            let windowHeight: CGFloat = 400

            let x = screenFrame.midX - windowWidth / 2
            let y = screenFrame.maxY - windowHeight - 100 // 100pt from top

            window.setFrame(NSRect(x: x, y: y, width: windowWidth, height: windowHeight), display: true)
        }

        // Show and activate
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        // Clear previous search
        viewModel.clearSearch()
    }

    public func hide() {
        window?.orderOut(nil)
    }

    private func createWindow() {
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 680, height: 400),
            styleMask: [.nonactivatingPanel, .titled, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )

        panel.isFloatingPanel = true
        panel.level = .floating
        panel.titlebarAppearsTransparent = true
        panel.titleVisibility = .hidden
        panel.isMovableByWindowBackground = true
        panel.backgroundColor = .clear
        panel.isOpaque = false
        panel.hasShadow = true

        // Allow key events
        panel.becomesKeyOnlyIfNeeded = false

        // Hide when loses focus
        panel.hidesOnDeactivate = true

        // Set content
        let contentView = SearchPanelView(viewModel: viewModel)
            .environmentObject(appState)

        panel.contentView = NSHostingView(rootView: contentView)

        // Handle escape key
        viewModel.onDismiss = { [weak self] in
            self?.hide()
        }

        self.window = panel
    }
}

/// View model for the search panel
@MainActor
public final class SearchPanelViewModel: ObservableObject {
    @Published var query = ""
    @Published var results: [SearchResult] = []
    @Published var selectedIndex = 0
    @Published var isLoading = false

    var onDismiss: (() -> Void)?

    private let appState: AppState
    private var searchTask: Task<Void, Never>?
    private let debounceDelay: UInt64

    public init(appState: AppState) {
        self.appState = appState
        self.debounceDelay = UInt64(appState.settings.search.debounceDelayMs) * 1_000_000
    }

    public func search() {
        // Cancel previous search
        searchTask?.cancel()

        let queryText = query.trimmingCharacters(in: .whitespacesAndNewlines)

        guard !queryText.isEmpty else {
            results = []
            selectedIndex = 0
            return
        }

        isLoading = true

        searchTask = Task {
            // Debounce
            try? await Task.sleep(nanoseconds: debounceDelay)

            guard !Task.isCancelled else { return }

            let searchResults = await appState.search(query: queryText)

            guard !Task.isCancelled else { return }

            results = searchResults
            selectedIndex = 0
            isLoading = false
        }
    }

    public func clearSearch() {
        query = ""
        results = []
        selectedIndex = 0
    }

    public func selectNext() {
        if selectedIndex < results.count - 1 {
            selectedIndex += 1
        }
    }

    public func selectPrevious() {
        if selectedIndex > 0 {
            selectedIndex -= 1
        }
    }

    public func openSelected() {
        guard selectedIndex < results.count else { return }
        let result = results[selectedIndex]
        openItem(result)
    }

    public func revealSelected() {
        guard selectedIndex < results.count else { return }
        let result = results[selectedIndex]
        revealItem(result)
    }

    public func copyPathSelected() {
        guard selectedIndex < results.count else { return }
        let result = results[selectedIndex]
        copyPath(result)
    }

    public func openItem(_ result: SearchResult) {
        let url = URL(fileURLWithPath: result.item.path)
        NSWorkspace.shared.open(url)

        // Record feedback
        Task {
            let feedback = FeedbackEntry(
                query: query,
                itemId: result.item.id,
                itemPath: result.item.path,
                action: .open,
                resultPosition: selectedIndex,
                totalResults: results.count
            )
            await appState.recordFeedback(feedback)
        }

        onDismiss?()
    }

    public func revealItem(_ result: SearchResult) {
        let url = URL(fileURLWithPath: result.item.path)
        NSWorkspace.shared.activateFileViewerSelecting([url])

        Task {
            let feedback = FeedbackEntry(
                query: query,
                itemId: result.item.id,
                itemPath: result.item.path,
                action: .reveal,
                resultPosition: selectedIndex,
                totalResults: results.count
            )
            await appState.recordFeedback(feedback)
        }

        onDismiss?()
    }

    public func copyPath(_ result: SearchResult) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(result.item.path, forType: .string)

        Task {
            let feedback = FeedbackEntry(
                query: query,
                itemId: result.item.id,
                itemPath: result.item.path,
                action: .copyPath,
                resultPosition: selectedIndex,
                totalResults: results.count
            )
            await appState.recordFeedback(feedback)
        }
    }
}
