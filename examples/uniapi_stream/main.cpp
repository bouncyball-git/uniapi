// uniapi demo: streams generated RGBA frames to a web page and measures how fast they get
// there. Replace RenderTestPattern with a real producer (video decoder, camera, renderer...).
//
// Settings come from <exe name>.conf next to the executable (written whenever the controls
// page applies new settings, or the Stats button or S key toggles the overlay), then from the
// command line, which overrides the file for this run only. The controls page sends the same
// options at runtime.
//
// Command line:
//   --timed                ~60 fps, paced by a timer (the default)
//   --benchmark            unthrottled: a new frame is sent as soon as a slot is released
//   --benchmark=transport  unthrottled, and neither renders nor draws: measures the
//                          hand-off alone
//   --slots=N              number of stream slots, 1-64 (default 3)
//   --size=WxH             frame resolution, e.g. --size=1920x1080 (default 640x360);
//                          presets: 360p, 720p, 1080p, 1440p, 4k, 8k (e.g. --size=4k)
//   --draw=MODE            how the page draws frames: 2d (default, GPU canvas),
//                          2d-cpu (canvas kept in CPU memory) or webgl
//   --stats=on|off         show the Native stats overlay (default off; also the Stats button)
//   --no-shared-memory     stream through the fetch fallback even where shared memory works
// Delivered fps and throughput are shown in the window title.
//
// What the pages call (see web/*.js):
//   getState() applySettings(options) run() clearHistory() saveResults() setStats(on)
// and the page's "draw-ms" event reports its draw time.

#include <uniapi/uniapi.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef UNIAPI_STREAM_EMBED_WEB
UNIAPI_ASSETS(stream_web);
#endif

namespace {

using uniapi::json;

constexpr char kWindowTitle[] = "uniapi stream";
constexpr int kFrameIntervalMs = 16;
constexpr int kMaxFrameDimension = 16384;            // Chromium's canvas limit per side
constexpr int kMaxSlots = 64;
constexpr size_t kMaxSharedBytes = size_t(4) << 30;  // all slots together
constexpr size_t kMaxHistory = 20;

enum class Mode { Timed, Benchmark, Transport };
const char* const kModeNames[] = {"timed", "benchmark", "transport"};

struct Settings {
    int width = 640;
    int height = 360;
    Mode mode = Mode::Timed;
    int slots = 3;            // one being drawn, one in flight, one being filled
    std::string draw = "2d";  // passed to the page as ?draw=
    bool stats = false;       // Native stats overlay on the demo page

    size_t FrameBytes() const { return size_t(width) * height * 4; }
};

// One run = the time the demo page spends streaming with one set of settings.
// Values are from its most recent full second; negative means "not measured".
struct RunResult {
    Settings settings;
    double fps = 0;
    double renderMs = -1;
    double drawMs = -1;
    std::time_t finished = 0;
};

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::tm LocalTime(std::time_t time) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return local;
}

std::string Format(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

// "/app/uniapi_stream.exe" -> "/app/uniapi_stream.conf"
std::string ConfigPath() {
    std::filesystem::path path = std::filesystem::u8path(uniapi::executablePath());
    return path.replace_extension(".conf").u8string();
}

// --- Settings: command line and .conf ---------------------------------------------------

// Whether `name` (e.g. "--size=") occurs in text; if so and `value` is given, it gets the text
// after it, up to the next space.
bool FindOption(std::string_view text, std::string_view name, std::string* value) {
    size_t at = text.find(name);
    if (at == std::string_view::npos) return false;
    if (value) {
        size_t start = at + name.size();
        size_t end = text.find_first_of(" \t", start);
        *value = std::string(text.substr(start, end == std::string_view::npos ? end : end - start));
    }
    return true;
}

// Parses "1920x1080" or a preset name.
bool ParseFrameSize(const std::string& value, int* width, int* height) {
    static const struct { const char* name; int width, height; } kPresets[] = {
        {"360p", 640, 360},    {"720p", 1280, 720}, {"1080p", 1920, 1080},
        {"1440p", 2560, 1440}, {"4k", 3840, 2160},  {"8k", 7680, 4320},
    };
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(tolower(c)); });
    for (const auto& preset : kPresets) {
        if (lower == preset.name) {
            *width = preset.width;
            *height = preset.height;
            return true;
        }
    }
    auto number = [](const std::string& digits, int* out) {
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        *out = std::stoi(digits);
        return true;
    };
    const size_t x = lower.find('x');
    int w = 0, h = 0;
    if (x == std::string::npos || !number(lower.substr(0, x), &w) || !number(lower.substr(x + 1), &h)) return false;
    if (w <= 0 || h <= 0 || w > kMaxFrameDimension || h > kMaxFrameDimension) return false;
    *width = w;
    *height = h;
    return true;
}

