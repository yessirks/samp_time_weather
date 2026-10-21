// samp_time_weather - TimeWeather.asi
//
// Change the time of day and the weather yourself in SA-MP (client side: only you see it).
// SA-MP servers set the time and the weather over the network and send it again now and then; this plugin writes your
// choice into the game every frame, so it stays, and it can hand control back to the server at any time.
//
// Press the open key (F9 by default) for the panel: Up/Down choose a row, Left/Right change it. Every key can be changed in
// TimeWeather.ini. "Single-player style" runs the clock like single-player (a game day in 24 minutes) and lets the weather
// change by itself, the way it does in single-player.
//
// How it draws and reads input is the same proven approach as in AutoPilot.asi: a hook on CPad::UpdatePads (per-frame logic,
// swallows the game's own input while the panel is open) and on Render2dStuff (drawing; SA-MP removes that hook, so a
// watchdog chains it back in).
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace tw {

// ============================================================================ the game (stock GTA:SA 1.0 US)
namespace game {

constexpr uintptr_t CALL_UpdatePads = 0x53BEE6;  // call CPad::UpdatePads (in CGame::Process)
constexpr uintptr_t FN_UpdatePads = 0x541DD0;
constexpr uintptr_t CALL_Render2d = 0x53EB12;    // call Render2dStuff (in Idle)
constexpr uintptr_t FN_Render2d = 0x53E230;

inline bool IsSupportedExe() { return *(const uint32_t*)0x82457C == 0x94BF; }

inline uint32_t TimeMs() { return *(volatile uint32_t*)0xB7CB84; }  // CTimer::m_snTimeInMilliseconds
inline bool Paused() {
    return *(volatile uint8_t*)0xB7CB49 || *(volatile uint8_t*)0xB7CB48 ||  // user / code paused
           *(volatile uint8_t*)0xBA67A4;                                    // FrontEndMenuManager.m_bMenuActive
}
inline int ScreenW() { return *(int*)0xC17044; }  // RsGlobal.maximumWidth
inline int ScreenH() { return *(int*)0xC17048; }
// Not in a cutscene and the HUD has not been switched off. (FrontEndMenuManager.m_bHudOn, 0xBA6769, is left out on purpose:
// in SA-MP it reads 0 on most frames while the HUD is clearly on screen, which made keys and the panel ignored at random.)
inline bool HudVisible() {
    return !*(volatile uint8_t*)0xB6F065   // TheCamera.m_bWideScreenOn (cutscene bars)
        && !*(volatile uint8_t*)0xB5F851   // CCutsceneMgr::ms_running
        && *(volatile uint8_t*)0xA444A0;   // CTheScripts::bDisplayHud
}

// CClock: hours/minutes are bytes; the game adds a minute when PerMinute ms have passed since LastTick
inline uint8_t& Hours() { return *(uint8_t*)0xB70153; }
inline uint8_t& Minutes() { return *(uint8_t*)0xB70152; }
inline uint16_t& Seconds() { return *(uint16_t*)0xB70150; }
inline uint32_t& LastTick() { return *(uint32_t*)0xB70158; }
inline uint32_t& MsPerMinute() { return *(uint32_t*)0xB7015C; }

// CWeather (the weather types are 16-bit values in this exe: ForceWeatherNow stores them with 'mov [mem],ax').
// With no forced weather (-1) the game itself moves on to the next weather of its list for the region every game hour.
inline int16_t& ForcedWeather() { return *(int16_t*)0xC81318; }  // -1 = none
inline int16_t& NewWeather() { return *(int16_t*)0xC8131C; }
inline int16_t& OldWeather() { return *(int16_t*)0xC81320; }

// void CHud::SetHelpMessage(text, quickMessage, permanent, addToBrief): the small message box at the top left
inline const auto SetHelpMessage = (void(__cdecl*)(const char*, bool, bool, bool))0x588BE0;

// CFont (queued text is drawn by the game at the end of Render2dStuff)
namespace font {
inline const auto SetScale = (void(__cdecl*)(float, float))0x719380;
inline const auto SetColor = (void(__cdecl*)(uint32_t))0x719430;  // CRGBA by value (r in the lowest byte)
inline const auto SetFontStyle = (void(__cdecl*)(uint8_t))0x719490;
inline const auto SetWrapx = (void(__cdecl*)(float))0x7194D0;
inline const auto SetAlphaFade = (void(__cdecl*)(float))0x719500;
inline const auto SetDropColor = (void(__cdecl*)(uint32_t))0x719510;
inline const auto SetDropShadowPosition = (void(__cdecl*)(int8_t))0x719570;
inline const auto SetEdge = (void(__cdecl*)(int8_t))0x719590;
inline const auto SetProportional = (void(__cdecl*)(bool))0x7195B0;
inline const auto SetBackground = (void(__cdecl*)(bool, bool))0x7195C0;
inline const auto SetJustify = (void(__cdecl*)(bool))0x719600;
inline const auto SetOrientation = (void(__cdecl*)(uint8_t))0x719610;  // 0 centre, 1 left, 2 right
inline const auto PrintString = (void(__cdecl*)(float, float, const char*))0x71A700;
}  // namespace font

// the game's pad / mouse state: cleared while the panel is open so nothing is driven, shot or looked at behind it
inline uint8_t* Pad0() { return (uint8_t*)0xB73458; }
inline uint8_t* MouseState() { return (uint8_t*)0xB73418; }  // +0..6 buttons / wheel bytes, floats at +8 (wheel), +0xC (x), +0x10 (y)
inline void SwallowInput() {
    std::memset(MouseState(), 0, 7);
    *(float*)(MouseState() + 0x08) = 0.0f;
    *(float*)(MouseState() + 0x0C) = 0.0f;
    *(float*)(MouseState() + 0x10) = 0.0f;
    std::memset(Pad0(), 0, 0x28);
}

// RenderWare immediate-mode 2D
struct RwIm2DVertex { float x, y, z, rhw; uint32_t color; float u, v; };  // 28 bytes
enum : int { rwRS_TEXTURERASTER = 1, rwRS_ZTESTENABLE = 6, rwRS_ZWRITEENABLE = 8, rwRS_SRCBLEND = 10, rwRS_DESTBLEND = 11,
             rwRS_VERTEXALPHAENABLE = 12, rwRS_FOGENABLE = 14, rwRS_CULLMODE = 20 };
enum : int { rwBLENDSRCALPHA = 5, rwBLENDINVSRCALPHA = 6, rwCULLMODENONE = 1, rwPRIMTYPETRILIST = 3 };
inline uint8_t* RwEngine() { return *(uint8_t**)0xC97B24; }
inline float RwNearScreenZ() { return *(float*)(RwEngine() + 0x18); }
inline int RwRenderStateSet(int s, uintptr_t v) { return (*(int(__cdecl**)(int, uintptr_t))(RwEngine() + 0x20))(s, v); }
inline int RwRenderStateGet(int s, uintptr_t* v) { return (*(int(__cdecl**)(int, uintptr_t*))(RwEngine() + 0x24))(s, v); }
inline int RwIm2DRenderPrimitive(int prim, RwIm2DVertex* verts, int count) {
    return (*(int(__cdecl**)(int, RwIm2DVertex*, int))(RwEngine() + 0x30))(prim, verts, count);
}

}  // namespace game

// ============================================================================ settings
const char* const kWeatherNames[23] = {
    "Extra sunny (LA)", "Sunny (LA)", "Extra sunny, smog (LA)", "Sunny, smog (LA)", "Cloudy (LA)", "Sunny (San Fierro)",
    "Extra sunny (San Fierro)", "Cloudy (San Fierro)", "Rainy (San Fierro)", "Foggy (San Fierro)", "Sunny (Las Venturas)",
    "Extra sunny (Las Venturas)", "Cloudy (Las Venturas)", "Extra sunny (countryside)", "Sunny (countryside)",
    "Cloudy (countryside)", "Rainy (countryside)", "Extra sunny (desert)", "Sunny (desert)", "Sandstorm", "Underwater",
    "Extra colours 1", "Extra colours 2"};

struct Preset { const char* name; int hour, minute; };
const Preset kPresets[] = {{"Dawn", 4, 30}, {"Sunrise", 6, 0}, {"Morning", 9, 0}, {"Noon", 12, 0}, {"Afternoon", 16, 0},
                           {"Sunset", 19, 30}, {"Dusk", 21, 0}, {"Night", 23, 0}, {"Midnight", 0, 0}};
constexpr int kNumPresets = (int)(sizeof kPresets / sizeof kPresets[0]);

// milliseconds per game minute: the game's normal speed is 1000 (one game minute per second: a game day is 24 real minutes)
constexpr int kNormalSpeed = 1000;
const int kSpeeds[] = {4000, 2000, 1000, 500, 250, 100, 50};
constexpr int kNumSpeeds = (int)(sizeof kSpeeds / sizeof kSpeeds[0]);

enum TimeMode { TM_SERVER = 0, TM_FIXED = 1, TM_RUNNING = 2 };
enum WeatherMode { WM_SERVER = 0, WM_CUSTOM = 1, WM_AUTO = 2 };

struct Settings {
    int timeMode = TM_SERVER;
    int hour = 12, minute = 0;
    int msPerMinute = kNormalSpeed;
    int weatherMode = WM_SERVER;
    int weather = 0;
    int preset = 3;
} st;

// ---- key bindings (TimeWeather.ini, [Keys])
struct Binding {
    int vk = 0;  // 0 = not bound
    bool ctrl = false, shift = false, alt = false;
};

struct Keys {
    Binding open, up, down, left, right, select, reset;                       // the panel
    Binding timeForward, timeBack, weatherNext, weatherPrev, spStyle, giveBack;  // optional quick keys (work without the panel)
} keys;

bool g_remember = true;
bool g_logOn = true;
FILE* g_log = nullptr;
std::string g_dir;
bool g_dirty = false;
double g_logT0 = 0.0;  // GetTickCount() at start, in seconds: log lines show the time since then

void Log(const char* fmt, ...) {
    if (!g_log || !g_logOn) return;
    std::fprintf(g_log, "[%7.1fs] ", GetTickCount() / 1000.0 - g_logT0);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

struct NamedKey { const char* name; int vk; };
const NamedKey kNamedKeys[] = {
    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"enter", VK_RETURN}, {"return", VK_RETURN},
    {"backspace", VK_BACK}, {"back", VK_BACK}, {"space", VK_SPACE}, {"tab", VK_TAB}, {"insert", VK_INSERT}, {"ins", VK_INSERT},
    {"delete", VK_DELETE}, {"del", VK_DELETE}, {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
    {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"capslock", VK_CAPITAL},
    {"pause", VK_PAUSE}, {"scrolllock", VK_SCROLL}, {"printscreen", VK_SNAPSHOT},
    {"numadd", VK_ADD}, {"num+", VK_ADD}, {"numsub", VK_SUBTRACT}, {"num-", VK_SUBTRACT}, {"nummul", VK_MULTIPLY},
    {"num*", VK_MULTIPLY}, {"numdiv", VK_DIVIDE}, {"num/", VK_DIVIDE}, {"numdecimal", VK_DECIMAL}, {"num.", VK_DECIMAL},
    {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2}, {"mousemiddle", VK_MBUTTON},
    {"[", VK_OEM_4}, {"]", VK_OEM_6}, {";", VK_OEM_1}, {"'", VK_OEM_7}, {",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD},
    {"-", VK_OEM_MINUS}, {"=", VK_OEM_PLUS}, {"/", VK_OEM_2}, {"\\", VK_OEM_5}, {"`", VK_OEM_3}};

// "F9", "Insert", "Numpad4", "A", "7", "0x78" or a plain number (a virtual-key code); 0 if it is not a key
int ParseVk(const std::string& raw) {
    const std::string t = Lower(Trim(raw));
    if (t.empty() || t == "none" || t == "off") return 0;
    for (const NamedKey& k : kNamedKeys)
        if (t == k.name) return k.vk;
    if (t.size() >= 2 && t[0] == 'f' && std::isdigit((unsigned char)t[1])) {
        const int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (t.size() == 7 && t.compare(0, 6, "numpad") == 0 && std::isdigit((unsigned char)t[6])) return VK_NUMPAD0 + (t[6] - '0');
    if (t.size() == 1 && std::isalnum((unsigned char)t[0])) return std::toupper((unsigned char)t[0]);
    char* end = nullptr;
    const long n = std::strtol(t.c_str(), &end, 0);  // decimal or 0x..
    if (end && *end == 0 && n > 0 && n < 256) return (int)n;
    return 0;
}

// "F9" or with modifiers: "Ctrl+F9", "Shift+Alt+Left"
Binding ParseBinding(const std::string& text) {
    Binding b;
    std::string rest = Trim(text);
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus == 0 || plus + 1 >= rest.size()) break;  // a lone "+" key or the key itself
        const std::string mod = Lower(Trim(rest.substr(0, plus)));
        if (mod == "ctrl" || mod == "control" || mod == "ctl") b.ctrl = true;
        else if (mod == "shift") b.shift = true;
        else if (mod == "alt") b.alt = true;
        else break;
        rest = Trim(rest.substr(plus + 1));
    }
    b.vk = ParseVk(rest);
    return b;
}

std::string VkName(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    for (const NamedKey& k : kNamedKeys)
        if (k.vk == vk && std::strlen(k.name) > 1) {
            std::string n = k.name;
            n[0] = (char)std::toupper((unsigned char)n[0]);
            if (n == "Pageup") n = "PageUp";
            if (n == "Pagedown") n = "PageDown";
            return n;
        }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    char name[64] = {};
    if (sc && GetKeyNameTextA((LONG)(sc << 16), name, sizeof name)) return name;
    char b[16];
    std::snprintf(b, sizeof b, "key %d", vk);
    return b;
}

std::string BindingName(const Binding& b) {
    if (!b.vk) return "(none)";
    return std::string(b.ctrl ? "Ctrl+" : "") + (b.shift ? "Shift+" : "") + (b.alt ? "Alt+" : "") + VkName(b.vk);
}

std::string IniPath() { return g_dir + "TimeWeather.ini"; }

int IniInt(const char* sec, const char* key, int def) { return (int)GetPrivateProfileIntA(sec, key, def, IniPath().c_str()); }
std::string IniStr(const char* sec, const char* key, const char* def) {
    char buf[128] = {};
    GetPrivateProfileStringA(sec, key, def, buf, sizeof buf, IniPath().c_str());
    return Trim(buf);
}
void IniPut(const char* sec, const char* key, int v) {
    char b[32];
    std::snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA(sec, key, b, IniPath().c_str());
}

// [Keys] Name=Binding. A key that is missing gets the default; an entry that is there but empty / "none" switches it off.
Binding LoadKey(const char* name, const char* def) {
    char buf[128] = {};
    const DWORD n = GetPrivateProfileStringA("Keys", name, "\x01", buf, sizeof buf, IniPath().c_str());
    if (n == 1 && buf[0] == '\x01') return ParseBinding(def);  // not in the file
    Binding b = ParseBinding(buf);
    if (!b.vk && Trim(buf).size() > 0 && Lower(Trim(buf)) != "none" && Lower(Trim(buf)) != "off") {
        Log("Keys: '%s=%s' is not a key name I know - using the default (%s)", name, buf, def);
        return ParseBinding(def);
    }
    return b;
}

void LoadSettings() {
    g_remember = IniInt("TimeWeather", "Remember", 1) != 0;
    g_logOn = IniInt("TimeWeather", "Log", 0) != 0;
    keys.open = LoadKey("Open", "F9");
    // the first version had a number in [TimeWeather] OpenKey: still honoured if the new [Keys] Open is not in the file
    if (IniStr("Keys", "Open", "").empty() && IniInt("TimeWeather", "OpenKey", 0) > 0)
        keys.open = ParseBinding(std::to_string(IniInt("TimeWeather", "OpenKey", VK_F9)));
    keys.up = LoadKey("Up", "Up");
    keys.down = LoadKey("Down", "Down");
    keys.left = LoadKey("Left", "Left");
    keys.right = LoadKey("Right", "Right");
    keys.select = LoadKey("Select", "Enter");
    keys.reset = LoadKey("GiveBack", "Backspace");
    keys.timeForward = LoadKey("HourForward", "none");
    keys.timeBack = LoadKey("HourBack", "none");
    keys.weatherNext = LoadKey("WeatherNext", "none");
    keys.weatherPrev = LoadKey("WeatherPrevious", "none");
    keys.spStyle = LoadKey("SinglePlayerStyle", "none");
    keys.giveBack = LoadKey("AllToServer", "none");
    if (!g_remember) return;
    st.timeMode = std::clamp(IniInt("Last", "TimeMode", TM_SERVER), 0, 2);
    st.hour = std::clamp(IniInt("Last", "Hour", 12), 0, 23);
    st.minute = std::clamp(IniInt("Last", "Minute", 0), 0, 59);
    st.msPerMinute = std::clamp(IniInt("Last", "MsPerMinute", kNormalSpeed), 20, 20000);
    st.weatherMode = std::clamp(IniInt("Last", "WeatherMode", WM_SERVER), 0, 2);
    st.weather = std::clamp(IniInt("Last", "Weather", 0), 0, 22);
}

void SaveSettings() {
    g_dirty = false;
    if (!g_remember) return;
    IniPut("Last", "TimeMode", st.timeMode);
    IniPut("Last", "Hour", st.hour);
    IniPut("Last", "Minute", st.minute);
    IniPut("Last", "MsPerMinute", st.msPerMinute);
    IniPut("Last", "WeatherMode", st.weatherMode);
    IniPut("Last", "Weather", st.weather);
}

// ============================================================================ applying the choice to the game
// What the server had set before we took over, so "server" gives it back.
struct Owned {
    bool time = false, weather = false;
    uint32_t origPerMinute = kNormalSpeed;
    int serverHour = -1, serverMinute = -1;
    bool serverSeen = false;        // the server moved the clock while we were holding it: serverHour/Minute is its value
    int16_t serverForced = -1, serverNew = 0;
    int wroteHour = -1, wroteMinute = -1;  // what we put in the clock last frame (fixed mode): a different value is the server's
    int wroteWeather = -1;                 // the same for the custom weather
    double expect = 0.0;  // running clock: the minute of the day we expect the game to be at
    bool expectValid = false;
    uint32_t lastNow = 0;
    bool autoValid = false;                // auto weather: prevNew / prevOld hold last frame's values
    int16_t prevNew = 0, prevOld = 0;
} own;

int CircDiff(int a, int b) { return ((a - b) % 1440 + 1440 + 720) % 1440 - 720; }

void SetClock(int h, int m) {
    game::Hours() = (uint8_t)h;
    game::Minutes() = (uint8_t)m;
    game::Seconds() = 0;
    game::LastTick() = game::TimeMs();
}

void ApplyTime(uint32_t now) {
    if (st.timeMode == TM_SERVER) {
        if (own.time) {  // hand it back: the server's last clock if it set one meanwhile, else carry on from where we are
            own.time = false;
            game::MsPerMinute() = own.origPerMinute;
            if (own.serverSeen) SetClock(own.serverHour, own.serverMinute);
            else game::LastTick() = now;
            Log("time: back to the server (%02d:%02d)", game::Hours(), game::Minutes());
        }
        return;
    }
    if (!own.time) {
        own.time = true;
        own.serverSeen = false;
        own.wroteHour = own.wroteMinute = -1;
        own.origPerMinute = game::MsPerMinute();
        own.expectValid = false;
        Log("time: taking over (server clock was %02d:%02d, %u ms per minute)", game::Hours(), game::Minutes(), own.origPerMinute);
    }
    if (st.timeMode == TM_FIXED) {
        game::MsPerMinute() = own.origPerMinute;
        if (own.wroteHour >= 0 && (game::Hours() != own.wroteHour || game::Minutes() != own.wroteMinute)) {
            // something else (the server) moved the clock since our last write: remember it for "back to the server"
            own.serverHour = game::Hours();
            own.serverMinute = game::Minutes();
            own.serverSeen = true;
        }
        SetClock(st.hour, st.minute);
        own.wroteHour = st.hour;
        own.wroteMinute = st.minute;
        return;
    }
    own.wroteHour = own.wroteMinute = -1;
    // running: the game's own clock ticks, at our speed. If the server moves it, put it back where it should be.
    game::MsPerMinute() = (uint32_t)st.msPerMinute;
    if (!own.expectValid) {
        own.expect = st.hour * 60 + st.minute;
        own.expectValid = true;
        own.lastNow = now;
        SetClock(st.hour, st.minute);
        return;
    }
    own.expect += (now - own.lastNow) / (double)st.msPerMinute;
    own.lastNow = now;
    if (own.expect >= 1440.0) own.expect -= 1440.0 * std::floor(own.expect / 1440.0);
    const int exp = (int)own.expect;
    const int cur = game::Hours() * 60 + game::Minutes();
    if (std::abs(CircDiff(cur, exp)) >= 2) {  // the server moved it
        own.serverHour = game::Hours();
        own.serverMinute = game::Minutes();
        own.serverSeen = true;
        SetClock(exp / 60, exp % 60);
    }
    st.hour = game::Hours();  // the panel shows the live time
    st.minute = game::Minutes();
}

void ApplyWeather() {
    if (st.weatherMode == WM_SERVER) {
        if (own.weather) {  // hand it back
            own.weather = false;
            game::ForcedWeather() = own.serverForced;
            const int16_t w = own.serverForced >= 0 ? own.serverForced : own.serverNew;
            game::NewWeather() = w;
            game::OldWeather() = w;
            Log("weather: back to the server (type %d)", (int)w);
        }
        return;
    }
    if (!own.weather) {
        own.weather = true;
        own.wroteWeather = -1;
        own.autoValid = false;
        own.serverForced = game::ForcedWeather();
        own.serverNew = game::NewWeather();
        Log("weather: taking over (server had forced %d, current %d)", (int)own.serverForced, (int)own.serverNew);
    }
    if (st.weatherMode == WM_CUSTOM) {
        own.autoValid = false;
        if (own.wroteWeather >= 0 && game::ForcedWeather() != own.wroteWeather) {
            // the server set a weather since our last write: remember it for "back to the server"
            own.serverForced = game::ForcedWeather();
            own.serverNew = game::ForcedWeather() >= 0 ? game::ForcedWeather() : game::NewWeather();
        }
        game::ForcedWeather() = (int16_t)st.weather;
        game::NewWeather() = (int16_t)st.weather;
        game::OldWeather() = (int16_t)st.weather;
        own.wroteWeather = st.weather;
        return;
    }
    // auto, like single-player: no weather is forced, so the game moves on to the next one of its list every game hour
    own.wroteWeather = -1;
    if (own.autoValid && game::ForcedWeather() >= 0) {
        // the server forced a weather since last frame (it sets the current and the previous one too): remember it for
        // "back to the server" and undo what it did to the blend
        own.serverForced = game::ForcedWeather();
        own.serverNew = game::ForcedWeather();
        game::NewWeather() = own.prevNew;
        game::OldWeather() = own.prevOld;
    }
    game::ForcedWeather() = -1;
    if (own.autoValid && game::NewWeather() != own.prevNew) Log("weather: the game moved on by itself to type %d (%s)", (int)game::NewWeather(), kWeatherNames[std::clamp((int)game::NewWeather(), 0, 22)]);
    own.prevNew = game::NewWeather();
    own.prevOld = game::OldWeather();
    own.autoValid = true;
}

// ============================================================================ the panel
struct Batch {
    std::vector<game::RwIm2DVertex> v;
    float z = 0.0f;
    void Tri(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c) {
        v.push_back({x1, y1, z, 1.0f, c, 0, 0});
        v.push_back({x2, y2, z, 1.0f, c, 0, 0});
        v.push_back({x3, y3, z, 1.0f, c, 0, 0});
    }
    void Rect(float x, float y, float w, float h, uint32_t c) {
        Tri(x, y, x + w, y, x + w, y + h, c);
        Tri(x, y, x + w, y + h, x, y + h, c);
    }
    void Flush() {
        if (v.empty()) return;
        uintptr_t raster = 0;
        game::RwRenderStateGet(game::rwRS_TEXTURERASTER, &raster);
        game::RwRenderStateSet(game::rwRS_TEXTURERASTER, 0);
        game::RwRenderStateSet(game::rwRS_ZTESTENABLE, 0);
        game::RwRenderStateSet(game::rwRS_ZWRITEENABLE, 0);
        game::RwRenderStateSet(game::rwRS_VERTEXALPHAENABLE, 1);
        game::RwRenderStateSet(game::rwRS_SRCBLEND, game::rwBLENDSRCALPHA);
        game::RwRenderStateSet(game::rwRS_DESTBLEND, game::rwBLENDINVSRCALPHA);
        game::RwRenderStateSet(game::rwRS_FOGENABLE, 0);
        game::RwRenderStateSet(game::rwRS_CULLMODE, game::rwCULLMODENONE);
        for (size_t i = 0; i < v.size(); i += 3000) {
            int cnt = (int)std::min<size_t>(3000, v.size() - i);
            game::RwIm2DRenderPrimitive(game::rwPRIMTYPETRILIST, &v[i], cnt);
        }
        game::RwRenderStateSet(game::rwRS_TEXTURERASTER, raster);
        v.clear();
    }
} g_batch;

inline uint32_t Argb(int a, int r, int g, int b) { return (uint32_t(a & 255) << 24) | ((r & 255) << 16) | ((g & 255) << 8) | (b & 255); }
inline uint32_t Rgba(int r, int g, int b, int a) { return (r & 255) | ((g & 255) << 8) | ((b & 255) << 16) | (uint32_t(a & 255) << 24); }

// In game text '~' starts a control code and the font code runs past the end of the string looking for the closing '~',
// so a lone '~' crashes the game. Strings starting with '*' are skipped by PrintString. Keep both out.
std::string SafeText(const char* s) {
    std::string out;
    for (; s && *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c < 0x20) continue;
        out += c == '~' ? '-' : (char)c;
    }
    if (!out.empty() && out[0] == '*') out[0] = '+';
    return out;
}

void Notify(const std::string& text) {
    const std::string safe = SafeText(text.c_str());
    game::SetHelpMessage(safe.c_str(), true, false, false);
}

enum Row { R_STYLE, R_TIMEMODE, R_HOUR, R_MINUTE, R_SPEED, R_PRESET, R_WEATHERMODE, R_WEATHER, R_COUNT };
bool g_open = false;
int g_sel = R_STYLE;

struct Line {
    float x, y, size;
    uint32_t color;  // CRGBA
    std::string text;
};
std::vector<Line> g_lines;

const char* TimeModeName(int m) { return m == TM_SERVER ? "Server" : m == TM_FIXED ? "Fixed" : "Running"; }
const char* WeatherModeName(int m) { return m == WM_SERVER ? "Server" : m == WM_CUSTOM ? "Custom" : "Auto"; }

std::string SpeedText(int ms) {
    const double dayMin = 1440.0 * ms / 60000.0;
    char b[64];
    if (ms == kNormalSpeed) std::snprintf(b, sizeof b, "normal (a day = %.0f min)", dayMin);
    else if (dayMin >= 10) std::snprintf(b, sizeof b, "a day = %.0f min", dayMin);
    else std::snprintf(b, sizeof b, "a day = %.1f min", dayMin);
    return b;
}

int SpeedIndex() {
    int best = 0;
    for (int i = 0; i < kNumSpeeds; i++)
        if (std::abs(kSpeeds[i] - st.msPerMinute) < std::abs(kSpeeds[best] - st.msPerMinute)) best = i;
    return best;
}

bool SinglePlayerStyleOn() { return st.timeMode == TM_RUNNING && st.msPerMinute == kNormalSpeed && st.weatherMode == WM_AUTO; }

// Builds the rectangles (into g_batch) and the text lines of the panel for this frame.
void BuildPanel(float W, float H) {
    (void)W;
    g_lines.clear();
    const float u = H / 448.0f;  // the game's own 640x448 layout unit
    const float px = 22 * u, py = 95 * u, pw = 306 * u, rowH = 17.5f * u, pad = 9 * u, valX = px + 108 * u;  // below SA-MP's chat lines
    const float lineSize = 15.5f * u;
    float y = py;
    const float panelH = (R_COUNT + 5.1f) * rowH + 2 * pad;
    g_batch.z = game::RwNearScreenZ();
    g_batch.Rect(px, py, pw, panelH, Argb(190, 10, 12, 18));
    g_batch.Rect(px, py, pw, 2.5f * u, Argb(255, 255, 170, 40));

    auto text = [&](float x, float yy, uint32_t c, const std::string& s) { g_lines.push_back({x, yy, lineSize, c, s}); };
    const uint32_t white = Rgba(255, 255, 255, 255), grey = Rgba(160, 165, 175, 255), gold = Rgba(255, 190, 70, 255),
                   blue = Rgba(120, 200, 255, 255), dim = Rgba(110, 115, 125, 255), green = Rgba(120, 230, 140, 255);

    y += pad;
    text(px + pad, y, gold, "TIME & WEATHER");
    char b[160];
    std::snprintf(b, sizeof b, "now %02d:%02d", game::Hours(), game::Minutes());
    text(px + pw - 75 * u, y, grey, b);
    y += rowH * 1.25f;

    // current values as shown on the panel (the running clock is shown live)
    const int hour = st.timeMode == TM_SERVER ? game::Hours() : st.hour;
    const int minute = st.timeMode == TM_SERVER ? game::Minutes() : st.minute;
    const int wType = st.weatherMode == WM_CUSTOM ? st.weather : std::clamp((int)game::NewWeather(), 0, 22);

    auto row = [&](int id, const char* label, const std::string& value, float bar /* <0: none */, bool enabled, uint32_t valueColor = 0) {
        const bool sel = g_sel == id;
        if (sel) g_batch.Rect(px + 3 * u, y - 1.5f * u, pw - 6 * u, rowH, Argb(120, 255, 170, 40));
        text(px + pad, y, enabled ? (sel ? white : grey) : dim, label);
        text(valX, y, enabled ? (valueColor ? valueColor : sel ? gold : white) : dim, sel ? "< " + value + " >" : value);
        if (bar >= 0.0f) {
            const float bx = px + pw - 52 * u, bw = 42 * u;
            g_batch.Rect(bx, y + 4.5f * u, bw, 4.0f * u, Argb(110, 255, 255, 255));
            g_batch.Rect(bx, y + 4.5f * u, bw * bar, 4.0f * u, Argb(255, 255, 170, 40));
        }
        y += rowH;
    };

    row(R_STYLE, "Quick start", SinglePlayerStyleOn() ? "Single-player style: on" : "Single-player style", -1.0f, true, SinglePlayerStyleOn() ? green : 0);
    y += rowH * 0.3f;
    row(R_TIMEMODE, "Time", TimeModeName(st.timeMode), -1.0f, true);
    std::snprintf(b, sizeof b, "%02d", hour);
    row(R_HOUR, "Hour", b, hour / 23.0f, true);
    std::snprintf(b, sizeof b, "%02d", minute);
    row(R_MINUTE, "Minute", b, minute / 59.0f, true);
    row(R_SPEED, "Clock speed", st.timeMode == TM_RUNNING ? SpeedText(st.msPerMinute) : std::string("(Running only)"), -1.0f, st.timeMode == TM_RUNNING);
    std::snprintf(b, sizeof b, "%s %02d:%02d", kPresets[st.preset].name, kPresets[st.preset].hour, kPresets[st.preset].minute);
    row(R_PRESET, "Preset", b, -1.0f, true);
    y += rowH * 0.3f;
    row(R_WEATHERMODE, "Weather", st.weatherMode == WM_AUTO ? "Auto (like single-player)" : WeatherModeName(st.weatherMode), -1.0f, true);
    std::snprintf(b, sizeof b, "%d  %s", wType, kWeatherNames[wType]);
    row(R_WEATHER, "Type", b, -1.0f, true);

    y += rowH * 0.3f;
    std::snprintf(b, sizeof b, "%s/%s choose   %s/%s change%s", BindingName(keys.up).c_str(), BindingName(keys.down).c_str(),
                  BindingName(keys.left).c_str(), BindingName(keys.right).c_str(),
                  g_sel == R_STYLE || g_sel == R_PRESET ? ("   " + BindingName(keys.select) + " apply").c_str()
                  : g_sel == R_TIMEMODE || g_sel == R_WEATHERMODE ? ("   " + BindingName(keys.select) + " switch").c_str() : "");
    text(px + pad, y, blue, b);
    y += rowH * 0.95f;
    std::snprintf(b, sizeof b, "%s: give everything back to the server", BindingName(keys.reset).c_str());
    text(px + pad, y, blue, b);
    y += rowH * 0.95f;
    std::snprintf(b, sizeof b, "%s: close", BindingName(keys.open).c_str());
    text(px + pad, y, blue, b);
}

void DrawPanel() {
    if (!g_open || !game::HudVisible() || game::Paused()) return;
    const float W = (float)game::ScreenW(), H = (float)game::ScreenH();
    if (W < 64 || H < 64) return;
    BuildPanel(W, H);
    g_batch.Flush();
    game::font::SetBackground(false, false);
    game::font::SetProportional(true);
    game::font::SetJustify(false);
    game::font::SetFontStyle(1);
    game::font::SetWrapx(W);
    game::font::SetAlphaFade(255.0f);
    game::font::SetDropShadowPosition(0);
    game::font::SetEdge(1);
    game::font::SetDropColor(Rgba(0, 0, 0, 255));
    game::font::SetOrientation(1);  // left aligned
    for (const Line& l : g_lines) {
        const float k = l.size / 15.5f;
        game::font::SetScale(0.36f * k, 0.9f * k);
        game::font::SetColor(l.color);
        const std::string safe = SafeText(l.text.c_str());
        game::font::PrintString(l.x, l.y, safe.c_str());
    }
}

// ============================================================================ input
bool Held(const Binding& b) {
    if (!b.vk) return false;
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return held(b.vk) && (!b.ctrl || held(VK_CONTROL)) && (!b.shift || held(VK_SHIFT)) && (!b.alt || held(VK_MENU));
}

// A key that repeats while it is held (after a short delay, faster the longer it is held). Returns how many steps to take.
struct KeyRepeat {
    bool down = false;
    uint32_t since = 0, last = 0;
    int Poll(const Binding& b, uint32_t now) {
        if (!Held(b)) { down = false; return 0; }
        if (!down) { down = true; since = last = now; return 1; }
        const uint32_t held = now - since;
        if (held < 350) return 0;
        const uint32_t interval = held > 2500 ? 25 : held > 1200 ? 45 : 90;
        if (now - last < interval) return 0;
        last = now;
        return held > 3500 ? 3 : 1;
    }
};
struct KeyEdge {
    bool down = false;
    bool Pressed(const Binding& b) {
        const bool d = Held(b);
        const bool p = d && !down;
        down = d;
        return p;
    }
};

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Make the time a choice of ours before changing it: switching from "server" starts from what the game shows now.
void StartFixedFromClock() {
    if (st.timeMode != TM_SERVER) return;
    st.timeMode = TM_FIXED;
    st.hour = game::Hours();
    st.minute = game::Minutes();
}

void AdjustTime(int deltaMinutes) {
    StartFixedFromClock();
    if (st.timeMode == TM_RUNNING) {  // the live clock is the truth
        st.hour = game::Hours();
        st.minute = game::Minutes();
    }
    const int total = ((st.hour * 60 + st.minute + deltaMinutes) % 1440 + 1440) % 1440;
    st.hour = total / 60;
    st.minute = total % 60;
    own.expectValid = false;  // running: start again from the new time
    g_dirty = true;
}

void SetTimeMode(int mode) {
    mode = ((mode % 3) + 3) % 3;
    Log("you set the time mode to %s", TimeModeName(mode));
    if (mode != TM_SERVER && st.timeMode == TM_SERVER) {
        st.hour = game::Hours();
        st.minute = game::Minutes();
    }
    st.timeMode = mode;
    own.expectValid = false;
    g_dirty = true;
}

void SetWeatherMode(int mode) {
    mode = ((mode % 3) + 3) % 3;
    if (mode == WM_CUSTOM && st.weatherMode != WM_CUSTOM) st.weather = std::clamp((int)game::NewWeather(), 0, 22);
    st.weatherMode = mode;
    own.autoValid = false;
    g_dirty = true;
}

void ApplyPreset() {
    StartFixedFromClock();
    st.hour = kPresets[st.preset].hour;
    st.minute = kPresets[st.preset].minute;
    own.expectValid = false;
    g_dirty = true;
}

// A game day in 24 minutes (the clock runs at the game's normal speed, from the time it is now) and the weather changes by
// itself every game hour, as in single-player.
void ApplySinglePlayerStyle() {
    if (st.timeMode == TM_SERVER) {
        st.hour = game::Hours();
        st.minute = game::Minutes();
    }
    st.timeMode = TM_RUNNING;
    st.msPerMinute = kNormalSpeed;
    own.expectValid = false;
    SetWeatherMode(WM_AUTO);
    g_dirty = true;
}

void GiveBackToServer() {
    Log("you asked for everything back to the server");
    st.timeMode = TM_SERVER;
    st.weatherMode = WM_SERVER;
    own.expectValid = false;
    own.autoValid = false;
    g_dirty = true;
}

// Left/Right (dir = -1/+1, steps = repeat count) or Enter (dir = 0) on the selected row.
void Change(int dir, int steps) {
    switch (g_sel) {
        case R_STYLE: ApplySinglePlayerStyle(); break;
        case R_TIMEMODE: SetTimeMode(st.timeMode + (dir ? dir : 1)); break;
        case R_HOUR: if (dir) AdjustTime(dir * 60 * steps); break;
        case R_MINUTE: if (dir) AdjustTime(dir * steps * (steps > 1 ? 5 : 1)); break;
        case R_SPEED:
            if (st.timeMode != TM_RUNNING) break;
            st.msPerMinute = kSpeeds[std::clamp(SpeedIndex() + (dir > 0 ? 1 : -1), 0, kNumSpeeds - 1)];  // kSpeeds is slow -> fast: right = faster
            own.expectValid = false;
            g_dirty = true;
            break;
        case R_PRESET:
            if (dir) st.preset = ((st.preset + dir) % kNumPresets + kNumPresets) % kNumPresets;
            ApplyPreset();
            break;
        case R_WEATHERMODE: SetWeatherMode(st.weatherMode + (dir ? dir : 1)); break;
        case R_WEATHER:
            if (st.weatherMode != WM_CUSTOM) SetWeatherMode(WM_CUSTOM);
            if (dir) st.weather = ((st.weather + dir * steps) % 23 + 23) % 23;
            g_dirty = true;
            break;
        default: break;
    }
}

void PanelInput(uint32_t now) {
    static KeyRepeat up, down, left, right;
    static KeyEdge enter, back;
    const int u = up.Poll(keys.up, now), d = down.Poll(keys.down, now), l = left.Poll(keys.left, now), r = right.Poll(keys.right, now);
    if (u) g_sel = (g_sel + R_COUNT - 1) % R_COUNT;
    if (d) g_sel = (g_sel + 1) % R_COUNT;
    if (l) Change(-1, l);
    if (r) Change(+1, r);
    if (enter.Pressed(keys.select)) Change(0, 1);
    if (back.Pressed(keys.reset)) GiveBackToServer();
}

std::string TimeText() {  // from the setting (the game's clock is only updated a moment later)
    char b[96];
    std::snprintf(b, sizeof b, "Time %02d:%02d (%s)", st.hour, st.minute, st.timeMode == TM_FIXED ? "stays fixed" : "running");
    return b;
}

// Optional keys that work without opening the panel; each one says what it did.
void QuickKeys() {
    static KeyEdge fwd, bck, nxt, prv, sp, give;
    if (fwd.Pressed(keys.timeForward)) { AdjustTime(60); Notify(TimeText()); }
    if (bck.Pressed(keys.timeBack)) { AdjustTime(-60); Notify(TimeText()); }
    const bool n = nxt.Pressed(keys.weatherNext), p = prv.Pressed(keys.weatherPrev);
    if (n || p) {
        if (st.weatherMode != WM_CUSTOM) SetWeatherMode(WM_CUSTOM);
        st.weather = ((st.weather + (n ? 1 : -1)) % 23 + 23) % 23;
        g_dirty = true;
        char b[96];
        std::snprintf(b, sizeof b, "Weather %d: %s", st.weather, kWeatherNames[st.weather]);
        Notify(b);
    }
    if (sp.Pressed(keys.spStyle)) { Log("quick key: single-player style"); ApplySinglePlayerStyle(); Notify("Single-player style: a day in 24 minutes, weather changes by itself"); }
    if (give.Pressed(keys.giveBack)) { GiveBackToServer(); Notify("Time and weather: back to the server"); }
}

// ============================================================================ the per-frame logic
void OnGameFrame() {
    static bool inited = false;
    static KeyEdge openKey;
    static uint32_t lastSave = 0;
    if (!inited) {
        inited = true;
        LoadSettings();
        Log("Keys: open %s, up %s, down %s, left %s, right %s, select %s, give back %s", BindingName(keys.open).c_str(), BindingName(keys.up).c_str(),
            BindingName(keys.down).c_str(), BindingName(keys.left).c_str(), BindingName(keys.right).c_str(), BindingName(keys.select).c_str(),
            BindingName(keys.reset).c_str());
        Log("Quick keys: hour+ %s, hour- %s, weather+ %s, weather- %s, single-player style %s, all to server %s", BindingName(keys.timeForward).c_str(),
            BindingName(keys.timeBack).c_str(), BindingName(keys.weatherNext).c_str(), BindingName(keys.weatherPrev).c_str(),
            BindingName(keys.spStyle).c_str(), BindingName(keys.giveBack).c_str());
        Log("Settings: time %s %02d:%02d (%d ms per minute), weather %s %d", TimeModeName(st.timeMode), st.hour, st.minute, st.msPerMinute,
            WeatherModeName(st.weatherMode), st.weather);
    }
    const bool focus = GameHasFocus();
    if (g_log && g_logOn) {  // log only: a bound key is down but nothing will act on it this frame - say why
        static uint32_t lastDiag = 0;
        const bool anyHeld = Held(keys.open) || Held(keys.spStyle) || Held(keys.weatherNext) || Held(keys.weatherPrev) ||
                             Held(keys.timeForward) || Held(keys.timeBack) || Held(keys.giveBack);
        if (anyHeld && (game::Paused() || !focus || !game::HudVisible()) && GetTickCount() - lastDiag > 400) {
            lastDiag = GetTickCount();
            Log("a bound key is down but ignored: paused %d, game window in front %d, HUD visible %d (wide-screen bars %d, cutscene %d, display-hud %d, hud-on %d)",
                game::Paused() ? 1 : 0, focus ? 1 : 0, game::HudVisible() ? 1 : 0, *(volatile uint8_t*)0xB6F065, *(volatile uint8_t*)0xB5F851,
                *(volatile uint8_t*)0xA444A0, *(volatile uint8_t*)0xBA6769);
        }
    }
    if (game::Paused()) return;
    const uint32_t now = game::TimeMs();
    if (focus) {
        if (openKey.Pressed(keys.open)) {
            if (g_open) { g_open = false; if (g_dirty) SaveSettings(); }
            else if (game::HudVisible()) g_open = true;
            Log("panel %s", g_open ? "opened" : "closed");
        }
    } else {
        openKey.Pressed(keys.open);
    }
    if (g_open) {
        if (!game::HudVisible()) {
            g_open = false;
        } else {
            if (focus) PanelInput(now);
            game::SwallowInput();
        }
    } else if (focus && game::HudVisible()) {
        QuickKeys();
    }
    ApplyTime(now);
    ApplyWeather();
    if (g_dirty && !g_open && GetTickCount() - lastSave > 1000) { lastSave = GetTickCount(); SaveSettings(); }
}

// ============================================================================ hooks
using VoidFn = void(__cdecl*)();
VoidFn g_origUpdatePads = nullptr, g_origRender2d = nullptr;
VoidFn g_baseUpdatePads = nullptr, g_baseRender2d = nullptr;  // what the calls went to when we first hooked them
uint32_t g_lastLogicTick = 0, g_lastRenderTick = 0;          // hook watchdogs

void CheckRenderHook();
void CheckLogicHook();

void __cdecl Hook_UpdatePads() {
    // If another mod hooked on top of us and we re-chained over it, its hook calls back into ours: run the original once
    // instead of recursing forever.
    static int depth = 0;
    if (depth) { g_baseUpdatePads(); return; }
    ++depth;
    g_origUpdatePads();
    --depth;
    g_lastLogicTick = GetTickCount();
    OnGameFrame();
    CheckRenderHook();
}

void __cdecl Hook_Render2dStuff() {
    static int depth = 0;
    if (depth) { g_baseRender2d(); return; }
    g_lastRenderTick = GetTickCount();
    CheckLogicHook();
    DrawPanel();
    ++depth;
    g_origRender2d();
    --depth;
}

bool PatchCall(uintptr_t site, void* hook, uintptr_t expected, VoidFn& orig, const char* name) {
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) { Log("Hook %s: unexpected byte %02X at %08X - not installed", name, p[0], (unsigned)site); return false; }
    const uintptr_t target = site + 5 + *(int32_t*)(p + 1);
    if (target != expected) Log("Hook %s: call already redirected to %08X by another mod - chaining", name, (unsigned)target);
    orig = (VoidFn)target;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) { Log("Hook %s: VirtualProtect failed", name); return false; }
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)hook - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// SA-MP replaces the Render2dStuff call with its own hook (its code calls the original function directly), which silently
// removes ours: the panel would never be drawn. If the game keeps running but our render hook is no longer called, chain it
// back in front of whatever took its place. The same the other way round for the per-frame logic hook.
void CheckRenderHook() {
    static uint32_t firstSeen = 0, lastRepair = 0;
    static int repairs = 0;
    const uint32_t t = GetTickCount();
    if (!firstSeen) firstSeen = t;
    const uint32_t last = std::max(g_lastRenderTick, firstSeen);
    if (t - last < 2500 || repairs >= 8 || (lastRepair && t - lastRepair < 2500)) return;
    lastRepair = t;
    const uintptr_t site = game::CALL_Render2d;
    const uint8_t* p = (const uint8_t*)site;
    const uintptr_t target = p[0] == 0xE8 ? site + 5 + *(const int32_t*)(p + 1) : 0;
    if (!target || target == (uintptr_t)&Hook_Render2dStuff) return;
    repairs++;
    Log("Render2dStuff hook was replaced by another mod (call now goes to %08X) - re-chaining", (unsigned)target);
    PatchCall(site, (void*)&Hook_Render2dStuff, game::FN_Render2d, g_origRender2d, "Render2dStuff");
}

