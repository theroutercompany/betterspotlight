#pragma once

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(bsCore)
Q_DECLARE_LOGGING_CATEGORY(bsIndex)
Q_DECLARE_LOGGING_CATEGORY(bsExtraction)
Q_DECLARE_LOGGING_CATEGORY(bsFs)
Q_DECLARE_LOGGING_CATEGORY(bsRanking)
Q_DECLARE_LOGGING_CATEGORY(bsIpc)

#define LOG_DEBUG(cat, ...) qCDebug(cat, __VA_ARGS__)
#define LOG_INFO(cat, ...)  qCInfo(cat, __VA_ARGS__)
#define LOG_WARN(cat, ...)  qCWarning(cat, __VA_ARGS__)
#define LOG_ERROR(cat, ...) qCCritical(cat, __VA_ARGS__)
