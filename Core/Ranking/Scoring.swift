import Foundation
import Shared

/// Scores and ranks search results
public struct ResultScorer: Sendable {
    private let settings: SearchSettings
    private let weights: ScoringWeights

    public init(settings: SearchSettings = SearchSettings(), weights: ScoringWeights = .default) {
        self.settings = settings
        self.weights = weights
    }

    /// Score a single result
    public func score(
        item: IndexItem,
        matchType: MatchType,
        baseScore: Double,
        context: QueryContext,
        frequency: ItemFrequency?
    ) -> Double {
        var score = baseScore

        // Match type boost
        score += matchTypeBoost(matchType)

        // Recency boost
        if settings.boostRecentFiles {
            score += recencyBoost(item.modificationDate)
        }

        // Frequency boost
        if settings.boostFrequentFiles, let freq = frequency {
            score += frequencyBoost(freq)
        }

        // Pinned items get highest boost
        if frequency?.isPinned == true {
            score += weights.pinnedBoost
        }

        // Context boosts
        score += contextBoost(item: item, context: context)

        // Junk penalty
        score += junkPenalty(item)

        return score
    }

    /// Rank a list of results
    public func rank(
        results: [(item: IndexItem, matchType: MatchType, baseScore: Double)],
        context: QueryContext,
        frequencies: [Int64: ItemFrequency]
    ) -> [ScoredResult] {
        var scored: [ScoredResult] = []

        for (item, matchType, baseScore) in results {
            let frequency = frequencies[item.id]
            let finalScore = score(
                item: item,
                matchType: matchType,
                baseScore: baseScore,
                context: context,
                frequency: frequency
            )

            scored.append(ScoredResult(
                item: item,
                score: finalScore,
                matchType: matchType,
                frequency: frequency
            ))
        }

        // Sort by score descending
        scored.sort { $0.score > $1.score }

        return scored
    }

    // MARK: - Boost Calculations

    private func matchTypeBoost(_ matchType: MatchType) -> Double {
        switch matchType {
        case .exactName:
            return weights.exactNameBoost
        case .prefixName:
            return weights.prefixNameBoost
        case .substringName:
            return weights.substringNameBoost
        case .pathToken:
            return weights.pathTokenBoost
        case .contentExact:
            return weights.contentExactBoost
        case .contentFuzzy:
            return weights.contentFuzzyBoost
        case .semantic:
            return weights.semanticBoost
        }
    }

    private func recencyBoost(_ modificationDate: Date) -> Double {
        let age = Date().timeIntervalSince(modificationDate)
        let hours = age / 3600

        if hours < 1 {
            return weights.recencyBoost * 1.0
        } else if hours < 24 {
            return weights.recencyBoost * 0.8
        } else if hours < 24 * 7 {
            return weights.recencyBoost * 0.5
        } else if hours < 24 * 30 {
            return weights.recencyBoost * 0.2
        } else {
            return 0
        }
    }

    private func frequencyBoost(_ frequency: ItemFrequency) -> Double {
        let count = frequency.openCount

        if count == 0 {
            return 0
        } else if count < 5 {
            return weights.frequencyBoost * 0.3
        } else if count < 20 {
            return weights.frequencyBoost * 0.6
        } else {
            return weights.frequencyBoost * 1.0
        }
    }

    private func contextBoost(item: IndexItem, context: QueryContext) -> Double {
        var boost = 0.0

        // Boost if in current working directory
        if let cwd = context.currentWorkingDirectory {
            if item.path.hasPrefix(cwd) {
                boost += weights.cwdBoost
            }

            // Check repo proximity
            if let repoRoot = PathRules.findRepoRoot(for: cwd) {
                if item.path.hasPrefix(repoRoot) {
                    boost += weights.repoProximityBoost
                }
            }
        }

        // Boost if in recent paths
        if context.recentPaths.contains(item.path) {
            boost += weights.recentPathBoost
        }

        // App-specific boosts
        if let appName = context.frontmostAppName?.lowercased() {
            // Boost code files when in IDE
            let ideApps = ["xcode", "vscode", "visual studio code", "intellij", "webstorm", "sublime", "atom", "vim", "neovim", "emacs"]
            if ideApps.contains(where: { appName.contains($0) }) {
                if isCodeFile(item) {
                    boost += weights.appContextBoost
                }
            }

            // Boost documents when in document apps
            let docApps = ["pages", "word", "docs", "notion", "obsidian"]
            if docApps.contains(where: { appName.contains($0) }) {
                if isDocumentFile(item) {
                    boost += weights.appContextBoost
                }
            }
        }

        return boost
    }

