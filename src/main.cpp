// Click Recorder - records your own clicks as .gdr2 files and replays them
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

struct ClickInput {
    int frame;   // physics tick the click happened on
    int button;  // 1 = jump, 2 = left, 3 = right (platformer)
    bool down;   // true = press, false = release
    bool p2;     // player 2 (two-player mode only)
};

enum class Mode { Record, Replay, Off };

// recording state
std::vector<ClickInput> g_inputs;
bool g_savedThisAttempt = false;

// replay state
std::vector<ClickInput> g_replay;
size_t g_replayIndex = 0;
bool g_replayActive = false;
bool g_injecting = false;

bool boolSetting(char const* key) { return Mod::get()->getSettingValue<bool>(key); }
double tpsSetting() { return Mod::get()->getSettingValue<double>("tps"); }

Mode currentMode() {
    auto m = Mod::get()->getSettingValue<std::string>("mode");
    if (m == "Replay") return Mode::Replay;
    if (m == "Off") return Mode::Off;
    return Mode::Record;
}

void notify(std::string const& text, NotificationIcon icon) {
    if (boolSetting("show-notifications") || icon == NotificationIcon::Error) {
        Notification::create(text, icon)->show();
    }
}

auto byFrame = [](ClickInput const& a, ClickInput const& b) { return a.frame < b.frame; };

