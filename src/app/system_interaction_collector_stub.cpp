#include "system_interaction_collector.h"

namespace bs {

struct SystemInteractionCollector::Impl {
    bool enabled = false;
};

SystemInteractionCollector::SystemInteractionCollector(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

SystemInteractionCollector::~SystemInteractionCollector() = default;

bool SystemInteractionCollector::enabled() const
{
    return m_impl->enabled;
}

void SystemInteractionCollector::setEnabled(bool enabled)
{
    if (m_impl->enabled == enabled) {
        return;
    }
    m_impl->enabled = enabled;

    QJsonObject health;
    health[QStringLiteral("enabled")] = enabled;
    health[QStringLiteral("platformSupported")] = false;
    emit collectorHealthChanged(health);
}

} // namespace bs

