#include "update_manager.h"

#include "core/shared/logging.h"

#import <Foundation/Foundation.h>

#ifdef BETTERSPOTLIGHT_ENABLE_SPARKLE
#import <Sparkle/Sparkle.h>
#endif

namespace bs {

struct UpdateManager::Impl {
#ifdef BETTERSPOTLIGHT_ENABLE_SPARKLE
    SPUStandardUpdaterController* controller = nil;
#endif
};

UpdateManager::UpdateManager(QObject* parent)
    : QObject(parent)
    , m_impl(new Impl())
{
}

UpdateManager::~UpdateManager()
{
#ifdef BETTERSPOTLIGHT_ENABLE_SPARKLE
    if (m_impl && m_impl->controller) {
        [m_impl->controller release];
        m_impl->controller = nil;
    }
#endif
    delete m_impl;
    m_impl = nullptr;
}

bool UpdateManager::available() const
{
    return m_available;
}

bool UpdateManager::automaticallyChecks() const
{
    return m_automaticallyChecks;
}

QString UpdateManager::lastStatus() const
{
    return m_lastStatus;
}

void UpdateManager::initialize()
{
#ifdef BETTERSPOTLIGHT_ENABLE_SPARKLE
    if (!m_impl) {
        setStatus(QStringLiteral("sparkle_impl_missing"));
        return;
    }
    if (m_impl->controller) {
        setStatus(QStringLiteral("ready"));
        return;
    }

    @try {
        m_impl->controller = [[SPUStandardUpdaterController alloc]
            initWithStartingUpdater:YES
                     updaterDelegate:nil
                  userDriverDelegate:nil];
        m_available = (m_impl->controller != nil);
        if (m_available) {
            [m_impl->controller.updater
                setAutomaticallyChecksForUpdates:m_automaticallyChecks];
            setStatus(QStringLiteral("ready"));
            LOG_INFO(bsCore, "UpdateManager: Sparkle initialized");
        } else {
            setStatus(QStringLiteral("sparkle_init_failed"));
            LOG_WARN(bsCore, "UpdateManager: Sparkle controller was null");
        }
    } @catch (NSException* exception) {
        m_available = false;
        setStatus(QStringLiteral("sparkle_exception"));
        LOG_ERROR(bsCore, "UpdateManager: Sparkle init exception: %s",
                  [[exception reason] UTF8String]);
    }
#else
    m_available = false;
    setStatus(QStringLiteral("sparkle_not_enabled"));
#endif
}

void UpdateManager::checkNow()
{
#ifdef BETTERSPOTLIGHT_ENABLE_SPARKLE
    if (!m_available || !m_impl || !m_impl->controller) {
        setStatus(QStringLiteral("sparkle_unavailable"));
        return;
    }
    @try {
        [m_impl->controller checkForUpdates:nil];
        setStatus(QStringLiteral("check_requested"));
        LOG_INFO(bsCore, "UpdateManager: manual update check requested");
    } @catch (NSException* exception) {
        setStatus(QStringLiteral("check_failed"));
        LOG_ERROR(bsCore, "UpdateManager: checkForUpdates exception: %s",
                  [[exception reason] UTF8String]);
    }
#else
    setStatus(QStringLiteral("sparkle_not_enabled"));
#endif
}

void UpdateManager::setAutomaticallyChecks(bool enabled)
{
    if (m_automaticallyChecks == enabled) {
        return;
    }
    m_automaticallyChecks = enabled;

#ifdef BETTERSPOTLIGHT_ENABLE_SPARKLE
    if (m_available && m_impl && m_impl->controller) {
        [m_impl->controller.updater setAutomaticallyChecksForUpdates:enabled];
    }
#endif
    emit statusChanged();
}

void UpdateManager::setStatus(const QString& status)
{
    if (m_lastStatus == status) {
        return;
    }
    m_lastStatus = status;
    emit statusChanged();
}

} // namespace bs
