# samp_time_weather

Change the **time of day and the weather in SA-MP** yourself, from a small in-game panel. A single `.asi` plugin.

It only changes what *you* see (client side). The server keeps its own time and weather for everyone else.

## Install

1. Download `TimeWeather.asi` from the [latest release](https://github.com/yessirks/samp_time_weather/releases/latest) (and `TimeWeather.ini` if you want to change the keys).
2. Put them in your GTA San Andreas folder (next to `gta_sa.exe`). You need an ASI loader (it comes with CLEO, SilentPatch, Ultimate ASI Loader ...); if other `.asi` mods already work for you, you have one.
3. Start SA-MP and press **F9**.

To remove it, delete the file.

## Using it

Press **F9** to open the panel, **Up/Down** to choose a row, **Left/Right** to change it, **F9** again to close.

- **Quick start: Single-player style** - the clock runs like in single-player (a game day in 24 minutes) and the weather changes by itself, the way it does in single-player. Press Enter.
- **Time** - *Server* (leave it to the server), *Fixed* (frozen at the hour you pick) or *Running* (the clock runs, at the speed you pick).
- **Hour / Minute / Preset** - set the time, or jump to Dawn, Sunrise, Noon, Sunset, Night, Midnight.
- **Weather** - *Server*, *Custom* (pick one of the 23 weathers) or *Auto* (the game changes it by itself every game hour).
- **Backspace** gives everything back to the server.

While the panel is open the game ignores your keyboard and mouse, so you won't drive off.
Your last choice is remembered for next time.

## Keys

All keys can be changed in `TimeWeather.ini` (key names like `F9`, `Insert`, `Numpad4`, `Ctrl+F9` ...). You can also give keys to optional quick actions that work without opening the panel: hour forward / back, next / previous weather, single-player style, everything back to the server. They are off until you set them.

## How it works

SA-MP sets the time and weather over the network and sends them again now and then. This plugin writes your choice into the game every frame, so it sticks, and it gives the server's values back when you ask. The "Auto" weather just stops the server from forcing one, so the game's own weather cycle runs, as in single-player.

## Good to know

- Tested with SA-MP 0.3.7-R5 and the GTA:SA 1.0 US exe. Other versions are not tested; on an exe it does not recognise it does nothing.
- Keyboard only (no mouse in the panel).
- If the server sets the time or weather while you have your own, yours stays; "back to the server" then gives you the server's latest value.

## Build

Needs [Zig](https://ziglang.org/) (`winget install -e --id zig.zig`). Run `build.bat`: it builds `build\TimeWeather.asi` (32-bit) and copies it into the game folder next to this one if it finds `gta_sa.exe` there.

## License

MIT, see [LICENSE](LICENSE).
