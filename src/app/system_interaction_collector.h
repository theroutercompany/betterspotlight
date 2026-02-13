#pragma once

#include <QObject>
#include <QJsonObject>

#include <memory>

namespace bs {

class SystemInteractionCollector : public QObject {
    Q_OBJECT

public:
    explicit SystemInteractionCollector(QObject* parent = nullptr);
    ~SystemInteractionCollector() override;

    bool enabled() const;
    void setEnabled(bool enabled);
    void setCaptureScope(bool appActivityEnabled, bool inputActivityEnabled);

signals:
    void behaviorEventCaptured(const QJsonObject& event);
    void collectorHealthChanged(const QJsonObject& health);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bs