// ---------- .gdr2 writer ----------
struct Writer {
    std::vector<uint8_t> out;
    void byte(uint8_t b) { out.push_back(b); }
    void varint(uint64_t v) {
        while (v >= 0x80) { byte(uint8_t((v & 0x7f) | 0x80)); v >>= 7; }
        byte(uint8_t(v));
    }
    void str(std::string const& s) {
        varint(s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
    void f32(float f) {
        uint32_t u; std::memcpy(&u, &f, 4);
        for (int i = 3; i >= 0; --i) byte(uint8_t(u >> (i * 8)));
    }
    void f64(double d) {
        uint64_t u; std::memcpy(&u, &d, 8);
        for (int i = 7; i >= 0; --i) byte(uint8_t(u >> (i * 8)));
    }
};

// ---------- .gdr2 reader ----------
struct Reader {
    std::vector<uint8_t> const& d;
    size_t p = 0;
    bool ok = true;
    uint8_t byte() {
        if (p >= d.size()) { ok = false; return 0; }
        return d[p++];
    }
    uint64_t varint() {
        uint64_t r = 0; int s = 0;
        while (ok) {
            uint8_t b = byte();
            r |= uint64_t(b & 0x7f) << s;
            s += 7;
            if (b < 0x80 || s > 63) break;
        }
        return r;
    }
    std::string str() {
        auto n = varint();
        if (!ok || p + n > d.size()) { ok = false; return {}; }
        std::string s(d.begin() + p, d.begin() + p + n);
        p += n;
        return s;
    }
    void skip(size_t n) {
        if (p + n > d.size()) { ok = false; return; }
        p += n;
    }
    double f64() {
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u = (u << 8) | byte();
        double v; std::memcpy(&v, &u, 8);
        return v;
    }
};

struct LoadedReplay {
    std::vector<ClickInput> inputs;
    double tps = 240;
    std::string levelName;
    bool platformer = false;
};

Result<LoadedReplay> loadGdr2(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Err("Couldn't open the replay file");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 4 || data[0] != 'G' || data[1] != 'D' || data[2] != 'R') {
        return Err("That file isn't a .gdr2 click file");
    }

    Reader r{data};
    r.p = 3;
    LoadedReplay out;
    r.varint();                 // format version
    r.str(); r.str(); r.str();  // input tag, author, description
    r.skip(4);                  // duration
    r.varint();                 // game version
    out.tps = r.f64();          // framerate / TPS
    r.varint(); r.varint();     // seed, coins
    r.byte();                   // low detail mode
    out.platformer = r.byte() != 0;
    r.str(); r.varint();        // bot name, bot version
    r.varint();                 // level id
    out.levelName = r.str();
    r.skip(size_t(r.varint())); // extensions
    auto deaths = r.varint();
    for (uint64_t i = 0; i < deaths && r.ok; ++i) r.varint();
    auto total = r.varint();
    auto p1 = r.varint();

    int frame = 0;
    for (uint64_t i = 0; i < total && r.ok; ++i) {
        if (i == p1) frame = 0;
        uint64_t x = r.varint();
        ClickInput in{};
        in.down = x & 1;
        in.p2 = i >= p1;
        if (out.platformer) {
            in.button = int((x >> 1) & 3);
            frame += int(x >> 3);
        } else {
            in.button = 1;
            frame += int(x >> 1);
        }
        in.frame = frame;
        out.inputs.push_back(in);
    }
    if (!r.ok) return Err("The replay file is damaged or cut off");
    if (!std::isfinite(out.tps) || out.tps <= 0) out.tps = 240;

    std::stable_sort(out.inputs.begin(), out.inputs.end(), byFrame);
    return Ok(std::move(out));
}

std::string safeName(std::string name) {
    for (auto& c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    if (name.empty()) name = "level";
    return name;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef GEODE_IS_WINDOWS
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

void saveRecording(PlayLayer* pl, bool completed) {
    if (g_inputs.empty() || !pl || !pl->m_level) return;

    double tps = tpsSetting();
    bool platformer = pl->m_isPlatformer;

    std::vector<ClickInput> p1, p2;
    for (auto const& in : g_inputs) (in.p2 ? p2 : p1).push_back(in);
    std::stable_sort(p1.begin(), p1.end(), byFrame);
    std::stable_sort(p2.begin(), p2.end(), byFrame);

    int lastFrame = 0;
    for (auto const& in : g_inputs) lastFrame = std::max(lastFrame, in.frame);

    std::string levelName = pl->m_level->m_levelName;
    std::string author = GameManager::get()->m_playerName;
    Writer w;
    w.out = {'G', 'D', 'R'};
    w.varint(2);                                          // format version
    w.str("");                                            // input tag
    w.str(author);                                        // author
    w.str(completed ? "completed" : "attempt");           // description
    w.f32(float(lastFrame / tps));                        // duration (s)
    w.varint(22081);                                      // game version
    w.f64(tps);                                           // framerate / TPS
    w.varint(0);                                          // seed
    w.varint(0);                                          // coins
    w.byte(0);                                            // low detail mode
    w.byte(platformer ? 1 : 0);                           // platformer
    w.str("ClickRecorder");                               // bot name
    w.varint(1);                                          // bot version
    w.varint(uint64_t(std::max(0, pl->m_level->m_levelID.value()))); // level id
    w.str(levelName);                                     // level name
    w.varint(0);                                          // extension size
    w.varint(0);                                          // deaths
    w.varint(p1.size() + p2.size());                      // total inputs
    w.varint(p1.size());                                  // player 1 inputs

    auto writeList = [&](std::vector<ClickInput> const& list) {
        int prev = 0;
        for (auto const& in : list) {
            uint64_t delta = uint64_t(std::max(0, in.frame - prev));
            prev = in.frame;
            if (platformer) w.varint((delta << 3) | (uint64_t(in.button & 3) << 1) | (in.down ? 1 : 0));
            else            w.varint((delta << 1) | (in.down ? 1 : 0));
        }
    };
    writeList(p1);
    writeList(p2);

    auto dir = Mod::get()->getSaveDir() / "recordings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto fileName = safeName(levelName) + "_" + timestamp() + (completed ? "" : "_attempt") + ".gdr2";
    auto path = dir / fileName;

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        log::error("Could not write {}", path.string());
        notify("Click Recorder: couldn't save the recording", NotificationIcon::Error);
        return;
    }
    f.write(reinterpret_cast<char const*>(w.out.data()), std::streamsize(w.out.size()));
    f.close();

    log::info("Saved {} inputs to {}", g_inputs.size(), path.string());
    notify("Saved clicks: " + fileName, NotificationIcon::Success);
}

} // namespace

// ---------- inputs: record yours, or play the file's ----------
class $modify(CRGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        bool inLevel = pl && pl == static_cast<GJBaseGameLayer*>(this);

        // During a replay, ignore your own clicks so they can't mess up the run
        if (inLevel && g_replayActive && !g_injecting) return;

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!inLevel || g_replayActive || currentMode() != Mode::Record) return;

