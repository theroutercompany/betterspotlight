import XCTest
@testable import Core

final class PathRulesTests: XCTestCase {

    // MARK: - Exclusion Tests

    func testBuiltInExclusionsMatchNodeModules() {
        let rules = PathRules()

        XCTAssertTrue(rules.shouldExclude("/Users/test/project/node_modules"))
        XCTAssertTrue(rules.shouldExclude("/Users/test/project/node_modules/package/index.js"))
    }

    func testBuiltInExclusionsMatchBuildFolders() {
        let rules = PathRules()

        XCTAssertTrue(rules.shouldExclude("/Users/test/project/build"))
        XCTAssertTrue(rules.shouldExclude("/Users/test/project/dist"))
        XCTAssertTrue(rules.shouldExclude("/Users/test/project/target"))
    }

    func testBuiltInExclusionsMatchCaches() {
        let rules = PathRules()

        XCTAssertTrue(rules.shouldExclude("/Users/test/.cache"))
        XCTAssertTrue(rules.shouldExclude("/Users/test/Library/Caches"))
    }

    func testNormalPathsNotExcluded() {
        let rules = PathRules()

        XCTAssertFalse(rules.shouldExclude("/Users/test/Documents/file.txt"))
        XCTAssertFalse(rules.shouldExclude("/Users/test/Projects/myapp/src/main.swift"))
    }

    func testOverridesWinOverRules() {
        let rules = PathRules(
            overrides: ["/Users/test/project/node_modules": .index]
        )

        // Override says to index, so should not be excluded
        XCTAssertFalse(rules.shouldExclude("/Users/test/project/node_modules"))
    }

    // MARK: - Sensitivity Tests

    func testSensitiveFoldersDetected() {
        let rules = PathRules()

        XCTAssertTrue(rules.isSensitive("/Users/test/.ssh"))
        XCTAssertTrue(rules.isSensitive("/Users/test/.ssh/id_rsa"))
        XCTAssertTrue(rules.isSensitive("/Users/test/.gnupg"))
        XCTAssertTrue(rules.isSensitive("/Users/test/.aws"))
        XCTAssertTrue(rules.isSensitive("/Users/test/.kube"))
        XCTAssertTrue(rules.isSensitive("/Users/test/project/.env"))
        XCTAssertTrue(rules.isSensitive("/Users/test/project/.env.local"))
    }

    func testNormalFoldersNotSensitive() {
        let rules = PathRules()

        XCTAssertFalse(rules.isSensitive("/Users/test/Documents"))
        XCTAssertFalse(rules.isSensitive("/Users/test/Projects"))
        XCTAssertFalse(rules.isSensitive("/Users/test/.config"))
    }

    // MARK: - Classification Tests

    func testClassificationForExcludedPaths() {
        let rules = PathRules()

        XCTAssertEqual(rules.classification(for: "/Users/test/project/node_modules"), .exclude)
    }

    func testClassificationForNormalPaths() {
        let rules = PathRules()

        XCTAssertEqual(rules.classification(for: "/Users/test/Documents/file.txt"), .index)
    }

    func testClassificationWithOverride() {
        let rules = PathRules(
            overrides: ["/Users/test/Documents": .metadataOnly]
        )

        XCTAssertEqual(rules.classification(for: "/Users/test/Documents/file.txt"), .metadataOnly)
    }

    // MARK: - Cloud Folder Detection

    func testCloudFolderDetection() {
        XCTAssertTrue(PathRules.isCloudFolder("/Users/test/Library/Mobile Documents"))
        XCTAssertTrue(PathRules.isCloudFolder("/Users/test/Dropbox"))
        XCTAssertTrue(PathRules.isCloudFolder("/Users/test/Google Drive"))
        XCTAssertTrue(PathRules.isCloudFolder("/Users/test/OneDrive"))
    }

    func testNonCloudFolders() {
        XCTAssertFalse(PathRules.isCloudFolder("/Users/test/Documents"))
        XCTAssertFalse(PathRules.isCloudFolder("/Users/test/Projects"))
    }

    // MARK: - Repo Detection

    func testRepoRootDetection() {
        // This test would need actual file system state
        // For unit tests, we just verify the method exists and handles edge cases
        XCTAssertNil(PathRules.findRepoRoot(for: "/"))
        XCTAssertNil(PathRules.findRepoRoot(for: ""))
    }

    // MARK: - Suggested Classification

    func testSuggestedClassificationForCommonFolders() {
        XCTAssertEqual(PathRules.suggestedClassification(for: "Documents"), .index)
        XCTAssertEqual(PathRules.suggestedClassification(for: "Desktop"), .index)
        XCTAssertEqual(PathRules.suggestedClassification(for: "Downloads"), .index)
        XCTAssertEqual(PathRules.suggestedClassification(for: "Projects"), .index)
        XCTAssertEqual(PathRules.suggestedClassification(for: "Developer"), .index)
    }

    func testSuggestedClassificationForExcludedFolders() {
        XCTAssertEqual(PathRules.suggestedClassification(for: "Library"), .exclude)
        XCTAssertEqual(PathRules.suggestedClassification(for: ".Trash"), .exclude)
        XCTAssertEqual(PathRules.suggestedClassification(for: ".npm"), .exclude)
        XCTAssertEqual(PathRules.suggestedClassification(for: ".cache"), .exclude)
    }

    func testSuggestedClassificationForSensitiveFolders() {
        XCTAssertEqual(PathRules.suggestedClassification(for: ".ssh"), .metadataOnly)
        XCTAssertEqual(PathRules.suggestedClassification(for: ".gnupg"), .metadataOnly)
        XCTAssertEqual(PathRules.suggestedClassification(for: ".aws"), .metadataOnly)
    }
}
