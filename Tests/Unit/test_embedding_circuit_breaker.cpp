#include <QtTest/QtTest>
#include "core/embedding/embedding_manager.h"

#include <chrono>

class TestEmbeddingCircuitBreaker : public QObject {
    Q_OBJECT

private slots:
    void testCircuitBreakerInitiallyClosed();
    void testCircuitBreakerOpensAfterThreshold();
    void testCircuitBreakerResetsOnSuccess();
    void testCircuitBreakerHalfOpenAfterDelay();
    void testCircuitBreakerConstants();
};

void TestEmbeddingCircuitBreaker::testCircuitBreakerInitiallyClosed()
{
    bs::EmbeddingCircuitBreaker cb;
    QVERIFY(!cb.isOpen());
    QCOMPARE(cb.consecutiveFailures.load(), 0);
}

void TestEmbeddingCircuitBreaker::testCircuitBreakerOpensAfterThreshold()
{
    bs::EmbeddingCircuitBreaker cb;

    // Record failures up to threshold
    for (int i = 0; i < bs::EmbeddingCircuitBreaker::kOpenThreshold; ++i) {
        cb.recordFailure();
    }

    // Circuit should now be open
    QVERIFY(cb.isOpen());
    QCOMPARE(cb.consecutiveFailures.load(),
             bs::EmbeddingCircuitBreaker::kOpenThreshold);
}

void TestEmbeddingCircuitBreaker::testCircuitBreakerResetsOnSuccess()
{
    bs::EmbeddingCircuitBreaker cb;

    // Record some failures (but not enough to open)
    cb.recordFailure();
    cb.recordFailure();
    cb.recordFailure();
    QCOMPARE(cb.consecutiveFailures.load(), 3);
    QVERIFY(!cb.isOpen());

    // Success should reset the counter
    cb.recordSuccess();
    QCOMPARE(cb.consecutiveFailures.load(), 0);
    QVERIFY(!cb.isOpen());
}

void TestEmbeddingCircuitBreaker::testCircuitBreakerHalfOpenAfterDelay()
{
    bs::EmbeddingCircuitBreaker cb;

    // Open the circuit
    for (int i = 0; i < bs::EmbeddingCircuitBreaker::kOpenThreshold; ++i) {
        cb.recordFailure();
    }
    QVERIFY(cb.isOpen());

    // Simulate time passing by manually setting lastFailureTime to the past
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    cb.lastFailureTime.store(now - bs::EmbeddingCircuitBreaker::kHalfOpenDelayMs - 1000);

    // Circuit should now be half-open (isOpen returns false to allow a retry)
    QVERIFY(!cb.isOpen());

    // After a successful retry, counter resets
    cb.recordSuccess();
    QCOMPARE(cb.consecutiveFailures.load(), 0);
    QVERIFY(!cb.isOpen());
}

void TestEmbeddingCircuitBreaker::testCircuitBreakerConstants()
{
    QCOMPARE(bs::EmbeddingCircuitBreaker::kOpenThreshold, 5);
    QCOMPARE(bs::EmbeddingCircuitBreaker::kHalfOpenDelayMs, 30000);
}

QTEST_MAIN(TestEmbeddingCircuitBreaker)
#include "test_embedding_circuit_breaker.moc"
