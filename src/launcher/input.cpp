#include <optional>

#include "input.h"
#include "state.h"

namespace launcher
{
namespace
{
std::optional<Action> key_action(SDL_Keycode key)
{
    switch (key) {
    case SDLK_UP:
        return Action::Up;
    case SDLK_DOWN:
        return Action::Down;
    case SDLK_LEFT:
        return Action::Left;
    case SDLK_RIGHT:
        return Action::Right;
    case SDLK_RETURN:
    case SDLK_SPACE:
    case SDLK_KP_ENTER:
        return Action::Select;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        return Action::Back;
    default:
        return std::nullopt;
    }
}
std::optional<Action> controller_action(Uint8 button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return Action::Up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return Action::Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return Action::Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return Action::Right;
    case SDL_CONTROLLER_BUTTON_A:
        return Action::Select;
    case SDL_CONTROLLER_BUTTON_B:
        return Action::Back;
    default:
        return std::nullopt;
    }
}
} // namespace

Input::Input()
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        open_pad(i);
}
Input::~Input()
{
    if (controller_)
        SDL_GameControllerClose(controller_);
    if (joystick_)
        SDL_JoystickClose(joystick_);
}
void Input::open_pad(int index)
{
    if (controller_ || joystick_)
        return;
    if (SDL_IsGameController(index))
        controller_ = SDL_GameControllerOpen(index);
    else
        joystick_ = SDL_JoystickOpen(index);
}
void Input::handle_axis(int axis, int value, State &state)
{
    int &previous = axis == 0 ? axis_x_ : axis_y_;
    int next = value > 16000 ? 1 : value < -16000 ? -1 : 0;
    if (next && next != previous)
        state.action(axis == 0 ? (next < 0 ? Action::Left : Action::Right)
                               : (next < 0 ? Action::Up : Action::Down));
    previous = next;
}
void Input::handle(const SDL_Event &event, State &state)
{
    switch (event.type) {
    case SDL_CONTROLLERDEVICEADDED:
        open_pad(event.cdevice.which);
        break;
    case SDL_JOYDEVICEADDED:
        open_pad(event.jdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (controller_ && !SDL_GameControllerGetAttached(controller_)) {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
        break;
    case SDL_JOYDEVICEREMOVED:
        if (joystick_ && !SDL_JoystickGetAttached(joystick_)) {
            SDL_JoystickClose(joystick_);
            joystick_ = nullptr;
        }
        break;
    case SDL_KEYDOWN:
        if (auto action = key_action(event.key.keysym.sym))
            state.action(*action);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        if (auto action = controller_action(event.cbutton.button))
            state.action(*action);
        break;
    case SDL_JOYBUTTONDOWN:
        if (joystick_ && event.jbutton.button == 0)
            state.action(Action::Select);
        else if (joystick_ && event.jbutton.button == 1)
            state.action(Action::Back);
        break;
    case SDL_JOYHATMOTION:
        if (joystick_) {
            const Uint8 hat = event.jhat.value;
            if ((hat & SDL_HAT_UP) && !(hat_ & SDL_HAT_UP))
                state.action(Action::Up);
            if ((hat & SDL_HAT_DOWN) && !(hat_ & SDL_HAT_DOWN))
                state.action(Action::Down);
            if ((hat & SDL_HAT_LEFT) && !(hat_ & SDL_HAT_LEFT))
                state.action(Action::Left);
            if ((hat & SDL_HAT_RIGHT) && !(hat_ & SDL_HAT_RIGHT))
                state.action(Action::Right);
            hat_ = hat;
        }
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (event.caxis.axis <= 1)
            handle_axis(event.caxis.axis, event.caxis.value, state);
        break;
    case SDL_JOYAXISMOTION:
        if (joystick_ && event.jaxis.axis <= 1)
            handle_axis(event.jaxis.axis, event.jaxis.value, state);
        break;
    default:
        break;
    }
}
} // namespace launcher
