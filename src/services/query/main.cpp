#include "query_service.h"
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("betterspotlight-query"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    bs::QueryService service;
    return service.run();
}
