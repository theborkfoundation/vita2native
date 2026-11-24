#pragma once

#include <SDL.h>
#include <filesystem>
#include <string>
#include <memory>
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
    AllGames
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
    Package,
    License,
    Start
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

struct State {
    State(Config config, std::string error);
    ~State();
    State(const State &) = delete;
    State &operator=(const State &) = delete;
    Config config;
    std::string config_error;
    bool running = true;
    bool picker = false;
    Screen screen = Screen::Home;
    HomeFocus home_focus = HomeFocus::RecentGames;
    HomeFocus previous_home_focus = HomeFocus::RecentGames;
    PickerKind pick_kind = PickerKind::Package;
    ImportFocus import_focus = ImportFocus::Package;
    int selected = 0;
    int pick_index = 0;
    int scroll = 0;
    int all_focus = 0;
    int previous = 0;
    int direction = 0;
    int queued = 0;
    Uint32 motion_start = 0;
    Uint32 focus_start = 0;
    Uint32 screen_start = SDL_GetTicks() + splash_duration;
    static constexpr Uint32 motion_duration = 300;
    static constexpr Uint32 entrance_duration = 600;
    bool moving = false;
    fs::path pkg;
    fs::path license;
    fs::path dir = fs::current_path();
    fs::path stage;
    std::vector<PickerItem> items;
    std::string message;
    std::unique_ptr<ImportWorker> worker;
    Uint32 start = SDL_GetTicks();

    void change_screen(Screen next);
    void set_home_focus(HomeFocus next);
    void move_game(int step);
    void update_motion();
    bool animating() const;
    void scan();
    void browse(PickerKind kind);
    bool import_running() const { return worker != nullptr; }
    void start_import();
    void poll();
    void action(Action action);

    // Values that can change the non-animated frame. The SDL loop can sleep
    // between events without repeatedly rendering on software-backed devices.
    auto visible_state() const
    {
        return std::make_tuple(screen, home_focus, all_focus, import_focus, selected, picker,
                               pick_index, scroll, dir, pkg, license, message, import_running());
    }

  private:
    void handle_picker(Action action);
    void handle_home(Action action);
    void handle_all_games(Action action);
    void handle_import(Action action);
};
} // namespace launcher
