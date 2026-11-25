#include "library.h"

#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace launcher
{
namespace fs = std::filesystem;

namespace
{
std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool has_wildcard(const std::string &s)
{
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

bool wildcard_match(const char *pat, const char *str)
{
    const char *star = nullptr;
    const char *ss = str;
    while (*str) {
        if (*pat == '?' || *pat == *str) {
            ++pat;
            ++str;
        } else if (*pat == '*') {
            star = pat++;
            ss = str;
        } else if (star) {
            pat = star + 1;
            str = ++ss;
        } else {
            return false;
        }
    }
    while (*pat == '*')
        ++pat;
    return *pat == '\0';
}

bool is_supported_image(const fs::path &p)
{
    const std::string ext = to_lower(p.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

void expand_glob_parts(const fs::path &dir, const std::vector<std::string> &parts, size_t idx,
                       bool filter_images, std::vector<fs::path> &out)
{
    if (idx == parts.size()) {
        std::error_code ec;
        if (fs::is_regular_file(dir, ec) && (!filter_images || is_supported_image(dir)))
            out.push_back(dir);
        return;
    }

    const std::string &part = parts[idx];
    if (part.empty() || part == ".") {
        expand_glob_parts(dir, parts, idx + 1, filter_images, out);
        return;
    }
    if (part == "**") {
        expand_glob_parts(dir, parts, idx + 1, filter_images, out);
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            std::error_code e;
            if (it->is_directory(e))
                expand_glob_parts(it->path(), parts, idx, filter_images, out);
            it.increment(ec);
        }
        return;
    }

    if (!has_wildcard(part)) {
        const fs::path next = dir / part;
        std::error_code ec;
        if (idx + 1 == parts.size()) {
            if (fs::is_regular_file(next, ec))
                out.push_back(next);
        } else if (fs::is_directory(next, ec)) {
            expand_glob_parts(next, parts, idx + 1, filter_images, out);
        }
        return;
    }

    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        const fs::path p = it->path();
        const std::string fname = p.filename().string();
        if (!fname.empty() && fname[0] != '.' && wildcard_match(part.c_str(), fname.c_str())) {
            std::error_code e;
            if (idx + 1 == parts.size()) {
                if (it->is_regular_file(e) && (!filter_images || is_supported_image(p)))
                    out.push_back(p);
            } else if (it->is_directory(e)) {
                expand_glob_parts(p, parts, idx + 1, filter_images, out);
            }
        }
        it.increment(ec);
    }
}

std::vector<fs::path> resolve_media_glob(const fs::path &game_dir, const std::string &pattern)
{
    std::vector<fs::path> out;
    if (pattern.empty())
        return out;

    fs::path p(pattern);
    fs::path root = game_dir;
    if (p.is_absolute()) {
        root = p.root_path();
        p = p.relative_path();
    }

    std::vector<std::string> parts;
    bool used_wildcard = false;
    for (const auto &comp : p) {
        const std::string s = comp.string();
        if (s.empty() || s == "." || s == "/")
            continue;
        if (s == "..")
            return out;
        if (has_wildcard(s))
            used_wildcard = true;
        parts.push_back(s);
    }
    if (parts.empty())
        return out;

    expand_glob_parts(root, parts, 0, used_wildcard, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const toml::node *find_key(const toml::table &root, const char *section_a, const char *section_b,
                           const char *key)
{
    if (const auto *n = root.get(key))
        return n;
    if (section_a) {
        if (const auto *t = root.get_as<toml::table>(section_a)) {
            if (const auto *n = t->get(key))
                return n;
        }
    }
    if (section_b) {
        if (const auto *t = root.get_as<toml::table>(section_b)) {
            if (const auto *n = t->get(key))
                return n;
        }
    }
    return nullptr;
}

std::string read_string_value(const toml::node *node)
{
    if (!node)
        return {};
    if (auto s = node->value<std::string>())
        return *s;
    if (auto i = node->value<int64_t>())
        return std::to_string(*i);
    if (auto d = node->value<double>()) {
        std::ostringstream oss;
        oss << std::setprecision(4) << *d;
        return oss.str();
    }
    return {};
}

void append_media_field(const fs::path &game_dir, const toml::node *node,
                        std::vector<fs::path> &dest)
{
    if (!node)
        return;
    if (auto s = node->value<std::string>()) {
        auto matches = resolve_media_glob(game_dir, *s);
        dest.insert(dest.end(), matches.begin(), matches.end());
    } else if (const auto *arr = node->as_array()) {
        for (const auto &elem : *arr) {
            if (auto s = elem.value<std::string>()) {
                auto matches = resolve_media_glob(game_dir, *s);
                dest.insert(dest.end(), matches.begin(), matches.end());
            }
        }
    }
    std::sort(dest.begin(), dest.end());
    dest.erase(std::unique(dest.begin(), dest.end()), dest.end());
}

std::string read_sfo_title(const fs::path &sfo_path)
{
    std::ifstream f(sfo_path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const std::streamoff size = f.tellg();
    if (size < 20 || size > 64 * 1024)
        return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(buf.data()), size))
        return {};

    auto u16le = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(buf[off]) | (static_cast<uint16_t>(buf[off + 1]) << 8);
    };
    auto u32le = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(buf[off]) | (static_cast<uint32_t>(buf[off + 1]) << 8) |
               (static_cast<uint32_t>(buf[off + 2]) << 16) |
               (static_cast<uint32_t>(buf[off + 3]) << 24);
    };
    if (u32le(0) != 0x46535000u)
        return {};

    const uint32_t keys_offset = u32le(8);
    const uint32_t values_offset = u32le(12);
    const uint32_t count = u32le(16);
    std::string title, stitle;

    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= static_cast<uint32_t>(size); ++i) {
        const size_t e = 20 + i * 16;
        const uint32_t key_off = keys_offset + u16le(e);
        const uint32_t val_off = values_offset + u32le(e + 12);
        if (key_off >= static_cast<uint32_t>(size) || val_off >= static_cast<uint32_t>(size))
            continue;
        const char *key = reinterpret_cast<const char *>(&buf[key_off]);
        const size_t key_max = static_cast<size_t>(size) - key_off;
        const size_t val_max = static_cast<size_t>(size) - val_off;
        if (!std::memchr(key, '\0', key_max))
            continue;
        const char *val = reinterpret_cast<const char *>(&buf[val_off]);
        const void *val_nul = std::memchr(val, '\0', val_max);
        const size_t val_len =
            val_nul ? static_cast<const char *>(val_nul) - val : val_max;
        if (std::strcmp(key, "TITLE") == 0 && title.empty())
            title.assign(val, val_len);
        else if (std::strcmp(key, "STITLE") == 0 && stitle.empty())
            stitle.assign(val, val_len);
    }

    std::string chosen = !title.empty() ? std::move(title) : std::move(stitle);
    for (char &c : chosen) {
        if (c == '\n' || c == '\r')
            c = ' ';
    }
    return chosen;
}

void load_metadata(GameEntry &entry)
{
    std::error_code ec;
    const fs::path sce_sys = entry.dir / "sce_sys";
    if (fs::is_directory(sce_sys, ec)) {
        if (auto sfo_title = read_sfo_title(sce_sys / "param.sfo"); !sfo_title.empty())
            entry.name = std::move(sfo_title);
        if (fs::is_regular_file(sce_sys / "icon0.png", ec))
            entry.icon = sce_sys / "icon0.png";
        if (fs::is_regular_file(sce_sys / "pic0.png", ec))
            entry.art = sce_sys / "pic0.png";
        else if (fs::is_regular_file(sce_sys / "livearea" / "contents" / "bg.png", ec))
            entry.art = sce_sys / "livearea" / "contents" / "bg.png";
    }

    const fs::path toml_path = entry.dir / "v2n.toml";
    if (!fs::is_regular_file(toml_path, ec))
        return;

    try {
        const toml::table root = toml::parse_file(toml_path.string());

        if (auto v = read_string_value(find_key(root, "meta", "game", "name")); !v.empty())
            entry.name = std::move(v);
        entry.description = read_string_value(find_key(root, "meta", "game", "description"));
        entry.category = read_string_value(find_key(root, "meta", "game", "category"));
        entry.rating = read_string_value(find_key(root, "meta", "game", "rating"));

        auto pick_single = [&](const char *key, const char *alias) -> fs::path {
            const toml::node *n = find_key(root, "media", "game", key);
            if (!n && alias)
                n = find_key(root, "media", "game", alias);
            if (!n)
                return {};
            if (auto s = n->value<std::string>()) {
                auto matches = resolve_media_glob(entry.dir, *s);
                if (!matches.empty())
                    return matches.front();
            }
            return {};
        };

        if (auto art = pick_single("art", "hero"); !art.empty())
            entry.art = std::move(art);
        if (auto icon = pick_single("icon", "boxart"); !icon.empty())
            entry.icon = std::move(icon);
        append_media_field(entry.dir, find_key(root, "media", "game", "screenshots"),
                           entry.screenshots);
    } catch (const std::exception &err) {
        fprintf(stderr, "Could not parse %s: %s\n", toml_path.string().c_str(), err.what());
    }
}
} // namespace

