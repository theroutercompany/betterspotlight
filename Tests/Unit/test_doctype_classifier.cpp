#include <QtTest/QtTest>

#include "core/query/doctype_classifier.h"

class TestDoctypeClassifier : public QObject {
    Q_OBJECT

private slots:
    void testLegalDocument();
    void testFinancialDocument();
    void testNoIntent();
    void testMultiWordPriority();
};

void TestDoctypeClassifier::testLegalDocument()
{
    auto result = bs::DoctypeClassifier::classify(QStringLiteral("lease agreement"));
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), QStringLiteral("legal_document"));
}

void TestDoctypeClassifier::testFinancialDocument()
{
    auto result = bs::DoctypeClassifier::classify(
        QStringLiteral("credit card application"));
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), QStringLiteral("financial_document"));
}

void TestDoctypeClassifier::testNoIntent()
{
    auto result = bs::DoctypeClassifier::classify(
        QStringLiteral("gobi desert photos"));
    // "photos" is not in our keyword set (only "photo" is), and "desert" is not
    // a doctype keyword, so this should return nullopt.
    QVERIFY(!result.has_value());
}

void TestDoctypeClassifier::testMultiWordPriority()
{
    // "credit card" should match the multi-word pattern for financial_document
    // before any single-word pattern could match.
    auto result = bs::DoctypeClassifier::classify(QStringLiteral("credit card"));
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), QStringLiteral("financial_document"));
}

QTEST_MAIN(TestDoctypeClassifier)
#include "test_doctype_classifier.moc"
