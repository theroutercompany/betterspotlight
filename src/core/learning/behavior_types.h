#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include <cstdint>

namespace bs {

struct BehaviorEventInputMeta {
    int keyEventCount = 0;
    int shortcutCount = 0;
    int scrollCount = 0;
    bool metadataOnly = true;
};

struct BehaviorEventMouseMeta {
    double moveDistancePx = 0.0;
    int clickCount = 0;
    int dragCount = 0;
};

struct BehaviorPrivacyFlags {
    bool secureInput = false;
    bool privateContext = false;
    bool denylistedApp = false;
    bool redacted = false;
};

struct BehaviorEvent {
    QString eventId;
    QDateTime timestamp;
    QString source;
    QString eventType;
    QString appBundleId;
    QString windowTitleHash;
    QString itemPath;
    int64_t itemId = 0;
    QString browserHostHash;
    BehaviorEventInputMeta inputMeta;
    BehaviorEventMouseMeta mouseMeta;
    BehaviorPrivacyFlags privacyFlags;
    double attributionConfidence = 0.0;
    QString contextEventId;
    QString activityDigest;
};

struct ContextFeatureVector {
    int version = 1;
    QString contextEventId;
    QString activityDigest;
    double appFocusMatch = 0.0;
    double keyboardActivity = 0.0;
    double mouseActivity = 0.0;
    double queryLength = 0.0;
    double resultRank = 0.0;
    double routerConfidence = 0.0;
    double semanticNeed = 0.0;
};

struct TrainingExample {
    QString sampleId;
    QString query;
    QString queryNormalized;
    int64_t itemId = 0;
    QString path;
    int label = -1;  // -1 unknown, 0 negative, 1 positive
    double weight = 1.0;
    QVector<double> denseFeatures;
    QString sourceEventId;
    QString appBundleId;
    QString contextEventId;
    QString activityDigest;
    double attributionConfidence = 0.0;
    QDateTime createdAt;
    bool consumed = false;
};

} // namespace bs
