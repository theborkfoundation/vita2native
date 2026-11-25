#include "state.h"
#include "extract.h"
#include "web_server.h"

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
class ImportEventQueue {
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
                          static_cast<ImportEventQueue *>(context)->send(
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
    ImportEventQueue channel_;
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
State::~State()
{
    stop_web_server();
}

bool State::web_server_running() const
{
    return web_server && web_server->running();
}

std::string State::web_server_url() const
{
    return web_server ? web_server->url() : std::string{};
}

void State::stop_web_server()
{
    if (web_server) {
        web_server->stop();
        web_server.reset();
    }
}

void State::toggle_web_server()
{
    if (web_server_running()) {
        stop_web_server();
        show_toast("Web import server stopped.", ToastKind::Info);
        return;
    }
    auto server = std::make_unique<WebServer>();
    WebServer::Callbacks cb;
    cb.status_json = [this] { return web_status_json(); };
    cb.is_busy = [this] {
        std::lock_guard<std::mutex> lock(web_mutex);
        return active_import_id != 0 || pending_web_import.has_value();
    };
    cb.submit_import = [this](const fs::path &p, const fs::path &lic, const fs::path &upload_dir,
                              std::string display_name) -> bool {
        std::lock_guard<std::mutex> lock(web_mutex);
        if (active_import_id != 0 || pending_web_import.has_value())
            return false;
        pending_web_import = PendingWebImport{ p, lic, upload_dir, std::move(display_name) };
        return true;
    };
    if (!server->start(config.games_path, std::move(cb), 8080)) {
        show_toast(server->last_error().empty() ? "Could not start web server."
                                                : server->last_error(),
                   ToastKind::Error);
        return;
    }
    web_server = std::move(server);
    show_toast("Web server listening at " + web_server->url(), ToastKind::Success);
}

std::string State::web_status_json() const
{
    std::lock_guard<std::mutex> lock(web_mutex);
    const bool busy = active_import_id != 0 || pending_web_import.has_value();
    std::string out = "{\"busy\":";
    out += busy ? "true" : "false";
    out += ",\"history\":[";
    for (size_t i = 0; i < import_history.size(); ++i) {
        if (i > 0)
            out += ',';
        const auto &e = import_history[i];
        const char *channel = e.channel == ImportChannel::Local ? "Local" : "Web";
        const char *status = e.status == ImportStatus::Running
                                 ? "running"
                                 : (e.status == ImportStatus::Success ? "success" : "failed");
        out += "{\"id\":" + std::to_string(e.id) + ",\"channel\":\"" + channel +
               "\",\"title\":\"" + json_escape(e.title) + "\",\"status\":\"" + status +
               "\",\"statusText\":\"" + json_escape(e.status_text) + "\"}";
    }
    out += "]}";
    return out;
}

void State::start_import()
{
    if (import_running()) {
        show_toast("Import is running; please wait.", ToastKind::Warning);
        return;
    }
    if (pkg.empty() || license.empty()) {
        show_toast("Select a package and work.bin first.", ToastKind::Warning);
        return;
    }
    begin_import(pkg, license, ImportChannel::Local, pkg.filename().string());
}

void State::begin_import(const fs::path &pkg_path, const fs::path &lic_path, ImportChannel channel,
                         std::string display_name, fs::path cleanup_dir)
{
    auto cleanup_upload = [&] {
        if (!cleanup_dir.empty()) {
            std::error_code rm_ec;
            fs::remove_all(cleanup_dir, rm_ec);
        }
    };
    auto record_failure = [&](const std::string &err_text) {
        std::lock_guard<std::mutex> lock(web_mutex);
        import_history.insert(import_history.begin(),
                              ImportHistoryEntry{ next_import_id++, channel,
                                                  std::move(display_name), err_text,
                                                  ImportStatus::Failed });
        ++import_history_revision;
        message = err_text;
        show_toast(err_text, ToastKind::Error);
    };

    std::error_code ec;
    fs::create_directories(config.games_path, ec);
    if (ec) {
        cleanup_upload();
        record_failure("Could not create games directory.");
        return;
    }
    std::string temp = (config.games_path / ".v2n-import-XXXXXX").string();
    std::vector<char> buf(temp.begin(), temp.end());
    buf.push_back(0);
    char *p = mkdtemp(buf.data());
    if (!p) {
        cleanup_upload();
        record_failure("Could not create import directory.");
        return;
    }
    stage = p;
    upload_cleanup_dir = std::move(cleanup_dir);
    message = "Importing... Please wait.";

    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(web_mutex);
        id = next_import_id++;
        active_import_id = id;
        import_history.insert(import_history.begin(),
                              ImportHistoryEntry{ id, channel, std::move(display_name),
                                                  "Starting...", ImportStatus::Running });
        ++import_history_revision;
    }

    try {
        worker = std::make_unique<ImportWorker>(pkg_path.string(), lic_path.string(), stage.string());
    } catch (const std::exception &) {
        std::error_code rm_ec;
        fs::remove_all(stage, rm_ec);
        if (!upload_cleanup_dir.empty()) {
            fs::remove_all(upload_cleanup_dir, rm_ec);
            upload_cleanup_dir.clear();
        }
        std::lock_guard<std::mutex> lock(web_mutex);
        active_import_id = 0;
        for (auto &entry : import_history) {
            if (entry.id == id) {
                entry.status = ImportStatus::Failed;
                entry.status_text = "Could not start worker.";
                break;
            }
        }
        ++import_history_revision;
        message = "Could not start import worker.";
        show_toast("Could not start import worker.", ToastKind::Error);
    }
}

void State::poll()
{
    update_toast();
    if (!worker) {
        std::optional<PendingWebImport> pending;
        {
            std::lock_guard<std::mutex> lock(web_mutex);
            if (pending_web_import.has_value()) {
                pending = std::move(pending_web_import);
                pending_web_import.reset();
            }
        }
        if (pending.has_value()) {
            begin_import(pending->pkg, pending->license, ImportChannel::Web,
                         std::move(pending->display_name), std::move(pending->upload_dir));
        }
        return;
    }

    auto update_active_entry = [&](const std::string &status_text,
                                   std::optional<ImportStatus> final_status = std::nullopt,
                                   std::optional<std::string> final_title = std::nullopt) {
        std::lock_guard<std::mutex> lock(web_mutex);
        for (auto &entry : import_history) {
            if (entry.id == active_import_id) {
                if (final_title.has_value())
                    entry.title = *final_title;
                entry.status_text = status_text;
                if (final_status.has_value())
                    entry.status = *final_status;
                break;
            }
        }
        if (final_status.has_value())
            active_import_id = 0;
        ++import_history_revision;
    };

    ImportEvent event;
    while (worker->try_receive(event)) {
        if (event.kind == ImportEvent::Kind::Progress) {
            message = event.text;
            update_active_entry(event.text);
            continue;
        }
        worker.reset(); // Joins the finished thread before its channel is destroyed.
        if (!upload_cleanup_dir.empty()) {
            std::error_code rm_ec;
            fs::remove_all(upload_cleanup_dir, rm_ec);
            upload_cleanup_dir.clear();
        }
        if (event.result != 0) {
            message = event.result < 0 ? "Could not create import log."
                                       : "Import failed. Log: " + (stage / "import.log").string();
            update_active_entry(event.result < 0 ? "Log error" : "Failed", ImportStatus::Failed);
            show_toast(message, ToastKind::Error);
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
            update_active_entry("Output invalid", ImportStatus::Failed);
            show_toast(message, ToastKind::Error);
            return;
        }
        fs::path dest = config.games_path / outputs[0].filename();
        if (move_title_no_replace(outputs[0], dest) != 0) {
            const std::string title_id = outputs[0].filename().string();
            if (errno == EEXIST || errno == ENOTEMPTY) {
                fs::remove_all(stage, ec);
                stage.clear();
                message = "Title already exists: " + title_id;
                update_active_entry("Already exists", ImportStatus::Failed, title_id);
            } else {
                fprintf(stderr, "Could not move title from %s to %s: %s\n", stage.string().c_str(),
                        dest.string().c_str(), std::strerror(errno));
                message = "Could not move title. Saved in " + stage.string();
                update_active_entry("Move failed", ImportStatus::Failed, title_id);
            }
            show_toast(message, ToastKind::Error);
            return;
        }
        const std::string title_id = dest.filename().string();
        fs::remove_all(stage, ec);
        refresh_games();
        std::string display_title = title_id;
        for (const auto &g : games) {
            if (g.title_id == title_id && !g.name.empty()) {
                display_title = g.name + " (" + title_id + ")";
                break;
            }
        }
        message = "Import complete: " + title_id;
        update_active_entry("Imported", ImportStatus::Success, display_title);
        show_toast("Imported " + display_title, ToastKind::Success);
        pkg.clear();
        license.clear();
        return;
    }
}
} // namespace launcher
