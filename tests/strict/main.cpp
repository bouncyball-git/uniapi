// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The strict calls test: AppOptions::strictCalls. The page calls C++'s exposed functions with the
// two mistakes nlohmann's decoding lets through (an extra argument, an object field the type
// hasn't), and with ones that fail either way (too few arguments, a wrong type). Strict, the two
// mistakes reject the call, saying which; --loose (the default options) they are let through, as
// before. Binary data, maps and plain JSON arguments are never refused. Exits with 0 when every
// check passes.
//
//   uniapi_strict_test [--loose] [--exit]

#include <uniapi/uniapi.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

UNIAPI_ASSETS(strict_web);

struct Settings {
    int volume = 0;
    std::string name;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, volume, name)

struct Mixer {
    Settings main;
    std::vector<Settings> channels;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Mixer, main, channels)

struct Tagged {
    uniapi::Bytes data;
    std::string label;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Tagged, data, label)

int main() {
    bool strict = true;
    bool keepOpen = true;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--loose") {
            strict = false;
        } else if (argument == "--exit") {
            keepOpen = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_strict_test [--loose] [--exit]\n", argument.c_str());
            return 2;
        }
    }
    uniapi::AppOptions options;
    options.title = strict ? "uniapi strict calls test" : "uniapi strict calls test (loose)";
    options.width = 520;
    options.height = 360;
    options.activate = keepOpen;
    options.startHidden = !keepOpen;
    options.strictCalls = strict;
    uniapi::App app(options);
    app.serve(strict_web);

    app.expose("add", [](int a, int b) { return a + b; });
    app.expose("set", [](Settings s) { return s.volume; });
    app.expose("setMixer", [](const Mixer& m) { return int(m.channels.size()); });
    app.expose("count", [](std::map<std::string, int> m) { return int(m.size()); });
    app.expose("keys", [](const uniapi::json& any) { return int(any.size()); });
    app.expose("withBytes", [](Tagged t) { return int(t.data.size()); });

    int passed = 0, failed = 0;
    std::string lines;
    auto check = [&](const std::string& name, bool ok, const std::string& detail) {
        ok ? ++passed : ++failed;
        const std::string line = std::string(ok ? "  pass  " : "  FAIL  ") + name + (ok ? "" : ": " + detail);
        std::printf("%s\n", line.c_str());
        lines += line + "\n";
    };
    auto value = [](const uniapi::json& r) { return r.contains("value") ? r["value"] : uniapi::json(); };
    auto error = [](const uniapi::json& r) { return r.value("error", std::string()); };

    bool started = false;
    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        app.call("run", uniapi::json::array(), [&](const uniapi::Result& result) {
            const uniapi::json r = result.ok ? result.value : uniapi::json::object();
            const std::string mode = strict ? "strict: " : "loose: ";
            if (strict) {
                check(mode + "an extra argument rejects the call, saying how many came", error(r["extra"]) == "expected 2 argument(s), got 3",
                      r["extra"].dump());
                check(mode + "a misspelled field rejects the call, naming it", error(r["misspelt"]) == "argument 1: unknown field \"volum\"",
                      r["misspelt"].dump());
                check(mode + "a misspelled field deeper down too (in a list of objects)",
                      error(r["nested"]) == "argument 1: unknown field \"channels[0].pan\"", r["nested"].dump());
            } else {
                check(mode + "an extra argument is ignored", value(r["extra"]) == 3, r["extra"].dump());
                check(mode + "a misspelled field is ignored (the field keeps its default)", value(r["misspelt"]) == 0, r["misspelt"].dump());
                check(mode + "a misspelled field deeper down is ignored", value(r["nested"]) == 1, r["nested"].dump());
            }
            check(mode + "too few arguments reject the call, saying how many came", error(r["few"]) == "expected 2 argument(s), got 1",
                  r["few"].dump());
            check(mode + "a wrong type rejects the call, naming the argument",
                  error(r["wrongType"]).rfind("argument 1: ", 0) == 0 && error(r["wrongType"]).find("type must be number") != std::string::npos,
                  r["wrongType"].dump());
            check(mode + "a right call answers", value(r["exact"]) == 3 && value(r["spelt"]) == 3,
                  r["exact"].dump() + " " + r["spelt"].dump());
            check(mode + "maps, plain JSON and binary data are taken as they come",
                  value(r["map"]) == 3 && value(r["json"]) == 2 && value(r["bytes"]) == 3,
                  r["map"].dump() + " " + r["json"].dump() + " " + r["bytes"].dump());
            std::printf("\nstrict calls (%s): %d passed, %d failed\n", strict ? "strict" : "loose", passed, failed);
            if (!keepOpen) {
                app.quit();
                return;
            }
            app.call("show", uniapi::json::array({lines}));
        });
    };
    int ticks = 0;
    app.every(std::chrono::seconds(30), [&] {
        if (ticks++ == 0 || keepOpen) return;
        ++failed;
        std::printf("the test didn't finish within 30 s\n");
        app.quit();
    });
    app.navigate("/index.html");
    app.run();
    return failed == 0 && started ? 0 : 1;
}
