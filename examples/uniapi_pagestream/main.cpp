// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// uniapi demo: page to page. uniapi_stream's frames, made in a page instead of C++: the main
// window's page (web/producer.html) fills a sink with RGBA frames, and a second window's page
// (web/viewer.html) gets the very slots as a stream (Sink::addWindow; with shared memory, the
// same memory, no copy) and draws them, as uniapi_stream's page does. For a picture computed once
// in one window (on the GPU, say) and shown in another. It measures how fast they get there.
//
// Command line (as uniapi_stream's, so the two compare):
//   --timed                ~60 fps, a frame every 16 ms (the default)
//   --benchmark            unthrottled: a new frame as soon as a slot is free
//   --benchmark=transport  unthrottled, and neither renders nor draws: the hand-off alone
//   --slots=N              number of slots, 1-64 (default 3)
//   --size=WxH             frame resolution, e.g. --size=1920x1080 (default 640x360);
//                          presets: 360p, 720p, 1080p, 1440p, 4k, 8k (e.g. --size=4k)
//   --draw=MODE            how the viewer draws frames: 2d (default), 2d-cpu or webgl
//   --no-shared-memory     uploaded by the producer page, fetched by the viewer's
//   --seconds=N            runs N seconds, prints the result (the average of the full seconds
//                          after the first) and quits
// Delivered fps, throughput and the pages' render and draw times are shown in the window titles.

#include <uniapi/uniapi.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#ifdef UNIAPI_PAGESTREAM_EMBED_WEB
UNIAPI_ASSETS(pagestream_web);
#endif

namespace {

using uniapi::json;

constexpr char kWindowTitle[] = "uniapi page stream";
constexpr int kMaxFrameDimension = 16384;            // Chromium's canvas limit per side
constexpr int kMaxSlots = 64;
constexpr size_t kMaxSharedBytes = size_t(4) << 30;  // all slots together

enum class Mode { Timed, Benchmark, Transport };
const char* const kModeNames[] = {"timed", "benchmark", "transport"};

struct Settings {
    int width = 640;
    int height = 360;
    Mode mode = Mode::Timed;
    int slots = 3;            // one being drawn, one in flight, one being filled
    std::string draw = "2d";  // passed to the viewer as ?draw=
    int seconds = 0;          // > 0: run that long, print the result, quit

