#include "web_server.h"

#include "web_app_js.h"
#include "web_index_html.h"
#include "web_style_css.h"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace launcher
{
namespace
{
std::string to_lower_ascii(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ascii(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r'))
        ++start;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        --end;
    return s.substr(start, end - start);
}

std::string detect_lan_ipv4()
{
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0)
        return "127.0.0.1";

    std::string fallback_link_local;
    std::string best;

    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        char buf[INET_ADDRSTRLEN]{};
        const auto *sa = reinterpret_cast<const struct sockaddr_in *>(ifa->ifa_addr);
        if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
            continue;
        std::string ip(buf);
        if (ip.rfind("169.254.", 0) == 0) {
            if (fallback_link_local.empty())
                fallback_link_local = ip;
        } else {
            best = ip;
            break;
        }
    }
    freeifaddrs(ifaddr);
    if (!best.empty())
        return best;
    if (!fallback_link_local.empty())
        return fallback_link_local;
    return "127.0.0.1";
}

bool send_all(int fd, const void *data, size_t len)
{
    const auto *ptr = static_cast<const char *>(data);
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, ptr + sent, len - sent, 0);
#endif
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void send_response(int fd, int status, const char *status_text, const char *content_type,
                   const void *body, size_t body_len)
{
    char header[512];
    const int n = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, content_type, body_len);
    if (n > 0)
        send_all(fd, header, static_cast<size_t>(n));
    if (body && body_len > 0)
        send_all(fd, body, body_len);
}

void send_json_error(int fd, int status, const char *status_text, const std::string &msg)
{
    const std::string body = "{\"error\":\"" + json_escape(msg) + "\"}";
    send_response(fd, status, status_text, "application/json; charset=utf-8", body.data(),
                  body.size());
}

std::string extract_param(const std::string &header_val, const std::string &key)
{
    const std::string needle = key + "=";
    size_t pos = 0;
    while (pos < header_val.size()) {
        const size_t found = header_val.find(needle, pos);
        if (found == std::string::npos)
            return {};
        if (found > 0 && header_val[found - 1] != ';' && header_val[found - 1] != ' ' &&
            header_val[found - 1] != '\t') {
            pos = found + needle.size();
            continue;
        }
        size_t val_start = found + needle.size();
        if (val_start >= header_val.size())
            return {};
        if (header_val[val_start] == '"') {
            ++val_start;
            const size_t val_end = header_val.find('"', val_start);
            if (val_end == std::string::npos)
                return {};
            return header_val.substr(val_start, val_end - val_start);
        }
        const size_t val_end = header_val.find_first_of("; \t\r\n", val_start);
        return header_val.substr(val_start, val_end - val_start);
    }
    return {};
}

class SocketReader {
  public:
    SocketReader(int fd, const std::atomic<bool> &stop_flag, std::string initial = {})
        : fd_(fd), stop_flag_(stop_flag), buf_(std::move(initial))
    {
    }

    size_t buffered_size() const { return buf_.size() - pos_; }

