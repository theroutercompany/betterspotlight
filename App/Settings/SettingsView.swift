import SwiftUI
import Shared

/// Main settings view
struct SettingsView: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        TabView {
            GeneralSettingsView()
                .tabItem {
                    Label("General", systemImage: "gear")
                }

            IndexingSettingsView()
                .tabItem {
                    Label("Indexing", systemImage: "doc.text.magnifyingglass")
                }

            ExclusionsSettingsView()
                .tabItem {
                    Label("Exclusions", systemImage: "minus.circle")
                }

            PrivacySettingsView()
                .tabItem {
                    Label("Privacy", systemImage: "lock.shield")
                }

            IndexHealthView()
                .tabItem {
                    Label("Index Health", systemImage: "heart.text.square")
                }
        }
        .frame(width: 600, height: 450)
    }
}

/// General settings tab
struct GeneralSettingsView: View {
    @EnvironmentObject var appState: AppState
    @State private var isRecordingHotkey = false

    var body: some View {
        Form {
            Section("Hotkey") {
                HStack {
                    Text("Activation Hotkey")
                    Spacer()
                    Button(hotkeyDescription) {
                        isRecordingHotkey = true
                    }
                    .buttonStyle(.bordered)
                }
            }

            Section("Search") {
                Stepper(
                    "Max Results: \(appState.settings.search.maxResults)",
                    value: $appState.settings.search.maxResults,
                    in: 10...100,
                    step: 10
                )

                Toggle("Fuzzy Matching", isOn: $appState.settings.search.enableFuzzyMatching)
                Toggle("Boost Recent Files", isOn: $appState.settings.search.boostRecentFiles)
                Toggle("Boost Frequently Opened", isOn: $appState.settings.search.boostFrequentFiles)
            }

            Section("Permissions") {
                HStack {
                    if appState.hasFullDiskAccess {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundColor(.green)
                        Text("Full Disk Access Granted")
                    } else {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundColor(.orange)
                        Text("Full Disk Access Required")
                        Spacer()
                        Button("Open Settings") {
                            appState.openFullDiskAccessSettings()
                        }
                    }
                }
            }
        }
        .formStyle(.grouped)
        .padding()
        .onChange(of: appState.settings) { _ in
            appState.saveSettings()
        }
    }

    private var hotkeyDescription: String {
        var parts: [String] = []
        let modifiers = appState.settings.hotkey.modifiers

        if modifiers & UInt32(NSEvent.ModifierFlags.command.rawValue) != 0 {
            parts.append("⌘")
        }
        if modifiers & UInt32(NSEvent.ModifierFlags.option.rawValue) != 0 {
            parts.append("⌥")
        }
        if modifiers & UInt32(NSEvent.ModifierFlags.control.rawValue) != 0 {
            parts.append("⌃")
        }
        if modifiers & UInt32(NSEvent.ModifierFlags.shift.rawValue) != 0 {
            parts.append("⇧")
        }

        parts.append(keyCodeToString(appState.settings.hotkey.keyCode))

        return parts.joined()
    }

    private func keyCodeToString(_ keyCode: UInt16) -> String {
        switch keyCode {
        case 49: return "Space"
        case 36: return "Return"
        case 48: return "Tab"
        case 53: return "Esc"
        default: return "Key\(keyCode)"
        }
    }
}

/// Indexing settings tab
struct IndexingSettingsView: View {
    @EnvironmentObject var appState: AppState
    @State private var showingFolderPicker = false

    var body: some View {
        Form {
            Section("Indexed Folders") {
                List {
                    ForEach(appState.settings.indexRoots) { root in
                        IndexRootRow(root: root)
                    }
                    .onDelete { indexSet in
                        appState.settings.indexRoots.remove(atOffsets: indexSet)
                    }
                }
                .frame(height: 150)

                HStack {
                    Button("Add Folder...") {
                        showingFolderPicker = true
                    }
                    Spacer()
                }
            }

            Section("Performance") {
                Stepper(
                    "Max File Size: \(appState.settings.indexing.maxFileSizeMB) MB",
                    value: $appState.settings.indexing.maxFileSizeMB,
                    in: 1...200,
                    step: 10
                )

                Stepper(
                    "Concurrent Extractions: \(appState.settings.indexing.maxConcurrentExtractions)",
                    value: $appState.settings.indexing.maxConcurrentExtractions,
                    in: 1...8
                )
            }

            Section("Features") {
                Toggle("Enable OCR for Images", isOn: $appState.settings.indexing.enableOCR)
                    .help("Extract text from screenshots and images using Vision OCR")

                Toggle("Enable Semantic Search", isOn: $appState.settings.indexing.enableSemanticIndex)
                    .help("Use embeddings for similarity-based search (requires more storage)")
            }
        }
        .formStyle(.grouped)
        .padding()
        .fileImporter(
            isPresented: $showingFolderPicker,
            allowedContentTypes: [.folder],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                let root = IndexRoot(path: url.path, classification: .index, isUserOverride: true)
                appState.settings.indexRoots.append(root)
                appState.saveSettings()
            }
        }
        .onChange(of: appState.settings) { _ in
            appState.saveSettings()
        }
    }
}

/// Row for an index root
struct IndexRootRow: View {
    let root: IndexRoot
    @EnvironmentObject var appState: AppState

