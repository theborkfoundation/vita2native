#include "state.h"
#include "extract.h"
#include "web_server.h"

#include <toml++/toml.hpp>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
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
bool is_valid_title_id(const std::string &id)
{
    if (id.empty() || id.size() > 64 || id == "." || id == "..")
        return false;
    for (unsigned char c : id) {
        if (c == '/' || c == '\\' || c < 0x20)
            return false;
    }
    return true;
}

std::string url_encode_component(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::vector<std::string> split_lines(const std::string &s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? s.size() - pos : nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            out.push_back(std::move(line));
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return out;
}

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
        home_focus = HomeFocus::WebUI;
        previous_home_focus = HomeFocus::WebUI;
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
        show_toast("Web UI stopped.", ToastKind::Info);
        return;
    }
    auto server = std::make_unique<WebServer>();
    WebServer::Callbacks cb;
    cb.status_json = [this] { return web_status_json(); };
    cb.games_json = [this] { return web_games_json(); };
    cb.game_asset = [this](const std::string &title_id, const std::string &kind,
                           const std::string &name, fs::path &out_path) -> bool {
        if (!is_valid_title_id(title_id))
            return false;
        const auto list = scan_games(config.games_path);
        for (const auto &g : list) {
            if (g.title_id != title_id)
                continue;
            if (kind == "art" && !g.art.empty()) {
                out_path = g.art;
                return true;
            }
            if (kind == "icon" && !g.icon.empty()) {
                out_path = g.icon;
                return true;
            }
            if (kind == "screenshot" && !name.empty()) {
                for (const auto &shot : g.screenshots) {
                    if (shot.filename().string() == name) {
                        out_path = shot;
                        return true;
                    }
                }
            }
            return false;
        }
        return false;
    };
    cb.update_game_meta = [this](const std::string &title_id, const MultipartForm &form,
                                 std::string &error_out) -> bool {
        if (!is_valid_title_id(title_id)) {
            error_out = "Invalid title ID.";
            return false;
        }
        std::error_code ec;
        const fs::path game_dir = config.games_path / title_id;
        if (!fs::is_directory(game_dir, ec)) {
            error_out = "Game not found.";
            return false;
        }
        const fs::path toml_path = game_dir / "v2n.toml";
        toml::table root;
        if (fs::is_regular_file(toml_path, ec)) {
            try {
                root = toml::parse_file(toml_path.string());
            } catch (...) {
                root = toml::table{};
            }
        }
        if (!root.contains("meta") || !root["meta"].is_table())
            root.insert_or_assign("meta", toml::table{});
        if (!root.contains("media") || !root["media"].is_table())
            root.insert_or_assign("media", toml::table{});
        auto &meta = *root["meta"].as_table();
        auto &media = *root["media"].as_table();

        auto set_str = [&](const char *key) {
            const auto it = form.fields.find(key);
            if (it == form.fields.end())
                return;
            if (it->second.empty())
                meta.erase(key);
            else
                meta.insert_or_assign(key, it->second);
        };
        set_str("name");
        set_str("description");
        set_str("category");
        set_str("rating");

        const fs::path media_dir = game_dir / "v2n-media";
        auto save_single_asset = [&](const char *field, const char *remove_field,
                                     const char *toml_key, const char *base_name) {
            if (auto rm = form.fields.find(remove_field);
                rm != form.fields.end() && rm->second == "1") {
                media.erase(toml_key);
                return;
            }
            const auto fit = form.files.find(field);
            if (fit == form.files.end() || fit->second.empty())
                return;
            fs::create_directories(media_dir, ec);
            const auto &up = fit->second.front();
            const std::string ext = up.path.extension().string();
            const fs::path dest = media_dir / (std::string(base_name) + ext);
            fs::copy_file(up.path, dest, fs::copy_options::overwrite_existing, ec);
            if (!ec)
                media.insert_or_assign(toml_key, std::string("v2n-media/") + dest.filename().string());
        };
        save_single_asset("art", "removeArt", "art", "art");
        save_single_asset("icon", "removeIcon", "icon", "icon");
        media.erase("boxart");

        std::vector<std::string> shots;
        if (const auto *arr = media["screenshots"].as_array()) {
            for (const auto &el : *arr) {
                if (auto s = el.value<std::string>())
                    shots.push_back(*s);
            }
        } else if (auto s = media["screenshots"].value<std::string>()) {
            shots.push_back(*s);
        }

        if (auto rm_it = form.fields.find("removeScreenshots"); rm_it != form.fields.end()) {
            const auto to_remove = split_lines(rm_it->second);
            shots.erase(std::remove_if(shots.begin(), shots.end(),
                                       [&](const std::string &s) {
                                           const std::string fname = fs::path(s).filename().string();
                                           return std::find(to_remove.begin(), to_remove.end(),
                                                            fname) != to_remove.end();
                                       }),
                        shots.end());
        }

        if (auto sit = form.files.find("screenshots"); sit != form.files.end()) {
            fs::create_directories(media_dir, ec);
            size_t idx = 1;
            for (const auto &up : sit->second) {
                const std::string ext = up.path.extension().string();
                fs::path dest;
                do {
                    dest = media_dir / ("screenshot-" + std::to_string(idx++) + ext);
                } while (fs::exists(dest, ec));
                fs::copy_file(up.path, dest, fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    shots.push_back(std::string("v2n-media/") + dest.filename().string());
            }
        }

        if (shots.empty()) {
            media.erase("screenshots");
        } else {
            toml::array arr;
            for (const auto &s : shots)
                arr.push_back(s);
            media.insert_or_assign("screenshots", std::move(arr));
        }

        std::ostringstream oss;
        oss << root << "\n";
        std::ofstream out(toml_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error_out = "Could not write v2n.toml.";
            return false;
        }
        const std::string text = oss.str();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();

        std::lock_guard<std::mutex> lock(web_mutex);
        pending_games_refresh = true;
        return true;
    };
    cb.delete_game = [this](const std::string &title_id, std::string &error_out) -> bool {
        if (!is_valid_title_id(title_id)) {
            error_out = "Invalid title ID.";
            return false;
        }
        std::error_code ec;
        const fs::path game_dir = config.games_path / title_id;
        if (!fs::is_directory(game_dir, ec)) {
            error_out = "Game not found.";
            return false;
        }
        fs::remove_all(game_dir, ec);
        if (ec) {
            error_out = "Could not delete game directory.";
            return false;
        }
        std::lock_guard<std::mutex> lock(web_mutex);
        pending_games_refresh = true;
        return true;
    };
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
    show_toast("Web UI listening at " + web_server->url(), ToastKind::Success);
}

