#include "system_interaction_collector.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMetaObject>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <cmath>
#include <utility>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

namespace bs {

namespace {

QString toQString(NSString* value)
{
    return value ? QString::fromUtf8(value.UTF8String) : QString();
}

QString newEventId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject makeInputMeta(int keyEvents, int shortcuts, int scrolls)
{
    QJsonObject inputMeta;
    inputMeta[QStringLiteral("keyEventCount")] = std::max(0, keyEvents);
    inputMeta[QStringLiteral("shortcutCount")] = std::max(0, shortcuts);
    inputMeta[QStringLiteral("scrollCount")] = std::max(0, scrolls);
    inputMeta[QStringLiteral("metadataOnly")] = true;
    return inputMeta;
}

QJsonObject makeMouseMeta(double distancePx, int clickCount, int dragCount)
{
    QJsonObject mouseMeta;
    mouseMeta[QStringLiteral("moveDistancePx")] = std::max(0.0, distancePx);
    mouseMeta[QStringLiteral("clickCount")] = std::max(0, clickCount);
    mouseMeta[QStringLiteral("dragCount")] = std::max(0, dragCount);
    return mouseMeta;
}

QJsonObject makePrivacyFlags(bool secureInput)
{
    QJsonObject privacy;
    privacy[QStringLiteral("secureInput")] = secureInput;
    privacy[QStringLiteral("privateContext")] = false;
    privacy[QStringLiteral("denylistedApp")] = false;
    privacy[QStringLiteral("redacted")] = secureInput;
    return privacy;
}

QString frontmostBundleId()
{
    NSRunningApplication* app = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (!app) {
        return {};
    }
    return toQString(app.bundleIdentifier);
}

} // namespace

struct SystemInteractionCollector::Impl {
    explicit Impl(SystemInteractionCollector* ownerIn)
        : owner(ownerIn)
    {
        flushTimer.setInterval(2000);
        flushTimer.setSingleShot(false);
        QObject::connect(&flushTimer, &QTimer::timeout, owner, [this]() {
            flushInputAggregation();
        });
    }

    ~Impl()
    {
        stop();
    }

    void postEvent(const QJsonObject& event)
    {
        QMetaObject::invokeMethod(owner,
                                  [ownerRef = owner, event]() {
            emit ownerRef->behaviorEventCaptured(event);
        },
                                  Qt::QueuedConnection);
    }

    void emitHealth() const
    {
        QJsonObject health;
        health[QStringLiteral("enabled")] = enabled;
        health[QStringLiteral("platformSupported")] = true;
        health[QStringLiteral("globalMonitorAttached")] = (globalMonitor != nil);
        health[QStringLiteral("appObserverAttached")] = (appObserver != nil);
        health[QStringLiteral("secureInputEnabled")] = IsSecureEventInputEnabled();
        health[QStringLiteral("captureAppActivityEnabled")] = captureAppActivityEnabled;
        health[QStringLiteral("captureInputActivityEnabled")] = captureInputActivityEnabled;
        emit owner->collectorHealthChanged(health);
    }

    void emitAppActivatedEvent(NSRunningApplication* app)
    {
        if (!enabled || !captureAppActivityEnabled) {
            return;
        }

        const QString bundleId = toQString(app.bundleIdentifier);
        if (bundleId.isEmpty()) {
            return;
        }
        lastFrontmostBundleId = bundleId;

        const bool secureInput = IsSecureEventInputEnabled();
        QJsonObject event;
        event[QStringLiteral("eventId")] = newEventId();
        event[QStringLiteral("timestamp")] =
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        event[QStringLiteral("source")] = QStringLiteral("system_collector");
        event[QStringLiteral("eventType")] = QStringLiteral("app_activated");
        event[QStringLiteral("appBundleId")] = bundleId;
        event[QStringLiteral("inputMeta")] = makeInputMeta(0, 0, 0);
        event[QStringLiteral("mouseMeta")] = makeMouseMeta(0.0, 0, 0);
        event[QStringLiteral("privacyFlags")] = makePrivacyFlags(secureInput);
        event[QStringLiteral("attributionConfidence")] = 0.15;
        postEvent(event);
    }

    void flushInputAggregation()
    {
        if (!enabled) {
            return;
        }

        const int keyEvents = keyEventCount.exchange(0);
        const int shortcuts = shortcutCount.exchange(0);
        const int scrolls = scrollCount.exchange(0);
        const int clicks = clickCount.exchange(0);
        const int drags = dragCount.exchange(0);
        const double moveDistance = moveDistancePx.exchange(0.0);

        if (keyEvents == 0 && shortcuts == 0 && scrolls == 0
            && clicks == 0 && drags == 0 && moveDistance <= 0.0) {
            return;
        }

        if (!captureInputActivityEnabled) {
            return;
        }

        const bool secureInput = IsSecureEventInputEnabled();
        const QString bundleId = frontmostBundleId();
        if (!bundleId.isEmpty()) {
            lastFrontmostBundleId = bundleId;
        }

        QJsonObject event;
        event[QStringLiteral("eventId")] = newEventId();
        event[QStringLiteral("timestamp")] =
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch());
        event[QStringLiteral("source")] = QStringLiteral("system_collector");
        event[QStringLiteral("eventType")] = QStringLiteral("input_activity");
        if (!lastFrontmostBundleId.isEmpty()) {
            event[QStringLiteral("appBundleId")] = lastFrontmostBundleId;
        }
        if (secureInput) {
            event[QStringLiteral("inputMeta")] = makeInputMeta(0, 0, scrolls);
            event[QStringLiteral("mouseMeta")] = makeMouseMeta(moveDistance, clicks, drags);
        } else {
            event[QStringLiteral("inputMeta")] = makeInputMeta(keyEvents, shortcuts, scrolls);
            event[QStringLiteral("mouseMeta")] = makeMouseMeta(moveDistance, clicks, drags);
        }
        event[QStringLiteral("privacyFlags")] = makePrivacyFlags(secureInput);
        event[QStringLiteral("attributionConfidence")] = 0.1;
        postEvent(event);
    }

