#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path runfilePath(const char *relativePath) {
    const char *testSrcdir = std::getenv("TEST_SRCDIR");
    const char *workspace = std::getenv("TEST_WORKSPACE");
    if (testSrcdir == nullptr || workspace == nullptr) {
        return {};
    }
    return std::filesystem::path(testSrcdir) / workspace / relativePath;
}

bool hasDisplayServer() {
    const char *display = std::getenv("DISPLAY");
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && display[0] != '\0')
        || (waylandDisplay != nullptr && waylandDisplay[0] != '\0');
}

int execProgram(const std::filesystem::path &program) {
    const std::string path = program.string();
    std::vector<char *> argv{
        const_cast<char *>(path.c_str()),
        nullptr,
    };
    execv(path.c_str(), argv.data());
    std::cerr << "failed to exec " << path << '\n';
    return 127;
}

}

int main() {
    if (!hasDisplayServer()) {
        std::cout << "SKIP: rendernode_alignment_e2e requires an X11/Wayland display with OpenGL; run under xvfb-run or a local desktop session.\n";
        return 0;
    }

    setenv("NICONEON_DANMAKU_WORKER", "off", 1);
    setenv("NICONEON_SIMD_MODE", "scalar", 1);

    const std::filesystem::path testBin = runfilePath("app-ui/rendernode_alignment_e2e_bin");
    if (testBin.empty()) {
        std::cerr << "Bazel runfiles environment is missing TEST_SRCDIR or TEST_WORKSPACE\n";
        return 127;
    }
    return execProgram(testBin);
}