    size_t FrameBytes() const { return size_t(width) * height * 4; }
};

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::string Format(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

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

bool ParseOptions(std::string_view text, Settings* settings, std::string* error) {
    Settings result;
    std::string value;
    if (FindOption(text, "--benchmark=transport", nullptr)) {
        result.mode = Mode::Transport;
    } else if (FindOption(text, "--benchmark", nullptr)) {
        result.mode = Mode::Benchmark;
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
    if (FindOption(text, "--seconds=", &value)) {
        result.seconds = atoi(value.c_str());
        if (result.seconds < 2) {
            *error = "--seconds must be 2 or more (the first second is a warm-up).";
            return false;
        }
    }
    if (result.FrameBytes() * result.slots > kMaxSharedBytes) {
        *error = Format("These settings need %.1f GB of memory for the slots; the limit is 4 GB. "
                        "Use a smaller size or fewer slots.",
                        result.FrameBytes() * result.slots / 1e9);
        return false;
    }
    *settings = std::move(result);
    return true;
}

// --- The demo ---------------------------------------------------------------------------

class Demo {
public:
    Demo(uniapi::App& app, Settings settings) : app_(app), settings_(std::move(settings)) {}

    void Start() {
        const Settings& s = settings_;
        // The viewer: a window of its own, beside the producer's.
        uniapi::WindowOptions options;
        options.title = std::string(kWindowTitle) + ": viewer";
        options.width = 900;
        options.height = 600;
        options.owned = false;
        viewer_ = &app_.openWindow(options);
        viewer_->onClosed = [this] {
            viewer_ = nullptr;  // destroyed once this returns
            app_.quit();
        };
        viewer_->on("draw-ms", [this](const json& value) { drawMs_ = value.get<double>(); });

        // The producer's sink, reaching the viewer's page. No onData: C++ only passes the slots
        // on, each going back to the producer once the viewer has drawn it.
        frames_ = &app_.sink("frames", s.FrameBytes(), s.slots);
        frames_->addWindow(*viewer_);

        app_.on("render-ms", [this](const json& value) { renderMs_ = value.get<double>(); });
        app_.onConnect = [this](const std::string& page) {
            if (page == "producer") StartRun();
        };
        app_.onDisconnect = [this](const std::string& page) {
            if (page == "producer") EndRun();
        };
        viewer_->navigate("/viewer.html?draw=" + s.draw);
        app_.navigate(Format("/producer.html?mode=%s&width=%d&height=%d", kModeNames[int(s.mode)], s.width, s.height));
    }

private:
    void StartRun() {
        windowStart_ = NowSeconds();
        receivedAtWindowStart_ = frames_->received();
        statsTimer_ = app_.every(std::chrono::milliseconds(100), [this] { ReportStats(); });
    }

    void EndRun() {
        app_.cancel(statsTimer_);
        statsTimer_ = 0;
    }

    // Once a second: frames the producer published, timings, the titles; with --seconds, the
    // result at the end.
    void ReportStats() {
        const double now = NowSeconds();
        const double seconds = now - windowStart_;
        if (seconds < 1.0) return;
        const Settings& s = settings_;
        const uint64_t received = frames_->received();
        const double fps = (received - receivedAtWindowStart_) / seconds;
        std::string title = Format("%s - %dx%d, %s, %s, %d slots, %s - %.0f fps (%.2f ms/frame)",
                                   kWindowTitle, s.width, s.height, kModeNames[int(s.mode)], s.draw.c_str(),
                                   s.slots, frames_->shared() ? "shared" : "fetch", fps, fps > 0 ? 1000.0 / fps : 0.0);
        if (s.mode != Mode::Transport) {
            title += Format(", %.2f GB/s", fps * s.FrameBytes() / 1e9);
            if (renderMs_ >= 0) title += Format(" - render %.2f ms", renderMs_);
            if (drawMs_ >= 0) title += Format(", draw %.2f ms", drawMs_);
        }
        app_.setTitle(title);
        if (viewer_) viewer_->setTitle(title);
        if (++secondsDone_ > 1) {  // the first second warms up
            fpsSum_ += fps;
            ++fpsCount_;
        }
        if (s.seconds > 0 && secondsDone_ >= s.seconds) {
            const double average = fpsCount_ ? fpsSum_ / fpsCount_ : 0;
            std::printf("%dx%d %s %s %d slots %s: %.1f fps, %.2f ms/frame, %.2f GB/s, render %.2f ms, draw %.2f ms\n",
                        s.width, s.height, kModeNames[int(s.mode)], s.draw.c_str(), s.slots,
                        frames_->shared() ? "shared" : "fetch", average, average > 0 ? 1000.0 / average : 0.0,
                        s.mode != Mode::Transport ? average * s.FrameBytes() / 1e9 : 0.0, renderMs_, drawMs_);
            std::fflush(stdout);
            app_.quit();
        }
        windowStart_ = now;
        receivedAtWindowStart_ = received;
    }

    uniapi::App& app_;
    Settings settings_;
    uniapi::Window* viewer_ = nullptr;
    uniapi::Sink* frames_ = nullptr;
    int statsTimer_ = 0;
    double windowStart_ = 0;
    uint64_t receivedAtWindowStart_ = 0;
    double renderMs_ = -1;  // the producer page's average render time ("render-ms")
    double drawMs_ = -1;    // the viewer page's average draw time ("draw-ms")
    int secondsDone_ = 0;
    double fpsSum_ = 0;
    int fpsCount_ = 0;
};

}  // namespace

int main() {
    const std::string arguments = uniapi::commandLine();
    Settings settings;
    std::string error;
    if (!ParseOptions(arguments, &settings, &error)) {
        uniapi::alert(kWindowTitle, error);
        return 1;
    }

    uniapi::AppOptions options;
    options.title = kWindowTitle;
    options.width = 640;
    options.height = 200;
    options.sharedMemory = arguments.find("--no-shared-memory") == std::string::npos;
    uniapi::App app(options);
#ifdef UNIAPI_PAGESTREAM_EMBED_WEB
    app.serve(pagestream_web);
#else
    app.serveDirectory(UNIAPI_PAGESTREAM_WEB_DIR);
#endif

    Demo demo(app, settings);
    demo.Start();
    return app.run();
}
