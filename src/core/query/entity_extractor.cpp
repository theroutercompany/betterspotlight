#include "core/query/entity_extractor.h"

#include <QChar>
#include <QSet>
#include <QStringList>

namespace bs {

namespace {

const QSet<QString>& capitalizedStopwords()
{
    static const QSet<QString> kStops = {
        QStringLiteral("The"),  QStringLiteral("A"),    QStringLiteral("My"),
        QStringLiteral("And"),  QStringLiteral("Or"),   QStringLiteral("In"),
        QStringLiteral("On"),   QStringLiteral("At"),   QStringLiteral("To"),
        QStringLiteral("For"),  QStringLiteral("Of"),   QStringLiteral("With"),
        QStringLiteral("That"), QStringLiteral("This"), QStringLiteral("It"),
    };
    return kStops;
}

const QSet<QString>& placeSuffixes()
{
    static const QSet<QString> kSuffixes = {
        QStringLiteral("desert"),    QStringLiteral("mountain"),
        QStringLiteral("river"),     QStringLiteral("city"),
        QStringLiteral("island"),    QStringLiteral("lake"),
        QStringLiteral("valley"),    QStringLiteral("park"),
        QStringLiteral("ocean"),     QStringLiteral("sea"),
        QStringLiteral("bay"),       QStringLiteral("canyon"),
        QStringLiteral("heights"),   QStringLiteral("falls"),
        QStringLiteral("peninsula"), QStringLiteral("harbor"),
        QStringLiteral("port"),      QStringLiteral("strait"),
        QStringLiteral("glacier"),   QStringLiteral("forest"),
        QStringLiteral("beach"),
    };
    return kSuffixes;
}

const QSet<QString>& orgMarkers()
{
    static const QSet<QString> kMarkers = {
        QStringLiteral("inc"),         QStringLiteral("corp"),
        QStringLiteral("llc"),         QStringLiteral("ltd"),
        QStringLiteral("co"),          QStringLiteral("group"),
        QStringLiteral("bank"),        QStringLiteral("university"),
        QStringLiteral("college"),     QStringLiteral("institute"),
        QStringLiteral("foundation"),  QStringLiteral("association"),
    };
    return kMarkers;
}

bool isCapitalized(const QString& word)
{
    if (word.isEmpty()) {
        return false;
    }
    return word.at(0).isUpper();
}

EntityType classifySequence(const QStringList& words)
{
    if (words.isEmpty()) {
        return EntityType::Other;
    }

    // Check for place suffix (last word)
    const QString lastLower = words.last().toLower();
    if (placeSuffixes().contains(lastLower)) {
        return EntityType::Place;
    }

    // Check for organization markers (any word)
    for (const QString& w : words) {
        if (orgMarkers().contains(w.toLower())) {
            return EntityType::Organization;
        }
    }

    // 2-3 word capitalized sequence => Person
    if (words.size() >= 2 && words.size() <= 3) {
        return EntityType::Person;
    }

    return EntityType::Other;
}

} // namespace

std::vector<Entity> EntityExtractor::extract(const QString& originalQuery)
{
    std::vector<Entity> results;

    // All-lowercase check: if no uppercase letters exist, return empty
    bool hasUpper = false;
    for (const QChar& ch : originalQuery) {
        if (ch.isUpper()) {
            hasUpper = true;
            break;
        }
    }
    if (!hasUpper) {
        return results;
    }

    const QStringList words = originalQuery.split(QChar(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return results;
    }

    // Build sequences of consecutive capitalized words
    QStringList currentSequence;
    int sequenceStartIndex = -1;

    auto flushSequence = [&]() {
        if (currentSequence.isEmpty()) {
            return;
        }

        // Filter out stopwords from the sequence
        QStringList filtered;
        for (const QString& w : currentSequence) {
            if (!capitalizedStopwords().contains(w)) {
                filtered.append(w);
            }
        }

        // If sequence starts at index 0 (sentence-initial), only include
        // if part of a multi-word capitalized sequence after filtering
        if (sequenceStartIndex == 0 && filtered.size() <= 1) {
            currentSequence.clear();
            sequenceStartIndex = -1;
            return;
        }

        if (filtered.isEmpty()) {
            currentSequence.clear();
            sequenceStartIndex = -1;
            return;
        }

        EntityType type = classifySequence(filtered);
        Entity entity;
        entity.text = filtered.join(QChar(' '));
        entity.type = type;
        results.push_back(std::move(entity));

        currentSequence.clear();
        sequenceStartIndex = -1;
    };

    for (int i = 0; i < words.size(); ++i) {
        const QString& word = words[i];
        if (isCapitalized(word)) {
            if (currentSequence.isEmpty()) {
                sequenceStartIndex = i;
            }
            currentSequence.append(word);
        } else {
            flushSequence();
        }
    }
    flushSequence();

    return results;
}

} // namespace bs