// Parses command-line style options on top of `base`; options not given keep its values.
bool ParseOptions(std::string_view text, const Settings& base, Settings* settings, std::string* error) {
    Settings result = base;
    std::string value;
    if (FindOption(text, "--benchmark=transport", nullptr)) {
        result.mode = Mode::Transport;
    } else if (FindOption(text, "--benchmark", nullptr)) {
        result.mode = Mode::Benchmark;
    } else if (FindOption(text, "--timed", nullptr)) {
        result.mode = Mode::Timed;
    }
    if (FindOption(text, "--slots=", &value)) {
        result.slots = atoi(value.c_str());
        if (result.slots < 1 || result.slots > kMaxSlots) {
            *error = "--slots must be between 1 and 64.";
            return false;
        }
    }
    if (FindOption(text, "--size=", &value) && !ParseFrameSize(value, &result.width, &result.height)) {
        *error = "Invalid --size. Use WxH (e.g. --size=1920x1080, up to 16384 per side) "
                 "or a preset: 360p, 720p, 1080p, 1440p, 4k, 8k.";
        return false;
    }
    if (FindOption(text, "--draw=", &value)) {
        if (value != "2d" && value != "2d-cpu" && value != "webgl") {
            *error = "Invalid --draw. Use 2d, 2d-cpu or webgl.";
            return false;
        }
        result.draw = value;
    }
    if (FindOption(text, "--stats=", &value)) {
        if (value != "on" && value != "off") {
            *error = "Invalid --stats. Use on or off.";
            return false;
        }
        result.stats = value == "on";
    }
    if (result.FrameBytes() * result.slots > kMaxSharedBytes) {
        *error = Format("These settings need %.1f GB of memory for the stream; the limit is 4 GB. "
                        "Use a smaller size or fewer slots.",
                        result.FrameBytes() * result.slots / 1e9);
        return false;
    }
    *settings = std::move(result);
    return true;
}

std::string Trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// The .conf file holds one "key = value" per line; '#' starts a comment.
//   size  = 1920x1080          or a preset: 360p, 720p, 1080p, 1440p, 4k, 8k
//   mode  = timed | benchmark | transport
//   slots = 3
//   draw  = 2d | 2d-cpu | webgl
//   stats = on | off
// Keys that are left out keep their defaults. A missing file is not an error.
bool LoadConfig(const std::string& path, Settings* settings, std::string* error) {
    std::ifstream file(std::filesystem::u8path(path));
    if (!file) return true;

    // Translate to command-line options so there is a single parser and validator.
    std::string options;
    std::string problem;
    std::string raw;
    for (int lineNumber = 1; problem.empty() && std::getline(file, raw); ++lineNumber) {
        std::string line = Trim(raw.substr(0, raw.find('#')));
        if (line.empty()) continue;
        size_t equals = line.find('=');
        std::string key = Trim(line.substr(0, equals));
        std::string value = equals == std::string::npos ? "" : Trim(line.substr(equals + 1));
        if (value.empty() || value.find_first_of(" \t") != std::string::npos) {
            problem = "expected \"key = value\"";
        } else if (key == "size" || key == "slots" || key == "draw" || key == "stats") {
            options += " --" + key + "=" + value;
        } else if (key == "mode") {
            if (value == "timed") options += " --timed";
            else if (value == "benchmark") options += " --benchmark";
            else if (value == "transport") options += " --benchmark=transport";
            else problem = "mode must be timed, benchmark or transport";
        } else {
            problem = "unknown key \"" + key + "\"";
        }
        if (!problem.empty()) problem = "line " + std::to_string(lineNumber) + ": " + problem;
    }
    if (problem.empty()) ParseOptions(options, Settings{}, settings, &problem);
    if (!problem.empty()) {
        *error = path + "\n" + problem;
        return false;
    }
    return true;
}

