import SwiftUI

/// Main search panel view
struct SearchPanelView: View {
    @ObservedObject var viewModel: SearchPanelViewModel
    @EnvironmentObject var appState: AppState
    @FocusState private var isSearchFieldFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            // Search field
            SearchFieldView(
                query: $viewModel.query,
                isLoading: viewModel.isLoading,
                onSubmit: { viewModel.openSelected() }
            )
            .focused($isSearchFieldFocused)
            .onChange(of: viewModel.query) { _ in
                viewModel.search()
            }

            Divider()

            // Results list
            if viewModel.results.isEmpty && !viewModel.query.isEmpty && !viewModel.isLoading {
                NoResultsView()
            } else {
                ResultsListView(
                    results: viewModel.results,
                    selectedIndex: viewModel.selectedIndex,
                    onSelect: { index in
                        viewModel.selectedIndex = index
                    },
                    onOpen: { result in
                        viewModel.openItem(result)
                    },
                    onReveal: { result in
                        viewModel.revealItem(result)
                    }
                )
            }

            // Footer with keyboard shortcuts
            FooterView()
        }
        .background(VisualEffectView(material: .hudWindow, blendingMode: .behindWindow))
        .cornerRadius(12)
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(Color.gray.opacity(0.2), lineWidth: 1)
        )
        .onAppear {
            isSearchFieldFocused = true
        }
        .onKeyPress(.escape) {
            viewModel.onDismiss?()
            return .handled
        }
        .onKeyPress(.downArrow) {
            viewModel.selectNext()
            return .handled
        }
        .onKeyPress(.upArrow) {
            viewModel.selectPrevious()
            return .handled
        }
        .onKeyPress(.return) {
            viewModel.openSelected()
            return .handled
        }
        .onKeyPress(characters: .init(charactersIn: "r"), phases: .down) { press in
            guard press.modifiers.contains(.command) else { return .ignored }
            viewModel.revealSelected()
            return .handled
        }
        .onKeyPress(characters: .init(charactersIn: "c"), phases: .down) { press in
            guard press.modifiers.contains(.command) && press.modifiers.contains(.shift) else { return .ignored }
            viewModel.copyPathSelected()
            return .handled
        }
    }
}

/// Search input field
struct SearchFieldView: View {
    @Binding var query: String
    let isLoading: Bool
    let onSubmit: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 20))
                .foregroundColor(.secondary)

            TextField("Search files...", text: $query)
                .textFieldStyle(.plain)
                .font(.system(size: 20))
                .onSubmit(onSubmit)

            if isLoading {
                ProgressView()
                    .scaleEffect(0.7)
            } else if !query.isEmpty {
                Button(action: { query = "" }) {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundColor(.secondary)
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }
}

/// List of search results
struct ResultsListView: View {
    let results: [SearchResult]
    let selectedIndex: Int
    let onSelect: (Int) -> Void
    let onOpen: (SearchResult) -> Void
    let onReveal: (SearchResult) -> Void

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(results.enumerated()), id: \.element.id) { index, result in
                        ResultRowView(
                            result: result,
                            isSelected: index == selectedIndex
                        )
                        .id(index)
                        .onTapGesture(count: 2) {
                            onOpen(result)
                        }
                        .onTapGesture(count: 1) {
                            onSelect(index)
                        }
                        .contextMenu {
                            Button("Open") { onOpen(result) }
                            Button("Reveal in Finder") { onReveal(result) }
                            Divider()
                            Button("Copy Path") {
                                NSPasteboard.general.clearContents()
                                NSPasteboard.general.setString(result.item.path, forType: .string)
                            }
                        }
                    }
                }
            }
            .onChange(of: selectedIndex) { newIndex in
                withAnimation(.easeInOut(duration: 0.1)) {
                    proxy.scrollTo(newIndex, anchor: .center)
                }
            }
        }
    }
}

/// Single result row
struct ResultRowView: View {
    let result: SearchResult
    let isSelected: Bool

    var body: some View {
        HStack(spacing: 12) {
            // File icon
            FileIconView(item: result.item)
                .frame(width: 32, height: 32)

            VStack(alignment: .leading, spacing: 2) {
                // Filename
                Text(result.item.filename)
                    .font(.system(size: 14, weight: .medium))
                    .lineLimit(1)

                // Path
                Text(result.item.parentPath)
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
                    .lineLimit(1)

                // Snippet if available
                if let snippet = result.snippet {
                    Text(snippet)
                        .font(.system(size: 11))
                        .foregroundColor(.secondary)
                        .lineLimit(2)
                }
            }

            Spacer()

            // Match type badge
            MatchTypeBadge(matchType: result.matchType)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(isSelected ? Color.accentColor.opacity(0.2) : Color.clear)
        .contentShape(Rectangle())
    }
}

/// File icon based on type
struct FileIconView: View {
    let item: IndexItem

    var body: some View {
        Image(nsImage: iconForItem(item))
            .resizable()
            .aspectRatio(contentMode: .fit)
    }

    private func iconForItem(_ item: IndexItem) -> NSImage {
        let url = URL(fileURLWithPath: item.path)
        return NSWorkspace.shared.icon(forFile: url.path)
    }
}

/// Badge showing match type
struct MatchTypeBadge: View {
    let matchType: MatchType

    var body: some View {
        Text(badgeText)
            .font(.system(size: 9, weight: .medium))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(badgeColor.opacity(0.2))
            .foregroundColor(badgeColor)
            .cornerRadius(4)
    }

    private var badgeText: String {
        switch matchType {
        case .exactName: return "Exact"
        case .prefixName: return "Prefix"
        case .substringName: return "Name"
        case .pathToken: return "Path"
        case .contentExact: return "Content"
        case .contentFuzzy: return "Fuzzy"
        case .semantic: return "Similar"
        }
    }

    private var badgeColor: Color {
        switch matchType {
        case .exactName: return .green
        case .prefixName: return .blue
        case .substringName: return .blue
        case .pathToken: return .orange
        case .contentExact: return .purple
        case .contentFuzzy: return .purple
        case .semantic: return .pink
        }
    }
}

/// Empty state
struct NoResultsView: View {
    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 32))
                .foregroundColor(.secondary)
            Text("No results found")
                .font(.headline)
                .foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// Footer with keyboard shortcuts
struct FooterView: View {
    var body: some View {
        HStack(spacing: 16) {
            KeyboardShortcutHint(keys: "↵", action: "Open")
            KeyboardShortcutHint(keys: "⌘R", action: "Reveal")
            KeyboardShortcutHint(keys: "⌘⇧C", action: "Copy Path")
            KeyboardShortcutHint(keys: "↑↓", action: "Navigate")
            KeyboardShortcutHint(keys: "esc", action: "Close")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(Color.black.opacity(0.05))
    }
}

struct KeyboardShortcutHint: View {
    let keys: String
    let action: String

    var body: some View {
        HStack(spacing: 4) {
            Text(keys)
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .padding(.horizontal, 4)
                .padding(.vertical, 2)
                .background(Color.gray.opacity(0.2))
                .cornerRadius(3)

            Text(action)
                .font(.system(size: 10))
                .foregroundColor(.secondary)
        }
    }
}

/// NSVisualEffectView wrapper
struct VisualEffectView: NSViewRepresentable {
    let material: NSVisualEffectView.Material
    let blendingMode: NSVisualEffectView.BlendingMode

    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = material
        view.blendingMode = blendingMode
        view.state = .active
        return view
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {
        nsView.material = material
        nsView.blendingMode = blendingMode
    }
}
