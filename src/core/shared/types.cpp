#include "core/shared/types.h"

namespace bs {

QString itemKindToString(ItemKind kind)
{
    switch (kind) {
    case ItemKind::Directory: return QStringLiteral("directory");
    case ItemKind::Text:      return QStringLiteral("text");
    case ItemKind::Code:      return QStringLiteral("code");
    case ItemKind::Markdown:  return QStringLiteral("markdown");
    case ItemKind::Pdf:       return QStringLiteral("pdf");
    case ItemKind::Image:     return QStringLiteral("image");
    case ItemKind::Archive:   return QStringLiteral("archive");
    case ItemKind::Binary:    return QStringLiteral("binary");
    case ItemKind::Unknown:   return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

ItemKind itemKindFromString(const QString& str)
{
    if (str == QLatin1String("directory")) return ItemKind::Directory;
    if (str == QLatin1String("text"))      return ItemKind::Text;
    if (str == QLatin1String("code"))      return ItemKind::Code;
    if (str == QLatin1String("markdown"))  return ItemKind::Markdown;
    if (str == QLatin1String("pdf"))       return ItemKind::Pdf;
    if (str == QLatin1String("image"))     return ItemKind::Image;
    if (str == QLatin1String("archive"))   return ItemKind::Archive;
    if (str == QLatin1String("binary"))    return ItemKind::Binary;
    return ItemKind::Unknown;
}

QString sensitivityToString(Sensitivity s)
{
    switch (s) {
    case Sensitivity::Normal:    return QStringLiteral("normal");
    case Sensitivity::Sensitive: return QStringLiteral("sensitive");
    case Sensitivity::Hidden:    return QStringLiteral("hidden");
    }
    return QStringLiteral("normal");
}

Sensitivity sensitivityFromString(const QString& str)
{
    if (str == QLatin1String("sensitive")) return Sensitivity::Sensitive;
    if (str == QLatin1String("hidden"))    return Sensitivity::Hidden;
    return Sensitivity::Normal;
}

} // namespace bs
