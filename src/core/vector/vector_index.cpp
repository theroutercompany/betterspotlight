#include "core/vector/vector_index.h"

#include "hnswlib/hnswlib.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <limits>

namespace bs {

namespace {

constexpr int kMetaVersion = 2;

} // namespace

VectorIndex::VectorIndex()
{
}

VectorIndex::VectorIndex(const IndexMetadata& metadata)
    : m_metadata(metadata)
{
}

VectorIndex::~VectorIndex()
{
}

bool VectorIndex::configure(const IndexMetadata& metadata)
{
    if (m_index) {
        qWarning() << "VectorIndex::configure ignored: index already initialized";
        return false;
    }
    if (metadata.dimensions <= 0) {
        qWarning() << "VectorIndex::configure rejected invalid dimensions:" << metadata.dimensions;
        return false;
    }
    m_metadata = metadata;
    return true;
}

bool VectorIndex::create(int initialCapacity)
{
    if (m_metadata.dimensions <= 0) {
        qCritical() << "VectorIndex::create requires a positive runtime dimension";
        return false;
    }

    try {
        const int capacity = std::max(initialCapacity, 1);
        m_space = std::make_unique<hnswlib::InnerProductSpace>(m_metadata.dimensions);
        m_index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            m_space.get(),
            static_cast<size_t>(capacity),
            static_cast<size_t>(kM),
            static_cast<size_t>(kEfConstruction));
        m_index->setEf(static_cast<size_t>(kEfSearch));
        m_nextLabel = 0;
        m_deletedCount = 0;
        return true;
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex::create failed:" << e.what();
        m_index.reset();
        m_space.reset();
        return false;
    }
}