// Written to a temporary file first so a failed write never leaves a half-written config.
bool SaveConfig(const std::string& path, const Settings& settings) {
    const std::filesystem::path target = std::filesystem::u8path(path);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) return false;
        file << "# uniapi_stream settings, written by the controls page and the Stats button.\n"
                "# Command-line options override these for a single run.\n"
             << "size  = " << settings.width << "x" << settings.height << "\n"
             << "mode  = " << kModeNames[int(settings.mode)] << "\n"
             << "slots = " << settings.slots << "\n"
             << "draw  = " << settings.draw << "\n"
             << "stats = " << (settings.stats ? "on" : "off") << "\n";
        if (!file) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) std::filesystem::remove(temporary, error);
    return !error;
}

json SettingsJson(const Settings& s) {
    return {{"width", s.width}, {"height", s.height}, {"mode", kModeNames[int(s.mode)]},
            {"slots", s.slots}, {"draw", s.draw}, {"stats", s.stats}};
}

json Measured(double value) { return value < 0 ? json() : json(value); }

// --- Native stats panel layout ----------------------------------------------------------
// Built and updated entirely from C++ with DOM command lists; the page only has the CSS.

namespace panel {
constexpr uint32_t kRoot = 1, kTitle = 2, kNote = 3, kList = 4, kBars = 5, kFooter = 6;
constexpr uint32_t kButton = 7;  // the page's own Stats button, bound by selector
constexpr uint32_t kFirstLabel = 20, kFirstValue = 40, kFirstBar = 100;
constexpr uint32_t kBarCount = 30;  // one bar per second of fps history
enum Row : uint32_t { kMode, kResolution, kTransfer, kFps, kFrameTime, kRender, kDraw, kDropped, kRowCount };
const char* const kRowLabels[kRowCount] = {"Mode",      "Resolution", "Transfer",  "Frames/s",
                                           "Per frame", "C++ render", "Page draw", "Dropped"};
}  // namespace panel

const char* DrawLabel(const std::string& draw) {
    if (draw == "2d-cpu") return "2D canvas, CPU";
    if (draw == "webgl") return "WebGL";
    return "2D canvas";
}

// --- The demo ---------------------------------------------------------------------------

class Demo {
public:
    Demo(uniapi::App& app, Settings settings, std::string configPath)
        : app_(app), settings_(std::move(settings)), configPath_(std::move(configPath)) {}

    void Start() {
        CreateFramesStream();
        app_.onConnect = [this](const std::string& page) {
            if (page == "demo") StartRun();
        };
        app_.onDisconnect = [this](const std::string& page) {
            if (page == "demo") EndRun();
        };

        app_.on("draw-ms", [this](const json& value) { drawMs_ = value.get<double>(); });
        app_.expose("getState", [this] { return StateJson(); });
        app_.expose("applySettings", [this](const std::string& options) { return ApplySettings(options); });
        app_.expose("run", [this] { app_.post([this] { NavigateToDemo(); }); });
        app_.expose("clearHistory", [this] {
            history_.clear();
            return StateJson();
        });
        app_.expose("setStats", [this](bool on) {
            settings_.stats = on;
            SaveConfig(configPath_, settings_);  // a failed save only loses the preference
            pageStale_ = true;
            SyncStatsPanel();
            UpdateStatsPanel();  // fill in values now instead of on the next second
            return on;
        });
        app_.expose("saveResults", [this](const json&, const uniapi::Reply& reply) { SaveResults(reply); });

        NavigateToDemo();
    }

private:
    void CreateFramesStream() {
        frames_ = &app_.stream("frames", settings_.FrameBytes(), settings_.slots);
        frames_->onSlotFreed = [this] {
            if (running_ && settings_.mode != Mode::Timed) {
                while (PumpFrame(false)) {}
            }
        };
    }

