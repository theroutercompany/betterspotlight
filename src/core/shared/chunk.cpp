#include "core/shared/chunk.h"
#include <QCryptographicHash>

namespace bs {

QString computeChunkId(const QString& filePath, int chunkIndex)
{
    const QString seed = filePath + QStringLiteral("#") + QString::number(chunkIndex);
    const QByteArray hash = QCryptographicHash::hash(
        seed.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toHex());
}

} // namespace bs
