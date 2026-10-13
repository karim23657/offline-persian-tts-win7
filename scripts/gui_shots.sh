#!/usr/bin/env bash
# Capture GUI screenshots under Xvfb + Wine: one per tab (Persian default),
# plus an English settings + main view after driving the language combo.
#
#   scripts/gui_shots.sh
#
# Needs: Xvfb, xdotool, imagemagick (import).  Writes screenshots/gui-*.png.
#
# Notes on the mechanics (worked out the hard way):
#  - The window title is Persian ("وین7-tts ..."), so search by the Latin
#    substring "tts" that appears in it, not "win7-tts".
#  - Tab switching uses the Ctrl+Tab accelerator.  Sending it with
#    `xdotool key --window` (XSendEvent) is ignored by Wine; the focus-based
#    `xdotool key` (XTEST) goes through the normal input path and works.
set -uo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
shots="$here/screenshots"
mkdir -p "$shots"
export WINEPREFIX="${WINEPREFIX:-/tmp/wp7}"
export WINEDEBUG=-all
display="${DISPLAY_NUM:-:99}"
export DISPLAY="$display"

if ! pgrep -f "Xvfb $display" >/dev/null 2>&1; then
  Xvfb "$display" -screen 0 1280x900x24 >/dev/null 2>&1 &
  xvfb_pid=$!
  sleep 2
fi

wine "$here/runtime/tts_gui.exe" >/dev/null 2>&1 &
gui_pid=$!
sleep 9   # window appears + model warms in the background

win="$(xdotool search --name 'tts' 2>/dev/null | tail -1 || true)"
if [[ -z "$win" ]]; then
  echo "GUI window not found"; kill "$gui_pid" 2>/dev/null; exit 1
fi
xdotool windowactivate --sync "$win" 2>/dev/null || true
xdotool windowfocus --sync "$win" 2>/dev/null || true
eval "$(xdotool getwindowgeometry --shell "$win")"

shot() { import -window "$win" +repage "$shots/$1"; }

# Persian tabs: main -> dictionary -> settings -> download
shot gui-1-main-fa.png
xdotool key ctrl+Tab; sleep 1.4; shot gui-2-dictionary.png
xdotool key ctrl+Tab; sleep 1.4; shot gui-3-settings.png
xdotool key ctrl+Tab; sleep 1.4; shot gui-4-download.png

# Back to settings, switch the language combo to English, then show main+settings.
xdotool key ctrl+Tab; sleep 0.6   # download -> main
xdotool key ctrl+Tab; sleep 0.6   # main -> dictionary
xdotool key ctrl+Tab; sleep 1.0   # dictionary -> settings
cx=$((X + 195)); cy=$((Y + 72))   # language combo (client ~195,72)
xdotool mousemove "$cx" "$cy"; sleep 0.3; xdotool click 1; sleep 0.6
xdotool key Down; sleep 0.4; xdotool key Return; sleep 1.0
shot gui-3-settings-en.png
xdotool key ctrl+Tab; sleep 0.6   # settings -> download
xdotool key ctrl+Tab; sleep 1.0   # download -> main
shot gui-5-main-en.png

kill "$gui_pid" 2>/dev/null || true
[[ -n "${xvfb_pid:-}" ]] && kill "$xvfb_pid" 2>/dev/null || true
echo "GUI screenshots written:"; ls -1 "$shots"/gui-*.png 2>/dev/null