std::vector<GameEntry> scan_games(const fs::path &games_path)
{
    std::vector<GameEntry> games;
    if (games_path.empty())
        return games;

    std::error_code ec;
    if (!fs::is_directory(games_path, ec))
        return games;

    fs::directory_iterator it(games_path, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code e;
        const fs::path dir = it->path();
        const std::string title_id = dir.filename().string();
        if (it->is_directory(e) && !title_id.empty() && title_id[0] != '.') {
            const fs::path eboot = dir / "eboot.bin";
            if (fs::is_regular_file(eboot, e)) {
                GameEntry entry;
                entry.title_id = title_id;
                entry.name = title_id;
                entry.dir = dir;
                entry.eboot = eboot;
                load_metadata(entry);
                games.push_back(std::move(entry));
            }
        }
        it.increment(ec);
    }

    std::sort(games.begin(), games.end(), [](const GameEntry &a, const GameEntry &b) {
        const std::string na = to_lower(a.name);
        const std::string nb = to_lower(b.name);
        if (na != nb)
            return na < nb;
        return a.title_id < b.title_id;
    });
    return games;
}

std::vector<fs::path> gallery_media(const GameEntry &entry)
{
    std::vector<fs::path> out;
    if (!entry.art.empty())
        out.push_back(entry.art);
    for (const auto &shot : entry.screenshots) {
        if (shot != entry.art)
            out.push_back(shot);
    }
    return out;
}
} // namespace launcher