std::string State::web_games_json() const
{
    const auto list = scan_games(config.games_path);
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0)
            out += ',';
        const auto &g = list[i];
        const std::string enc_id = url_encode_component(g.title_id);
        out += "{\"titleId\":\"" + json_escape(g.title_id) + "\",\"name\":\"" +
               json_escape(g.name) + "\",\"description\":\"" + json_escape(g.description) +
               "\",\"category\":\"" + json_escape(g.category) + "\",\"rating\":\"" +
               json_escape(g.rating) + "\",\"hasArt\":" + (g.art.empty() ? "false" : "true") +
               ",\"hasIcon\":" + (g.icon.empty() ? "false" : "true") +
               ",\"artUrl\":" +
               (g.art.empty() ? "null"
                              : ("\"/api/games/" + enc_id + "/asset?kind=art\"")) +
               ",\"iconUrl\":" +
               (g.icon.empty() ? "null"
                               : ("\"/api/games/" + enc_id + "/asset?kind=icon\"")) +
               ",\"screenshots\":[";
        for (size_t s = 0; s < g.screenshots.size(); ++s) {
            if (s > 0)
                out += ',';
            const std::string fname = g.screenshots[s].filename().string();
            out += "{\"name\":\"" + json_escape(fname) + "\",\"url\":\"/api/games/" + enc_id +
                   "/asset?kind=screenshot&name=" + url_encode_component(fname) + "\"}";
        }
        out += "]}";
    }
    out += "]";
    return out;
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
        const char *status = e.status == ImportStatus::Running
                                 ? "running"
                                 : (e.status == ImportStatus::Success ? "success" : "failed");
        out += "{\"id\":" + std::to_string(e.id) + ",\"channel\":\"Web\",\"title\":\"" +
               json_escape(e.title) + "\",\"status\":\"" + status + "\",\"statusText\":\"" +
               json_escape(e.status_text) + "\"}";
    }
    out += "]}";
    return out;
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
    bool need_refresh = false;
    {
        std::lock_guard<std::mutex> lock(web_mutex);
        if (pending_games_refresh) {
            need_refresh = true;
            pending_games_refresh = false;
        }
    }
    if (need_refresh)
        refresh_games();
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
        return;
    }
}
} // namespace launcher
