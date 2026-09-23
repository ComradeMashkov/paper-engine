#include "paper/editor/application.hpp"
int main(int argc, char** argv) {
    paper::editor::Options options;
    options.shaders = PAPER_EDITOR_SHADER_DIR;
    return paper::editor::run(argc, argv, std::move(options));
}