        bool twoPlayer = m_levelSettings && m_levelSettings->m_twoPlayerMode;
        g_inputs.push_back({
            int(m_gameState.m_currentProgress),
            button,
            down,
            twoPlayer && !isPlayer1,
        });
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (g_replayActive && pl && pl == static_cast<GJBaseGameLayer*>(this)) {
            int now = int(m_gameState.m_currentProgress);
            g_injecting = true;
            while (g_replayIndex < g_replay.size() && g_replay[g_replayIndex].frame <= now) {
                auto const& in = g_replay[g_replayIndex++];
                this->handleButton(in.down, in.button, !in.p2);
            }
            g_injecting = false;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------- attempts, checkpoints, completion ----------
class $modify(CRPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_inputs.clear();
        g_savedThisAttempt = false;
        g_replay.clear();
        g_replayIndex = 0;
        g_replayActive = false;

        if (currentMode() == Mode::Replay) {
            auto path = Mod::get()->getSettingValue<std::filesystem::path>("replay-file");
            auto res = loadGdr2(path);
            if (!res) {
                notify("Click Recorder: " + res.unwrapErr(), NotificationIcon::Error);
            } else {
                auto rep = std::move(res).unwrap();
                g_replay = std::move(rep.inputs);
                g_replayActive = !g_replay.empty();
                if (std::abs(rep.tps - tpsSetting()) > 0.5) {
                    notify(fmt::format("Replay was made at {} TPS but the game is set to {}. Clicks may land in the wrong spots.",
                        rep.tps, tpsSetting()), NotificationIcon::Warning);
                }
                if (g_replayActive) {
                    notify(fmt::format("Replaying {} ({} clicks)", rep.levelName, g_replay.size() / 2), NotificationIcon::Info);
                }
            }
        }

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (g_replayActive) m_isTestMode = true;  // a replayed run never counts as your own progress
        return true;
    }

    void resetLevel() {
        // Save the attempt that just ended, if asked to
        if (!g_replayActive && currentMode() == Mode::Record && boolSetting("save-on-death")
            && !g_savedThisAttempt && !m_isPracticeMode) {
            saveRecording(this, false);
        }

        PlayLayer::resetLevel();
        g_savedThisAttempt = false;
        int now = int(m_gameState.m_currentProgress);

        if (g_replayActive) {
            m_isTestMode = true;
            // Restart the file from wherever this attempt starts (0, or a checkpoint)
            g_replayIndex = 0;
            while (g_replayIndex < g_replay.size() && g_replay[g_replayIndex].frame < now) ++g_replayIndex;
            return;
        }

        if (!m_isPracticeMode) {
            g_inputs.clear();
            return;
        }

        // Practice mode: keep clicks up to the checkpoint, drop the rest
        std::erase_if(g_inputs, [now](ClickInput const& in) { return in.frame > now; });

        // Release any button that was still held at the checkpoint
        for (int p = 0; p < 2; ++p) {
            for (int b = 1; b <= 3; ++b) {
                bool held = false;
                for (auto const& in : g_inputs)
                    if (in.p2 == bool(p) && in.button == b) held = in.down;
                if (held) g_inputs.push_back({now, b, false, bool(p)});
            }
        }
    }

    void levelComplete() {
        if (g_replayActive) {
            m_isTestMode = true;
        } else if (currentMode() == Mode::Record && !g_savedThisAttempt) {
            saveRecording(this, true);
            g_savedThisAttempt = true;
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_inputs.clear();
        g_replay.clear();
        g_replayActive = false;
        PlayLayer::onQuit();
    }
};
