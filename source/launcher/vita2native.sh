#!/bin/bash

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -d "/opt/system/Tools/PortMaster/" ]; then
    controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
    controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
    controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/storage/roms/ports/PortMaster/" ]; then
    controlfolder="/storage/roms/ports/PortMaster"
elif [ -d "$SCRIPT_DIR/PortMaster/" ]; then
    controlfolder="$SCRIPT_DIR/PortMaster"
else
    controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/vita2native"

cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

$GPTOKEYB "launcher" &
pm_platform_helper "$GAMEDIR/launcher" >/dev/null 2>&1 || true
pm_platform_helper "$GAMEDIR/vita2native" >/dev/null 2>&1 || true

./launcher

pm_finish
