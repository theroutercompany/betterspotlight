#include "core/query/stopwords.h"

namespace bs {

const QSet<QString>& queryStopwords()
{
    static const QSet<QString> stopwords = {
        QStringLiteral("a"),
        QStringLiteral("an"),
        QStringLiteral("any"),
        QStringLiteral("and"),
        QStringLiteral("are"),
        QStringLiteral("at"),
        QStringLiteral("for"),
        QStringLiteral("from"),
        QStringLiteral("how"),
        QStringLiteral("in"),
        QStringLiteral("is"),
        QStringLiteral("it"),
        QStringLiteral("my"),
        QStringLiteral("of"),
        QStringLiteral("on"),
        QStringLiteral("or"),
        QStringLiteral("that"),
        QStringLiteral("there"),
        QStringLiteral("the"),
        QStringLiteral("to"),
        QStringLiteral("what"),
        QStringLiteral("when"),
        QStringLiteral("where"),
        QStringLiteral("which"),
        QStringLiteral("who"),
        QStringLiteral("why"),
        QStringLiteral("with"),
    };
    return stopwords;
}

} // namespace bs