    void NavigateToDemo() { app_.navigate("/index.html?draw=" + settings_.draw); }

    // Animated RGBA gradient written directly into the stream slot.
    void RenderTestPattern(uint8_t* pixels, uint32_t frame) const {
        const int width = settings_.width;
        for (int y = 0; y < settings_.height; ++y) {
            uint8_t* row = pixels + size_t(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                row[x * 4 + 0] = static_cast<uint8_t>(x + frame * 2);
                row[x * 4 + 1] = static_cast<uint8_t>(y + frame);
                row[x * 4 + 2] = static_cast<uint8_t>((x + y) / 2 - frame * 3);
                row[x * 4 + 3] = 255;
            }
        }
    }

    // Returns false when no slot is free. In timed mode that frame is dropped; in benchmark
    // mode it just means the pipeline is full.
    bool PumpFrame(bool countDrop) {
        uniapi::Stream::Slot slot;
        if (!frames_->acquire(slot)) {
            if (countDrop) ++dropped_;
            return false;
        }
        const bool draw = settings_.mode != Mode::Transport;
        if (draw) {
            double start = NowSeconds();
            RenderTestPattern(slot.data, frame_);
            renderSeconds_ += NowSeconds() - start;
            ++renderCount_;
        }
        json meta = {{"width", settings_.width}, {"height", settings_.height}, {"frame", frame_},
                     {"dropped", dropped_}, {"draw", draw}};
        if (!frames_->publish(slot, settings_.FrameBytes(), meta)) return false;
        ++frame_;
        return true;
    }

    // Called when the demo page connects.
    void StartRun() {
        running_ = true;
        currentRun_ = RunResult{settings_};
        fpsHistory_.clear();
        drawMs_ = -1;
        windowStart_ = NowSeconds();
        deliveredAtWindowStart_ = frames_->delivered();
        renderSeconds_ = 0;
        renderCount_ = 0;
        dropped_ = 0;  // per run, like the other stats (frame_ keeps counting: it animates the pattern)
        panelShown_ = false;
        pageStale_ = true;
        SyncStatsPanel();

        statsTimer_ = app_.every(std::chrono::milliseconds(100), [this] { ReportStats(); });
        if (settings_.mode == Mode::Timed) {
            frameTimer_ = app_.every(std::chrono::milliseconds(kFrameIntervalMs), [this] { PumpFrame(true); });
        } else {
            while (PumpFrame(false)) {}  // fill the pipeline
        }
    }

    // Called when the demo page goes away.
    void EndRun() {
        running_ = false;
        app_.cancel(frameTimer_);
        app_.cancel(statsTimer_);
        frameTimer_ = statsTimer_ = 0;
        if (currentRun_.fps > 0) {
            currentRun_.finished = std::time(nullptr);
            history_.insert(history_.begin(), currentRun_);
            if (history_.size() > kMaxHistory) history_.pop_back();
        }
        currentRun_ = RunResult{};
        app_.setTitle(kWindowTitle);
    }

    // Once a second: frames the page handed back, timings, title and overlay.
    void ReportStats() {
        const double now = NowSeconds();
        const double seconds = now - windowStart_;
        if (seconds < 1.0) return;

        const Settings& s = settings_;
        const uint64_t delivered = frames_->delivered();
        const double fps = (delivered - deliveredAtWindowStart_) / seconds;
        currentRun_.fps = fps;
        std::string title = Format("%s - %dx%d, %s, %s, %d slots, %s - %.0f fps (%.2f ms/frame)",
                                   kWindowTitle, s.width, s.height, kModeNames[int(s.mode)],
                                   s.draw.c_str(), s.slots, frames_->shared() ? "shared" : "fetch",
                                   fps, fps > 0 ? 1000.0 / fps : 0.0);
        // Transport mode never touches the pixels, so bytes/s would only be fps * frame size.
        if (s.mode != Mode::Transport) {
            double renderMs = renderCount_ ? renderSeconds_ * 1000.0 / renderCount_ : 0;
            currentRun_.renderMs = renderMs;
            currentRun_.drawMs = drawMs_;
            title += Format(", %.2f GB/s - render %.2f ms", fps * s.FrameBytes() / 1e9, renderMs);
            if (drawMs_ >= 0) title += Format(", draw %.2f ms", drawMs_);  // after the page's first report
        }
        app_.setTitle(title);
        RecordFps(fps);
        SyncStatsPanel();
        UpdateStatsPanel();
        windowStart_ = now;
        deliveredAtWindowStart_ = delivered;
        renderSeconds_ = 0;
        renderCount_ = 0;
    }

