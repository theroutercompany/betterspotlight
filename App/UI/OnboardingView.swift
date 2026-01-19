import SwiftUI

/// First-run onboarding flow
struct OnboardingView: View {
    @EnvironmentObject var appState: AppState
    @State private var currentStep = 0
    @State private var homeDirectoryItems: [HomeDirectoryItem] = []

    var body: some View {
        VStack(spacing: 0) {
            // Progress indicator
            ProgressIndicator(currentStep: currentStep, totalSteps: 3)
                .padding()

            // Content
            TabView(selection: $currentStep) {
                WelcomeStep()
                    .tag(0)

                PermissionsStep()
                    .tag(1)

                HomeMapStep(items: $homeDirectoryItems)
                    .tag(2)
            }
            .tabViewStyle(.automatic)

            // Navigation
            HStack {
                if currentStep > 0 {
                    Button("Back") {
                        withAnimation {
                            currentStep -= 1
                        }
                    }
                    .buttonStyle(.bordered)
                }

                Spacer()

                if currentStep < 2 {
                    Button("Continue") {
                        withAnimation {
                            currentStep += 1
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!canContinue)
                } else {
                    Button("Start Indexing") {
                        completeOnboarding()
                    }
                    .buttonStyle(.borderedProminent)
                }
            }
            .padding()
        }
        .frame(width: 600, height: 500)
        .task {
            await loadHomeDirectory()
        }
    }

    private var canContinue: Bool {
        switch currentStep {
        case 1:
            return appState.hasFullDiskAccess
        default:
            return true
        }
    }

    private func loadHomeDirectory() async {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let fm = FileManager.default

        guard let contents = try? fm.contentsOfDirectory(atPath: home) else { return }

        var items: [HomeDirectoryItem] = []

        for name in contents.sorted() {
            let path = (home as NSString).appendingPathComponent(name)
            var isDirectory: ObjCBool = false

            guard fm.fileExists(atPath: path, isDirectory: &isDirectory), isDirectory.boolValue else {
                continue
            }

            let suggestedClassification = PathRules.suggestedClassification(for: name)
            let isCloudFolder = PathRules.isCloudFolder(path)

            items.append(HomeDirectoryItem(
                name: name,
                path: path,
                classification: isCloudFolder ? .exclude : suggestedClassification,
                isCloudFolder: isCloudFolder,
                isSensitive: SensitiveFolderConfig.defaultPatterns.contains(name)
            ))
        }

        homeDirectoryItems = items
    }

    private func completeOnboarding() {
        print("BetterSpotlight: Completing onboarding...")

        let roots = homeDirectoryItems
            .filter { $0.classification != .exclude }
            .map { IndexRoot(path: $0.path, classification: $0.classification, isUserOverride: false) }

        print("BetterSpotlight: Selected \(roots.count) roots to index")

        appState.completeOnboarding(with: roots)

        // Close the onboarding window and switch to accessory mode
        DispatchQueue.main.async {
            NSApp.setActivationPolicy(.accessory)
            NSApp.keyWindow?.close()
        }
    }
}

/// Progress indicator dots
struct ProgressIndicator: View {
    let currentStep: Int
    let totalSteps: Int

    var body: some View {
        HStack(spacing: 8) {
            ForEach(0..<totalSteps, id: \.self) { step in
                Circle()
                    .fill(step <= currentStep ? Color.accentColor : Color.gray.opacity(0.3))
                    .frame(width: 8, height: 8)
            }
        }
    }
}

/// Welcome step
struct WelcomeStep: View {
    var body: some View {
        VStack(spacing: 24) {
            Image(systemName: "magnifyingglass.circle.fill")
                .font(.system(size: 80))
                .foregroundColor(.accentColor)

            Text("Welcome to BetterSpotlight")
                .font(.largeTitle)
                .fontWeight(.bold)

            Text("A faster, smarter file search for developers")
                .font(.title3)
                .foregroundColor(.secondary)

            VStack(alignment: .leading, spacing: 16) {
                FeatureRow(
                    icon: "bolt.fill",
                    title: "Instant Search",
                    description: "Find files instantly with keyboard-first navigation"
                )

                FeatureRow(
                    icon: "doc.text.magnifyingglass",
                    title: "Content Search",
                    description: "Search inside files, not just by filename"
                )

                FeatureRow(
                    icon: "lock.shield.fill",
                    title: "Private & Offline",
                    description: "Everything stays on your Mac, no cloud required"
                )

                FeatureRow(
                    icon: "gearshape.fill",
                    title: "Developer-Focused",
                    description: "Smart defaults that exclude build artifacts and caches"
                )
            }
            .padding(.top, 16)
        }
        .padding(32)
    }
}

/// Feature row for welcome screen
struct FeatureRow: View {
    let icon: String
    let title: String
    let description: String

