#pragma once

#include <QObject>

namespace bs {

// Bridges QSystemTrayIcon actions from C++ to QML signal handlers.
// This small QObject defines the signals that QML Connections can listen to.
class StatusBarBridge : public QObject {
    Q_OBJECT

public:
    explicit StatusBarBridge(QObject* parent = nullptr) : QObject(parent) {}

signals:
    void showSearchRequested();
    void showSettingsRequested();
    void showIndexHealthRequested();
};

} // namespace bs
