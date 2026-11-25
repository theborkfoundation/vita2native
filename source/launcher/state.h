#pragma once

#include <SDL.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "config.h"
#include "library.h"

namespace launcher
{
namespace fs = std::filesystem;

enum class Screen {
    Home,
    Import,
    AllGames,
    GameInfo
};
enum class HomeFocus {
    RecentGames,
    AllGames,
    Import
};
enum class PickerKind {
    Package,
    License
};
enum class ImportFocus {
    Local,
    Web
};
enum class ImportChannel {
    Local,
    Web
};
enum class ImportStatus {
    Running,
    Success,
    Failed
};
struct ImportHistoryEntry {
    uint64_t id = 0;
    ImportChannel channel = ImportChannel::Local;
    std::string title;
    std::string status_text;
    ImportStatus status = ImportStatus::Running;
};
enum class GameInfoFocus {
    Play,
    Screenshots
};
enum class ConfirmKind {
    None,
    Quit
};
enum class ToastKind {
    Info,
    Success,
    Warning,
    Error
};
enum class Action {
    Up,
    Down,
    Left,
    Right,
    Select,
    Back
};

inline constexpr Uint32 splash_duration = 1600;
inline constexpr Uint32 focus_duration = 220;
inline constexpr int picker_page_size = 8;
inline constexpr int screen_width = 640;
inline constexpr int screen_height = 480;

struct PickerItem {
    fs::path path;
    bool dir;
};

// All visible state lives here, so navigation can be exercised without a window.
class ImportWorker;
class WebServer;

struct PendingWebImport {
    fs::path pkg;
    fs::path license;
    fs::path upload_dir;
    std::string display_name;
};

struct State {
    State(Config config, std::string error);
    ~State();
    State(const State &) = delete;
    State &operator=(const State &) = delete;
    Config config;
    std::string config_error;
    std::vector<GameEntry> games;
    int games_revision = 0;
    bool running = true;
    bool picker = false;
    ConfirmKind confirm = ConfirmKind::None;
    ConfirmKind previous_confirm = ConfirmKind::None;
    Screen screen = Screen::Home;
    Screen info_return_screen = Screen::Home;
    HomeFocus home_focus = HomeFocus::RecentGames;
    HomeFocus previous_home_focus = HomeFocus::RecentGames;
    PickerKind pick_kind = PickerKind::Package;
    ImportFocus import_focus = ImportFocus::Local;
    GameInfoFocus info_focus = GameInfoFocus::Play;
    int selected = 0;
    int pick_index = 0;
    int scroll = 0;
    int all_focus = 0;
    int info_game = 0;
    int screenshot_index = 0;
    int previous = 0;
    int direction = 0;
    float motion_from = 0.0f;
    Uint32 motion_start = 0;
    Uint32 focus_start = 0;
    Uint32 confirm_start = 0;
    Uint32 screen_start = SDL_GetTicks() + splash_duration;
    static constexpr Uint32 motion_duration = 260;
    static constexpr Uint32 confirm_duration = 200;
    static constexpr Uint32 entrance_duration = 600;
    static constexpr Uint32 toast_duration = 2600;
    static constexpr Uint32 toast_anim_duration = 180;
    bool moving = false;
    fs::path pkg;
    fs::path license;
    fs::path dir = fs::current_path();
    fs::path stage;
    fs::path upload_cleanup_dir;
    std::vector<PickerItem> items;
    std::vector<ImportHistoryEntry> import_history;
    int import_history_revision = 0;
    uint64_t next_import_id = 1;
    uint64_t active_import_id = 0;
    std::string message;
    std::string toast;
    std::optional<fs::path> launch_eboot;
    ToastKind toast_kind = ToastKind::Info;
    Uint32 toast_start = 0;
    Uint32 toast_hide_start = 0;
    std::unique_ptr<ImportWorker> worker;
    std::unique_ptr<WebServer> web_server;
    mutable std::mutex web_mutex;
    std::optional<PendingWebImport> pending_web_import;
    Uint32 start = SDL_GetTicks();

    int recent_count() const;
    void refresh_games();
    void change_screen(Screen next);
    void open_game_info(int index, Screen return_to);
    void set_home_focus(HomeFocus next);
    void set_confirm(ConfirmKind next);
    void show_toast(std::string text, ToastKind kind = ToastKind::Info);
    void move_game(int step);
    float motion_offset(Uint32 now) const;
    void update_motion();
    void update_toast();
    bool animating() const;
    void scan();
    void browse(PickerKind kind);
    bool import_running() const { return worker != nullptr; }
    bool web_server_running() const;
    std::string web_server_url() const;
    void toggle_web_server();
    void stop_web_server();
    void start_import();
    void poll();
    void action(Action action);

    // Values that can change the non-animated frame. The SDL loop can sleep
    // between events without repeatedly rendering on software-backed devices.
    auto visible_state() const
    {
        return std::make_tuple(games_revision, screen, home_focus, all_focus, import_focus,
                               info_focus, info_game, screenshot_index, selected, picker,
                               confirm, pick_index, scroll, dir, pkg, license, message, toast,
                               toast_kind, toast_hide_start != 0, import_running(),
                               import_history_revision, web_server_running(), web_server_url());
    }

  private:
    void begin_import(const fs::path &pkg_path, const fs::path &lic_path, ImportChannel channel,
                      std::string display_name, fs::path cleanup_dir = {});
    std::string web_status_json() const;
    void handle_picker(Action action);
    void handle_home(Action action);
    void handle_all_games(Action action);
    void handle_import(Action action);
    void handle_game_info(Action action);
};
} // namespace launcher
