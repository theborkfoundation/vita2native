#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace launcher
{

struct UploadedFile {
    std::filesystem::path path;
    std::string filename;
};

struct MultipartForm {
    std::unordered_map<std::string, std::string> fields;
    std::unordered_map<std::string, std::vector<UploadedFile>> files;
    std::filesystem::path upload_dir;
};

class WebServer {
  public:
    struct Callbacks {
        std::function<std::string()> status_json;
        std::function<std::string()> games_json;
        std::function<bool(const std::string &title_id, const std::string &kind,
                           const std::string &name, std::filesystem::path &out_path)>
            game_asset;
        std::function<bool(const std::string &title_id, const MultipartForm &form,
                           std::string &error_out)>
            update_game_meta;
        std::function<bool(const std::string &title_id, std::string &error_out)> delete_game;
        std::function<bool()> is_busy;
        std::function<bool(const std::filesystem::path &pkg, const std::filesystem::path &license,
                           const std::filesystem::path &upload_dir, std::string display_name)>
            submit_import;
    };

    WebServer() = default;
    ~WebServer();

    WebServer(const WebServer &) = delete;
    WebServer &operator=(const WebServer &) = delete;

    bool start(const std::filesystem::path &staging_root, Callbacks callbacks,
               uint16_t preferred_port = 8080);
    void stop();

    bool running() const { return running_.load(); }
    const std::string &url() const { return url_; }
    const std::string &last_error() const { return last_error_; }

  private:
    void serve_loop();
    void handle_client(int client_fd);

    std::filesystem::path staging_root_;
    Callbacks callbacks_;
    int listen_fd_ = -1;
    int wake_pipe_[2] = { -1, -1 };
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_requested_{ false };
    std::thread thread_;
    std::string url_;
    std::string last_error_;
};

std::string json_escape(const std::string &input);

} // namespace launcher
