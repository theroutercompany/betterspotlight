#include "platform_integration.h"

#include <QtGlobal>

#import <AppKit/AppKit.h>
#if __has_include(<ServiceManagement/ServiceManagement.h>)
#import <ServiceManagement/ServiceManagement.h>
#endif

namespace bs {

namespace {

QString toQString(NSString* value)
{
    return value ? QString::fromUtf8(value.UTF8String) : QString();
}

class MacPlatformIntegration final : public PlatformIntegration {
public:
    PlatformOperationResult setLaunchAtLogin(bool enabled) override
    {
#if __has_include(<ServiceManagement/ServiceManagement.h>)
        if (@available(macOS 13.0, *)) {
            SMAppService* service = [SMAppService mainAppService];
            if (!service) {
                return {false, QStringLiteral("Launch-at-login service is unavailable.")};
            }

            NSError* error = nil;
            const BOOL ok = enabled
                ? [service registerAndReturnError:&error]
                : [service unregisterAndReturnError:&error];
            if (ok) {
                return {true, enabled
                                   ? QStringLiteral("Launch at login enabled.")
                                   : QStringLiteral("Launch at login disabled.")};
            }

            const QString detail = error ? toQString(error.localizedDescription)
                                         : QStringLiteral("Unknown ServiceManagement error");
            return {
                false,
                QStringLiteral("Unable to update launch-at-login (%1).").arg(detail),
            };
        }
#endif
        Q_UNUSED(enabled);
        return {false, QStringLiteral("Launch-at-login requires macOS 13 or newer.")};
    }

    PlatformOperationResult setShowInDock(bool enabled) override
    {
        if (!NSApp) {
            return {false, QStringLiteral("Application instance is unavailable.")};
        }

        const NSApplicationActivationPolicy targetPolicy =
            enabled ? NSApplicationActivationPolicyRegular
                    : NSApplicationActivationPolicyAccessory;
        const BOOL ok = [NSApp setActivationPolicy:targetPolicy];
        if (!ok) {
            return {false, QStringLiteral("Failed to apply Dock visibility policy.")};
        }

        if (enabled) {
            [NSApp activateIgnoringOtherApps:NO];
        }

        return {true, enabled ? QStringLiteral("Dock icon shown.")
                              : QStringLiteral("Dock icon hidden.")};
    }
};

} // namespace

std::unique_ptr<PlatformIntegration> createApplePlatformIntegration()
{
    return std::make_unique<MacPlatformIntegration>();
}

} // namespace bs
