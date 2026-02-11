#include "inference_service.h"

#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("betterspotlight-inference"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    bs::InferenceService service;
    return service.run();
}
