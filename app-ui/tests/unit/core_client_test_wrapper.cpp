#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "rules_cc/cc/runfiles/runfiles.h"

namespace {

using rules_cc::cc::runfiles::Runfiles;

std::filesystem::path executableRunfilePath(const Runfiles &runfiles, const char *relativePath) {
    const char *workspace = std::getenv("TEST_WORKSPACE");
    if (workspace == nullptr) {
        return {};
    }
    std::string path = std::string(workspace) + "/" + relativePath;
#ifdef _WIN32
    path += ".exe";
#endif
    return runfiles.Rlocation(path);
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
    std::string error;
    std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
    if (runfiles == nullptr) {
        std::cerr << "failed to initialize Bazel runfiles: " << error << '\n';
        return 127;
    }

    const std::filesystem::path testBin = executableRunfilePath(*runfiles, "app-ui/core_client_test_bin");
    if (testBin.empty()) {
        std::cerr << "failed to locate core_client_test_bin in Bazel runfiles\n";
        return 127;
    }

    if (std::getenv("NICONEON_FAKE_CORE_BIN") == nullptr) {
        const std::filesystem::path fakeCore = executableRunfilePath(*runfiles, "app-ui/niconeon-fake-core");
        if (fakeCore.empty()) {
            std::cerr << "failed to locate niconeon-fake-core in Bazel runfiles\n";
            return 127;
        }
        setenv("NICONEON_FAKE_CORE_BIN", fakeCore.c_str(), 1);
    }

    return execProgram(testBin);
}