    bool fill_more(size_t max_chunk = 65536)
    {
        if (pos_ > 0) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
        const size_t old_size = buf_.size();
        buf_.resize(old_size + max_chunk);
        while (!stop_flag_.load()) {
            const ssize_t n = recv(fd_, &buf_[old_size], max_chunk, 0);
            if (n > 0) {
                buf_.resize(old_size + static_cast<size_t>(n));
                return true;
            }
            if (n == 0) {
                buf_.resize(old_size);
                return false;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            buf_.resize(old_size);
            return false;
        }
        buf_.resize(old_size);
        return false;
    }

    bool read_until(const std::string &delim, std::string &out, size_t max_bytes, size_t &remaining)
    {
        out.clear();
        while (true) {
            const size_t found = buf_.find(delim, pos_);
            if (found != std::string::npos) {
                const size_t len = found - pos_;
                if (len > max_bytes || len + delim.size() > remaining)
                    return false;
                out.assign(buf_, pos_, len);
                pos_ = found + delim.size();
                remaining -= (len + delim.size());
                return true;
            }
            if (buffered_size() > max_bytes)
                return false;
            if (remaining <= buffered_size())
                return false;
            if (!fill_more(std::min<size_t>(65536, remaining - buffered_size())))
                return false;
        }
    }

    bool read_exact(size_t count, std::string &out, size_t &remaining)
    {
        if (count > remaining)
            return false;
        while (buffered_size() < count) {
            if (!fill_more(std::min<size_t>(65536, remaining - buffered_size())))
                return false;
        }
        out.assign(buf_, pos_, count);
        pos_ += count;
        remaining -= count;
        return true;
    }

    // Streams bytes to `file` until `delim` is encountered. Consumes `delim`.
    bool stream_until(const std::string &delim, std::FILE *file, size_t &remaining,
                      size_t &bytes_written)
    {
        bytes_written = 0;
        while (true) {
            const size_t avail = buffered_size();
            const size_t found = buf_.find(delim, pos_);
            if (found != std::string::npos) {
                const size_t len = found - pos_;
                if (len + delim.size() > remaining)
                    return false;
                if (file && len > 0) {
                    if (std::fwrite(&buf_[pos_], 1, len, file) != len)
                        return false;
                    bytes_written += len;
                }
                pos_ = found + delim.size();
                remaining -= (len + delim.size());
                return true;
            }
            if (avail > delim.size()) {
                const size_t safe_len = avail - delim.size();
                if (safe_len > remaining)
                    return false;
                if (file && safe_len > 0) {
                    if (std::fwrite(&buf_[pos_], 1, safe_len, file) != safe_len)
                        return false;
                    bytes_written += safe_len;
                }
                pos_ += safe_len;
                remaining -= safe_len;
            }
            if (remaining <= buffered_size())
                return false;
            if (!fill_more(std::min<size_t>(65536, remaining - buffered_size())))
                return false;
        }
    }

  private:
    int fd_;
    const std::atomic<bool> &stop_flag_;
    std::string buf_;
    size_t pos_ = 0;
};
} // namespace

std::string json_escape(const std::string &input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start(const std::filesystem::path &staging_root, Callbacks callbacks,
                      uint16_t preferred_port)
{
    stop();
    last_error_.clear();
    staging_root_ = staging_root;
    callbacks_ = std::move(callbacks);

    if (pipe(wake_pipe_) != 0) {
        last_error_ = "Could not create wake pipe.";
        return false;
    }
    for (int fd : wake_pipe_)
        fcntl(fd, F_SETFL, O_NONBLOCK);

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        last_error_ = "Could not create server socket.";
        stop();
        return false;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    auto try_bind = [this](uint16_t port) -> bool {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        return bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0;
    };

    if (!try_bind(preferred_port) && (preferred_port == 0 || !try_bind(0))) {
        last_error_ = "Could not bind web server port.";
        stop();
        return false;
    }

    if (listen(listen_fd_, 8) != 0) {
        last_error_ = "Could not listen on web server socket.";
        stop();
        return false;
    }

    struct sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr *>(&bound), &len) != 0) {
        last_error_ = "Could not read bound port.";
        stop();
        return false;
    }
    const uint16_t actual_port = ntohs(bound.sin_port);
    url_ = "http://" + detect_lan_ipv4() + ":" + std::to_string(actual_port);

    stop_requested_.store(false);
    running_.store(true);
    try {
        thread_ = std::thread(&WebServer::serve_loop, this);
    } catch (...) {
        last_error_ = "Could not start web server thread.";
        stop();
        return false;
    }
    return true;
}

void WebServer::stop()
{
    stop_requested_.store(true);
    if (wake_pipe_[1] >= 0) {
        const char byte = 1;
        const ssize_t ignored = write(wake_pipe_[1], &byte, 1);
        (void)ignored;
    }
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    for (int &fd : wake_pipe_) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    running_.store(false);
    url_.clear();
}

void WebServer::serve_loop()
{
    while (!stop_requested_.load()) {
        struct pollfd fds[2]{};
        fds[0].fd = listen_fd_;
        fds[0].events = POLLIN;
        fds[1].fd = wake_pipe_[0];
        fds[1].events = POLLIN;

        const int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (stop_requested_.load() || (fds[1].revents & POLLIN))
            break;
        if (fds[0].revents & POLLIN) {
            const int client_fd = accept(listen_fd_, nullptr, nullptr);
            if (client_fd >= 0) {
                handle_client(client_fd);
                close(client_fd);
            }
        }
    }
    running_.store(false);
}

