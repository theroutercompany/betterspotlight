#pragma once

#include "core/models/model_manifest.h"

#include <QString>

#include <memory>
#include <string>
#include <vector>

namespace bs {

class ModelSession {
public:
    explicit ModelSession(const ModelManifestEntry& manifest);
    ~ModelSession();

    ModelSession(const ModelSession&) = delete;
    ModelSession& operator=(const ModelSession&) = delete;
    ModelSession(ModelSession&&) = delete;
    ModelSession& operator=(ModelSession&&) = delete;

    bool initialize(const QString& modelPath);
    bool isAvailable() const;

    const ModelManifestEntry& manifest() const;
    const std::vector<std::string>& outputNames() const;
    const std::string& selectedProvider() const;
    bool coreMlRequested() const;
    bool coreMlAttached() const;

    // Returns the underlying Ort::Session* as void* to avoid header leaks.
    // Callers must cast back to Ort::Session* in .cpp files that include the ONNX header.
    void* rawSession() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    ModelManifestEntry m_manifest;
    std::vector<std::string> m_outputNames;
    std::string m_selectedProvider = "cpu";
    bool m_coreMlRequested = false;
    bool m_coreMlAttached = false;
    bool m_available = false;
};

} // namespace bs
