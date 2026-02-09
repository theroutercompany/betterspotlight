#include <QtTest/QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "core/extraction/extraction_manager.h"

class TestExtractionExtensionFallback : public QObject {
    Q_OBJECT

private slots:
    void testKnownCodeExtensionElExtractsAsText();
    void testUnknownCodeExtensionFallsBackWhenTextLike();
    void testUnknownKindFallsBackWhenTextLike();
    void testUnknownCodeExtensionRejectsBinaryLikePayload();
};

void TestExtractionExtensionFallback::testKnownCodeExtensionElExtractsAsText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.path() + QStringLiteral("/init.el");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("(defun hello-world ()\n  (message \"hello\"))\n");
    file.close();

    bs::ExtractionManager manager;
    const auto result = manager.extract(path, bs::ItemKind::Code);

    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("defun")));
}

void TestExtractionExtensionFallback::testUnknownCodeExtensionFallsBackWhenTextLike()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.path() + QStringLiteral("/script.unknowncodeext");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("#!/usr/bin/env custom\n");
    file.write("function compute(value) {\n  return value + 1;\n}\n");
    file.close();

    bs::ExtractionManager manager;
    const auto result = manager.extract(path, bs::ItemKind::Code);

    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("compute")));
}

void TestExtractionExtensionFallback::testUnknownKindFallsBackWhenTextLike()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.path() + QStringLiteral("/README.customext");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("This is plain text with an uncommon extension.\n");
    file.write("Fallback should still extract this content.\n");
    file.close();

    bs::ExtractionManager manager;
    const auto result = manager.extract(path, bs::ItemKind::Unknown);

    QCOMPARE(result.status, bs::ExtractionResult::Status::Success);
    QVERIFY(result.content.has_value());
    QVERIFY(result.content->contains(QStringLiteral("uncommon extension")));
}

void TestExtractionExtensionFallback::testUnknownCodeExtensionRejectsBinaryLikePayload()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.path() + QStringLiteral("/blob.unknowncodeext");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray binary("\x00\xff\x10\x01\x02\x03\x00\xff", 8);
    file.write(binary);
    file.close();

    bs::ExtractionManager manager;
    const auto result = manager.extract(path, bs::ItemKind::Code);

    QCOMPARE(result.status, bs::ExtractionResult::Status::UnsupportedFormat);
}

QTEST_MAIN(TestExtractionExtensionFallback)
#include "test_extraction_extension_fallback.moc"