    private func junkPenalty(_ item: IndexItem) -> Double {
        let path = item.path.lowercased()

        // Penalize common junk paths
        let junkPatterns = [
            "/node_modules/",
            "/.git/",
            "/build/",
            "/dist/",
            "/target/",
            "/.cache/",
            "/coverage/",
            "/__pycache__/",
            "/.next/",
            "/vendor/",
        ]

        for pattern in junkPatterns {
            if path.contains(pattern) {
                return weights.junkPenalty
            }
        }

        // Penalize hidden files slightly (but not too much - some are important)
        if item.filename.hasPrefix(".") && !isImportantDotfile(item.filename) {
            return weights.hiddenFilePenalty
        }

        return 0
    }

    private func isCodeFile(_ item: IndexItem) -> Bool {
        let codeExtensions = ["swift", "py", "js", "ts", "go", "rs", "java", "cpp", "c", "h", "rb", "php"]
        return codeExtensions.contains(item.fileExtension)
    }

    private func isDocumentFile(_ item: IndexItem) -> Bool {
        let docExtensions = ["md", "txt", "doc", "docx", "pdf", "rtf", "pages"]
        return docExtensions.contains(item.fileExtension)
    }

    private func isImportantDotfile(_ filename: String) -> Bool {
        let important = [
            ".gitignore", ".gitconfig", ".zshrc", ".bashrc", ".vimrc",
            ".env", ".editorconfig", ".prettierrc", ".eslintrc"
        ]
        return important.contains(filename) || filename.hasSuffix("rc")
    }
}

/// Result with computed score
public struct ScoredResult: Sendable {
    public let item: IndexItem
    public let score: Double
    public let matchType: MatchType
    public let frequency: ItemFrequency?

    public init(item: IndexItem, score: Double, matchType: MatchType, frequency: ItemFrequency?) {
        self.item = item
        self.score = score
        self.matchType = matchType
        self.frequency = frequency
    }
}

/// Configurable weights for scoring
public struct ScoringWeights: Sendable {
    public let exactNameBoost: Double
    public let prefixNameBoost: Double
    public let substringNameBoost: Double
    public let pathTokenBoost: Double
    public let contentExactBoost: Double
    public let contentFuzzyBoost: Double
    public let semanticBoost: Double

    public let recencyBoost: Double
    public let frequencyBoost: Double
    public let pinnedBoost: Double

    public let cwdBoost: Double
    public let repoProximityBoost: Double
    public let recentPathBoost: Double
    public let appContextBoost: Double

    public let junkPenalty: Double
    public let hiddenFilePenalty: Double

    public init(
        exactNameBoost: Double = 100,
        prefixNameBoost: Double = 80,
        substringNameBoost: Double = 50,
        pathTokenBoost: Double = 30,
        contentExactBoost: Double = 20,
        contentFuzzyBoost: Double = 10,
        semanticBoost: Double = 15,
        recencyBoost: Double = 20,
        frequencyBoost: Double = 25,
        pinnedBoost: Double = 200,
        cwdBoost: Double = 30,
        repoProximityBoost: Double = 15,
        recentPathBoost: Double = 20,
        appContextBoost: Double = 10,
        junkPenalty: Double = -50,
        hiddenFilePenalty: Double = -10
    ) {
        self.exactNameBoost = exactNameBoost
        self.prefixNameBoost = prefixNameBoost
        self.substringNameBoost = substringNameBoost
        self.pathTokenBoost = pathTokenBoost
        self.contentExactBoost = contentExactBoost
        self.contentFuzzyBoost = contentFuzzyBoost
        self.semanticBoost = semanticBoost
        self.recencyBoost = recencyBoost
        self.frequencyBoost = frequencyBoost
        self.pinnedBoost = pinnedBoost
        self.cwdBoost = cwdBoost
        self.repoProximityBoost = repoProximityBoost
        self.recentPathBoost = recentPathBoost
        self.appContextBoost = appContextBoost
        self.junkPenalty = junkPenalty
        self.hiddenFilePenalty = hiddenFilePenalty
    }

    public static let `default` = ScoringWeights()
}
