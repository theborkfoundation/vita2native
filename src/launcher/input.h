#pragma once

#include <SDL.h>

namespace launcher
{
struct State;

// SDL owns the event queue; this adapter translates keyboard and pad events
// into the same navigation actions. It also owns the active controller handle.
class Input
{
  public:
    Input();
    ~Input();
    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;
    void handle(const SDL_Event &event, State &state);

  private:
    void open_pad(int index);
    void handle_axis(int axis, int value, State &state);
    SDL_GameController *controller_ = nullptr;
    SDL_Joystick *joystick_ = nullptr;
    int axis_x_ = 0;
    int axis_y_ = 0;
    Uint8 hat_ = SDL_HAT_CENTERED;
};
} // namespace launcher
