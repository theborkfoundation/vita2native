#include <algorithm>
#include <cctype>

#include "state.h"

namespace launcher
{
void State::change_screen(Screen next)
{
    screen = next;
    import_focus = ImportFocus::Package;
    message.clear();
    screen_start = SDL_GetTicks();
}

void State::set_home_focus(HomeFocus next)
{
    if (home_focus != next) {
        previous_home_focus = home_focus;
        home_focus = next;
        focus_start = SDL_GetTicks();
    }
}

void State::move_game(int step)
{
    if (moving) {
        queued = std::clamp(queued + step, -8, 8);
        return;
    }
    previous = selected;
    selected = (selected + step + game_count) % game_count;
    direction = step;
    motion_start = SDL_GetTicks();
    moving = true;
    message.clear();
}

void State::update_motion()
{
    if (moving && SDL_GetTicks() - motion_start >= motion_duration) {
        moving = false;
        if (queued) {
            int step = queued > 0 ? 1 : -1;
            queued -= step;
            move_game(step);
        }
    }
}

bool State::animating() const
{
    Uint32 now = SDL_GetTicks();
    return moving || (now - start >= splash_duration && now - screen_start < entrance_duration) ||
           (focus_start && now - focus_start < focus_duration);
}

void State::scan()
{
    items.clear();
    std::error_code ec;
    if (dir.has_parent_path() && dir.parent_path() != dir)
        items.push_back({ dir.parent_path(), true });
    std::vector<PickerItem> found;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        auto p = it->path();
        std::error_code e;
        bool d = it->is_directory(e);
        bool file = it->is_regular_file(e);
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (d || (file &&
                  (pick_kind == PickerKind::Package ? ext == ".pkg" : p.filename() == "work.bin")))
            found.push_back({ p, d });
        it.increment(ec);
    }
    std::sort(found.begin(), found.end(), [](const PickerItem &a, const PickerItem &b) {
        return a.dir != b.dir ? a.dir > b.dir
                              : a.path.filename().string() < b.path.filename().string();
    });
    items.insert(items.end(), found.begin(), found.end());
    pick_index = scroll = 0;
    if (ec)
        message = "Could not read this directory.";
}

void State::browse(PickerKind kind)
{
    pick_kind = kind;
    picker = true;
    message.clear();
    screen_start = SDL_GetTicks();
    scan();
}

void State::handle_picker(Action action)
{
    if (action == Action::Back) {
        picker = false;
        screen_start = SDL_GetTicks();
        return;
    }
    if (action == Action::Up && !items.empty())
        pick_index = std::max(0, pick_index - 1);
    if (action == Action::Down && !items.empty())
        pick_index = std::min(static_cast<int>(items.size()) - 1, pick_index + 1);
    if (action == Action::Select && !items.empty()) {
        const auto &item = items[pick_index];
        if (item.dir) {
            dir = item.path;
            scan();
        } else {
            (pick_kind == PickerKind::Package ? pkg : license) = item.path;
            picker = false;
            screen_start = SDL_GetTicks();
            message.clear();
        }
    }
    if (pick_index < scroll)
        scroll = pick_index;
    if (pick_index >= scroll + picker_page_size)
        scroll = pick_index - picker_page_size + 1;
}

void State::handle_home(Action action)
{
    if (action == Action::Up && home_focus != HomeFocus::RecentGames)
        set_home_focus(HomeFocus::RecentGames);
    else if (action == Action::Down && home_focus == HomeFocus::RecentGames)
        set_home_focus(HomeFocus::AllGames);
    else if (home_focus == HomeFocus::RecentGames &&
             (action == Action::Left || action == Action::Right))
        move_game(action == Action::Left ? -1 : 1);
    else if (home_focus != HomeFocus::RecentGames && action == Action::Left)
        set_home_focus(HomeFocus::AllGames);
    else if (home_focus != HomeFocus::RecentGames && action == Action::Right)
        set_home_focus(HomeFocus::Import);
    else if (action == Action::Select) {
        if (home_focus == HomeFocus::AllGames) {
            moving = false;
            queued = 0;
            all_focus = selected;
            change_screen(Screen::AllGames);
        } else if (home_focus == HomeFocus::Import) {
            moving = false;
            queued = 0;
            change_screen(Screen::Import);
        } else {
            message = "Games are sample entries for now.";
        }
    }
}

void State::handle_all_games(Action action)
{
    constexpr int columns = 2;
    if (action == Action::Left && all_focus % columns == 1)
        --all_focus;
    else if (action == Action::Right && all_focus % columns == 0)
        ++all_focus;
    else if (action == Action::Up && all_focus >= columns)
        all_focus -= columns;
    else if (action == Action::Down && all_focus < columns)
        all_focus += columns;
    else if (action == Action::Select) {
        selected = all_focus;
        message = "Games are sample entries for now.";
    }
}

void State::handle_import(Action action)
{
    if (action == Action::Up) {
        if (import_focus == ImportFocus::Start)
            import_focus = ImportFocus::License;
        else if (import_focus == ImportFocus::License)
            import_focus = ImportFocus::Package;
    } else if (action == Action::Down) {
        if (import_focus == ImportFocus::Package)
            import_focus = ImportFocus::License;
        else if (import_focus == ImportFocus::License)
            import_focus = ImportFocus::Start;
    } else if (action == Action::Select) {
        if (import_focus == ImportFocus::Package)
            browse(PickerKind::Package);
        else if (import_focus == ImportFocus::License)
            browse(PickerKind::License);
        else
            start_import();
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
            message = "Import is running; please wait.";
        return;
    }
    if (picker) {
        handle_picker(action);
        return;
    }
    if (action == Action::Back) {
        if (screen == Screen::Home) {
            running = false;
        } else {
            Screen from = screen;
            change_screen(Screen::Home);
            home_focus = from == Screen::AllGames ? HomeFocus::AllGames : HomeFocus::Import;
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
    case Screen::Import:
        handle_import(action);
        break;
    }
}
} // namespace launcher
