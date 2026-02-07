#include "indexer_service.h"
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("betterspotlight-indexer"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    bs::IndexerService service;
    return service.run();
}