    // --- Pages: settings and history ---

    json StateJson() const {
        json history = json::array();
        for (const RunResult& run : history_) {
            const bool pixels = run.settings.mode != Mode::Transport;
            history.push_back({{"settings", SettingsJson(run.settings)},
                               {"fps", run.fps},
                               {"gbps", Measured(pixels ? run.fps * run.settings.FrameBytes() / 1e9 : -1)},
                               {"renderMs", Measured(pixels ? run.renderMs : -1)},
                               {"drawMs", Measured(pixels ? run.drawMs : -1)}});
        }
        return {{"settings", SettingsJson(settings_)}, {"configPath", configPath_}, {"history", history}};
    }

    // Throws on bad options; the page gets the message as a rejected promise.
    bool ApplySettings(const std::string& options) {
        Settings settings;
        std::string error;
        // The page always sends every option, so parse on top of the defaults.
        if (!ParseOptions(options, Settings{}, &settings, &error)) throw std::runtime_error(error);

        const Settings previous = settings_;
        settings_ = settings;
        if (settings.FrameBytes() != previous.FrameBytes() || settings.slots != previous.slots) {
            try {
                CreateFramesStream();  // replaces the stream only once the new one exists
            } catch (const std::exception&) {
                settings_ = previous;
                throw std::runtime_error("Could not allocate memory for the stream. "
                                         "The previous settings are still active.");
            }
        }
        if (!SaveConfig(configPath_, settings_)) {
            throw std::runtime_error("The new settings are active, but could not be saved to " +
                                     configPath_ + ". Check that the folder is writable.");
        }
        app_.post([this] { NavigateToDemo(); });  // after the reply has gone out
        return true;
    }

    // Oldest run first, one row per run; empty fields mean "not measured".
    bool WriteResultsCsv(const std::string& path) const {
        std::ofstream file(std::filesystem::u8path(path), std::ios::trunc);
        if (!file) return false;
        auto number = [](double value, int digits) {
            return value < 0 ? std::string() : Format("%.*f", digits, value);
        };
        file << "finished,width,height,mode,draw,slots,fps,ms_per_frame,render_ms,draw_ms,gb_per_s\n";
        for (auto run = history_.rbegin(); run != history_.rend(); ++run) {
            const Settings& s = run->settings;
            const bool pixels = s.mode != Mode::Transport;
            const std::tm t = LocalTime(run->finished);
            file << Format("%04d-%02d-%02d %02d:%02d:%02d,%d,%d,%s,%s,%d,", t.tm_year + 1900, t.tm_mon + 1,
                           t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, s.width, s.height,
                           kModeNames[int(s.mode)], pixels ? s.draw.c_str() : "", s.slots)
                 << number(run->fps, 1) << "," << number(run->fps > 0 ? 1000.0 / run->fps : -1, 3) << ","
                 << number(pixels ? run->renderMs : -1, 3) << "," << number(pixels ? run->drawMs : -1, 3)
                 << "," << number(pixels ? run->fps * s.FrameBytes() / 1e9 : -1, 3) << "\n";
        }
        return bool(file);
    }