    void installObservers()
    {
        if (appObserver == nil) {
            NSNotificationCenter* workspaceCenter = NSWorkspace.sharedWorkspace.notificationCenter;
            appObserver = [workspaceCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
                                                       object:nil
                                                        queue:NSOperationQueue.mainQueue
                                                   usingBlock:^(NSNotification* note) {
                NSRunningApplication* app =
                    note.userInfo[NSWorkspaceApplicationKey];
                if (app) {
                    emitAppActivatedEvent(app);
                }
            }];
        }

        if (globalMonitor == nil) {
            const NSEventMask mask =
                NSEventMaskKeyDown
                | NSEventMaskFlagsChanged
                | NSEventMaskScrollWheel
                | NSEventMaskMouseMoved
                | NSEventMaskLeftMouseDown
                | NSEventMaskRightMouseDown
                | NSEventMaskOtherMouseDown
                | NSEventMaskLeftMouseDragged
                | NSEventMaskRightMouseDragged
                | NSEventMaskOtherMouseDragged;

            globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                                    handler:^(NSEvent* event) {
                if (!enabled) {
                    return;
                }
                switch (event.type) {
                case NSEventTypeKeyDown: {
                    keyEventCount.fetch_add(1);
                    const NSEventModifierFlags mods =
                        (event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask);
                    if (mods & (NSEventModifierFlagCommand
                                | NSEventModifierFlagOption
                                | NSEventModifierFlagControl)) {
                        shortcutCount.fetch_add(1);
                    }
                    break;
                }
                case NSEventTypeFlagsChanged:
                    shortcutCount.fetch_add(1);
                    break;
                case NSEventTypeScrollWheel:
                    scrollCount.fetch_add(1);
                    break;
                case NSEventTypeMouseMoved:
                case NSEventTypeLeftMouseDragged:
                case NSEventTypeRightMouseDragged:
                case NSEventTypeOtherMouseDragged: {
                    const double dx = event.deltaX;
                    const double dy = event.deltaY;
                    moveDistancePx.fetch_add(std::sqrt((dx * dx) + (dy * dy)));
                    if (event.type != NSEventTypeMouseMoved) {
                        dragCount.fetch_add(1);
                    }
                    break;
                }
                case NSEventTypeLeftMouseDown:
                case NSEventTypeRightMouseDown:
                case NSEventTypeOtherMouseDown:
                    clickCount.fetch_add(1);
                    break;
                default:
                    break;
                }
            }];
        }
    }

    void removeObservers()
    {
        if (globalMonitor != nil) {
            [NSEvent removeMonitor:globalMonitor];
            globalMonitor = nil;
        }

        if (appObserver != nil) {
            [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:appObserver];
            appObserver = nil;
        }
    }

    void start()
    {
        if (enabled) {
            return;
        }
        enabled = true;
        lastFrontmostBundleId = frontmostBundleId();
        installObservers();
        flushTimer.start();
        emitHealth();
    }

    void stop()
    {
        if (!enabled) {
            return;
        }
        enabled = false;
        flushTimer.stop();
        removeObservers();
        emitHealth();
    }

    void setCaptureScope(bool appActivityEnabled, bool inputActivityEnabled)
    {
        const bool nextAppActivityEnabled = appActivityEnabled;
        const bool nextInputActivityEnabled = inputActivityEnabled;
        if (captureAppActivityEnabled == nextAppActivityEnabled
            && captureInputActivityEnabled == nextInputActivityEnabled) {
            return;
        }
        captureAppActivityEnabled = nextAppActivityEnabled;
        captureInputActivityEnabled = nextInputActivityEnabled;
        emitHealth();
    }

    SystemInteractionCollector* owner = nullptr;
    bool enabled = false;
    bool captureAppActivityEnabled = true;
    bool captureInputActivityEnabled = true;
    id globalMonitor = nil;
    id appObserver = nil;
    QTimer flushTimer;
    QString lastFrontmostBundleId;

    std::atomic<int> keyEventCount{0};
    std::atomic<int> shortcutCount{0};
    std::atomic<int> scrollCount{0};
    std::atomic<int> clickCount{0};
    std::atomic<int> dragCount{0};
    std::atomic<double> moveDistancePx{0.0};
};

SystemInteractionCollector::SystemInteractionCollector(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>(this))
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
    if (enabled) {
        m_impl->start();
    } else {
        m_impl->stop();
    }
}

void SystemInteractionCollector::setCaptureScope(bool appActivityEnabled, bool inputActivityEnabled)
{
    m_impl->setCaptureScope(appActivityEnabled, inputActivityEnabled);
}

} // namespace bs
