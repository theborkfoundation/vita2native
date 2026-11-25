#include "state.h"
#include "extract.h"

#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#elif defined(__APPLE__)
#include <sys/stdio.h>
#endif

namespace launcher
{
namespace
{
// Staging and the destination share a directory/filesystem. Never replace an
// existing title, even if it appears after the import begins.
int move_title_no_replace(const fs::path &from, const fs::path &to)
{
#ifdef __linux__
    return static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD,
                                    to.c_str(), RENAME_NOREPLACE));
#elif defined(__APPLE__)
    return renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#else
    (void)from;
    (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}

struct ImportEvent {
    enum class Kind { Progress, Finished } kind;
    std::string text;
    int result = 0;
};

// A bounded, single-consumer channel. Sending waits for space; receiving never
// blocks the UI. Closing wakes a sender during shutdown.
class ImportChannel {
  public:
    bool send(ImportEvent event)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [&] { return closed_ || queue_.size() < capacity; });
        if (closed_)
            return false;
        queue_.push_back(std::move(event));
        return true;
    }

    bool try_receive(ImportEvent &event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty())
            return false;
        event = std::move(queue_.front());
        queue_.pop_front();
        space_.notify_one();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        space_.notify_all();
    }

  private:
    static constexpr size_t capacity = 8;
    std::mutex mutex_;
    std::condition_variable space_;
    std::deque<ImportEvent> queue_;
    bool closed_ = false;
};
} // namespace

class ImportWorker {
  public:
    ImportWorker(std::string package, std::string license, std::string output)
        : thread_([this, package = std::move(package), license = std::move(license),
                   output = std::move(output)] {
              int result = -1;
              const std::string log_path = output + "/import.log";
              std::FILE *log = std::fopen(log_path.c_str(), "w");
              if (log) {
                  result = 1;
                  try {
                      V2nExtractOptions options;
                      options.pkg_path = package.c_str();
                      options.work_bin = license.c_str();
                      options.out_dir = output.c_str();
                      options.log = log;
                      options.progress_context = &channel_;
                      options.progress = [](void *context, const char *message) {
                          static_cast<ImportChannel *>(context)->send(
                              {ImportEvent::Kind::Progress, message});
                      };
                      result = v2n_extract_package(options);
                  } catch (const std::exception &e) {
                      std::fprintf(log, "Import failed: %s\n", e.what());
                  } catch (...) {
                      std::fprintf(log, "Import failed: unexpected exception\n");
                  }
                  std::fclose(log);
              }
              channel_.send({ImportEvent::Kind::Finished, {}, result});
          })
    {
    }

    ~ImportWorker()
    {
        channel_.close();
        if (thread_.joinable())
            thread_.join();
    }

    bool try_receive(ImportEvent &event) { return channel_.try_receive(event); }

  private:
    ImportChannel channel_;
    std::thread thread_;
};

State::State(Config settings, std::string error)
    : config(std::move(settings)), config_error(std::move(error))
{
    if (config_error.empty())
        refresh_games();
    if (games.empty()) {
        home_focus = HomeFocus::Import;
        previous_home_focus = HomeFocus::Import;
    }
}
State::~State() = default;

void State::start_import()
{
    if (import_running())
        return;
    if (pkg.empty() || license.empty()) {
        message = "Select a package and work.bin first.";
        return;
    }
    std::error_code ec;
    fs::create_directories(config.games_path, ec);
    if (ec) {
        message = "Could not create games directory.";
        return;
    }
    std::string temp = (config.games_path / ".v2n-import-XXXXXX").string();
    std::vector<char> buf(temp.begin(), temp.end());
    buf.push_back(0);
    char *p = mkdtemp(buf.data());
    if (!p) {
        message = "Could not create import directory.";
        return;
    }
    stage = p;
    message = "Importing... Please wait.";
    try {
        worker = std::make_unique<ImportWorker>(pkg.string(), license.string(), stage.string());
    } catch (const std::exception &) {
        std::error_code ec;
        fs::remove_all(stage, ec);
        message = "Could not start import worker.";
    }
}

void State::poll()
{
    update_toast();
    if (!worker)
        return;
    ImportEvent event;
    while (worker->try_receive(event)) {
        if (event.kind == ImportEvent::Kind::Progress) {
            message = event.text;
            continue;
        }
        worker.reset(); // Joins the finished thread before its channel is destroyed.
        if (event.result != 0) {
            message = event.result < 0 ? "Could not create import log."
                                       : "Import failed. Log: " + (stage / "import.log").string();
            return;
        }
        std::error_code ec;
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
        fs::path dest = config.games_path / outputs[0].filename();
        if (move_title_no_replace(outputs[0], dest) != 0) {
            if (errno == EEXIST || errno == ENOTEMPTY)
                message = "Title exists. Saved in " + stage.string();
            else {
                fprintf(stderr, "Could not move title from %s to %s: %s\n", stage.string().c_str(),
                        dest.string().c_str(), std::strerror(errno));
                message = "Could not move title. Saved in " + stage.string();
            }
            return;
        }
        fs::remove_all(stage, ec);
        refresh_games();
        message = "Import complete: " + dest.filename().string();
        pkg.clear();
        license.clear();
        return;
    }
}
} // namespace launcher
