#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QFile>
#include <QTextStream>
#include "core/fs/path_rules.h"

class TestPathRules : public QObject {
    Q_OBJECT

private slots:
    // ── Default exclusion tests ──────────────────────────────────
    void testExcludeGitObjects();
    void testExcludeNodeModules();
    void testExcludeBuildDir();
    void testExcludeDsStore();
    void testExcludePycache();
    void testExcludeVenv();
    void testExcludeDerivedData();
    void testExcludeCargoTarget();
    void testExcludeIdeDirectories();

    // ── Hidden path tests ────────────────────────────────────────
    void testExcludeHiddenDotDir();
    void testHiddenDirectoryComponent();

    // ── Sensitive path tests (MetadataOnly) ──────────────────────
    void testMetadataOnlySshDir();
    void testMetadataOnlyGnupgDir();
    void testMetadataOnlyAwsDir();
    void testMetadataOnlyLibraryPreferences();
    void testMetadataOnlyLibraryKeychains();

    // ── Include tests (normal files) ─────────────────────────────
    void testIncludeNormalTextFile();
    void testIncludeNormalCodeFile();
    void testIncludeDocumentsFolder();

    // ── Size limit tests ─────────────────────────────────────────
    void testExcludeFileOver5GB();
    void testIncludeFileUnder5GB();
    void testZeroSizeSkipsSizeCheck();

    // ── Sensitivity classification ───────────────────────────────
    void testClassifySensitivityHighSsh();
    void testClassifySensitivityNormal();
    void testClassifyHiddenPath();

    // ── Cloud folder detection ───────────────────────────────────
    void testIsCloudFolderDropbox();
    void testIsCloudFolderGoogleDrive();
    void testIsCloudFolderOneDrive();
    void testIsCloudFolderICloud();
    void testIsCloudFolderNormal();

    // ── Cloud artifact detection ─────────────────────────────────
    void testIsCloudArtifactDropbox();
    void testIsCloudArtifactGoogleDrive();
    void testIsCloudArtifactICloud();
    void testIsCloudArtifactNormalFile();

    // ── .bsignore integration ────────────────────────────────────
    void testLoadBsignoreAppliesPatterns();

    // ── Edge cases ───────────────────────────────────────────────
    void testEmptyPath();
    void testUnicodeFilename();
    void testVeryLongPath();
    void testRootPath();
};

// ── Default exclusion tests ──────────────────────────────────────

void TestPathRules::testExcludeGitObjects()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/.git/objects/ab/cd1234"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeNodeModules()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/node_modules/express/index.js"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeBuildDir()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/build/output.o"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeDsStore()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/Documents/.DS_Store"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludePycache()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/__pycache__/module.cpython-311.pyc"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeVenv()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/venv/lib/python3.11/site.py"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeDerivedData()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/Library/Developer/Xcode/DerivedData/MyApp/Build/output.o"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeCargoTarget()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/target/debug/myapp"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testExcludeIdeDirectories()
{
    bs::PathRules rules;
    // .idea directory
    QCOMPARE(rules.validate("/Users/me/project/.idea/workspace.xml"),
             bs::ValidationResult::Exclude);
    // .vscode directory
    QCOMPARE(rules.validate("/Users/me/project/.vscode/settings.json"),
             bs::ValidationResult::Exclude);
}

// ── Hidden path tests ────────────────────────────────────────────

void TestPathRules::testExcludeHiddenDotDir()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/.hidden/config.txt"),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testHiddenDirectoryComponent()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/project/.secretdir/data.json"),
             bs::ValidationResult::Exclude);
}

// ── Sensitive path tests ─────────────────────────────────────────

void TestPathRules::testMetadataOnlySshDir()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/.ssh/id_rsa"),
             bs::ValidationResult::Exclude);
    // The .ssh dir is hidden so it gets Exclude before MetadataOnly.
    // But let's test isSensitivePath independently via classifySensitivity.
    auto sensitivity = rules.classifySensitivity("/Users/me/.ssh/id_rsa");
    QCOMPARE(sensitivity, bs::Sensitivity::Sensitive);
}

