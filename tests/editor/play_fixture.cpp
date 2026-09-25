#include "paper/content/document.hpp"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

// Test-only child process: no engine, game, scripts, audio or GPU.
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path descriptor;
    bool fail = false, ignoreStop = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--paper-play" && i + 1 < argc)
            descriptor = argv[++i];
        else if (arg == "--fail")
            fail = true;
        else if (arg == "--ignore-stop")
            ignoreStop = true;
    }
    try {
        const auto spec = paper::content::read(descriptor);
        if (spec.at("format") != "paper.play" || spec.at("version") != 1)
            throw std::runtime_error("Invalid fixture descriptor");
        if (fail) {
            std::cerr << "Fixture runtime failed to load content" << std::endl;
            return 7;
        }
        if (ignoreStop)
            std::signal(SIGTERM, SIG_IGN);
        const auto directory = descriptor.parent_path();
        std::ofstream(directory / "state" / "ready") << "isolated persistence";
        std::cout << "Play fixture ready" << std::endl;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline && fs::exists(descriptor)) {
            if (!ignoreStop && fs::exists(directory / "stop.request"))
                return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
