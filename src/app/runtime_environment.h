#pragma once

#include <QString>
#include <QStringList>

namespace bs {

struct RuntimeContext {
    QString instanceId;
    QString runtimeRoot;
    QString runtimeDir;
    QString socketDir;
    QString pidDir;
    QString metadataPath;
    QString lockPath;
};

QString runtimeRootPath();
QString makeInstanceId();
bool processIsAlive(qint64 pid);

bool initRuntimeContext(RuntimeContext* context, QString* error = nullptr);
void cleanupOrphanRuntimeDirectories(const RuntimeContext& context,
                                    QStringList* removedDirectories = nullptr);

} // namespace bs
