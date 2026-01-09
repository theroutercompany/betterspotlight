import XCTest
@testable import Core
@testable import Shared

final class ScoringTests: XCTestCase {

    var scorer: ResultScorer!

    override func setUp() {
        super.setUp()
        scorer = ResultScorer()
    }

    // MARK: - Match Type Scoring

    func testExactNameMatchScoresHighest() {
        let item = makeItem(path: "/test/file.txt")

        let exactScore = scorer.score(
            item: item,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        let prefixScore = scorer.score(
            item: item,
            matchType: .prefixName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        XCTAssertGreaterThan(exactScore, prefixScore)
    }

    func testPrefixMatchScoresHigherThanSubstring() {
        let item = makeItem(path: "/test/file.txt")

        let prefixScore = scorer.score(
            item: item,
            matchType: .prefixName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        let substringScore = scorer.score(
            item: item,
            matchType: .substringName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        XCTAssertGreaterThan(prefixScore, substringScore)
    }

    func testContentMatchScoresLowerThanNameMatch() {
        let item = makeItem(path: "/test/file.txt")

        let nameScore = scorer.score(
            item: item,
            matchType: .substringName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        let contentScore = scorer.score(
            item: item,
            matchType: .contentExact,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        XCTAssertGreaterThan(nameScore, contentScore)
    }

    // MARK: - Recency Boost

    func testRecentFilesGetBoost() {
        let recentItem = makeItem(path: "/test/recent.txt", modificationDate: Date())
        let oldItem = makeItem(path: "/test/old.txt", modificationDate: Date().addingTimeInterval(-30 * 24 * 3600))

        let recentScore = scorer.score(
            item: recentItem,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        let oldScore = scorer.score(
            item: oldItem,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        XCTAssertGreaterThan(recentScore, oldScore)
    }

    // MARK: - Frequency Boost

    func testFrequentlyOpenedFilesGetBoost() {
        let item = makeItem(path: "/test/file.txt")
        let frequency = ItemFrequency(itemId: 1, path: "/test/file.txt", openCount: 20, lastOpened: Date())

        let withFrequency = scorer.score(
            item: item,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: frequency
        )

        let withoutFrequency = scorer.score(
            item: item,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        XCTAssertGreaterThan(withFrequency, withoutFrequency)
    }

    // MARK: - Pinned Items

    func testPinnedItemsGetHighestBoost() {
        let item = makeItem(path: "/test/file.txt")
        let pinnedFrequency = ItemFrequency(itemId: 1, path: "/test/file.txt", openCount: 1, lastOpened: Date(), isPinned: true)
        let normalFrequency = ItemFrequency(itemId: 1, path: "/test/file.txt", openCount: 100, lastOpened: Date(), isPinned: false)

        let pinnedScore = scorer.score(
            item: item,
            matchType: .substringName,
            baseScore: 0,
            context: QueryContext(),
            frequency: pinnedFrequency
        )

        let normalScore = scorer.score(
            item: item,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: normalFrequency
        )

        XCTAssertGreaterThan(pinnedScore, normalScore)
    }

    // MARK: - Context Boost

    func testCwdProximityBoost() {
        let item = makeItem(path: "/Users/test/project/src/file.txt")

        let inCwd = scorer.score(
            item: item,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(currentWorkingDirectory: "/Users/test/project"),
            frequency: nil
        )

        let outsideCwd = scorer.score(
            item: item,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(currentWorkingDirectory: "/Users/other"),
            frequency: nil
        )

        XCTAssertGreaterThan(inCwd, outsideCwd)
    }

    // MARK: - Junk Penalty

    func testJunkPathsGetPenalty() {
        let normalItem = makeItem(path: "/Users/test/project/src/main.swift")
        let junkItem = makeItem(path: "/Users/test/project/node_modules/pkg/index.js")

        let normalScore = scorer.score(
            item: normalItem,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        let junkScore = scorer.score(
            item: junkItem,
            matchType: .exactName,
            baseScore: 0,
            context: QueryContext(),
            frequency: nil
        )

        XCTAssertGreaterThan(normalScore, junkScore)
    }

    // MARK: - Ranking

    func testRankingSortsCorrectly() {
        let items = [
            (makeItem(path: "/test/exact.txt"), MatchType.substringName, 10.0),
            (makeItem(path: "/test/best.txt"), MatchType.exactName, 20.0),
            (makeItem(path: "/test/content.txt"), MatchType.contentFuzzy, 5.0),
        ]

        let ranked = scorer.rank(
            results: items,
            context: QueryContext(),
            frequencies: [:]
        )

        // Best match (exact name with higher base score) should be first
        XCTAssertEqual(ranked[0].item.path, "/test/best.txt")
    }

    // MARK: - Helpers

    private func makeItem(
        path: String,
        modificationDate: Date = Date()
    ) -> IndexItem {
        IndexItem(
            id: Int64(path.hashValue),
            path: path,
            kind: .file,
            size: 1000,
            modificationDate: modificationDate,
            creationDate: modificationDate
        )
    }
}
