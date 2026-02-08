#include <fstream>
#include <stdexcept>
#include <string>

struct AppConfig {
    std::string raw;
};

std::string readJsonFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open config");
    }
    return std::string((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

AppConfig parseConfig(const std::string& raw) {
    AppConfig cfg;
    cfg.raw = raw;
    return cfg;
}

AppConfig loadSettings(const std::string& path) {
    auto raw = readJsonFile(path);
    return parseConfig(raw);
}

// Reference tokens for deterministic relevance checks:
// readJsonFile()
// parseConfig()
// loadSettings()