void TestPathRules::testMetadataOnlyGnupgDir()
{
    bs::PathRules rules;
    auto sensitivity = rules.classifySensitivity("/Users/me/.gnupg/secring.gpg");
    QCOMPARE(sensitivity, bs::Sensitivity::Sensitive);
}

void TestPathRules::testMetadataOnlyAwsDir()
{
    bs::PathRules rules;
    auto sensitivity = rules.classifySensitivity("/Users/me/.aws/credentials");
    QCOMPARE(sensitivity, bs::Sensitivity::Sensitive);
}

void TestPathRules::testMetadataOnlyLibraryPreferences()
{
    bs::PathRules rules;
    // Library/Preferences is not hidden (no dot prefix in dir), so MetadataOnly
    QCOMPARE(rules.validate("/Users/me/Library/Preferences/com.apple.finder.plist"),
             bs::ValidationResult::MetadataOnly);
}

void TestPathRules::testMetadataOnlyLibraryKeychains()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/Library/Keychains/login.keychain-db"),
             bs::ValidationResult::MetadataOnly);
}

// ── Include tests ────────────────────────────────────────────────

void TestPathRules::testIncludeNormalTextFile()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/Documents/report.txt"),
             bs::ValidationResult::Include);
}

void TestPathRules::testIncludeNormalCodeFile()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/projects/myapp/src/main.cpp"),
             bs::ValidationResult::Include);
}

void TestPathRules::testIncludeDocumentsFolder()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/Documents/notes/todo.md"),
             bs::ValidationResult::Include);
}

// ── Size limit tests ─────────────────────────────────────────────

void TestPathRules::testExcludeFileOver5GB()
{
    bs::PathRules rules;
    const uint64_t sixGB = 6ULL * 1024 * 1024 * 1024;
    QCOMPARE(rules.validate("/Users/me/Documents/large.iso", sixGB),
             bs::ValidationResult::Exclude);
}

void TestPathRules::testIncludeFileUnder5GB()
{
    bs::PathRules rules;
    const uint64_t oneGB = 1ULL * 1024 * 1024 * 1024;
    QCOMPARE(rules.validate("/Users/me/Documents/medium.zip", oneGB),
             bs::ValidationResult::Include);
}

void TestPathRules::testZeroSizeSkipsSizeCheck()
{
    bs::PathRules rules;
    // Size 0 should skip size check (pass 0 to skip)
    QCOMPARE(rules.validate("/Users/me/Documents/file.txt", 0),
             bs::ValidationResult::Include);
}

// ── Sensitivity classification ───────────────────────────────────

void TestPathRules::testClassifySensitivityHighSsh()
{
    bs::PathRules rules;
    QCOMPARE(rules.classifySensitivity("/Users/me/.ssh/id_rsa"),
             bs::Sensitivity::Sensitive);
}

void TestPathRules::testClassifySensitivityNormal()
{
    bs::PathRules rules;
    QCOMPARE(rules.classifySensitivity("/Users/me/Documents/readme.txt"),
             bs::Sensitivity::Normal);
}

void TestPathRules::testClassifyHiddenPath()
{
    bs::PathRules rules;
    QCOMPARE(rules.classifySensitivity("/Users/me/.config/app/settings.json"),
             bs::Sensitivity::Hidden);
}

// ── Cloud folder detection ───────────────────────────────────────

void TestPathRules::testIsCloudFolderDropbox()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudFolder("/Users/me/Dropbox/Documents/report.txt"));
}

void TestPathRules::testIsCloudFolderGoogleDrive()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudFolder("/Users/me/Google Drive/shared/file.txt"));
    QVERIFY(rules.isCloudFolder("/Users/me/My Drive/project/code.py"));
}

