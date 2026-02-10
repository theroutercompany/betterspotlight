#include "platform_integration.h"

#include <QtGlobal>

namespace bs {

namespace {

class DefaultPlatformIntegration final : public PlatformIntegration {
public:
    PlatformOperationResult setLaunchAtLogin(bool enabled) override
    {
        Q_UNUSED(enabled);
        return {false, QStringLiteral("Launch-at-login integration is unavailable on this platform.")};
    }

    PlatformOperationResult setShowInDock(bool enabled) override
    {
        Q_UNUSED(enabled);
        return {false, QStringLiteral("Dock visibility integration is unavailable on this platform.")};
    }
};

} // namespace

#if defined(Q_OS_MACOS)
std::unique_ptr<PlatformIntegration> createApplePlatformIntegration();
#endif

std::unique_ptr<PlatformIntegration> PlatformIntegration::create()
{
#if defined(Q_OS_MACOS)
    if (auto integration = createApplePlatformIntegration()) {
        return integration;
    }
#endif
    return std::make_unique<DefaultPlatformIntegration>();
}

} // namespace bs