void CheckLogicHook() {
    static uint32_t firstSeen = 0;
    static int repairs = 0;
    const uint32_t t = GetTickCount();
    if (!firstSeen) firstSeen = t;
    if (game::Paused()) { g_lastLogicTick = t; return; }
    const uint32_t last = g_lastLogicTick ? g_lastLogicTick : firstSeen;
    if (t - last < 3000 || repairs >= 3) return;
    g_lastLogicTick = t;
    const uintptr_t site = game::CALL_UpdatePads;
    const uint8_t* p = (const uint8_t*)site;
    const uintptr_t target = p[0] == 0xE8 ? site + 5 + *(const int32_t*)(p + 1) : 0;
    if (target == (uintptr_t)&Hook_UpdatePads) return;
    repairs++;
    Log("UpdatePads hook was replaced by another mod (call now goes to %08X) - re-chaining", (unsigned)target);
    if (target) PatchCall(site, (void*)&Hook_UpdatePads, game::FN_UpdatePads, g_origUpdatePads, "UpdatePads");
}

void Init(HMODULE self) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);
    g_logOn = IniInt("TimeWeather", "Log", 0) != 0;
    g_logT0 = GetTickCount() / 1000.0;
    if (g_logOn) g_log = std::fopen((g_dir + "TimeWeather.log").c_str(), "w");
    Log("TimeWeather.asi v1 starting");
    if (!game::IsSupportedExe()) { Log("Unsupported gta_sa.exe (need 1.0 US) - TimeWeather disabled"); return; }
    const bool ok = PatchCall(game::CALL_UpdatePads, (void*)&Hook_UpdatePads, game::FN_UpdatePads, g_origUpdatePads, "UpdatePads");
    g_baseUpdatePads = g_origUpdatePads;
    if (ok) {
        PatchCall(game::CALL_Render2d, (void*)&Hook_Render2dStuff, game::FN_Render2d, g_origRender2d, "Render2dStuff");
        g_baseRender2d = g_origRender2d;
    }
    Log(ok ? "Hooks installed" : "Hooks NOT installed - TimeWeather disabled");
}

}  // namespace tw

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        tw::Init(inst);
    }
    return TRUE;
}