    var body: some View {
        HStack(spacing: 16) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundColor(.accentColor)
                .frame(width: 32)

            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.headline)
                Text(description)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
            }
        }
    }
}

/// Permissions step
struct PermissionsStep: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        VStack(spacing: 24) {
            Image(systemName: appState.hasFullDiskAccess ? "checkmark.shield.fill" : "lock.shield")
                .font(.system(size: 60))
                .foregroundColor(appState.hasFullDiskAccess ? .green : .orange)

            Text("Full Disk Access Required")
                .font(.title)
                .fontWeight(.bold)

            Text("BetterSpotlight needs Full Disk Access to index your files. This permission stays on your Mac and is never shared.")
                .multilineTextAlignment(.center)
                .foregroundColor(.secondary)
                .frame(maxWidth: 400)

            if appState.hasFullDiskAccess {
                Label("Full Disk Access Granted", systemImage: "checkmark.circle.fill")
                    .foregroundColor(.green)
                    .font(.headline)
            } else {
                VStack(spacing: 16) {
                    Button("Open System Settings") {
                        appState.openFullDiskAccessSettings()
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)

                    Text("After enabling, return here to continue")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    Button("Check Again") {
                        appState.checkFullDiskAccess()
                    }
                    .buttonStyle(.bordered)
                }
            }
        }
        .padding(32)
        .onAppear {
            appState.checkFullDiskAccess()
        }
    }
}

/// Home directory mapping step
struct HomeMapStep: View {
    @Binding var items: [HomeDirectoryItem]

    var body: some View {
        VStack(spacing: 16) {
            Text("Configure Your Home Directory")
                .font(.title)
                .fontWeight(.bold)

            Text("Choose how to index each folder. You can change these settings later.")
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)

            // Legend
            HStack(spacing: 24) {
                LegendItem(color: .green, label: "Index (full search)")
                LegendItem(color: .orange, label: "Metadata only")
                LegendItem(color: .gray, label: "Exclude")
            }
            .font(.caption)

            // Folder list
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach($items) { $item in
                        HomeDirectoryItemRow(item: $item)
                        Divider()
                    }
                }
            }
            .background(Color(NSColor.controlBackgroundColor))
            .cornerRadius(8)
            .frame(height: 250)
        }
        .padding(32)
    }
}

/// Legend item
struct LegendItem: View {
    let color: Color
    let label: String

    var body: some View {
        HStack(spacing: 4) {
            Circle()
                .fill(color)
                .frame(width: 8, height: 8)
            Text(label)
        }
    }
}

/// Row for a home directory item
struct HomeDirectoryItemRow: View {
    @Binding var item: HomeDirectoryItem

    var body: some View {
        HStack {
            Image(systemName: item.name.hasPrefix(".") ? "folder.fill" : "folder")
                .foregroundColor(colorForClassification(item.classification))

            VStack(alignment: .leading) {
                HStack {
                    Text(item.name)
                        .font(.body)

                    if item.isCloudFolder {
                        Text("Cloud")
                            .font(.caption2)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 2)
                            .background(Color.blue.opacity(0.2))
                            .cornerRadius(4)
                    }

                    if item.isSensitive {
                        Text("Sensitive")
                            .font(.caption2)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 2)
                            .background(Color.orange.opacity(0.2))
                            .cornerRadius(4)
                    }
                }
            }

            Spacer()

            Picker("", selection: $item.classification) {
                Text("Index").tag(FolderClassification.index)
                Text("Metadata").tag(FolderClassification.metadataOnly)
                Text("Exclude").tag(FolderClassification.exclude)
            }
            .pickerStyle(.segmented)
            .frame(width: 200)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    private func colorForClassification(_ classification: FolderClassification) -> Color {
        switch classification {
        case .index: return .green
        case .metadataOnly: return .orange
        case .exclude: return .gray
        }
    }
}

/// Model for home directory items
struct HomeDirectoryItem: Identifiable {
    let id = UUID()
    let name: String
    let path: String
    var classification: FolderClassification
    let isCloudFolder: Bool
    let isSensitive: Bool
}