bool VectorIndex::load(const std::string& indexPath, const std::string& metaPath)
{
    QFileInfo indexInfo(QString::fromStdString(indexPath));
    if (!indexInfo.exists() || !indexInfo.isFile()) {
        qCritical() << "VectorIndex::load missing index file:" << indexInfo.filePath();
        return false;
    }

    // Corrupted/truncated payloads can make downstream HNSW cleanup paths unsafe.
    // Reject clearly invalid blobs before attempting to deserialize.
    constexpr qint64 kMinSerializedIndexBytes = 96;
    if (indexInfo.size() < kMinSerializedIndexBytes) {
        qCritical() << "VectorIndex::load index payload too small:" << indexInfo.size();
        return false;
    }

    QFile metaFile(QString::fromStdString(metaPath));
    if (!metaFile.open(QIODevice::ReadOnly)) {
        qCritical() << "VectorIndex::load failed to open meta file:" << metaFile.fileName();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument metaDoc = QJsonDocument::fromJson(metaFile.readAll(), &parseError);
    metaFile.close();
    if (parseError.error != QJsonParseError::NoError || !metaDoc.isObject()) {
        qCritical() << "VectorIndex::load invalid meta JSON:" << parseError.errorString();
        return false;
    }

    const QJsonObject meta = metaDoc.object();
    const int dimensions = meta.value(QStringLiteral("dimensions")).toInt(-1);
    if (dimensions <= 0) {
        qCritical() << "VectorIndex::load missing/invalid dimensions in metadata";
        return false;
    }
    if (m_metadata.dimensions > 0 && dimensions != m_metadata.dimensions) {
        qCritical() << "VectorIndex::load dimension mismatch:" << dimensions
                    << "expected" << m_metadata.dimensions;
        return false;
    }

    m_metadata.dimensions = dimensions;
    m_metadata.schemaVersion = meta.value(QStringLiteral("version")).toInt(kMetaVersion);
    m_metadata.modelId = meta.value(QStringLiteral("model_id"))
                             .toString(meta.value(QStringLiteral("model")).toString(QStringLiteral("unknown")))
                             .toStdString();
    m_metadata.generationId = meta.value(QStringLiteral("generation_id"))
                                  .toString(QStringLiteral("v1"))
                                  .toStdString();
    m_metadata.provider = meta.value(QStringLiteral("provider"))
                              .toString(QStringLiteral("cpu"))
                              .toStdString();

    const int efConstruction = meta.value(QStringLiteral("ef_construction")).toInt(kEfConstruction);
    const int m = meta.value(QStringLiteral("m")).toInt(kM);
    if (efConstruction != kEfConstruction || m != kM) {
        qWarning() << "VectorIndex::load metadata params differ from compiled defaults"
                   << "ef_construction=" << efConstruction
                   << "m=" << m;
    }

    const uint64_t totalElementsMeta = meta.value(QStringLiteral("total_elements")).toVariant().toULongLong();
    const uint64_t nextLabelMeta = meta.value(QStringLiteral("next_label")).toVariant().toULongLong();
    const int deletedElementsMeta = meta.value(QStringLiteral("deleted_elements")).toInt(0);

    uint64_t targetCapacity = static_cast<uint64_t>(kInitialCapacity);
    targetCapacity = std::max(targetCapacity, totalElementsMeta + 1);
    targetCapacity = std::max(targetCapacity, nextLabelMeta + 1);
    targetCapacity = std::max(targetCapacity, totalElementsMeta * 2);

    if (targetCapacity > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        qCritical() << "VectorIndex::load target capacity too large:" << targetCapacity;
        return false;
    }

    try {
        m_space = std::make_unique<hnswlib::InnerProductSpace>(m_metadata.dimensions);
        m_index = std::make_unique<hnswlib::HierarchicalNSW<float>>(m_space.get());
        m_index->loadIndex(indexPath, m_space.get(), static_cast<size_t>(targetCapacity));
        m_index->setEf(static_cast<size_t>(kEfSearch));
        m_nextLabel = nextLabelMeta;
        m_deletedCount = std::max(deletedElementsMeta, 0);
        return true;
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex::load failed:" << e.what();
        m_index.reset();
        m_space.reset();
        return false;
    }
}

bool VectorIndex::save(const std::string& indexPath, const std::string& metaPath)
{
    if (!m_index) {
        qWarning() << "VectorIndex::save called with unavailable index";
        return false;
    }

    try {
        m_index->saveIndex(indexPath);
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex::save failed to persist index:" << e.what();
        return false;
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("version"), kMetaVersion);
    meta.insert(QStringLiteral("model_id"), QString::fromStdString(m_metadata.modelId));
    meta.insert(QStringLiteral("generation_id"), QString::fromStdString(m_metadata.generationId));
    meta.insert(QStringLiteral("provider"), QString::fromStdString(m_metadata.provider));
    meta.insert(QStringLiteral("dimensions"), m_metadata.dimensions);
    meta.insert(QStringLiteral("total_elements"), totalElements());
    meta.insert(QStringLiteral("deleted_elements"), deletedElements());
    meta.insert(QStringLiteral("next_label"), static_cast<qint64>(m_nextLabel));
    meta.insert(QStringLiteral("ef_construction"), kEfConstruction);
    meta.insert(QStringLiteral("m"), kM);
    meta.insert(QStringLiteral("last_persisted"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QFile metaFile(QString::fromStdString(metaPath));
    if (!metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCritical() << "VectorIndex::save failed to open meta file for write:" << metaFile.fileName();
        return false;
    }

    const QJsonDocument doc(meta);
    const qint64 written = metaFile.write(doc.toJson(QJsonDocument::Indented));
    metaFile.close();
    if (written < 0) {
        qCritical() << "VectorIndex::save failed writing meta file:" << metaFile.fileName();
        return false;
    }
    return true;
}

uint64_t VectorIndex::addVector(const float* embedding)
{
    if (!m_index || embedding == nullptr) {
        qWarning() << "VectorIndex::addVector called with unavailable index or null embedding";
        return std::numeric_limits<uint64_t>::max();
    }

    std::lock_guard<std::mutex> lock(m_writeMutex);

    if (!ensureCapacityForOneMore()) {
        return std::numeric_limits<uint64_t>::max();
    }

    const uint64_t label = m_nextLabel;
    try {
        m_index->addPoint(embedding, static_cast<hnswlib::labeltype>(label));
        ++m_nextLabel;
        return label;
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex::addVector failed:" << e.what();
        return std::numeric_limits<uint64_t>::max();
    }
}

bool VectorIndex::deleteVector(uint64_t label)
{
    if (!m_index) {
        qWarning() << "VectorIndex::deleteVector called with unavailable index";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_writeMutex);
    try {
        m_index->markDelete(static_cast<hnswlib::labeltype>(label));
        ++m_deletedCount;
        return true;
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex::deleteVector failed:" << e.what();
        return false;
    }
}

std::vector<VectorIndex::KnnResult> VectorIndex::search(const float* queryVector, int k)
{
    std::vector<KnnResult> results;
    if (!m_index || queryVector == nullptr || k <= 0) {
        return results;
    }

    std::lock_guard<std::mutex> lock(m_writeMutex);
    try {
        m_index->setEf(static_cast<size_t>(kEfSearch));
        auto queue = m_index->searchKnn(queryVector, static_cast<size_t>(k));
        results.reserve(queue.size());
        while (!queue.empty()) {
            const auto entry = queue.top();
            queue.pop();
            results.push_back(KnnResult{static_cast<uint64_t>(entry.second), entry.first});
        }
        std::sort(results.begin(), results.end(), [](const KnnResult& a, const KnnResult& b) {
            return a.distance < b.distance;
        });
        return results;
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex::search failed:" << e.what();
        return {};
    }
}

int VectorIndex::totalElements() const
{
    if (!m_index) {
        return 0;
    }
    return static_cast<int>(m_index->getCurrentElementCount());
}

int VectorIndex::deletedElements() const
{
    return m_deletedCount;
}

bool VectorIndex::needsRebuild() const
{
    const int total = totalElements();
    if (total <= 0) {
        return false;
    }
    return static_cast<double>(m_deletedCount) / static_cast<double>(total) > 0.20;
}

bool VectorIndex::isAvailable() const
{
    return m_index != nullptr;
}

uint64_t VectorIndex::nextLabel() const
{
    return m_nextLabel;
}

int VectorIndex::dimensions() const
{
    return m_metadata.dimensions;
}

const VectorIndex::IndexMetadata& VectorIndex::metadata() const
{
    return m_metadata;
}

bool VectorIndex::ensureCapacityForOneMore()
{
    if (!m_index) {
        qWarning() << "VectorIndex::ensureCapacityForOneMore called with unavailable index";
        return false;
    }

    const size_t current = m_index->getCurrentElementCount();
    const size_t maxElements = m_index->getMaxElements();
    if (maxElements == 0) {
        qCritical() << "VectorIndex has zero max elements";
        return false;
    }

    const size_t threshold = (maxElements * 8) / 10;
    if (current < threshold) {
        return true;
    }

    const size_t newCapacity = maxElements * 2;
    if (newCapacity <= maxElements) {
        qCritical() << "VectorIndex resize overflow";
        return false;
    }

    try {
        m_index->resizeIndex(newCapacity);
        qWarning() << "VectorIndex resized to capacity" << static_cast<qulonglong>(newCapacity);
        return true;
    } catch (const std::exception& e) {
        qCritical() << "VectorIndex resize failed:" << e.what();
        return false;
    }
}

} // namespace bs
