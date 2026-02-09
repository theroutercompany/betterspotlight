#include <QtTest/QtTest>
#include "core/query/doctype_classifier.h"
#include "core/query/structured_query.h"
#include "core/shared/search_result.h"
#include "core/shared/scoring_types.h"

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

class TestStructuredQueryBoost : public QObject {
    Q_OBJECT

private slots:
    void testTemporalBoostInRange();
    void testTemporalBoostNearMiss();
    void testTemporalBoostOutOfRange();
    void testDocTypeExtensionMatching();
    void testDocTypeUnknownIntent();
    void testEntityNameMatch();
    void testEntityPathMatch();
    void testEntityCapBehavior();
    void testExtensionsForAllIntents();

private:
    // Helper: apply structured query boosts to a single result
    // (mirrors the logic in query_service.cpp)
    double computeSqBoost(const bs::StructuredQuery& structured,
                          bs::SearchResult& candidate,
                          const bs::ScoringWeights& weights = {}) const;
};

double TestStructuredQueryBoost::computeSqBoost(
    const bs::StructuredQuery& structured,
    bs::SearchResult& candidate,
    const bs::ScoringWeights& weights) const
{
    double sqBoost = 0.0;

    if (structured.temporal.has_value() && !candidate.modificationDate.isEmpty()) {
        bool ok = false;
        double modAt = candidate.modificationDate.toDouble(&ok);
        if (!ok) {
            const QDateTime dt = QDateTime::fromString(candidate.modificationDate, Qt::ISODate);
            if (dt.isValid()) {
                modAt = static_cast<double>(dt.toSecsSinceEpoch());
                ok = true;
            }
        }
        if (ok) {
            if (modAt >= structured.temporal->startEpoch &&
                modAt <= structured.temporal->endEpoch) {
                sqBoost += static_cast<double>(weights.temporalBoostWeight);
            } else {
                const double rangeSize = structured.temporal->endEpoch - structured.temporal->startEpoch;
                if (modAt >= structured.temporal->startEpoch - rangeSize &&
                    modAt <= structured.temporal->endEpoch + rangeSize) {
                    sqBoost += static_cast<double>(weights.temporalNearWeight);
                }
            }
        }
    }

    if (structured.docTypeIntent.has_value()) {
        const auto exts = bs::DoctypeClassifier::extensionsForIntent(*structured.docTypeIntent);
        const QString ext = QFileInfo(candidate.path).suffix().toLower();
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            sqBoost += static_cast<double>(weights.docTypeIntentWeight);
        }
    }

    double entityBoost = 0.0;
    for (const auto& entity : structured.entities) {
        if (candidate.name.contains(entity.text, Qt::CaseInsensitive) ||
            candidate.path.contains(entity.text, Qt::CaseInsensitive)) {
            entityBoost += static_cast<double>(weights.entityMatchWeight);
        }
    }
    sqBoost += std::min(entityBoost, static_cast<double>(weights.entityMatchCap));

    return sqBoost;
}

void TestStructuredQueryBoost::testTemporalBoostInRange()
{
    bs::StructuredQuery sq;
    sq.temporal = bs::TemporalRange{
        1700000000.0,  // ~Nov 2023
        1702500000.0,  // ~Dec 2023
    };

    bs::SearchResult result;
    result.path = QStringLiteral("/home/user/report.pdf");
    result.name = QStringLiteral("report.pdf");
    result.modificationDate = QStringLiteral("1701000000.0"); // Within range

    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 12.0); // temporalBoostWeight default
}

void TestStructuredQueryBoost::testTemporalBoostNearMiss()
{
    bs::StructuredQuery sq;
    sq.temporal = bs::TemporalRange{
        1700000000.0,
        1702500000.0,
    };
    const double rangeSize = sq.temporal->endEpoch - sq.temporal->startEpoch;

    bs::SearchResult result;
    result.path = QStringLiteral("/home/user/report.pdf");
    result.name = QStringLiteral("report.pdf");
    // Just outside the range, but within 2x range
    result.modificationDate = QString::number(sq.temporal->startEpoch - rangeSize * 0.5);

    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 6.0); // temporalNearWeight default
}

void TestStructuredQueryBoost::testTemporalBoostOutOfRange()
{
    bs::StructuredQuery sq;
    sq.temporal = bs::TemporalRange{
        1700000000.0,
        1702500000.0,
    };
    const double rangeSize = sq.temporal->endEpoch - sq.temporal->startEpoch;

    bs::SearchResult result;
    result.path = QStringLiteral("/home/user/old.pdf");
    result.name = QStringLiteral("old.pdf");
    // Far outside the range (well beyond 2x buffer)
    result.modificationDate = QString::number(sq.temporal->startEpoch - rangeSize * 3.0);

    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 0.0);
}

