#pragma once

namespace launcher
{
struct State;
namespace ui
{
bool initialize();
void shutdown();
void begin_frame(int drawable_width);
void draw(const State &state);
void end_frame();
} // namespace ui
} // namespace launcher
