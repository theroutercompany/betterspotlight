import SwiftUI
import Shared

/// Index health monitoring view
struct IndexHealthView: View {
    @EnvironmentObject var appState: AppState
    @State private var isRefreshing = false

    var body: some View {
        VStack(spacing: 0) {
            if let health = appState.indexHealth {
                IndexHealthContent(health: health)
            } else {
                LoadingView()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .task {
            await refreshHealth()
        }
    }

    private func refreshHealth() async {
        isRefreshing = true
        await appState.refreshIndexHealth()
        isRefreshing = false
    }
}

/// Health status content
struct IndexHealthContent: View {
    let health: IndexHealthSnapshot
    @EnvironmentObject var appState: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Status overview
                StatusOverviewCard(health: health)

                // Statistics
                StatisticsCard(health: health)

                // Index roots
                IndexRootsCard(roots: health.roots)

                // Recent errors
                if !health.recentErrors.isEmpty {
                    RecentErrorsCard(errors: health.recentErrors)
                }

                // Actions
                ActionsCard()
            }
            .padding()
        }
    }
}

/// Status overview card
struct StatusOverviewCard: View {
    let health: IndexHealthSnapshot

    var body: some View {
        GroupBox("Status") {
            HStack(spacing: 20) {
                StatusIndicator(status: health.status)

                VStack(alignment: .leading) {
                    Text(statusDescription)
                        .font(.headline)
                    Text(statusDetail)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                Spacer()

                if health.queueLength > 0 {
                    VStack(alignment: .trailing) {
                        Text("\(health.queueLength)")
                            .font(.title2)
                            .fontWeight(.semibold)
                        Text("in queue")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
            }
            .padding(.vertical, 8)
        }
    }

    private var statusDescription: String {
        switch health.status {
        case .healthy: return "Index is healthy"
        case .degraded: return "Index is degraded"
        case .unhealthy: return "Index has issues"
        case .rebuilding: return "Rebuilding index..."
        }
    }

    private var statusDetail: String {
        if let lastEvent = health.lastEventProcessed {
            let formatter = RelativeDateTimeFormatter()
            return "Last updated \(formatter.localizedString(for: lastEvent, relativeTo: Date()))"
        }
        return "No events processed yet"
    }
}

/// Status indicator icon
struct StatusIndicator: View {
    let status: IndexHealthStatus

    var body: some View {
        Image(systemName: iconName)
            .font(.title)
            .foregroundColor(color)
    }

    private var iconName: String {
        switch status {
        case .healthy: return "checkmark.circle.fill"
        case .degraded: return "exclamationmark.triangle.fill"
        case .unhealthy: return "xmark.circle.fill"
        case .rebuilding: return "arrow.triangle.2.circlepath"
        }
    }

    private var color: Color {
        switch status {
        case .healthy: return .green
        case .degraded: return .orange
        case .unhealthy: return .red
        case .rebuilding: return .blue
        }
    }
}

/// Statistics card
struct StatisticsCard: View {
    let health: IndexHealthSnapshot

    var body: some View {
        GroupBox("Statistics") {
            LazyVGrid(columns: [
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible())
            ], spacing: 16) {
                StatisticItem(
                    title: "Items",
                    value: formatNumber(health.totalItems),
                    icon: "doc"
                )

                StatisticItem(
                    title: "Content Chunks",
                    value: formatNumber(health.totalContentChunks),
                    icon: "text.quote"
                )

                StatisticItem(
                    title: "Index Size",
                    value: health.formattedIndexSize,
                    icon: "internaldrive"
                )

                StatisticItem(
                    title: "Preview Cache",
                    value: health.formattedPreviewCacheSize,
                    icon: "photo.stack"
                )
            }
            .padding(.vertical, 8)
        }
    }

    private func formatNumber(_ value: Int64) -> String {
        let formatter = NumberFormatter()
        formatter.numberStyle = .decimal
        return formatter.string(from: NSNumber(value: value)) ?? "\(value)"
    }
}

/// Single statistic item
struct StatisticItem: View {
    let title: String
    let value: String
    let icon: String

    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundColor(.accentColor)

            Text(value)
                .font(.headline)

            Text(title)
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }
}

/// Index roots status card
struct IndexRootsCard: View {
    let roots: [IndexRootStatus]