void WebServer::handle_client(int client_fd)
{
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    struct timeval tv{};
    tv.tv_sec = 2;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string raw;
    char tmp[4096];
    size_t header_end = std::string::npos;
    while (!stop_requested_.load() && raw.size() < 32768) {
        const ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            raw.append(tmp, static_cast<size_t>(n));
            header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos)
                break;
        } else if (n == 0) {
            return;
        } else {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return;
        }
    }
    if (header_end == std::string::npos) {
        send_json_error(client_fd, 400, "Bad Request", "Invalid HTTP headers.");
        return;
    }

    const std::string header_block = raw.substr(0, header_end);
    const std::string leftover = raw.substr(header_end + 4);

    const size_t line_end = header_block.find("\r\n");
    const std::string req_line =
        line_end == std::string::npos ? header_block : header_block.substr(0, line_end);
    const size_t sp1 = req_line.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : req_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        send_json_error(client_fd, 400, "Bad Request", "Malformed request line.");
        return;
    }
    const std::string method = req_line.substr(0, sp1);
    std::string path = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = path.find('?');
    if (q != std::string::npos)
        path.resize(q);

    size_t content_length = 0;
    bool has_content_length = false;
    bool expect_continue = false;
    std::string content_type;

    size_t pos = line_end == std::string::npos ? header_block.size() : line_end + 2;
    while (pos < header_block.size()) {
        const size_t next = header_block.find("\r\n", pos);
        const std::string line = header_block.substr(
            pos, next == std::string::npos ? header_block.size() - pos : next - pos);
        pos = next == std::string::npos ? header_block.size() : next + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = to_lower_ascii(trim_ascii(line.substr(0, colon)));
        const std::string val = trim_ascii(line.substr(colon + 1));
        if (name == "content-length") {
            content_length = static_cast<size_t>(std::strtoull(val.c_str(), nullptr, 10));
            has_content_length = true;
        } else if (name == "content-type") {
            content_type = val;
        } else if (name == "expect" && to_lower_ascii(val).find("100-continue") != std::string::npos) {
            expect_continue = true;
        }
    }

    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            send_response(client_fd, 200, "OK", "text/html; charset=utf-8", web_index_html,
                          sizeof(web_index_html));
        } else if (path == "/style.css") {
            send_response(client_fd, 200, "OK", "text/css; charset=utf-8", web_style_css,
                          sizeof(web_style_css));
        } else if (path == "/app.js") {
            send_response(client_fd, 200, "OK", "application/javascript; charset=utf-8", web_app_js,
                          sizeof(web_app_js));
        } else if (path == "/api/status") {
            const std::string json =
                callbacks_.status_json ? callbacks_.status_json() : "{\"busy\":false,\"history\":[]}";
            send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json.data(),
                          json.size());
        } else {
            send_json_error(client_fd, 404, "Not Found", "Not found.");
        }
        return;
    }

    if (method != "POST" || path != "/api/import") {
        send_json_error(client_fd, 405, "Method Not Allowed", "Unsupported method or path.");
        return;
    }

    if (callbacks_.is_busy && callbacks_.is_busy()) {
        send_json_error(client_fd, 409, "Conflict", "An import is already in progress.");
        return;
    }

    if (!has_content_length || content_length == 0) {
        send_json_error(client_fd, 411, "Length Required", "Content-Length is required.");
        return;
    }

    if (to_lower_ascii(content_type).find("multipart/form-data") == std::string::npos) {
        send_json_error(client_fd, 400, "Bad Request", "Expected multipart/form-data.");
        return;
    }

    const std::string boundary = extract_param(content_type, "boundary");
    if (boundary.empty()) {
        send_json_error(client_fd, 400, "Bad Request", "Missing multipart boundary.");
        return;
    }

    if (expect_continue) {
        static constexpr char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!send_all(client_fd, kContinue, sizeof(kContinue) - 1))
            return;
    }

    std::error_code ec;
    std::filesystem::create_directories(staging_root_, ec);
    if (ec) {
        send_json_error(client_fd, 500, "Internal Server Error",
                        "Could not prepare upload staging directory.");
        return;
    }

    std::string tmpl = (staging_root_ / ".v2n-web-XXXXXX").string();
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');
    char *dir_ptr = mkdtemp(tmpl_buf.data());
    if (!dir_ptr) {
        send_json_error(client_fd, 500, "Internal Server Error",
                        "Could not create temporary upload directory.");
        return;
    }
    const std::filesystem::path upload_dir(dir_ptr);
    auto cleanup_upload_dir = [&] {
        std::error_code rm_ec;
        std::filesystem::remove_all(upload_dir, rm_ec);
    };

    const std::filesystem::path pkg_path = upload_dir / "package.pkg";
    const std::filesystem::path lic_path = upload_dir / "work.bin";
    bool has_pkg = false;
    bool has_lic = false;
    std::string pkg_display_name;

    SocketReader reader(client_fd, stop_requested_, leftover);
    size_t remaining = content_length;

    const std::string first_boundary = "--" + boundary;
    const std::string next_boundary = "\r\n--" + boundary;

    std::string preamble;
    if (!reader.read_until(first_boundary, preamble, 4096, remaining)) {
        cleanup_upload_dir();
        send_json_error(client_fd, 400, "Bad Request", "Malformed multipart body.");
        return;
    }

    while (remaining > 0 && !stop_requested_.load()) {
        std::string marker;
        if (!reader.read_exact(2, marker, remaining)) {
            cleanup_upload_dir();
            send_json_error(client_fd, 400, "Bad Request", "Truncated multipart stream.");
            return;
        }
        if (marker == "--") {
            break; // End of multipart stream.
        }
        if (marker != "\r\n") {
            cleanup_upload_dir();
            send_json_error(client_fd, 400, "Bad Request", "Invalid multipart boundary suffix.");
            return;
        }

        std::string part_headers;
        if (!reader.read_until("\r\n\r\n", part_headers, 16384, remaining)) {
            cleanup_upload_dir();
            send_json_error(client_fd, 400, "Bad Request", "Invalid multipart part headers.");
            return;
        }

        std::string field_name;
        std::string file_name;
        size_t hpos = 0;
        while (hpos < part_headers.size()) {
            const size_t hend = part_headers.find("\r\n", hpos);
            const std::string hline = part_headers.substr(
                hpos, hend == std::string::npos ? part_headers.size() - hpos : hend - hpos);
            hpos = hend == std::string::npos ? part_headers.size() : hend + 2;
            const size_t cpos = hline.find(':');
            if (cpos == std::string::npos)
                continue;
            if (to_lower_ascii(trim_ascii(hline.substr(0, cpos))) == "content-disposition") {
                const std::string val = hline.substr(cpos + 1);
                field_name = extract_param(val, "name");
                file_name = extract_param(val, "filename");
            }
        }

        std::FILE *out_file = nullptr;
        if (field_name == "pkg") {
            out_file = std::fopen(pkg_path.c_str(), "wb");
            if (!file_name.empty())
                pkg_display_name = std::filesystem::path(file_name).filename().string();
        } else if (field_name == "license" || field_name == "work_bin") {
            out_file = std::fopen(lic_path.c_str(), "wb");
        }

        size_t written = 0;
        const bool ok = reader.stream_until(next_boundary, out_file, remaining, written);
        if (out_file)
            std::fclose(out_file);
        if (!ok) {
            cleanup_upload_dir();
            send_json_error(client_fd, 400, "Bad Request", "Failed reading multipart payload.");
            return;
        }
        if (field_name == "pkg" && written > 0)
            has_pkg = true;
        if ((field_name == "license" || field_name == "work_bin") && written > 0)
            has_lic = true;
    }

    if (!has_pkg || !has_lic) {
        cleanup_upload_dir();
        send_json_error(client_fd, 400, "Bad Request",
                        "Both a .pkg file and work.bin file are required.");
        return;
    }

    if (pkg_display_name.empty())
        pkg_display_name = "Web package";

    if (!callbacks_.submit_import ||
        !callbacks_.submit_import(pkg_path, lic_path, upload_dir, std::move(pkg_display_name))) {
        cleanup_upload_dir();
        send_json_error(client_fd, 409, "Conflict", "An import is already in progress.");
        return;
    }

    static constexpr char kAccepted[] = "{\"ok\":true}";
    send_response(client_fd, 202, "Accepted", "application/json; charset=utf-8", kAccepted,
                  sizeof(kAccepted) - 1);
}

} // namespace launcher
