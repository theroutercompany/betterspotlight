#pragma once

#include <QString>

#include <memory>

namespace bs {

struct PlatformOperationResult {
    bool success = false;
    QString message;
};

class PlatformIntegration {
public:
    virtual ~PlatformIntegration() = default;

    virtual PlatformOperationResult setLaunchAtLogin(bool enabled) = 0;
    virtual PlatformOperationResult setShowInDock(bool enabled) = 0;

    static std::unique_ptr<PlatformIntegration> create();
};

} // namespace bs
