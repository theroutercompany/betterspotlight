#include "system_interaction_collector.h"

namespace bs {

struct SystemInteractionCollector::Impl {
    bool enabled = false;
    bool captureAppActivityEnabled = true;
    bool captureInputActivityEnabled = true;
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
    health[QStringLiteral("captureAppActivityEnabled")] = m_impl->captureAppActivityEnabled;
    health[QStringLiteral("captureInputActivityEnabled")] = m_impl->captureInputActivityEnabled;
    emit collectorHealthChanged(health);
}

void SystemInteractionCollector::setCaptureScope(bool appActivityEnabled, bool inputActivityEnabled)
{
    if (m_impl->captureAppActivityEnabled == appActivityEnabled
        && m_impl->captureInputActivityEnabled == inputActivityEnabled) {
        return;
    }
    m_impl->captureAppActivityEnabled = appActivityEnabled;
    m_impl->captureInputActivityEnabled = inputActivityEnabled;

    QJsonObject health;
    health[QStringLiteral("enabled")] = m_impl->enabled;
    health[QStringLiteral("platformSupported")] = false;
    health[QStringLiteral("captureAppActivityEnabled")] = m_impl->captureAppActivityEnabled;
    health[QStringLiteral("captureInputActivityEnabled")] = m_impl->captureInputActivityEnabled;
    emit collectorHealthChanged(health);
}

} // namespace bs
