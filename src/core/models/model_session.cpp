#include "core/models/model_session.h"

#include "core/shared/logging.h"

#include <QFile>

#if defined(ONNXRUNTIME_FOUND) && __has_include(<onnxruntime_cxx_api.h>)
#define BS_WITH_ONNX 1
#include <dlfcn.h>
#include <onnxruntime_cxx_api.h>
#else
#define BS_WITH_ONNX 0
#endif

namespace bs {

namespace {

#if BS_WITH_ONNX
constexpr uint32_t kCoreMlFlagUseCpuAndGpu = 0U;

#ifdef ORT_COREML_FLAG_CREATE_MLPROGRAM
constexpr uint32_t kCoreMlFlagCreateMlProgram = ORT_COREML_FLAG_CREATE_MLPROGRAM;
#elif defined(COREML_FLAG_CREATE_MLPROGRAM)
constexpr uint32_t kCoreMlFlagCreateMlProgram = COREML_FLAG_CREATE_MLPROGRAM;
#else
constexpr uint32_t kCoreMlFlagCreateMlProgram = 0U;
#endif

using AppendCoreMlProviderFn = OrtStatus* (*)(OrtSessionOptions* options, uint32_t coremlFlags);

Ort::Env& ortEnvironment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "betterspotlight-models");
    return env;
}
#endif

} // anonymous namespace

class ModelSession::Impl {
public:
#if BS_WITH_ONNX
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
#endif
};

ModelSession::ModelSession(const ModelManifestEntry& manifest)
    : m_impl(std::make_unique<Impl>())
    , m_manifest(manifest)
{
}

ModelSession::~ModelSession() = default;

bool ModelSession::initialize(const QString& modelPath)
{
#if BS_WITH_ONNX
    if (modelPath.isEmpty() || !QFile::exists(modelPath)) {
        LOG_WARN(bsCore, "ModelSession: model file missing at %s", qPrintable(modelPath));
        m_available = false;
        return false;
    }

    try {
        m_impl->sessionOptions.SetIntraOpNumThreads(2);
        m_impl->sessionOptions.SetInterOpNumThreads(1);
        m_impl->sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        m_impl->sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const uint32_t coremlFlags = kCoreMlFlagUseCpuAndGpu | kCoreMlFlagCreateMlProgram;
        auto appendCoreMlProvider = reinterpret_cast<AppendCoreMlProviderFn>(
            dlsym(RTLD_DEFAULT, "OrtSessionOptionsAppendExecutionProvider_CoreML"));
        if (appendCoreMlProvider != nullptr) {
            OrtSessionOptions* rawOptions = m_impl->sessionOptions;
            OrtStatus* coremlStatus = appendCoreMlProvider(rawOptions, coremlFlags);
            if (coremlStatus != nullptr) {
                const OrtApi& api = Ort::GetApi();
                LOG_WARN(bsCore, "ModelSession: CoreML EP unavailable, falling back to CPU: %s",
                         api.GetErrorMessage(coremlStatus));
                api.ReleaseStatus(coremlStatus);
            }
        } else {
            LOG_INFO(bsCore, "ModelSession: CoreML EP symbol not found, using CPU provider");
        }

        m_impl->session = std::make_unique<Ort::Session>(
            ortEnvironment(), modelPath.toUtf8().constData(), m_impl->sessionOptions);

        // Validate that all expected inputs exist in the model
        Ort::AllocatorWithDefaultOptions allocator;
        const size_t inputCount = m_impl->session->GetInputCount();

        std::vector<std::string> modelInputs;
        modelInputs.reserve(inputCount);
        for (size_t i = 0; i < inputCount; ++i) {
            Ort::AllocatedStringPtr inputName = m_impl->session->GetInputNameAllocated(i, allocator);
            if (inputName.get() != nullptr) {
                modelInputs.emplace_back(inputName.get());
            }
        }

        for (const QString& expectedInput : m_manifest.inputs) {
            const std::string expected = expectedInput.toStdString();
            bool found = false;
            for (const std::string& actual : modelInputs) {
                if (actual == expected) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG_WARN(bsCore, "ModelSession: required input '%s' not found in model",
                         qPrintable(expectedInput));
                m_impl->session.reset();
                m_available = false;
                return false;
            }
        }

        // Capture output names
        const size_t outputCount = m_impl->session->GetOutputCount();
        m_outputNames.clear();
        m_outputNames.reserve(outputCount);
        for (size_t i = 0; i < outputCount; ++i) {
            Ort::AllocatedStringPtr outputName = m_impl->session->GetOutputNameAllocated(i, allocator);
            if (outputName.get() != nullptr && outputName.get()[0] != '\0') {
                m_outputNames.emplace_back(outputName.get());
            }
        }

        if (m_outputNames.empty()) {
            LOG_WARN(bsCore, "ModelSession: no output names found in model");
            m_impl->session.reset();
            m_available = false;
            return false;
        }

        LOG_INFO(bsCore, "ModelSession: initialized '%s' with %zu inputs, %zu outputs",
                 qPrintable(m_manifest.name),
                 modelInputs.size(), m_outputNames.size());

        m_available = true;
        return true;
    } catch (const Ort::Exception& ex) {
        LOG_WARN(bsCore, "ModelSession: ONNX initialization failed: %s", ex.what());
    }

    m_available = false;
    return false;
#else
    Q_UNUSED(modelPath);
    LOG_INFO(bsCore, "ModelSession: ONNX Runtime not enabled, session unavailable");
    m_available = false;
    return false;
#endif
}

bool ModelSession::isAvailable() const
{
    return m_available;
}

const ModelManifestEntry& ModelSession::manifest() const
{
    return m_manifest;
}

const std::vector<std::string>& ModelSession::outputNames() const
{
    return m_outputNames;
}

void* ModelSession::rawSession() const
{
#if BS_WITH_ONNX
    if (m_impl->session) {
        return m_impl->session.get();
    }
#endif
    return nullptr;
}

} // namespace bs
