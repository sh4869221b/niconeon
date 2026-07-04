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
    const std::filesystem::path testBin = runfilePath("app-ui/core_client_test_bin");
    if (testBin.empty()) {
        std::cerr << "Bazel runfiles environment is missing TEST_SRCDIR or TEST_WORKSPACE\n";
        return 127;
    }

    if (std::getenv("NICONEON_FAKE_CORE_BIN") == nullptr) {
        const std::filesystem::path fakeCore = runfilePath("app-ui/niconeon-fake-core");
        setenv("NICONEON_FAKE_CORE_BIN", fakeCore.c_str(), 1);
    }

    return execProgram(testBin);
}