    var body: some View {
        GroupBox("Indexed Locations") {
            VStack(spacing: 0) {
                ForEach(roots) { root in
                    IndexRootStatusRow(root: root)
                    if root.id != roots.last?.id {
                        Divider()
                    }
                }
            }
        }
    }
}

/// Single index root status row
struct IndexRootStatusRow: View {
    let root: IndexRootStatus

    var body: some View {
        HStack {
            Image(systemName: iconForClassification(root.classification))
                .foregroundColor(colorForClassification(root.classification))

            VStack(alignment: .leading) {
                Text((root.path as NSString).lastPathComponent)
                    .font(.body)
                Text(root.path)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Spacer()

            VStack(alignment: .trailing) {
                Text("\(root.itemCount) items")
                    .font(.caption)

                if root.pendingUpdates > 0 {
                    Text("\(root.pendingUpdates) pending")
                        .font(.caption)
                        .foregroundColor(.orange)
                }

                if root.errors > 0 {
                    Text("\(root.errors) errors")
                        .font(.caption)
                        .foregroundColor(.red)
                }
            }
        }
        .padding(.vertical, 8)
    }

    private func iconForClassification(_ classification: FolderClassification) -> String {
        switch classification {
        case .index: return "folder.fill"
        case .metadataOnly: return "folder"
        case .exclude: return "folder.badge.minus"
        }
    }

    private func colorForClassification(_ classification: FolderClassification) -> Color {
        switch classification {
        case .index: return .blue
        case .metadataOnly: return .orange
        case .exclude: return .gray
        }
    }
}

/// Recent errors card
struct RecentErrorsCard: View {
    let errors: [IndexFailure]
    @State private var isExpanded = false

    var body: some View {
        GroupBox("Recent Errors") {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(Array(errors.prefix(isExpanded ? 20 : 5).enumerated()), id: \.offset) { _, error in
                    ErrorRow(error: error)
                }

                if errors.count > 5 {
                    Button(isExpanded ? "Show Less" : "Show All (\(errors.count))") {
                        withAnimation {
                            isExpanded.toggle()
                        }
                    }
                    .buttonStyle(.link)
                }
            }
        }
    }
}

/// Single error row
struct ErrorRow: View {
    let error: IndexFailure

    var body: some View {
        HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle")
                .foregroundColor(.orange)

            VStack(alignment: .leading, spacing: 2) {
                Text((error.path as NSString).lastPathComponent)
                    .font(.body)

                Text("\(error.stage): \(error.error)")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .lineLimit(2)

                Text("Failed \(error.failureCount) time(s)")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }

            Spacer()
        }
        .padding(.vertical, 4)
    }
}

/// Actions card
struct ActionsCard: View {
    @EnvironmentObject var appState: AppState
    @State private var isRebuilding = false
    @State private var showingConfirmation = false

    var body: some View {
        GroupBox("Actions") {
            HStack(spacing: 16) {
                Button {
                    Task {
                        await appState.refreshIndexHealth()
                    }
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }

                Button {
                    showingConfirmation = true
                } label: {
                    Label("Rebuild Index", systemImage: "arrow.triangle.2.circlepath")
                }
                .disabled(isRebuilding)

                Button {
                    // Clear preview cache
                } label: {
                    Label("Clear Cache", systemImage: "trash")
                }

                Spacer()
            }
            .padding(.vertical, 8)
        }
        .confirmationDialog(
            "Rebuild Index?",
            isPresented: $showingConfirmation,
            titleVisibility: .visible
        ) {
            Button("Rebuild", role: .destructive) {
                isRebuilding = true
                // Would trigger rebuild
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This will delete the existing index and rebuild it from scratch. This may take a while.")
        }
    }
}

/// Loading state
struct LoadingView: View {
    var body: some View {
        VStack {
            ProgressView()
            Text("Loading index health...")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }
}
