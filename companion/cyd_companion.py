"""Companion app for the ESP32-2432S028 ("Cheap Yellow Display") now-playing screen.

Over USB it feeds the board what's playing (via Windows' media controls, Spotify preferred),
album art, the time, the forecast and typing activity for the bongo cat, and passes the
board's play/pause/skip taps back to the player. It is also where the board's standalone
WiFi mode is set up: WiFi network, forecast city and Spotify login.

Typing activity is only a count of key presses; which keys were pressed is never looked at.
"""
import asyncio
import base64
import collections
import datetime as dt
import hashlib
import http.server
import io
import json
import os
import queue
import secrets
import sys
import threading
import time
import tkinter as tk
import unicodedata
import urllib.parse
import webbrowser
from tkinter import colorchooser, messagebox, ttk

import pystray
import psutil
import requests
import serial
import serial.tools.list_ports
from PIL import Image, ImageDraw
from pynput import keyboard
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)
from winrt.windows.storage.streams import Buffer, DataReader, InputStreamOptions

BAUD = 460800
DEFAULT_ART_SIZE = 136  # the board reports its real art box size in its status message
CH340_VID = 0x1A86
CONFIG_PATH = os.path.join(os.environ.get("APPDATA", "."), "cyd-companion", "config.json")
FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
GEOCODE_URL = "https://geocoding-api.open-meteo.com/v1/search"
SPOTIFY_REDIRECT = "http://127.0.0.1:8888/callback"
SPOTIFY_SCOPES = "user-read-currently-playing user-read-playback-state user-modify-playback-state"
STARTUP_FILE = os.path.join(os.environ.get("APPDATA", "."), r"Microsoft\Windows\Start Menu\Programs\Startup\CYD Companion.cmd")
# board rotation value for each choice (TFT_eSPI: 1/3 landscape, 0/2 portrait)
ORIENTATIONS = {"Landscape": 1, "Landscape (upside down)": 3, "Portrait": 0, "Portrait (upside down)": 2}
CAT_STYLES = ["Classic white", "Orange tabby", "Midnight black", "Calico"]
CAT_SHAPES = ["Chibi", "Chubby", "Original"]

# Theme colour slots, in the order the board expects them
THEME_SLOTS = ["Background", "Clock bar", "Cards", "Lines", "Text", "Secondary text", "Dim text", "Accent"]
THEMES = {
    "Midnight": ["#0D0F16", "#141721", "#1E2230", "#343A4E", "#F2F3F7", "#C4C8D4", "#8A90A2", "#1ED760"],
    "OLED black": ["#000000", "#000000", "#141414", "#2C2C2C", "#FFFFFF", "#CACACA", "#8C8C8C", "#1ED760"],
    "Synthwave": ["#140A24", "#1E0F35", "#2A1748", "#4A2A72", "#FFF0FF", "#E0B8F0", "#A07CC0", "#FF4FD8"],
    "Ocean": ["#06141F", "#0B1F2E", "#12304A", "#1F4A6E", "#EAF6FF", "#B5D6EE", "#7FA3C0", "#3CC8FF"],
    "Forest": ["#0C140E", "#132018", "#1C2E22", "#2F4A36", "#F0F7EE", "#C6DCC4", "#8EA88E", "#9BE564"],
    "Sunset": ["#1A0E0C", "#261512", "#3A201A", "#5A3328", "#FFF3EC", "#F2C9B5", "#C08E78", "#FF8A3D"],
    "Paper (light)": ["#EEF0F4", "#FFFFFF", "#FFFFFF", "#D3D7DF", "#16181E", "#3C424E", "#7A8090", "#12A150"],
}

DEFAULTS = {
    "rotation": 1,
    "theme": "Midnight",
    "custom_theme": THEMES["Midnight"],
    "album_accent": True,
    "cat": 0,
    "shape": 0,
    "brightness": 255,
    "port": "",
    "city": "",
    "lat": None,
    "lon": None,
    "fahrenheit": False,
    "h12": False,
    "invert": True,
    "flip_x": False,
    "flip_y": False,
    "spotify_client_id": "",
}


def load_config():
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            return {**DEFAULTS, **json.load(f)}
    except (OSError, ValueError):
        return dict(DEFAULTS)


def save_config(cfg):
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)


def tray_image():
    """A tiny white cat face for the tray, drawn here so there's no icon file to ship."""
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ink, fur, pink = (52, 44, 56, 255), (255, 250, 246, 255), (255, 150, 176, 255)
    d.ellipse((2, 2, 62, 62), fill=(30, 215, 96, 255))
    for ear in (((12, 30), (16, 8), (30, 20)), ((52, 30), (48, 8), (34, 20))):
        d.polygon(ear, fill=fur, outline=ink)
    d.ellipse((8, 16, 56, 54), fill=fur, outline=ink, width=2)
    d.ellipse((19, 30, 25, 38), fill=ink)
    d.ellipse((39, 30, 45, 38), fill=ink)
    d.ellipse((14, 38, 20, 42), fill=pink)
    d.ellipse((44, 38, 50, 42), fill=pink)
    d.line((28, 42, 30, 45, 32, 42, 34, 45, 36, 42), fill=ink, width=2)
    return img


