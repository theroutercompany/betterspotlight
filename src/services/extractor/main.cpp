#include "extractor_service.h"
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("betterspotlight-extractor"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    bs::ExtractorService service;
    return service.run();
}
