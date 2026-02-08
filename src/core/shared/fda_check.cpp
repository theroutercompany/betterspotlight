#include "core/shared/fda_check.h"
#include "core/shared/logging.h"

#include <QStandardPaths>

#include <dirent.h>

namespace bs {

bool FdaCheck::hasFullDiskAccess()
{
    // ~/Library/Mail/ is a well-known FDA-gated directory.
    // If we can list its contents, we have FDA.
    const QString homePath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString testPath = homePath + QStringLiteral("/Library/Mail");

    DIR* dir = opendir(testPath.toUtf8().constData());
    if (dir) {
        closedir(dir);
        LOG_INFO(bsFs, "Full Disk Access: GRANTED");
        return true;
    }

    LOG_WARN(bsFs, "Full Disk Access: NOT GRANTED (cannot read %s)", qUtf8Printable(testPath));
    return false;
}

QString FdaCheck::instructionMessage()
{
    return QStringLiteral(
        "BetterSpotlight requires Full Disk Access to index all files.\n\n"
        "To grant access:\n"
        "1. Open System Settings > Privacy & Security > Full Disk Access\n"
        "2. Click the '+' button\n"
        "3. Add BetterSpotlight to the list\n"
        "4. Restart BetterSpotlight");
}

} // namespace bs