def ascii_fold(s):
    """The board's fonts are ASCII only: strip accents, drop what can't be shown."""
    out = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().strip()
    return out or ("?" if s else "")


def rgb565(hex_colour):
    r, g, b = (int(hex_colour[i:i + 2], 16) for i in (1, 3, 5))
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def prepare_art(raw, size):
    """Centre-crop to a square and size it exactly to the board's art box as a baseline JPEG."""
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2))
    img = img.resize((size, size), Image.LANCZOS)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85, progressive=False)
    return out.getvalue()


# ---------- weather ----------

def search_city(name):
    r = requests.get(GEOCODE_URL, params={"name": name, "count": 8, "language": "en", "format": "json"}, timeout=10)
    r.raise_for_status()
    return r.json().get("results", [])


def fetch_forecast(lat, lon, fahrenheit):
    params = {
        "latitude": lat,
        "longitude": lon,
        "current": "temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max",
        "timezone": "auto",
        "forecast_days": 5,
    }
    if fahrenheit:
        params |= {"temperature_unit": "fahrenheit", "wind_speed_unit": "mph"}
    r = requests.get(FORECAST_URL, params=params, timeout=10)
    r.raise_for_status()
    j = r.json()
    cur, daily = j["current"], j["daily"]
    days = [
        [
            (dt.date.fromisoformat(d).weekday() + 1) % 7,  # board wants 0 = Sunday
            daily["weather_code"][i] or 0,
            round(daily["temperature_2m_max"][i] or 0),
            round(daily["temperature_2m_min"][i] or 0),
        ]
        for i, d in enumerate(daily["time"][:5])
    ]
    return {
        "t": "wx",
        "tmp": cur["temperature_2m"],
        "code": cur["weather_code"],
        "day": cur["is_day"],
        "hum": cur["relative_humidity_2m"],
        "wind": cur["wind_speed_10m"],
        "pop": (daily["precipitation_probability_max"] or [0])[0] or 0,
        "f": fahrenheit,
        "d": days,
    }


# ---------- Spotify login (for the board's standalone mode) ----------

class SpotifyLoginError(Exception):
    pass


def spotify_login(client_id, timeout=600):
    """Authorization-code + PKCE flow in the browser. Returns a refresh token for the board."""
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_response(404)
                self.end_headers()
                return
            got = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            if got.get("state") != state:
                # a tab left over from an earlier attempt: ignore it and keep waiting for this one
                msg = "This tab is from an earlier login attempt. Close it and use the newest Spotify tab."
            elif "error" in got:
                result.update(got)
                msg = f"Spotify said: {got['error']}. Close this tab and check the app."
            else:
                result.update(got)
                msg = "Spotify is connected. You can close this tab."
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(f"<h2 style='font-family:sans-serif'>{msg}</h2>".encode())

        def log_message(self, *args):
            pass

    try:
        server = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
    except OSError:
        raise SpotifyLoginError("port 8888 is busy. Is another Spotify login still open?")
    server.timeout = 1
    try:
        webbrowser.open("https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": SPOTIFY_REDIRECT,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
            "scope": SPOTIFY_SCOPES,
            "state": state,
        }))
        deadline = time.monotonic() + timeout
        while "code" not in result and "error" not in result and time.monotonic() < deadline:
            server.handle_request()
    finally:
        server.server_close()
    if "error" in result:
        raise SpotifyLoginError(f"Spotify said \"{result['error']}\" (access_denied means Cancel was pressed)")
    if "code" not in result:
        raise SpotifyLoginError(f"no answer from Spotify within {timeout // 60} minutes")
    r = requests.post("https://accounts.spotify.com/api/token", data={
        "grant_type": "authorization_code",
        "code": result["code"],
        "redirect_uri": SPOTIFY_REDIRECT,
        "client_id": client_id,
        "code_verifier": verifier,
    }, timeout=15)
    if r.status_code != 200:
        try:
            detail = r.json().get("error_description") or r.json().get("error")
        except ValueError:
            detail = r.text[:120]
        raise SpotifyLoginError(f"Spotify refused the login ({r.status_code}): {detail}")
    return r.json()["refresh_token"]


# ---------- board link ----------

class Board:
    """Line-delimited JSON over the CH340 serial port."""

    def __init__(self, on_message, on_status):
        self._ser = None
        self._lock = threading.Lock()
        self.on_message = on_message
        self.on_status = on_status

    @staticmethod
    def find_port(preferred=""):
        ports = list(serial.tools.list_ports.comports())
        if preferred and any(p.device == preferred for p in ports):
            return preferred
        return next((p.device for p in ports if p.vid == CH340_VID), None)

    @property
    def connected(self):
        return self._ser is not None

    def connect(self, port):
        s = serial.Serial()
        s.port = port
        s.baudrate = BAUD
        s.timeout = 0.2
        s.write_timeout = 2
        s.dtr = False  # keep the auto-reset circuit idle so opening the port doesn't reboot the board
        s.rts = False
        s.open()
        self._ser = s
        threading.Thread(target=self._read_loop, args=(s,), daemon=True).start()
        self.on_status(f"Connected on {port}")

    def close(self, s=None):
        if s is not None and s is not self._ser:
            return
        s, self._ser = self._ser, None
        if s:
            try:
                s.close()
            except serial.SerialException:
                pass
            self.on_status("Looking for the board...")

    def send(self, msg, payload=b""):
        s = self._ser
        if not s:
            return False
        data = json.dumps(msg, separators=(",", ":")).encode() + b"\n" + payload
        try:
            with self._lock:
                s.write(data)
            return True
        except (serial.SerialException, OSError):
            self.close(s)
            return False

    def _read_loop(self, s):
        buf = b""
        while self._ser is s:
            try:
                buf += s.read(512)
            except (serial.SerialException, OSError):
                self.close(s)
                return
            *lines, buf = buf.split(b"\n")
            for line in lines:
                line = line.strip()
                if line.startswith(b"{"):  # skip boot-ROM noise
                    try:
                        self.on_message(json.loads(line))
                    except ValueError:
                        pass
            if len(buf) > 8192:
                buf = b""


