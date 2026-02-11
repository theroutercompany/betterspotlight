#include <QtTest/QtTest>

#include "core/query/query_router.h"

class TestQueryRouter : public QObject {
    Q_OBJECT

private slots:
    void testClassifiesPathOrCode();
    void testClassifiesNaturalLanguage();
    void testClassifiesShortAmbiguous();
    void testDomainSignals();
};

void TestQueryRouter::testClassifiesPathOrCode()
{
    const std::vector<QString> keyTokens = {QStringLiteral("src"), QStringLiteral("cpp")};
    const bs::QueryRouterResult routed = bs::QueryRouter::route(
        QStringLiteral("src/core/query/rules_engine.cpp"),
        QStringLiteral("src/core/query/rules_engine.cpp"),
        keyTokens);
    QVERIFY(routed.valid);
    QCOMPARE(routed.queryClass, bs::QueryClass::PathOrCode);
    QVERIFY(routed.routerConfidence >= 0.8f);
}

void TestQueryRouter::testClassifiesNaturalLanguage()
{
    const std::vector<QString> keyTokens = {
        QStringLiteral("meeting"),
        QStringLiteral("notes"),
        QStringLiteral("rollout"),
        QStringLiteral("plan"),
    };
    const bs::QueryRouterResult routed = bs::QueryRouter::route(
        QStringLiteral("meeting notes rollout plan"),
        QStringLiteral("meeting notes rollout plan"),
        keyTokens);
    QVERIFY(routed.valid);
    QCOMPARE(routed.queryClass, bs::QueryClass::NaturalLanguage);
    QVERIFY(routed.semanticNeedScore > 0.5f);
}

void TestQueryRouter::testClassifiesShortAmbiguous()
{
    const std::vector<QString> keyTokens = {QStringLiteral("budget")};
    const bs::QueryRouterResult routed = bs::QueryRouter::route(
        QStringLiteral("budget"),
        QStringLiteral("budget"),
        keyTokens);
    QVERIFY(routed.valid);
    QCOMPARE(routed.queryClass, bs::QueryClass::ShortAmbiguous);
}

void TestQueryRouter::testDomainSignals()
{
    const std::vector<QString> devTokens = {
        QStringLiteral("build"),
        QStringLiteral("api"),
        QStringLiteral("error"),
    };
    const bs::QueryRouterResult devRouted = bs::QueryRouter::route(
        QStringLiteral("build api error"),
        QStringLiteral("build api error"),
        devTokens);
    QCOMPARE(devRouted.queryDomain, bs::QueryDomain::DevCode);

    const std::vector<QString> financeTokens = {
        QStringLiteral("tax"),
        QStringLiteral("receipt"),
    };
    const bs::QueryRouterResult financeRouted = bs::QueryRouter::route(
        QStringLiteral("tax receipt"),
        QStringLiteral("tax receipt"),
        financeTokens);
    QCOMPARE(financeRouted.queryDomain, bs::QueryDomain::Finance);
}

QTEST_MAIN(TestQueryRouter)
#include "test_query_router.moc"