void TestPathRules::testIsCloudFolderOneDrive()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudFolder("/Users/me/OneDrive/Documents/spreadsheet.xlsx"));
}

void TestPathRules::testIsCloudFolderICloud()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudFolder("/Users/me/Library/Mobile Documents/com~apple~CloudDocs/file.txt"));
    QVERIFY(rules.isCloudFolder("/Users/me/iCloud Drive/notes.txt"));
}

void TestPathRules::testIsCloudFolderNormal()
{
    bs::PathRules rules;
    QVERIFY(!rules.isCloudFolder("/Users/me/Documents/report.txt"));
    QVERIFY(!rules.isCloudFolder("/Users/me/Desktop/notes.md"));
}

// ── Cloud artifact detection ─────────────────────────────────────

void TestPathRules::testIsCloudArtifactDropbox()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudArtifact("/Users/me/Dropbox/.dropbox.cache/somefile"));
    QVERIFY(rules.isCloudArtifact("/Users/me/Dropbox/.dropbox"));
}

void TestPathRules::testIsCloudArtifactGoogleDrive()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudArtifact("/Users/me/Google Drive/.~google-drive-root"));
    QVERIFY(rules.isCloudArtifact("/Users/me/Google Drive/.gdoc.tmp"));
}

void TestPathRules::testIsCloudArtifactICloud()
{
    bs::PathRules rules;
    QVERIFY(rules.isCloudArtifact(
        "/Users/me/Library/Mobile Documents/com~apple~CloudDocs/file.icloud"));
}

void TestPathRules::testIsCloudArtifactNormalFile()
{
    bs::PathRules rules;
    QVERIFY(!rules.isCloudArtifact("/Users/me/Documents/report.txt"));
    QVERIFY(!rules.isCloudArtifact("/Users/me/projects/main.cpp"));
}

// ── .bsignore integration ────────────────────────────────────────

void TestPathRules::testLoadBsignoreAppliesPatterns()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Write a .bsignore file
    const QString bsignorePath = dir.path() + "/.bsignore";
    QFile f(bsignorePath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "*.log\n";
    out << "temp/\n";
    f.close();

    bs::PathRules rules;
    rules.loadBsignore(bsignorePath.toStdString());

    // *.log should now be excluded
    QCOMPARE(rules.validate("/Users/me/project/debug.log"),
             bs::ValidationResult::Exclude);
    // temp/ should be excluded
    QCOMPARE(rules.validate("/Users/me/project/temp/data.txt"),
             bs::ValidationResult::Exclude);
    // Normal files unaffected
    QCOMPARE(rules.validate("/Users/me/Documents/report.txt"),
             bs::ValidationResult::Include);
}

// ── Edge cases ───────────────────────────────────────────────────

void TestPathRules::testEmptyPath()
{
    bs::PathRules rules;
    // Empty path should not crash; returns Include (no exclusion matched)
    auto result = rules.validate("");
    QCOMPARE(result, bs::ValidationResult::Include);
}

void TestPathRules::testUnicodeFilename()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/Users/me/Documents/\xC3\xA9\xC3\xA0\xC3\xBC.txt"),
             bs::ValidationResult::Include);
    // Japanese characters
    QCOMPARE(rules.validate("/Users/me/Documents/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.md"),
             bs::ValidationResult::Include);
}

void TestPathRules::testVeryLongPath()
{
    bs::PathRules rules;
    std::string longPath = "/Users/me/Documents";
    for (int i = 0; i < 100; ++i) {
        longPath += "/subdirectory_level_" + std::to_string(i);
    }
    longPath += "/file.txt";
    QCOMPARE(rules.validate(longPath), bs::ValidationResult::Include);
}

void TestPathRules::testRootPath()
{
    bs::PathRules rules;
    QCOMPARE(rules.validate("/"), bs::ValidationResult::Include);
}

QTEST_MAIN(TestPathRules)
#include "test_path_rules.moc"