    void SaveResults(const uniapi::Reply& reply) {
        if (history_.empty()) {
            reply.reject("There are no results to save yet.");
            return;
        }
        // The Save dialog runs its own message loop, which must not run inside a WebView event.
        app_.post([this, reply] {
            const std::tm now = LocalTime(std::time(nullptr));
            uniapi::SaveDialogOptions options;
            options.title = "Save benchmark results";
            options.defaultName = Format("benchmark-results-%04d%02d%02d-%02d%02d%02d.csv", now.tm_year + 1900,
                                         now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
            options.filterName = "CSV file (*.csv)";
            options.filterPattern = "*.csv";
            std::optional<std::string> path = app_.saveFileDialog(options);
            if (!path) {
                reply.resolve({{"cancelled", true}});
            } else if (!WriteResultsCsv(*path)) {
                reply.reject("Could not write " + *path + ". Check that the folder is writable.");
            } else {
                reply.resolve({{"saved", true}, {"path", *path}, {"runs", history_.size()}});
            }
        });
    }

    // --- Native stats panel ---

    // Makes the page match settings_.stats: builds or removes the panel and sets the Stats
    // button's pressed state, in one command list. If the list can't be sent now it is tried
    // again on the next once-a-second update.
    void SyncStatsPanel() {
        const bool want = settings_.stats;
        if (!running_ || (want == panelShown_ && !pageStale_)) return;
        bool sent = SendDom([&](uniapi::DomCommandWriter& dom) {
            dom.Bind(panel::kButton, "#stats-button");
            dom.SetAttribute(panel::kButton, "aria-pressed", want ? "true" : "false");
            if (want && !panelShown_) WriteStatsPanel(dom);
            if (!want && panelShown_) dom.Remove(panel::kRoot);
        });
        if (!sent) return;
        if (want && !panelShown_) panelUpdates_ = 0;
        panelShown_ = want;
        pageStale_ = false;
    }

    bool SendDom(const std::function<void(uniapi::DomCommandWriter&)>& write) {
        return app_.sendDom([&](uniapi::DomCommandWriter& dom) {
            write(dom);
            lastListCommands_ = dom.commands();
            lastListBytes_ = dom.size();
        });
    }

    static void WriteStatsPanel(uniapi::DomCommandWriter& dom) {
        using namespace panel;
        dom.Create(kRoot, "section");
        dom.SetAttribute(kRoot, "class", "native-panel");
        dom.SetAttribute(kRoot, "aria-label", "Native stats");
        dom.Create(kTitle, "h2");
        dom.SetText(kTitle, "Native stats");
        dom.Append(kRoot, kTitle);
        dom.Create(kNote, "p");
        dom.SetAttribute(kNote, "class", "note");
        dom.SetText(kNote, "Built and updated by C++ through DOM command lists");
        dom.Append(kRoot, kNote);

        dom.Create(kList, "dl");
        dom.Append(kRoot, kList);
        for (uint32_t row = 0; row < kRowCount; ++row) {
            dom.Create(kFirstLabel + row, "dt");
            dom.SetText(kFirstLabel + row, kRowLabels[row]);
            dom.Create(kFirstValue + row, "dd");
            dom.SetText(kFirstValue + row, "—");
            dom.Append(kList, kFirstLabel + row);
            dom.Append(kList, kFirstValue + row);
        }

        dom.Create(kBars, "div");
        dom.SetAttribute(kBars, "class", "bars");
        dom.SetAttribute(kBars, "aria-hidden", "true");
        dom.Append(kRoot, kBars);
        for (uint32_t i = 0; i < kBarCount; ++i) {
            dom.Create(kFirstBar + i, "div");
            dom.SetStyle(kFirstBar + i, "height", "0%");
            dom.Append(kBars, kFirstBar + i);
        }
        dom.ToggleClass(kFirstBar + kBarCount - 1, "latest", true);

        dom.Create(kFooter, "p");
        dom.SetAttribute(kFooter, "class", "footer");
        dom.Append(kRoot, kFooter);
        dom.Append(uniapi::DomCommandWriter::kBody, kRoot);
    }

    // Kept even while the panel is hidden, so the chart is populated when it is shown.
    void RecordFps(double fps) {
        fpsHistory_.push_back(fps);
        if (fpsHistory_.size() > panel::kBarCount) fpsHistory_.erase(fpsHistory_.begin());
    }

    void UpdateStatsPanel() {
        using namespace panel;
        if (!panelShown_ || fpsHistory_.empty()) return;
        const double fps = fpsHistory_.back();
        const uint32_t previousCommands = lastListCommands_;
        const size_t previousBytes = lastListBytes_;

        const bool sent = SendDom([&](uniapi::DomCommandWriter& dom) {
            static const char* const kModeLabels[] = {"Timed", "Benchmark", "Transfer only"};
            const Settings& s = settings_;
            const bool pixels = s.mode != Mode::Transport;
            dom.SetText(kFirstValue + kMode, Format("%s · %s", kModeLabels[int(s.mode)],
                                                    pixels ? DrawLabel(s.draw) : "no drawing"));
            dom.SetText(kFirstValue + kResolution, Format("%d × %d", s.width, s.height));
            dom.SetText(kFirstValue + kTransfer, frames_->shared() ? "Shared memory" : "Fetch (copy)");
            dom.SetText(kFirstValue + kFps, Format(fps >= 100 ? "%.0f" : "%.1f", fps));
            dom.SetText(kFirstValue + kFrameTime, fps > 0 ? Format("%.2f ms", 1000.0 / fps) : "—");
            dom.SetText(kFirstValue + kRender,
                        pixels && currentRun_.renderMs >= 0 ? Format("%.2f ms", currentRun_.renderMs) : "—");
            dom.SetText(kFirstValue + kDraw, pixels && drawMs_ >= 0 ? Format("%.2f ms", drawMs_) : "—");
            dom.SetText(kFirstValue + kDropped, Format("%u", dropped_));

            // Right-aligned history: the newest second is the last bar.
            const double maxFps = *std::max_element(fpsHistory_.begin(), fpsHistory_.end());
            const size_t empty = kBarCount - fpsHistory_.size();
            for (uint32_t i = 0; i < kBarCount; ++i) {
                double value = i < empty ? 0 : fpsHistory_[i - empty];
                dom.SetStyle(kFirstBar + i, "height", Format("%.1f%%", maxFps > 0 ? value * 100.0 / maxFps : 0.0));
            }
            dom.SetText(kFooter, Format("Update #%u · previous one: %u commands, %zu bytes", panelUpdates_ + 1,
                                        previousCommands, previousBytes));
        });
        if (sent) ++panelUpdates_;  // counts lists that went out, so the footer doesn't skip numbers
    }

    uniapi::App& app_;
    Settings settings_;
    const std::string configPath_;
    uniapi::Stream* frames_ = nullptr;

    bool running_ = false;  // the demo page is connected and streaming
    int frameTimer_ = 0;
    int statsTimer_ = 0;
    uint32_t frame_ = 0;
    uint32_t dropped_ = 0;
    RunResult currentRun_;
    std::vector<RunResult> history_;  // most recent first

    // Per-second statistics.
    double windowStart_ = 0;
    uint64_t deliveredAtWindowStart_ = 0;
    double renderSeconds_ = 0;  // C++ time spent producing frames
    uint32_t renderCount_ = 0;
    double drawMs_ = -1;        // the page's average draw time, from its "draw-ms" event

    // Overlay state.
    bool panelShown_ = false;  // what the page currently shows
    bool pageStale_ = true;    // the Stats button's pressed state may be out of date
    std::vector<double> fpsHistory_;
    uint32_t panelUpdates_ = 0;
    uint32_t lastListCommands_ = 0;
    size_t lastListBytes_ = 0;
};

}  // namespace

int main() {
    const std::string configPath = ConfigPath();
    Settings saved;
    std::string error;
    if (!LoadConfig(configPath, &saved, &error)) {
        uniapi::alert(kWindowTitle, "The settings file is invalid and was ignored:\n\n" + error);
        saved = Settings{};
    }
    const std::string arguments = uniapi::commandLine();
    Settings settings;
    if (!ParseOptions(arguments, saved, &settings, &error)) {
        uniapi::alert(kWindowTitle, error);
        return 1;
    }

    uniapi::AppOptions options;
    options.title = kWindowTitle;
    options.width = 900;
    options.height = 600;
    options.sharedMemory = arguments.find("--no-shared-memory") == std::string::npos;
    uniapi::App app(options);
#ifdef UNIAPI_STREAM_EMBED_WEB
    app.serve(stream_web);
#else
    app.serveDirectory(UNIAPI_STREAM_WEB_DIR);
#endif

    Demo demo(app, settings, configPath);
    demo.Start();
    return app.run();
}
