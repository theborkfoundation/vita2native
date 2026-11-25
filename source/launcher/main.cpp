#include <SDL.h>

#include "input.h"
#include "state.h"
#include "ui.h"
#ifdef __APPLE__
#include "macos_activate.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using namespace launcher;

// Snapshot mode makes it possible to inspect each screen without input devices.
void configure_snapshot(State &state)
{
    state.start = SDL_GetTicks() - 2000;
    const char *screen = getenv("V2N_LAUNCHER_SCREEN");
    if (screen && strcmp(screen, "import") == 0)
        state.screen = Screen::Import;
    if (screen && strcmp(screen, "all") == 0)
        state.screen = Screen::AllGames;
    if (screen && strcmp(screen, "game-info") == 0 && !state.games.empty())
        state.open_game_info(0, Screen::Home);
    if (screen && strcmp(screen, "home-focus") == 0)
        state.home_focus = state.previous_home_focus = HomeFocus::AllGames;
    if (screen && strcmp(screen, "picker") == 0) {
        state.screen = Screen::Import;
        state.browse(PickerKind::Package);
    }
    state.screen_start = SDL_GetTicks() - 1000;
    if (screen && strcmp(screen, "motion-right") == 0) {
        state.move_game(1);
        state.motion_start = SDL_GetTicks() - 100;
    }
    if (screen && strcmp(screen, "motion-left") == 0) {
        state.move_game(-1);
        state.motion_start = SDL_GetTicks() - 100;
    }
    if (screen && strcmp(screen, "confirm-quit") == 0) {
        state.set_confirm(ConfirmKind::Quit);
        state.confirm_start = SDL_GetTicks() - 1000;
    }
}

void save_snapshot(const char *path, const uint8_t *xrgb_pixels, int width, int height)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Could not save screenshot\n");
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t *px = &xrgb_pixels[(static_cast<size_t>(y) * width + x) * 4];
            const uint8_t rgb_px[3] = { px[2], px[1], px[0] };
            fwrite(rgb_px, 1, 3, file);
        }
    }
    fclose(file);
}

void present(SDL_Renderer *renderer, SDL_Texture *texture, bool upload)
{
    if (upload)
        SDL_UpdateTexture(texture, nullptr, ui::framebuffer(), ui::render_width() * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

void run(SDL_Renderer *renderer, SDL_Texture *texture, const ConfigResult &settings,
         const std::string &warning)
{
    State state(settings.config, settings.error);
    state.message = warning;
    const char *capture = getenv("V2N_LAUNCHER_SCREENSHOT");
    if (capture)
        configure_snapshot(state);
    Input input;
    bool dirty = true;
    bool need_present = true;
    bool was_splash = true;
    bool was_animating = false;
    while (state.running) {
        const auto before = state.visible_state();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 event.window.event == SDL_WINDOWEVENT_SHOWN))
                need_present = true;
            if (event.type == SDL_QUIT && !state.import_running())
                state.running = false;
            input.handle(event, state);
        }
        state.poll();
        state.update_motion();
        if (before != state.visible_state())
            dirty = true;
        const bool splash = SDL_GetTicks() - state.start < splash_duration;
        const bool animating = state.animating();
        if (splash || was_splash != splash || animating || was_animating)
            dirty = true;
        was_splash = splash;
        was_animating = animating;

        if (dirty) {
            dirty = false;
            ui::sync(state);
            const bool flushed = ui::render(capture != nullptr);
            if (capture) {
                save_snapshot(capture, ui::framebuffer(), ui::render_width(), ui::render_height());
                state.running = false;
            }
            if (flushed)
                present(renderer, texture, true);
            else if (need_present)
                present(renderer, texture, false);
            need_present = false;
        } else if (need_present) {
            need_present = false;
            present(renderer, texture, false);
        }
        SDL_Delay(16);
    }
}
} // namespace

int main(int, char **)
{
    const launcher::ConfigResult settings = launcher::load_config();
    if (!settings.error.empty())
        fprintf(stderr, "%s\n", settings.error.c_str());
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) <
        0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        "V2N Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screen_width, screen_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_RaiseWindow(window);
#ifdef __APPLE__
    launcher::activate_macos_app();
#endif
    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    int out_w = screen_width;
    int out_h = screen_height;
    if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 || out_w <= 0 || out_h <= 0) {
        out_w = screen_width;
        out_h = screen_height;
    }
    int result = 0;
    std::string logo_warning;
    if (!launcher::ui::initialize(settings.error.empty() ? settings.config.logo_path :
                                                          std::filesystem::path{},
                                  out_w, out_h, logo_warning)) {
        fprintf(stderr, "Failed to initialize LVGL or font\n");
        result = 1;
    } else {
        SDL_RenderSetLogicalSize(renderer, launcher::ui::render_width(),
                                 launcher::ui::render_height());
        SDL_Texture *texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              launcher::ui::render_width(), launcher::ui::render_height());
        if (!texture) {
            fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
            result = 1;
        } else {
            run(renderer, texture, settings, logo_warning);
            SDL_DestroyTexture(texture);
        }
        launcher::ui::shutdown();
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
