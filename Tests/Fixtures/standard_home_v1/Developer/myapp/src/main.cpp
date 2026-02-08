#include <iostream>
#include <string>
#include "config_parser.hpp"

int main() {
    std::cout << "BetterSpotlight service bootstrap" << std::endl;
    std::string env = "development";
    if (env == "development") {
        std::cout << "Verbose logs enabled" << std::endl;
    }

    std::cout << "Loading configuration..." << std::endl;
    std::cout << "Starting IPC loop..." << std::endl;
    std::cout << "Ready" << std::endl;
    return 0;
}

// Application entrypoint for test fixture content.
// Keep source realistic for indexer text extraction.
# note line 1
