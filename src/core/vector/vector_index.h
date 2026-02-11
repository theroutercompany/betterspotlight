#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hnswlib {
class InnerProductSpace;

template <typename dist_t>
class HierarchicalNSW;
} // namespace hnswlib

namespace bs {

class VectorIndex {
public:
    struct KnnResult {
        uint64_t label = 0;
        float distance = 0.0f;
    };

    struct IndexMetadata {
        int schemaVersion = 2;
        int dimensions = 0;
        std::string modelId = "unknown";
        std::string generationId = "v1";
        std::string provider = "cpu";
    };

    static constexpr int kM = 16;
    static constexpr int kEfConstruction = 200;
    static constexpr int kEfSearch = 50;
    static constexpr int kInitialCapacity = 100000;

    VectorIndex();
    explicit VectorIndex(const IndexMetadata& metadata);
    ~VectorIndex();

    VectorIndex(const VectorIndex&) = delete;
    VectorIndex& operator=(const VectorIndex&) = delete;

    bool configure(const IndexMetadata& metadata);
    bool create(int initialCapacity = kInitialCapacity);
    bool load(const std::string& indexPath, const std::string& metaPath);
    bool save(const std::string& indexPath, const std::string& metaPath);

    uint64_t addVector(const float* embedding);
    bool deleteVector(uint64_t label);

    std::vector<KnnResult> search(const float* queryVector, int k = 50);

    int totalElements() const;
    int deletedElements() const;
    bool needsRebuild() const;
    bool isAvailable() const;
    uint64_t nextLabel() const;
    int dimensions() const;
    const IndexMetadata& metadata() const;

private:
    bool ensureCapacityForOneMore();

    IndexMetadata m_metadata;
    std::unique_ptr<hnswlib::InnerProductSpace> m_space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> m_index;
    uint64_t m_nextLabel = 0;
    int m_deletedCount = 0;
    mutable std::mutex m_writeMutex;
};

} // namespace bs
