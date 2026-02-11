#include <QtTest/QtTest>

#include "core/vector/search_merger.h"

#include <algorithm>

class TestDualIndexCalibration : public QObject {
    Q_OBJECT

private slots:
    void testWeightedNormalizationBounds();
    void testMonotonicCalibration();
};

void TestDualIndexCalibration::testWeightedNormalizationBounds()
{
    const float threshold = 0.62f;
    const float strongNorm = bs::SearchMerger::normalizeSemanticScore(0.82f, threshold);
    const float fastNorm = bs::SearchMerger::normalizeSemanticScore(0.74f, threshold);

    const double combined = std::min(1.0, (0.60 * strongNorm) + (0.40 * fastNorm));
    const double calibratedCosine = static_cast<double>(threshold)
        + ((1.0 - static_cast<double>(threshold)) * combined);

    QVERIFY(combined >= 0.0);
    QVERIFY(combined <= 1.0);
    QVERIFY(calibratedCosine >= threshold);
    QVERIFY(calibratedCosine <= 1.0);
}

void TestDualIndexCalibration::testMonotonicCalibration()
{
    const float threshold = 0.66f;
    const float fastNorm = bs::SearchMerger::normalizeSemanticScore(0.72f, threshold);
    const float strongLow = bs::SearchMerger::normalizeSemanticScore(0.70f, threshold);
    const float strongHigh = bs::SearchMerger::normalizeSemanticScore(0.85f, threshold);

    const double combinedLow = std::min(1.0, (0.60 * strongLow) + (0.40 * fastNorm));
    const double combinedHigh = std::min(1.0, (0.60 * strongHigh) + (0.40 * fastNorm));
    QVERIFY(combinedHigh >= combinedLow);
}

QTEST_MAIN(TestDualIndexCalibration)
#include "test_dual_index_calibration.moc"