# ---------- media + typing ----------

class MediaWatcher:
    """Follows Windows' media session (Spotify preferred) on its own asyncio thread."""

    def __init__(self):
        self.lock = threading.Lock()
        self.state = None  # payload of the board's "now" message, or None
        self.art = None  # the player's thumbnail as-is; resized per board when sent
        self.version = 0
        self.art_version = 0
        self.commands = queue.Queue()
        self._key = None
        self._art_retry_at = []
        self._art_hash = None
        threading.Thread(target=lambda: asyncio.run(self._run()), daemon=True).start()

    async def _run(self):
        mgr = await MediaManager.request_async()
        last_poll = 0.0
        play_when_ready_until = 0.0  # Spotify was just launched; press play once it shows up
        while True:
            try:
                session = self._pick(mgr)
                while not self.commands.empty():
                    cmd = self.commands.get_nowait()
                    if cmd == "play" and not session:
                        os.startfile("spotify:")  # opens the Spotify desktop app
                        play_when_ready_until = time.monotonic() + 20
                    elif session:
                        await {
                            "toggle": session.try_toggle_play_pause_async,
                            "play": session.try_play_async,
                            "resume": session.try_play_async,
                            "pause": session.try_pause_async,
                            "next": session.try_skip_next_async,
                            "prev": session.try_skip_previous_async,
                        }[cmd]()
                        last_poll = 0.0
                if play_when_ready_until and session:
                    if await session.try_play_async() or time.monotonic() > play_when_ready_until:
                        play_when_ready_until = 0.0
                        last_poll = 0.0
                elif play_when_ready_until and time.monotonic() > play_when_ready_until:
                    play_when_ready_until = 0.0
                if time.monotonic() - last_poll >= 0.5:
                    last_poll = time.monotonic()
                    await self._poll(session)
            except Exception as e:  # sessions come and go under us; never let the watcher die
                print("media:", e)
            await asyncio.sleep(0.1)

    @staticmethod
    def _pick(mgr):
        for s in mgr.get_sessions():
            if "spotify" in (s.source_app_user_model_id or "").lower():
                return s
        return mgr.get_current_session()

    async def _poll(self, session):
        state, key, props = None, None, None
        if session is not None:
            props = await session.try_get_media_properties_async()
            if props and props.title:
                info = session.get_playback_info()
                tl = session.get_timeline_properties()
                playing = info.playback_status == PlaybackStatus.PLAYING
                dur = max((tl.end_time - tl.start_time).total_seconds(), 0)
                pos = tl.position.total_seconds()
                updated = tl.last_updated_time
                if playing and updated.year > 1601:
                    if updated.tzinfo is None:
                        updated = updated.replace(tzinfo=dt.timezone.utc)
                    pos += (dt.datetime.now(dt.timezone.utc) - updated).total_seconds()
                pos = min(max(pos, 0), dur) if dur else 0
                key = (props.title, props.artist)
                state = {
                    "title": ascii_fold(props.title)[:90],
                    "artist": ascii_fold(props.artist)[:90],
                    "album": ascii_fold(props.album_title)[:90],
                    "dur": int(dur * 1000),
                    "pos": int(pos * 1000),
                    "play": playing,
                }

        art_changed = False
        if key != self._key:
            self._key = key
            # some players publish the new thumbnail a moment after the title, so look twice more
            now = time.monotonic()
            self._art_retry_at = [now, now + 1.5, now + 4] if state else []
            self._art_hash = None
            with self.lock:
                self.art = None
            art_changed = True
        if self._art_retry_at and time.monotonic() >= self._art_retry_at[0]:
            self._art_retry_at.pop(0)
            raw = await self._read_thumbnail(props.thumbnail) if props and props.thumbnail else None
            digest = hashlib.sha1(raw).digest() if raw else None
            if raw and digest != self._art_hash:
                try:
                    Image.open(io.BytesIO(raw)).verify()
                    art = raw
                except (OSError, SyntaxError):
                    art = None
                if art:
                    self._art_hash = digest
                    with self.lock:
                        self.art = art
                    art_changed = True

        with self.lock:
            core = lambda s: s and {k: v for k, v in s.items() if k != "pos"}
            if core(state) != core(self.state) or key != getattr(self, "_published_key", None):
                self.version += 1
            self._published_key = key
            self.state = state
            if art_changed:
                self.art_version += 1

    @staticmethod
    async def _read_thumbnail(ref):
        try:
            stream = await ref.open_read_async()
            buf = Buffer(stream.size)
            await stream.read_async(buf, buf.capacity, InputStreamOptions.READ_AHEAD)
            data = bytearray(buf.length)
            DataReader.from_buffer(buf).read_bytes(data)
            return bytes(data)
        except OSError:
            return None


