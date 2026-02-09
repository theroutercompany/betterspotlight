#include <QtTest/QtTest>

#include <QDir>
#include <QFileInfo>

class TestDocsMemoLocation : public QObject {
    Q_OBJECT

private slots:
    void testExecutiveMemoLivesUnderDocs();
};

void TestDocsMemoLocation::testExecutiveMemoLivesUnderDocs()
{
#ifdef BETTERSPOTLIGHT_SOURCE_DIR
    const QString repoRoot = QString::fromUtf8(BETTERSPOTLIGHT_SOURCE_DIR);
#else
    const QString repoRoot = QDir::currentPath();
#endif
    const QString rootMemo = QDir(repoRoot)
                                 .filePath(QStringLiteral(
                                     "EXECUTIVE_RETRIEVAL_REMEDIATION_MEMO_2026-02-09.md"));
    const QString docsMemo = QDir(repoRoot)
                                 .filePath(QStringLiteral(
                                     "docs/EXECUTIVE_RETRIEVAL_REMEDIATION_MEMO_2026-02-09.md"));

    QVERIFY2(!QFileInfo::exists(rootMemo),
             qPrintable(QStringLiteral("Memo must not exist in repo root: %1").arg(rootMemo)));
    QVERIFY2(QFileInfo::exists(docsMemo),
             qPrintable(QStringLiteral("Memo is missing from docs/: %1").arg(docsMemo)));
}

QTEST_MAIN(TestDocsMemoLocation)
#include "test_docs_memo_location.moc"
