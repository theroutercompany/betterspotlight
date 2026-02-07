import Foundation

/// A sample Swift file for text extraction testing.
/// Uses actors, async/await, and Codable — patterns found in BetterSpotlight.
actor DocumentIndex {
    private var items: [String: IndexEntry] = [:]

    struct IndexEntry: Codable, Sendable {
        let path: String
        let title: String
        let score: Double
        let lastModified: Date
    }

    func insert(_ entry: IndexEntry) {
        items[entry.path] = entry
    }

    func search(query: String) -> [IndexEntry] {
        items.values
            .filter { $0.title.localizedCaseInsensitiveContains(query) }
            .sorted { $0.score > $1.score }
    }

    var count: Int { items.count }
}

@main
struct SampleApp {
    static func main() async {
        let index = DocumentIndex()
        let entry = DocumentIndex.IndexEntry(
            path: "/Users/test/notes.md",
            title: "Meeting Notes",
            score: 0.95,
            lastModified: Date()
        )
        await index.insert(entry)
        let results = await index.search(query: "meeting")
        print("Found \(results.count) results")
    }
}
