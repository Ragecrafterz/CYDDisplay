# CYD Now Playing

A Spotify "now playing" screen with a forecast page and a bongo cat, for the
**ESP32-2432S028** ("Cheap Yellow Display", the 2.8" 320×240 touchscreen board).

## Features

- **Now playing**: album art, title (scrolls when long), artist, album, progress and
  play / pause / skip buttons. The page takes on a colour picked from the album art.
- **Forecast**: swipe sideways for current weather and the next five days
  ([Open-Meteo](https://open-meteo.com), no API key).
- **Clock bar** across the top 15% of the screen, with the current temperature.
- **Bongo cat** in a chat-head bubble: slaps the desk while you type, bops to music,
  blinks when idle, naps after two minutes. Drag it anywhere; let go and it flies to the
  nearest edge or corner. Tap it to startle it.
- **Three cat shapes** (chibi, chubby, original) in **four colours** (classic white, orange
  tabby, midnight black, calico).
- **Idle screen**: when nothing is playing, the music page turns into a big bongo cat with a
  **Play Spotify** button. Next to it: the laptop's CPU, RAM and typing speed, or on its own,
  WiFi signal and today's weather. It switches back as soon as music starts.
- **Themes**: Midnight, OLED black, Synthwave, Ocean, Forest, Sunset and Paper (light), or
  pick every colour yourself. The accent can follow the album art or stay fixed.
- **On-screen settings**: tap the gear in the clock bar for brightness, cat colour and shape,
  12/24-hour clock and screen rotation. Changes sync back to the companion app.
- **Landscape or portrait**, switchable from the board or the companion app.
- **Two ways to run**, switching automatically:
  - **USB**: a small Windows tray app sends what's playing (from the Spotify desktop app via
    Windows' media controls), album art, time, forecast, laptop stats and typing activity.
    No Spotify developer account needed.
  - **Standalone over WiFi**: the board fetches the forecast and time itself, and Spotify via
    the Web API once you link it. It takes over whenever the laptop isn't connected.

## Hardware

ESP32-2432S028R, the **two-USB** revision (micro-USB + USB-C). The one-USB original should
work with `-DILI9341_DRIVER` in place of `-DILI9341_2_DRIVER` in `firmware/platformio.ini`.
If colours look like a photo negative or taps land in the wrong place, fix it from the
companion app's **Display** section, no reflash needed.

## Setup (Windows)

1. Install [Python 3.10+](https://www.python.org/downloads/) with **Add python.exe to PATH** ticked.
2. Run `setup.bat`. It creates `.venv` and installs everything, PlatformIO included.
3. Plug in the board and run `flash.bat`. The first build downloads the ESP32 toolchain, which
   takes a few minutes.
4. Run `Start CYD Companion.bat`. The app lives in the system tray (the cat icon). Closing the
   window keeps it running, and **Start with Windows** is a checkbox in the app.
5. In the app, search for your city and pick it.

### Just flashing a release

To flash without building, download `cyd-now-playing.bin` from the
[releases](../../releases) and write it at offset `0x0`:

```bash
python -m esptool --chip esp32 write-flash 0x0 cyd-now-playing.bin
```

You still need the companion app (steps 1, 2, 4 and 5) for USB mode and for setting up WiFi.

### Standalone mode

In the companion app, enter your WiFi (the board only does 2.4 GHz) and send it to the board.
For Spotify without the laptop:

1. Create an app at [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard).
2. Add the redirect URI `http://127.0.0.1:8888/callback` and tick **Web API**.
3. Paste the Client ID into the companion app and press **Connect Spotify**.

Showing what's playing works with any Spotify account. Play, pause, skip and **Play Spotify**
from the board need Spotify Premium. **Play Spotify** starts on your Spotify desktop app if
it's open, then a phone, and only falls back to a browser web player when nothing else is
running.

## Using it

| Do this | to |
| --- | --- |
| Swipe left / right | switch between music and forecast |
| Tap the gear (top right) | open settings; tap it again or **Done** to go back |
| Drag the cat | move it; it snaps to the nearest edge or corner |
| Tap the cat | startle it |
| Tap **Play Spotify** (idle screen) | start music on your laptop or another Spotify device |

## Privacy and security

- Typing activity is a **count** of key presses, used only to animate the cat and estimate
  words per minute. Which keys were pressed is never read, stored or sent.
- The board stores your WiFi password and Spotify refresh token in its own flash (NVS). The
  companion app keeps neither: its settings file (`%APPDATA%\cyd-companion\config.json`)
  only holds the city, display options, theme and the Spotify Client ID.
- In standalone mode the board talks to Spotify over HTTPS **without checking Spotify's
  certificate**, a common shortcut on small boards. Someone on the same network could in
  principle intercept the board's Spotify session, so don't link Spotify on networks you
  don't trust.

## How it fits together

```
companion/cyd_companion.py   Windows tray app: media session, album art, typing count,
                             laptop stats, forecast, settings, Spotify login
firmware/src/main.cpp        setup/loop, USB protocol, touch, settings storage, backlight
firmware/src/ui.cpp          clock bar, music / idle / weather / settings pages, both orientations
firmware/src/cat.cpp         the cat: shapes, colours, moods, bubble dragging, big idle cat
firmware/src/net.cpp         standalone mode: WiFi, NTP, Open-Meteo, Spotify Web API
```

USB protocol, 460800 baud, one JSON object per line:

- Laptop → board: `now` (track, position, playing), `art` (followed by that many raw JPEG
  bytes, sized to the board's art box), `time`, `wx`, `keys`, `sys` (CPU, RAM, words per
  minute), `cfg` (settings) and `status` (asks the board for its status).
- Board → laptop: `status` (settings, WiFi, art box size; also sent on boot and whenever a
  setting changes on the board) and `cmd` (`resume` / `pause` / `next` / `prev` / `play`)
  when its buttons are tapped.
- Lines starting with `# ` are the board's debug log; the companion ignores them.

## Building a release

`flash.bat` builds and uploads in one go. `make-release.bat` writes
`dist\cyd-now-playing.bin`, a single file flashable at `0x0`. Attach it to a GitHub release
rather than committing it.

Build output goes to `%USERPROFILE%\.cyd-build`, because deep library paths inside the
project folder can exceed Windows' 260-character path limit.

## Credits

- Libraries: [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder),
  [ArduinoJson](https://arduinojson.org), [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen),
  [pystray](https://github.com/moses-palmer/pystray), [pynput](https://github.com/moses-palmer/pynput),
  [PyWinRT](https://github.com/pywinrt/pywinrt), [psutil](https://github.com/giampaolo/psutil).
- Weather data by [Open-Meteo](https://open-meteo.com) (CC BY 4.0).
- The cat is an original drawing inspired by the Bongo Cat meme.
