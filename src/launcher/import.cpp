#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "state.h"

namespace launcher
{
void State::start_import()
{
    if (child > 0)
        return;
    if (pkg.empty() || license.empty()) {
        message = "Select a package and work.bin first.";
        return;
    }
    std::string temp = (fs::current_path() / ".v2n-import-XXXXXX").string();
    std::vector<char> buf(temp.begin(), temp.end());
    buf.push_back(0);
    char *p = mkdtemp(buf.data());
    if (!p) {
        message = "Could not create import directory.";
        return;
    }
    stage = p;
    std::string executable;
    const char *env = getenv("V2N_EXTRACT");
    if (env && *env)
        executable = env;
    else {
        char *base = SDL_GetBasePath();
        if (base) {
            fs::path sibling = fs::path(base) / "v2n-extract";
            SDL_free(base);
            if (access(sibling.c_str(), X_OK) == 0)
                executable = sibling.string();
        }
        if (executable.empty())
            executable = "v2n-extract";
    }
    child = fork();
    if (child == 0) {
        int fd = open((stage / "import.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execlp(executable.c_str(), executable.c_str(), "--pkg", pkg.c_str(), "--work-bin",
               license.c_str(), "--out", stage.c_str(), (char *)nullptr);
        _exit(127);
    }
    if (child < 0) {
        child = -1;
        std::error_code ec;
        fs::remove_all(stage, ec);
        message = "Could not start extractor.";
    } else
        message = "Importing... Please wait.";
}

void State::poll()
{
    if (child <= 0)
        return;
    int status = 0;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == 0)
        return;
    child = -1;
    std::error_code ec;
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        message = (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127)
                      ? "Extractor not found. Set V2N_EXTRACT."
                      : "Import failed. Log: " + stage.filename().string() + "/import.log";
        return;
    }
    std::vector<fs::path> outputs;
    fs::directory_iterator it(stage, ec), end;
    while (!ec && it != end) {
        if (it->is_directory(ec) && it->path().filename().string().rfind(".v2n-temp-", 0) != 0)
            outputs.push_back(it->path());
        it.increment(ec);
    }
    if (ec || outputs.size() != 1) {
        message = "Import output invalid. See staging folder.";
        return;
    }
    fs::path dest = fs::current_path() / outputs[0].filename();
    if (fs::exists(dest, ec) || ec) {
        message = "Title exists. Saved in " + stage.filename().string();
        return;
    }
    fs::rename(outputs[0], dest, ec);
    if (ec) {
        message = "Could not move title. See staging folder.";
        return;
    }
    fs::remove_all(stage, ec);
    message = "Import complete: " + dest.filename().string();
    pkg.clear();
    license.clear();
}
} // namespace launcher