class KeyCounter:
    """Counts key presses for the cat's paws. Which keys were pressed is never looked at."""

    def __init__(self):
        self._n = 0
        self._times = collections.deque()  # press times over the last minute, for words per minute
        self._lock = threading.Lock()
        listener = keyboard.Listener(on_press=self._on_press)
        listener.daemon = True
        listener.start()

    def _on_press(self, _key):
        with self._lock:
            self._n += 1
            self._times.append(time.monotonic())

    def wpm(self):
        with self._lock:
            cutoff = time.monotonic() - 60
            while self._times and self._times[0] < cutoff:
                self._times.popleft()
            return round(len(self._times) / 5)  # the usual 5 keystrokes per word

    def take(self):
        with self._lock:
            n, self._n = self._n, 0
        return n


# ---------- app ----------

class App:
    def __init__(self, root, hidden=False):
        self.root = root
        self.told_about_tray = hidden
        self.cfg = load_config()
        self.events = queue.Queue()
        self.board = Board(lambda m: self.events.put(("msg", m)), lambda s: self.events.put(("status", s)))
        self.media = MediaWatcher()
        self.keys = KeyCounter()
        self.weather = None
        self.weather_at = -1e9
        self.wx_dirty = threading.Event()
        self.resend = threading.Event()
        self.board_info = {}
        self._build_ui()
        self._start_tray()
        root.protocol("WM_DELETE_WINDOW", self.hide)
        if hidden:
            root.withdraw()
        threading.Thread(target=self._feed_loop, daemon=True).start()
        threading.Thread(target=self._weather_loop, daemon=True).start()
        root.after(100, self._drain_events)

    # ----- tray -----

    def _start_tray(self):
        menu = pystray.Menu(
            pystray.MenuItem("Open CYD Companion", lambda: self.events.put(("show", None)), default=True),
            pystray.MenuItem("Play / pause", lambda: self.media.commands.put("toggle")),
            pystray.MenuItem("Next track", lambda: self.media.commands.put("next")),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Quit", lambda: self.events.put(("quit", None))),
        )
        self.tray = pystray.Icon("cyd-companion", tray_image(), "CYD Companion", menu)
        self.tray.run_detached()

    def show(self):
        self.root.deiconify()
        self.root.lift()
        self.root.focus_force()

    def hide(self):
        self.root.withdraw()
        if not self.told_about_tray:
            self.told_about_tray = True
            self.tray.notify("Still running in the tray. Right-click the cat icon to quit.", "CYD Companion")

    def quit(self):
        self.tray.stop()
        self.board.close()
        self.root.destroy()

    # ----- UI -----

    def _build_ui(self):
        r = self.root
        r.title("CYD Companion")
        r.minsize(820, 0)
        pad = {"padx": 10, "pady": 6}
        main = ttk.Frame(r, padding=10)
        main.pack(fill="both", expand=True)
        main.columnconfigure(0, weight=1)
        main.columnconfigure(1, weight=1)
        self._syncing = False  # true while mirroring the board's settings into the widgets

        st = ttk.LabelFrame(main, text="Status", padding=8)
        st.grid(row=0, column=0, sticky="ew", **pad)
        st.columnconfigure(1, weight=1)
        self.board_var = tk.StringVar(value="Looking for the board...")
        self.playing_var = tk.StringVar(value="Nothing playing")
        self.wx_var = tk.StringVar(value="No city picked yet")
        self.net_var = tk.StringVar(value="-")
        for i, (label, var) in enumerate([("Board", self.board_var), ("Playing", self.playing_var),
                                          ("Forecast", self.wx_var), ("Board WiFi", self.net_var)]):
            ttk.Label(st, text=label + ":").grid(row=i, column=0, sticky="w", padx=(0, 8))
            ttk.Label(st, textvariable=var).grid(row=i, column=1, sticky="w")

        city = ttk.LabelFrame(main, text="Forecast city", padding=8)
        city.grid(row=1, column=0, sticky="ew", **pad)
        city.columnconfigure(0, weight=1)
        self.city_entry = ttk.Entry(city)
        self.city_entry.grid(row=0, column=0, sticky="ew")
        self.city_entry.bind("<Return>", lambda e: self._search_city())
        ttk.Button(city, text="Search", command=self._search_city).grid(row=0, column=1, padx=(6, 0))
        self.city_list = tk.Listbox(city, height=5, activestyle="none")
        self.city_list.grid(row=1, column=0, columnspan=2, sticky="ew", pady=6)
        self.city_list.bind("<Double-Button-1>", lambda e: self._use_city())
        row = ttk.Frame(city)
        row.grid(row=2, column=0, columnspan=2, sticky="ew")
        ttk.Button(row, text="Use selected city", command=self._use_city).pack(side="left")
        self.f_var = tk.BooleanVar(value=self.cfg["fahrenheit"])
        ttk.Radiobutton(row, text="°F", variable=self.f_var, value=True, command=self._units_changed).pack(side="right")
        ttk.Radiobutton(row, text="°C", variable=self.f_var, value=False, command=self._units_changed).pack(side="right")
        self.city_results = []

        look = ttk.LabelFrame(main, text="Look", padding=8)
        look.grid(row=0, column=1, rowspan=2, sticky="new", **pad)
        look.columnconfigure(1, weight=1)
        ttk.Label(look, text="Theme").grid(row=0, column=0, sticky="w")
        self.theme_var = tk.StringVar(value=self.cfg["theme"] if self.cfg["theme"] in THEMES or self.cfg["theme"] == "Custom" else "Midnight")
        theme = ttk.Combobox(look, textvariable=self.theme_var, values=list(THEMES) + ["Custom"], state="readonly", width=20)
        theme.grid(row=0, column=1, sticky="w", padx=8, pady=2)
        theme.bind("<<ComboboxSelected>>", lambda e: self._theme_changed())
        ttk.Button(look, text="Custom colours...", command=self._open_colours).grid(row=0, column=2, sticky="e")
        self.swatch_row = tk.Frame(look)
        self.swatch_row.grid(row=1, column=0, columnspan=3, sticky="w", pady=(4, 6))
        self.alb_var = tk.BooleanVar(value=self.cfg["album_accent"])
        ttk.Checkbutton(look, text="Accent colour follows the album art", variable=self.alb_var,
                        command=self._theme_changed).grid(row=2, column=0, columnspan=3, sticky="w")
        ttk.Label(look, text="Cat colours").grid(row=3, column=0, sticky="w", pady=(8, 2))
        self.cat_var = tk.StringVar(value=CAT_STYLES[self.cfg["cat"] % 4])
        cat = ttk.Combobox(look, textvariable=self.cat_var, values=CAT_STYLES, state="readonly", width=20)
        cat.grid(row=3, column=1, sticky="w", padx=8, pady=(8, 2))
        cat.bind("<<ComboboxSelected>>", lambda e: self._cat_changed())
        ttk.Label(look, text="Cat shape").grid(row=5, column=0, sticky="w", pady=2)
        self.shape_var = tk.StringVar(value=CAT_SHAPES[self.cfg["shape"] % 3])
        shape = ttk.Combobox(look, textvariable=self.shape_var, values=CAT_SHAPES, state="readonly", width=20)
        shape.grid(row=5, column=1, sticky="w", padx=8, pady=2)
        shape.bind("<<ComboboxSelected>>", lambda e: self._cat_changed())
        ttk.Label(look, text="Brightness").grid(row=6, column=0, sticky="w", pady=2)
        self.bri_var = tk.IntVar(value=self.cfg["brightness"])
        bri = ttk.Scale(look, from_=10, to=255, variable=self.bri_var, command=lambda v: self._brightness_changed())
        bri.grid(row=6, column=1, columnspan=2, sticky="ew", padx=8, pady=2)
        self._bri_sent_at = 0.0
        self._bri_job = None
        self._draw_swatches()

        disp = ttk.LabelFrame(main, text="Display", padding=8)
        disp.grid(row=2, column=1, sticky="new", **pad)
        row = ttk.Frame(disp)
        row.grid(row=0, column=0, sticky="w", pady=(0, 4))
        ttk.Label(row, text="Screen orientation").pack(side="left")
        names = {v: k for k, v in ORIENTATIONS.items()}
        self.rot_var = tk.StringVar(value=names.get(self.cfg["rotation"], "Landscape"))
        rot = ttk.Combobox(row, textvariable=self.rot_var, values=list(ORIENTATIONS), state="readonly", width=24)
        rot.pack(side="left", padx=8)
        rot.bind("<<ComboboxSelected>>", lambda e: self._display_changed())
        self.disp_vars = {}
        for i, (key, text) in enumerate([("h12", "12-hour clock"), ("invert", "Invert colours (fixes a negative-looking screen)"),
                                         ("flip_x", "Flip touch left/right"), ("flip_y", "Flip touch up/down")]):
            v = tk.BooleanVar(value=self.cfg[key])
            self.disp_vars[key] = v
            ttk.Checkbutton(disp, text=text, variable=v, command=self._display_changed).grid(row=i + 1, column=0, sticky="w")
        self.startup_var = tk.BooleanVar(value=os.path.exists(STARTUP_FILE))
        ttk.Checkbutton(disp, text="Start with Windows (in the tray)", variable=self.startup_var,
                        command=self._startup_changed).grid(row=6, column=0, sticky="w", pady=(4, 0))

        wifi = ttk.LabelFrame(main, text="Standalone mode (board on its own, no laptop)", padding=8)
        wifi.grid(row=2, column=0, sticky="new", **pad)
        wifi.columnconfigure(1, weight=1)
        ttk.Label(wifi, text="WiFi name").grid(row=0, column=0, sticky="w")
        self.ssid_entry = ttk.Entry(wifi)
        self.ssid_entry.grid(row=0, column=1, sticky="ew", pady=2)
        ttk.Label(wifi, text="Password").grid(row=1, column=0, sticky="w")
        self.pass_entry = ttk.Entry(wifi, show="•")
        self.pass_entry.grid(row=1, column=1, sticky="ew", pady=2)
        ttk.Button(wifi, text="Send WiFi to board", command=self._send_wifi).grid(row=2, column=1, sticky="e", pady=(2, 8))
        ttk.Label(wifi, text="The board only does 2.4 GHz WiFi.", foreground="gray").grid(row=2, column=0, sticky="w")

        ttk.Separator(wifi).grid(row=3, column=0, columnspan=2, sticky="ew", pady=4)
        ttk.Label(wifi, text="Spotify Client ID").grid(row=4, column=0, sticky="w")
        self.spid_entry = ttk.Entry(wifi)
        self.spid_entry.insert(0, self.cfg["spotify_client_id"])
        self.spid_entry.grid(row=4, column=1, sticky="ew", pady=2)
        row = ttk.Frame(wifi)
        row.grid(row=5, column=0, columnspan=2, sticky="ew", pady=(2, 0))
        ttk.Button(row, text="How do I get one?", command=self._spotify_help).pack(side="left")
        ttk.Button(row, text="Connect Spotify...", command=self._connect_spotify).pack(side="right")

        if self.cfg["city"]:
            self.wx_var.set(f"{self.cfg['city']} (loading...)")

    def _search_city(self):
        name = self.city_entry.get().strip()
        if not name:
            return
        self.city_list.delete(0, "end")
        self.city_list.insert("end", "Searching...")

        def work():
            try:
                res = search_city(name)
            except requests.RequestException as e:
                self.events.put(("cities", e))
                return
            self.events.put(("cities", res))

        threading.Thread(target=work, daemon=True).start()

    def _use_city(self):
        sel = self.city_list.curselection()
        if not sel or not self.city_results:
            messagebox.showinfo("CYD Companion", "Search for your city and pick it from the list first.")
            return
        c = self.city_results[sel[0]]
        self.cfg.update(city=c["name"], lat=round(c["latitude"], 4), lon=round(c["longitude"], 4))
        save_config(self.cfg)
        self.wx_var.set(f"{c['name']} (loading...)")
        self.board.send({"t": "cfg", "city": ascii_fold(c["name"])[:47], "lat": self.cfg["lat"], "lon": self.cfg["lon"]})
        self.wx_dirty.set()

    def _units_changed(self):
        self.cfg["fahrenheit"] = self.f_var.get()
        save_config(self.cfg)
        self.board.send({"t": "cfg", "f": self.cfg["fahrenheit"]})
        self.wx_dirty.set()

    def _display_changed(self):
        if self._syncing:
            return
        for key, v in self.disp_vars.items():
            self.cfg[key] = v.get()
        self.cfg["rotation"] = ORIENTATIONS[self.rot_var.get()]
        save_config(self.cfg)
        c = self.cfg
        self.board.send({"t": "cfg", "h12": c["h12"], "inv": c["invert"], "tfx": c["flip_x"], "tfy": c["flip_y"],
                         "rot": c["rotation"]})

    # ----- look -----

    def _theme_colours(self):
        name = self.theme_var.get()
        return list(self.cfg["custom_theme"]) if name == "Custom" else list(THEMES.get(name, THEMES["Midnight"]))

    def _draw_swatches(self):
        for w in self.swatch_row.winfo_children():
            w.destroy()
        for slot, colour in zip(THEME_SLOTS, self._theme_colours()):
            sw = tk.Frame(self.swatch_row, width=26, height=18, bg=colour, highlightthickness=1, highlightbackground="#888")
            sw.pack(side="left", padx=2)
            sw.bind("<Button-1>", lambda e: self._open_colours())

    def _send_theme(self):
        self.board.send({"t": "cfg", "theme": [rgb565(c) for c in self._theme_colours()], "alb": self.cfg["album_accent"]})

    def _theme_changed(self):
        if self._syncing:
            return
        self.cfg["theme"] = self.theme_var.get()
        self.cfg["album_accent"] = self.alb_var.get()
        save_config(self.cfg)
        self._draw_swatches()
        self._send_theme()

    def _open_colours(self):
        """Pick every theme colour by hand. Starts from whatever theme is showing."""
        if getattr(self, "_colour_win", None) and self._colour_win.winfo_exists():
            self._colour_win.lift()
            return
        if self.theme_var.get() != "Custom":
            self.cfg["custom_theme"] = self._theme_colours()
        win = self._colour_win = tk.Toplevel(self.root)
        win.title("Custom colours")
        win.resizable(False, False)
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill="both")
        ttk.Label(frame, text="Click a colour to change it. Changes show on the board straight away.").grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))
        swatches = []

        def pick(i):
            got = colorchooser.askcolor(self.cfg["custom_theme"][i], parent=win, title=THEME_SLOTS[i])[1]
            if not got:
                return
            self.cfg["custom_theme"][i] = got.upper()
            swatches[i].configure(bg=got)
            self.theme_var.set("Custom")
            self._theme_changed()

        for i, slot in enumerate(THEME_SLOTS):
            ttk.Label(frame, text=slot).grid(row=i + 1, column=0, sticky="w", pady=2)
            sw = tk.Frame(frame, width=60, height=22, bg=self.cfg["custom_theme"][i], highlightthickness=1,
                          highlightbackground="#888", cursor="hand2")
            sw.grid(row=i + 1, column=1, padx=10, pady=2)
            sw.bind("<Button-1>", lambda e, i=i: pick(i))
            swatches.append(sw)
            ttk.Button(frame, text="Change...", command=lambda i=i: pick(i)).grid(row=i + 1, column=2, pady=2)
        ttk.Button(frame, text="Close", command=win.destroy).grid(row=len(THEME_SLOTS) + 1, column=2, sticky="e", pady=(10, 0))

    def _cat_changed(self):
        if self._syncing:
            return
        self.cfg["cat"] = CAT_STYLES.index(self.cat_var.get())
        self.cfg["shape"] = CAT_SHAPES.index(self.shape_var.get())
        save_config(self.cfg)
        self.board.send({"t": "cfg", "cat": self.cfg["cat"], "shape": self.cfg["shape"]})

    def _brightness_changed(self):
        if self._syncing:
            return
        self.cfg["brightness"] = int(self.bri_var.get())
        if time.monotonic() - self._bri_sent_at > 0.08:  # live while dragging, without flooding the link
            self._bri_sent_at = time.monotonic()
            self.board.send({"t": "cfg", "bri": self.cfg["brightness"]})
        if self._bri_job:
            self.root.after_cancel(self._bri_job)
        self._bri_job = self.root.after(300, self._brightness_settled)

    def _brightness_settled(self):
        self._bri_job = None
        save_config(self.cfg)
        self.board.send({"t": "cfg", "bri": self.cfg["brightness"]})

    def _adopt_board_settings(self, m):
        """Settings changed on the board's own settings page show up here too."""
        names = {v: k for k, v in ORIENTATIONS.items()}
        self._syncing = True
        try:
            for key, board_key in (("h12", "h12"), ("invert", "inv"), ("flip_x", "tfx"), ("flip_y", "tfy")):
                if board_key in m:
                    self.cfg[key] = bool(m[board_key])
                    self.disp_vars[key].set(self.cfg[key])
            if "rot" in m:
                self.cfg["rotation"] = m["rot"]
                self.rot_var.set(names.get(m["rot"], "Landscape"))
            if "cat" in m:
                self.cfg["cat"] = m["cat"] % 4
                self.cat_var.set(CAT_STYLES[self.cfg["cat"]])
            if "shape" in m:
                self.cfg["shape"] = m["shape"] % 3
                self.shape_var.set(CAT_SHAPES[self.cfg["shape"]])
            if "bri" in m and not self._bri_job:
                self.cfg["brightness"] = m["bri"]
                self.bri_var.set(m["bri"])
            save_config(self.cfg)
        finally:
            self._syncing = False

    def _startup_changed(self):
        if self.startup_var.get():
            pythonw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
            with open(STARTUP_FILE, "w", encoding="utf-8") as f:
                f.write(f'@start "" "{pythonw}" "{os.path.abspath(__file__)}" --hidden\n')
        elif os.path.exists(STARTUP_FILE):
            os.remove(STARTUP_FILE)

    def _send_wifi(self):
        ssid = self.ssid_entry.get().strip()
        if not ssid:
            messagebox.showinfo("CYD Companion", "Type your WiFi name first.")
            return
        if not self.board.send({"t": "cfg", "ssid": ssid, "pass": self.pass_entry.get()}):
            messagebox.showwarning("CYD Companion", "The board isn't connected.")
            return
        self.pass_entry.delete(0, "end")
        self.net_var.set(f"connecting to {ssid}...")

    def _spotify_help(self):
        messagebox.showinfo("Spotify Client ID", (
            "This is only needed for the board to show Spotify on its own over WiFi.\n\n"
            "1. Go to developer.spotify.com/dashboard and log in.\n"
            "2. Create app. Any name and description.\n"
            f"3. Redirect URI: {SPOTIFY_REDIRECT}\n"
            "4. Tick 'Web API', save.\n"
            "5. Copy the Client ID into this box and press Connect Spotify.\n\n"
            "Play/pause/skip from the board in standalone mode needs Spotify Premium."
        ))

    def _connect_spotify(self):
        client_id = self.spid_entry.get().strip()
        if not client_id:
            self._spotify_help()
            return
        if not self.board.connected:
            messagebox.showwarning("CYD Companion", "Connect the board first.")
            return
        if getattr(self, "_spotify_busy", False):
            messagebox.showinfo("Spotify", "A Spotify login is already waiting. Finish it in the newest browser tab.")
            return
        self.cfg["spotify_client_id"] = client_id
        save_config(self.cfg)
        self._spotify_busy = True

        def work():
            try:
                token = spotify_login(client_id)
                ok = self.board.send({"t": "cfg", "spid": client_id, "sprt": token})
                self.events.put(("spotify", "Spotify is linked to the board." if ok else "Logged in, but the board disconnected. Try again."))
            except (SpotifyLoginError, requests.RequestException) as e:
                self.events.put(("spotify", f"Spotify login failed: {e}"))
            finally:
                self._spotify_busy = False

        threading.Thread(target=work, daemon=True).start()

    # ----- background work -----

    def _drain_events(self):
        while not self.events.empty():
            kind, val = self.events.get_nowait()
            if kind == "status":
                self.board_var.set(val)
            elif kind == "msg":
                self._on_board_message(val)
            elif kind == "wx":
                unit = "F" if val["f"] else "C"
                self.wx_var.set(f"{self.cfg['city']}: {round(val['tmp'])}°{unit}")
            elif kind == "wx_error":
                self.wx_var.set(val)
            elif kind == "cities":
                self.city_list.delete(0, "end")
                if isinstance(val, Exception):
                    self.city_list.insert("end", f"Search failed: {val}")
                    self.city_results = []
                else:
                    self.city_results = val
                    for c in val:
                        self.city_list.insert("end", ", ".join(x for x in (c.get("name"), c.get("admin1"), c.get("country")) if x))
                    if not val:
                        self.city_list.insert("end", "No matches")
            elif kind == "spotify":
                messagebox.showinfo("Spotify", val)
            elif kind == "show":
                self.show()
            elif kind == "quit":
                self.quit()
                return
        with self.media.lock:
            s = self.media.state
        self.playing_var.set(f"{'▶' if s['play'] else '⏸'} {s['title']} - {s['artist']}" if s else "Nothing playing")
        self.root.after(200, self._drain_events)

    def _on_board_message(self, m):
        if m.get("t") == "cmd" and m.get("c") in ("toggle", "play", "resume", "pause", "next", "prev"):
            self.media.commands.put(m["c"])
        elif m.get("t") == "status":
            self.board_info = m
            if m.get("boot"):
                self.resend.set()
            if not m.get("ssid"):
                net = "not set up"
            elif m.get("wifi"):
                net = f"connected to {m['ssid']} ({m.get('ip')})"
            else:
                net = f"not connected (trying {m['ssid']})"
            net += ", Spotify linked" if m.get("sp") else ", Spotify not linked"
            self.net_var.set(net)
            self._adopt_board_settings(m)
            # a freshly flashed board doesn't know the city yet
            if not m.get("city") and self.cfg["lat"] is not None:
                self.board.send({"t": "cfg", "city": ascii_fold(self.cfg["city"])[:47],
                                 "lat": self.cfg["lat"], "lon": self.cfg["lon"], "f": self.cfg["fahrenheit"]})

    def _feed_loop(self):
        sent_version = sent_art = -1
        last_now = last_time = last_try = last_status = last_sys = 0.0
        psutil.cpu_percent(None)  # prime it; later calls report usage since the previous one
        last_wx = None
        while True:
            t = time.monotonic()
            if not self.board.connected:
                if t - last_try > 2:
                    last_try = t
                    port = Board.find_port(self.cfg["port"])
                    if port:
                        try:
                            self.board.connect(port)
                            self.resend.set()
                        except serial.SerialException as e:
                            self.events.put(("status", f"Can't open {port}: {e}"))
                time.sleep(0.1)
                continue

            if self.resend.is_set():
                self.resend.clear()
                sent_version = sent_art = -1
                last_time = last_status = 0.0
                last_wx = None
                self.board.send({"t": "status"})  # the board's own settings flow back into the app

            n = self.keys.take()
            if n:
                self.board.send({"t": "keys", "n": n})

            with self.media.lock:
                state, art, art_v, ver = self.media.state, self.media.art, self.media.art_version, self.media.version
            if ver != sent_version or t - last_now >= 1:
                self.board.send({"t": "now", **state} if state else {"t": "now", "title": ""})
                sent_version, last_now = ver, t
            size = self.board_info.get("art", DEFAULT_ART_SIZE)
            if (art_v, size) != sent_art:  # new art, or the board's art box changed size
                if art and state:
                    try:
                        jpeg = prepare_art(art, size)
                        self.board.send({"t": "art", "len": len(jpeg)}, jpeg)
                    except OSError:
                        pass
                sent_art = (art_v, size)

            if t - last_time >= 30:
                now = dt.datetime.now().astimezone()
                self.board.send({"t": "time", "e": int(now.timestamp() + now.utcoffset().total_seconds())})
                last_time = t
            if t - last_sys >= 2:
                self.board.send({"t": "sys", "cpu": round(psutil.cpu_percent(None)),
                                 "ram": round(psutil.virtual_memory().percent), "wpm": self.keys.wpm()})
                last_sys = t
            if t - last_status >= 10:
                self.board.send({"t": "status"})
                last_status = t
            wx = self.weather
            if wx is not None and wx is not last_wx:
                self.board.send(wx)
                last_wx = wx
            time.sleep(0.1)

    def _weather_loop(self):
        while True:
            if self.cfg["lat"] is not None and (self.wx_dirty.is_set() or time.monotonic() - self.weather_at > 600):
                self.wx_dirty.clear()
                try:
                    wx = fetch_forecast(self.cfg["lat"], self.cfg["lon"], self.cfg["fahrenheit"])
                    wx["city"] = ascii_fold(self.cfg["city"])[:47]
                    self.weather = wx
                    self.weather_at = time.monotonic()
                    self.events.put(("wx", wx))
                except (requests.RequestException, KeyError, ValueError) as e:
                    self.events.put(("wx_error", f"Forecast failed, retrying: {e}"))
                    self.weather_at = time.monotonic() - 540  # try again in a minute
            self.wx_dirty.wait(5)


def main():
    root = tk.Tk()
    App(root, hidden="--hidden" in sys.argv)
    root.mainloop()


if __name__ == "__main__":
    main()