void TestStructuredQueryBoost::testDocTypeExtensionMatching()
{
    bs::StructuredQuery sq;
    sq.docTypeIntent = QStringLiteral("financial_document");

    bs::SearchResult pdfResult;
    pdfResult.path = QStringLiteral("/home/user/budget.pdf");
    pdfResult.name = QStringLiteral("budget.pdf");
    const double pdfBoost = computeSqBoost(sq, pdfResult);
    QCOMPARE(pdfBoost, 10.0); // docTypeIntentWeight

    bs::SearchResult xlsxResult;
    xlsxResult.path = QStringLiteral("/home/user/budget.xlsx");
    xlsxResult.name = QStringLiteral("budget.xlsx");
    const double xlsxBoost = computeSqBoost(sq, xlsxResult);
    QCOMPARE(xlsxBoost, 10.0);

    // Non-matching extension
    bs::SearchResult txtResult;
    txtResult.path = QStringLiteral("/home/user/notes.txt");
    txtResult.name = QStringLiteral("notes.txt");
    const double txtBoost = computeSqBoost(sq, txtResult);
    QCOMPARE(txtBoost, 0.0);
}

void TestStructuredQueryBoost::testDocTypeUnknownIntent()
{
    bs::StructuredQuery sq;
    sq.docTypeIntent = QStringLiteral("unknown_type");

    bs::SearchResult result;
    result.path = QStringLiteral("/home/user/file.pdf");
    result.name = QStringLiteral("file.pdf");
    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 0.0);
}

void TestStructuredQueryBoost::testEntityNameMatch()
{
    bs::StructuredQuery sq;
    sq.entities.push_back({QStringLiteral("Johnson"), bs::EntityType::Person});

    bs::SearchResult result;
    result.path = QStringLiteral("/home/user/Johnson_contract.pdf");
    result.name = QStringLiteral("Johnson_contract.pdf");
    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 8.0); // entityMatchWeight
}

void TestStructuredQueryBoost::testEntityPathMatch()
{
    bs::StructuredQuery sq;
    sq.entities.push_back({QStringLiteral("Acme"), bs::EntityType::Organization});

    bs::SearchResult result;
    result.path = QStringLiteral("/home/user/Acme/report.pdf");
    result.name = QStringLiteral("report.pdf");
    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 8.0);
}

void TestStructuredQueryBoost::testEntityCapBehavior()
{
    bs::StructuredQuery sq;
    sq.entities.push_back({QStringLiteral("Alice"), bs::EntityType::Person});
    sq.entities.push_back({QStringLiteral("Bob"), bs::EntityType::Person});
    sq.entities.push_back({QStringLiteral("Charlie"), bs::EntityType::Person});

    bs::SearchResult result;
    result.path = QStringLiteral("/home/Alice/Bob/Charlie/file.pdf");
    result.name = QStringLiteral("file.pdf");

    // 3 * 8.0 = 24.0, but capped at entityMatchCap=16.0
    const double boost = computeSqBoost(sq, result);
    QCOMPARE(boost, 16.0); // entityMatchCap
}

void TestStructuredQueryBoost::testExtensionsForAllIntents()
{
    // Verify all known intents return non-empty extension lists
    const QStringList knownIntents = {
        QStringLiteral("legal_document"),
        QStringLiteral("financial_document"),
        QStringLiteral("job_document"),
        QStringLiteral("presentation"),
        QStringLiteral("image"),
        QStringLiteral("spreadsheet"),
        QStringLiteral("notes"),
        QStringLiteral("documentation"),
        QStringLiteral("report"),
        QStringLiteral("application_form"),
        QStringLiteral("reference_material"),
    };

    for (const QString& intent : knownIntents) {
        const auto exts = bs::DoctypeClassifier::extensionsForIntent(intent);
        QVERIFY2(!exts.empty(), qPrintable(QStringLiteral("No extensions for intent: ") + intent));
    }

    // Unknown intent should return empty
    QVERIFY(bs::DoctypeClassifier::extensionsForIntent(QStringLiteral("unknown")).empty());
}

QTEST_MAIN(TestStructuredQueryBoost)
#include "test_structured_query_boost.moc"