    var body: some View {
        HStack {
            Image(systemName: iconForClassification(root.classification))
                .foregroundColor(colorForClassification(root.classification))

            VStack(alignment: .leading) {
                Text((root.path as NSString).lastPathComponent)
                    .font(.headline)
                Text(root.path)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Spacer()

            Picker("", selection: classificationBinding(for: root)) {
                Text("Index").tag(FolderClassification.index)
                Text("Metadata Only").tag(FolderClassification.metadataOnly)
                Text("Exclude").tag(FolderClassification.exclude)
            }
            .pickerStyle(.menu)
            .frame(width: 130)
        }
    }

    private func iconForClassification(_ classification: FolderClassification) -> String {
        switch classification {
        case .index: return "doc.text.magnifyingglass"
        case .metadataOnly: return "doc"
        case .exclude: return "minus.circle"
        }
    }

    private func colorForClassification(_ classification: FolderClassification) -> Color {
        switch classification {
        case .index: return .green
        case .metadataOnly: return .orange
        case .exclude: return .red
        }
    }

    private func classificationBinding(for root: IndexRoot) -> Binding<FolderClassification> {
        Binding(
            get: { root.classification },
            set: { newValue in
                if let index = appState.settings.indexRoots.firstIndex(where: { $0.id == root.id }) {
                    appState.settings.indexRoots[index].classification = newValue
                    appState.saveSettings()
                }
            }
        )
    }
}

/// Exclusions settings tab
struct ExclusionsSettingsView: View {
    @EnvironmentObject var appState: AppState
    @State private var newPattern = ""

    var body: some View {
        Form {
            Section("Exclusion Patterns") {
                List {
                    ForEach(appState.settings.exclusionPatterns) { pattern in
                        ExclusionPatternRow(pattern: pattern)
                    }
                    .onDelete { indexSet in
                        appState.settings.exclusionPatterns.remove(atOffsets: indexSet)
                        appState.saveSettings()
                    }
                }
                .frame(height: 250)

                HStack {
                    TextField("Add pattern (e.g., node_modules)", text: $newPattern)
                        .textFieldStyle(.roundedBorder)

                    Button("Add") {
                        guard !newPattern.isEmpty else { return }
                        let pattern = ExclusionPattern(
                            pattern: newPattern,
                            isRegex: false,
                            isBuiltIn: false,
                            isEnabled: true
                        )
                        appState.settings.exclusionPatterns.append(pattern)
                        appState.saveSettings()
                        newPattern = ""
                    }
                    .disabled(newPattern.isEmpty)
                }
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}

/// Row for an exclusion pattern
struct ExclusionPatternRow: View {
    let pattern: ExclusionPattern
    @EnvironmentObject var appState: AppState

    var body: some View {
        HStack {
            Toggle("", isOn: enabledBinding)
                .labelsHidden()

            VStack(alignment: .leading) {
                Text(pattern.pattern)
                    .font(.body)
                if let description = pattern.description {
                    Text(description)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }

            Spacer()

            if pattern.isBuiltIn {
                Text("Built-in")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            if pattern.isRegex {
                Text("Regex")
                    .font(.caption)
                    .padding(.horizontal, 4)
                    .padding(.vertical, 2)
                    .background(Color.purple.opacity(0.2))
                    .cornerRadius(4)
            }
        }
    }

    private var enabledBinding: Binding<Bool> {
        Binding(
            get: { pattern.isEnabled },
            set: { newValue in
                if let index = appState.settings.exclusionPatterns.firstIndex(where: { $0.id == pattern.id }) {
                    appState.settings.exclusionPatterns[index].isEnabled = newValue
                    appState.saveSettings()
                }
            }
        )
    }
}

/// Privacy settings tab
struct PrivacySettingsView: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        Form {
            Section("Sensitive Folders") {
                Text("The following folders are treated as sensitive. Content is searchable but previews are masked and embeddings are disabled by default.")
                    .font(.caption)
                    .foregroundColor(.secondary)

                List {
                    ForEach(appState.settings.sensitiveFolders, id: \.path) { config in
                        SensitiveFolderRow(config: config)
                    }
                }
                .frame(height: 150)
            }

            Section("Feedback & Learning") {
                Toggle("Enable Local Feedback Logging", isOn: $appState.settings.privacy.enableFeedbackLogging)
                    .help("Log search queries and selections locally to improve ranking")

                if appState.settings.privacy.enableFeedbackLogging {
                    Stepper(
                        "Retention: \(appState.settings.privacy.feedbackRetentionDays) days",
                        value: $appState.settings.privacy.feedbackRetentionDays,
                        in: 7...365,
                        step: 7
                    )
                }

                Toggle("Mask Sensitive Previews", isOn: $appState.settings.privacy.maskSensitivePreviews)
            }
        }
        .formStyle(.grouped)
        .padding()
        .onChange(of: appState.settings) { _ in
            appState.saveSettings()
        }
    }
}

/// Row for a sensitive folder
struct SensitiveFolderRow: View {
    let config: SensitiveFolderConfig
    @EnvironmentObject var appState: AppState

    var body: some View {
        HStack {
            Image(systemName: "lock.shield")
                .foregroundColor(.orange)

            Text((config.path as NSString).lastPathComponent)

            Spacer()

            Toggle("Content Search", isOn: contentSearchBinding)
                .toggleStyle(.switch)
                .controlSize(.small)
        }
    }

    private var contentSearchBinding: Binding<Bool> {
        Binding(
            get: { config.allowContentSearch },
            set: { newValue in
                if let index = appState.settings.sensitiveFolders.firstIndex(where: { $0.path == config.path }) {
                    appState.settings.sensitiveFolders[index].allowContentSearch = newValue
                    appState.saveSettings()
                }
            }
        )
    }
}
