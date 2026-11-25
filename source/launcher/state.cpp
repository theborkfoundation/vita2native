#include <algorithm>
#include <cctype>
#include <unistd.h>

#include "state.h"

namespace launcher
{
int State::recent_count() const
{
    return std::min<int>(static_cast<int>(games.size()), recent_game_limit);
}

void State::refresh_games()
{
    games = scan_games(config.games_path);
    ++games_revision;
    const int n = static_cast<int>(games.size());
    const int rc = recent_count();
    if (rc == 0) {
        selected = 0;
        previous = 0;
        moving = false;
        motion_from = 0.0f;
        if (home_focus == HomeFocus::RecentGames) {
            home_focus = HomeFocus::WebUI;
            previous_home_focus = HomeFocus::WebUI;
        }
    } else {
        selected = std::clamp(selected, 0, rc - 1);
        previous = std::clamp(previous, 0, rc - 1);
    }
    if (n == 0) {
        all_focus = 0;
        info_game = 0;
    } else {
        all_focus = std::clamp(all_focus, 0, n - 1);
        info_game = std::clamp(info_game, 0, n - 1);
    }
}

void State::change_screen(Screen next)
{
    if (next != Screen::WebUI)
        stop_web_server();
    screen = next;
    message.clear();
    screen_start = SDL_GetTicks();
}

void State::open_game_info(int index, Screen return_to)
{
    if (index < 0 || index >= static_cast<int>(games.size()))
        return;
    moving = false;
    motion_from = 0.0f;
    info_game = index;
    info_return_screen = return_to;
    info_focus = GameInfoFocus::Play;
    screenshot_index = 0;
    change_screen(Screen::GameInfo);
}

void State::set_home_focus(HomeFocus next)
{
    if (home_focus != next) {
        previous_home_focus = home_focus;
        home_focus = next;
        focus_start = SDL_GetTicks();
    }
}

void State::set_confirm(ConfirmKind next)
{
    if (confirm != next) {
        previous_confirm = confirm;
        confirm = next;
        confirm_start = SDL_GetTicks();
    }
}

void State::show_toast(std::string text, ToastKind kind)
{
    toast = std::move(text);
    toast_kind = kind;
    toast_start = SDL_GetTicks();
    toast_hide_start = 0;
}

float State::motion_offset(Uint32 now) const
{
    if (!moving)
        return 0.0f;
    float t = std::clamp(
        static_cast<float>(now - motion_start) / static_cast<float>(motion_duration), 0.0f, 1.0f);
    const float inv = 1.0f - t;
    const float ease = 1.0f - inv * inv * inv;
    return motion_from * (1.0f - ease);
}

void State::move_game(int step)
{
    const int rc = recent_count();
    if (rc <= 1)
        return;
    const Uint32 now = SDL_GetTicks();
    const float current = motion_offset(now);
    previous = selected;
    selected = (selected + step + rc) % rc;
    direction = step;
    if (rc == 2 && current * step >= 0.0f)
        motion_from = static_cast<float>(step);
    else
        motion_from = std::clamp(current + static_cast<float>(step), -3.5f, 3.5f);
    motion_start = now;
    moving = true;
}

void State::update_motion()
{
    if (moving && SDL_GetTicks() - motion_start >= motion_duration) {
        moving = false;
        motion_from = 0.0f;
    }
}

void State::update_toast()
{
    if (toast.empty())
        return;
    const Uint32 now = SDL_GetTicks();
    if (toast_hide_start == 0) {
        if (now - toast_start >= toast_duration)
            toast_hide_start = now;
    } else if (now - toast_hide_start >= toast_anim_duration) {
        toast.clear();
        toast_start = 0;
        toast_hide_start = 0;
    }
}

bool State::animating() const
{
    Uint32 now = SDL_GetTicks();
    const bool toast_anim =
        !toast.empty() && ((now - toast_start < toast_anim_duration) ||
                           (toast_hide_start != 0 && now - toast_hide_start < toast_anim_duration));
    return moving || toast_anim ||
           (now - start >= splash_duration && now - screen_start < entrance_duration) ||
           (focus_start && now - focus_start < focus_duration) ||
           (confirm_start && now - confirm_start < confirm_duration);
}

void State::handle_home(Action action)
{
    const int rc = recent_count();
    if (action == Action::Up && home_focus != HomeFocus::RecentGames && rc > 0)
        set_home_focus(HomeFocus::RecentGames);
    else if (action == Action::Down && home_focus == HomeFocus::RecentGames)
        set_home_focus(HomeFocus::AllGames);
    else if (home_focus == HomeFocus::RecentGames &&
             (action == Action::Left || action == Action::Right))
        move_game(action == Action::Left ? -1 : 1);
    else if (home_focus != HomeFocus::RecentGames && action == Action::Left)
        set_home_focus(HomeFocus::AllGames);
    else if (home_focus != HomeFocus::RecentGames && action == Action::Right)
        set_home_focus(HomeFocus::WebUI);
    else if (action == Action::Select) {
        if (home_focus == HomeFocus::AllGames) {
            moving = false;
            motion_from = 0.0f;
            all_focus = games.empty() ? 0 : std::clamp(selected, 0, static_cast<int>(games.size()) - 1);
            change_screen(Screen::AllGames);
        } else if (home_focus == HomeFocus::WebUI) {
            moving = false;
            motion_from = 0.0f;
            change_screen(Screen::WebUI);
        } else if (rc > 0) {
            open_game_info(selected, Screen::Home);
        }
    }
}

void State::handle_all_games(Action action)
{
    const int count = static_cast<int>(games.size());
    if (count == 0)
        return;
    constexpr int columns = 2;
    if (action == Action::Left && all_focus % columns == 1)
        --all_focus;
    else if (action == Action::Right && all_focus % columns == 0 && all_focus + 1 < count)
        ++all_focus;
    else if (action == Action::Up && all_focus >= columns)
        all_focus -= columns;
    else if (action == Action::Down) {
        if (all_focus + columns < count)
            all_focus += columns;
        else if (all_focus / columns < (count - 1) / columns)
            all_focus = count - 1;
    } else if (action == Action::Select) {
        if (all_focus < recent_count())
            selected = all_focus;
        open_game_info(all_focus, Screen::AllGames);
    }
}

void State::handle_web_ui(Action action)
{
    if (action == Action::Select)
        toggle_web_server();
}

void State::handle_game_info(Action action)
{
    if (info_game < 0 || info_game >= static_cast<int>(games.size()))
        return;
    const auto &game = games[info_game];
    const int shot_count = static_cast<int>(game.screenshots.size());

    if (action == Action::Up) {
        info_focus = GameInfoFocus::Play;
    } else if (action == Action::Down && shot_count > 0) {
        info_focus = GameInfoFocus::Screenshots;
    } else if (action == Action::Left && info_focus == GameInfoFocus::Screenshots && shot_count > 0) {
        screenshot_index = std::max(0, screenshot_index - 1);
    } else if (action == Action::Right && info_focus == GameInfoFocus::Screenshots &&
               shot_count > 0) {
        screenshot_index = std::min(shot_count - 1, screenshot_index + 1);
    } else if (action == Action::Select && info_focus == GameInfoFocus::Play) {
#ifdef __APPLE__
        show_toast("Launching is not supported on macOS.", ToastKind::Info);
#else
        std::error_code ec;
        if (!fs::is_regular_file(config.loader_path, ec) ||
            access(config.loader_path.c_str(), X_OK) != 0) {
            show_toast("Loader not found or not executable.", ToastKind::Error);
            return;
        }
        if (!fs::is_regular_file(game.eboot, ec)) {
            show_toast("Could not find eboot.bin.", ToastKind::Error);
            return;
        }
        launch_eboot = game.eboot;
        running = false;
#endif
    }
}

void State::action(Action action)
{
    if (!config_error.empty()) {
        if (action == Action::Back || action == Action::Select)
            running = false;
        return;
    }
    if (import_running()) {
        if (action == Action::Back)
            show_toast("Import is running; please wait.", ToastKind::Warning);
        return;
    }
    if (confirm != ConfirmKind::None) {
        if (action == Action::Select) {
            if (confirm == ConfirmKind::Quit)
                running = false;
            set_confirm(ConfirmKind::None);
        } else if (action == Action::Back) {
            set_confirm(ConfirmKind::None);
        }
        return;
    }
    if (action == Action::Back) {
        if (screen == Screen::Home) {
            set_confirm(ConfirmKind::Quit);
        } else if (screen == Screen::GameInfo) {
            change_screen(info_return_screen);
            if (info_return_screen == Screen::Home) {
                home_focus = recent_count() > 0 ? HomeFocus::RecentGames : HomeFocus::AllGames;
                previous_home_focus = home_focus;
                focus_start = 0;
            }
        } else {
            Screen from = screen;
            change_screen(Screen::Home);
            home_focus = from == Screen::AllGames ? HomeFocus::AllGames : HomeFocus::WebUI;
            previous_home_focus = home_focus;
            focus_start = 0;
        }
        return;
    }
    switch (screen) {
    case Screen::Home:
        handle_home(action);
        break;
    case Screen::AllGames:
        handle_all_games(action);
        break;
    case Screen::WebUI:
        handle_web_ui(action);
        break;
    case Screen::GameInfo:
        handle_game_info(action);
        break;
    }
}
} // namespace launcher
