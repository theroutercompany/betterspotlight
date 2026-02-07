#pragma once

#include "core/ipc/service_base.h"
#include "core/extraction/extraction_manager.h"

namespace bs {

class ExtractorService : public ServiceBase {
    Q_OBJECT
public:
    explicit ExtractorService(QObject* parent = nullptr);

protected:
    QJsonObject handleRequest(const QJsonObject& request) override;

private:
    QJsonObject handleExtractText(uint64_t id, const QJsonObject& params);
    QJsonObject handleExtractMetadata(uint64_t id, const QJsonObject& params);
    QJsonObject handleIsSupported(uint64_t id, const QJsonObject& params);
    QJsonObject handleCancelExtraction(uint64_t id, const QJsonObject& params);

    ExtractionManager m_extractor;
};

} // namespace bs
