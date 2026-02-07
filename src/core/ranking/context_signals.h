#pragma once

#include "core/shared/search_result.h"
#include "core/shared/scoring_types.h"

#include <QString>
#include <QMap>
#include <QSet>

namespace bs {

class ContextSignals {
public:
    ContextSignals();

    // Compute CWD proximity boost.
    // Returns full cwdBoostWeight if the file is within maxDepth directory
    // levels of cwdPath. The boost decays linearly with depth:
    //   boost = cwdBoostWeight * (1.0 - depth / (maxDepth + 1))
    double cwdProximityBoost(const QString& filePath, const QString& cwdPath,
                             int cwdBoostWeight, int maxDepth = 2) const;

    // Compute app-context boost.
    // Returns appContextBoostWeight if the file's extension matches the
    // frontmost application's associated file types.
    double appContextBoost(const QString& filePath,
                           const QString& frontmostAppBundleId,
                           int appContextBoostWeight) const;

private:
    // Map: bundle ID -> set of file extensions (without leading dot) that get boosted
    QMap<QString, QSet<QString>> m_appExtensionMap;
    void initAppExtensionMap();
};

} // namespace bs
