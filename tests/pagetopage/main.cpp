// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The page-to-page test: Sink::addWindow against real web views. The main window's page fills a
// sink (app.sink); a further window's page receives the same slots as a stream (app.stream),
// with shared memory the same memory. It checks the picture arrives intact in both that page and
// C++; that the writing page can't have a slot again while the reading page holds it, nor while
// C++ does (before release); that a reading page that reloads, or a window that closes, holding a
// slot lets it go; removeWindow; a replaced sink keeping its windows; and the mistakes refused.
// Exits with 0 when every check passes.
//
//   uniapi_pagetopage_test                     keeps the main window open with the results
//   uniapi_pagetopage_test --exit              closes when done, without taking focus (ctest)
//   uniapi_pagetopage_test --no-shared-memory  uploaded by the writing page, fetched by the reading one

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

UNIAPI_ASSETS(pagetopage_web);

namespace {

using uniapi::json;
using namespace std::chrono_literals;

constexpr char kTitle[] = "uniapi page-to-page test";
constexpr size_t kSize = 256 * 1024;  // a picture's bytes

struct Checks {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> lines;

    void Check(const std::string& name, bool ok, const std::string& detail = {}) {
        ok ? ++passed : ++failed;
        std::string line = std::string(ok ? "  pass  " : "  FAIL  ") + name + (ok || detail.empty() ? "" : ": " + detail);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        lines.push_back(std::move(line));
    }
};

// Waits (without blocking the window) until `done` is true, or the timeout: false then.
uniapi::Task<bool> WaitFor(uniapi::App& app, std::function<bool()> done, std::chrono::milliseconds timeout = 10s) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::steady_clock::now() - start > timeout) co_return false;
        co_await app.sleep(10ms);
    }
    co_return true;
}

// What arrived with a sequence number, from a list of {seq, sum, ...}.
const json* Find(const std::vector<json>& list, int seq) {
    for (const json& item : list) {
        if (item.value("seq", -1) == seq) return &item;
    }
    return nullptr;
}

uint64_t SumOf(const std::vector<json>& list, int seq) {
    const json* item = Find(list, seq);
    return item ? item->value("sum", uint64_t(0)) : 0;
}

}  // namespace

int main() {
    bool keepOpen = true;
    bool shared = true;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--exit") {
            keepOpen = false;
        } else if (argument == "--no-shared-memory") {
            shared = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_pagetopage_test [--exit] [--no-shared-memory]\n", argument.c_str());
            return 2;
        }
    }

    uniapi::AppOptions options;
    options.title = kTitle;
    options.width = 640;
    options.height = 420;
    options.sharedMemory = shared;
    options.activate = keepOpen;
    uniapi::App app(options);
    app.serve(pagetopage_web);
    Checks checks;

    std::vector<json> filled;    // the writing page: what it published
    std::vector<json> seen;      // the reading page: what it received
    std::vector<json> received;  // C++ (onData): what it received
    app.on("filled", [&](const json& data) { filled.push_back(data); });

    uniapi::Window* reader = nullptr;
    int readerConnects = 0;
    bool readerClosed = false;
    bool started = false;

    uniapi::Sink* sink = nullptr;
    std::optional<uniapi::Sink::Slot> held;  // a slot C++ keeps (meta hold "app") until it releases it
    auto onData = [&](const uniapi::Sink::Slot& slot, const json& meta) {
        uint64_t sum = 0;
        for (size_t i = 0; i < slot.size; ++i) sum += slot.data[i];
        received.push_back({{"seq", meta.value("seq", -1)}, {"sum", sum}, {"size", slot.size}});
        if (meta.contains("hold") && (meta["hold"] == "app" || meta["hold"] == "both")) {
            held = slot;
            return;
        }
        sink->release(slot);
    };
    // Asks the writing page for picture `seq`; hold: who keeps it ("reader", "app") or nobody.
    auto fill = [&](int seq, const char* hold = nullptr) {
        app.emit("fill", {{"seq", seq}, {"size", kSize}, {"hold", hold ? json(hold) : json()}});
    };
    auto hasFilled = [&](int seq) { return Find(filled, seq) != nullptr; };

    auto flow = [&]() -> uniapi::Task<> {
        // The reading window.
        uniapi::WindowOptions readerOptions;
        readerOptions.title = std::string(kTitle) + ": reader";
        readerOptions.width = 420;
        readerOptions.height = 200;
        readerOptions.activate = keepOpen;
        reader = &app.openWindow(readerOptions);
        reader->onConnect = [&](const std::string&) { ++readerConnects; };
        reader->onClosed = [&] {
            readerClosed = true;
            reader = nullptr;  // destroyed once this returns
        };
        reader->on("seen", [&](const json& data) { seen.push_back(data); });
        reader->navigate("/reader.html");
        bool ok = co_await WaitFor(app, [&] { return readerConnects == 1; });
        checks.Check("the reading window's page connects", ok);

        // The sink: one slot, so every hold shows at once. Its own window, and a stream of its
        // name in the reading window, are refused.
        sink = &app.sink("pictures", kSize, 1);
        sink->onData = onData;
        sink->addWindow(*reader);
        bool ownRefused = false, clash = false;
        try {
            sink->addWindow(app);
        } catch (const std::invalid_argument&) {
            ownRefused = true;
        }
        try {
            reader->stream("pictures", 16, 1);
        } catch (const std::invalid_argument&) {
            clash = true;
        }
        checks.Check("addWindow: the sink's own window and a stream of its name in the reading window are refused",
                     ownRefused && clash && sink->windows().size() == 1 && sink->windows()[0] == reader,
                     std::to_string(ownRefused) + " " + std::to_string(clash) + " " + std::to_string(sink->windows().size()));

        // 1. A picture: the reading page and C++ get it intact.
        fill(1);
        ok = co_await WaitFor(app, [&] { return Find(seen, 1) && Find(received, 1) && hasFilled(1); });
        const uint64_t sum1 = SumOf(filled, 1);
        checks.Check("the writing page's picture reaches the reading page and C++ intact (Sink::addWindow)",
                     ok && sum1 != 0 && SumOf(seen, 1) == sum1 && SumOf(received, 1) == sum1
                         && Find(seen, 1)->value("size", size_t(0)) == kSize,
                     std::to_string(sum1) + " " + std::to_string(SumOf(seen, 1)) + " " + std::to_string(SumOf(received, 1)));
        checks.Check("shared memory as the app has it: the writing page writes it, the reading page maps it",
                     sink->shared() == app.sharedMemory() && reader->sharedMemory() == app.sharedMemory(),
                     std::to_string(sink->shared()) + " " + std::to_string(reader->sharedMemory()) + " "
                         + std::to_string(app.sharedMemory()));

        // 1b. No copy: with shared memory the reading page holds the very memory C++ has (a byte
        // C++ changes shows there); without it, the copy it fetched (nothing shows).
        std::vector<json> resummed;
        reader->on("resummed", [&](const json& data) { resummed.push_back(data); });
        fill(12, "both");
        ok = co_await WaitFor(app, [&] { return Find(seen, 12) && held.has_value(); });
        const uint8_t before = held->data[0];
        const_cast<uint8_t*>(held->data)[0] = uint8_t(before + 1);  // (the test's own poke into the slot)
        reader->emit("resum");
        ok = ok && co_await WaitFor(app, [&] { return !resummed.empty(); });
        const uint64_t sum12 = SumOf(seen, 12), again = ok ? resummed[0].value("sum", uint64_t(0)) : 0;
        checks.Check(app.sharedMemory() ? "no copy: the reading page reads the very memory C++ has (a byte changed shows there)"
                                        : "without shared memory the reading page has a copy of its own (a byte changed doesn't show)",
                     ok && again == (app.sharedMemory() ? sum12 + 1 : sum12),
                     std::to_string(sum12) + " -> " + std::to_string(again));
        reader->emit("letGo");
        sink->release(*held);
        held.reset();
        ok = co_await WaitFor(app, [&] { return hasFilled(12); });  // (published before the hold; settled)

        // 2. The reading page holds the slot: the writing page waits for it.
        fill(2, "reader");
        ok = co_await WaitFor(app, [&] { return Find(seen, 2) && Find(received, 2); });
        fill(3);
        co_await app.sleep(200ms);
        const bool waited = ok && !hasFilled(3);
        reader->emit("letGo");
        ok = co_await WaitFor(app, [&] { return hasFilled(3) && Find(seen, 3) && Find(received, 3); });
        checks.Check("while the reading page holds the slot, the writing page can't have it; handed back, it can",
                     waited && ok && SumOf(seen, 3) == SumOf(filled, 3), std::to_string(waited) + " " + std::to_string(ok));

        // 3. C++ holds the slot (no release yet): the same.
        fill(4, "app");
        ok = co_await WaitFor(app, [&] { return Find(seen, 4) && held.has_value(); });
        fill(5);
        co_await app.sleep(200ms);
        const bool waitedForApp = ok && !hasFilled(5);
        sink->release(*held);
        held.reset();
        ok = co_await WaitFor(app, [&] { return hasFilled(5) && Find(seen, 5) && Find(received, 5); });
        checks.Check("while C++ holds the slot (before release), the writing page can't have it either",
                     waitedForApp && ok, std::to_string(waitedForApp) + " " + std::to_string(ok));

        // 4. The reading page reloads while it holds the slot: the slot comes back.
        fill(6, "reader");
        ok = co_await WaitFor(app, [&] { return Find(seen, 6) != nullptr; });
        fill(7);
        co_await app.sleep(150ms);
        const bool heldThere = ok && !hasFilled(7);
        reader->navigate("/reader.html?again");
        ok = co_await WaitFor(app, [&] { return hasFilled(7) && readerConnects == 2; });
        co_await app.sleep(100ms);
        checks.Check("a reading page that goes while holding the slot lets it go; its new page receives again",
                     heldThere && ok && sink->windows().size() == 1, std::to_string(heldThere) + " " + std::to_string(ok));

        // 5. removeWindow: the reading page gets no more; C++ still does.
        sink->removeWindow(*reader);
        const size_t seenBefore = seen.size();
        fill(8);
        ok = co_await WaitFor(app, [&] { return Find(received, 8) != nullptr; });
        co_await app.sleep(150ms);
        checks.Check("removed, the reading page gets no more pictures; C++ still does",
                     ok && seen.size() == seenBefore && sink->windows().empty(), std::to_string(seen.size() - seenBefore));

        // 6. Replaced, the sink keeps the windows it reaches.
        sink->addWindow(*reader);
        sink = &app.sink("pictures", kSize, 1);
        sink->onData = onData;
        fill(9);
        ok = co_await WaitFor(app, [&] { return Find(seen, 9) && Find(received, 9); });
        checks.Check("replaced, the sink keeps the windows it reaches",
                     ok && SumOf(seen, 9) == SumOf(filled, 9) && SumOf(received, 9) == SumOf(filled, 9)
                         && sink->windows().size() == 1,
                     std::to_string(ok) + " " + std::to_string(sink->windows().size()));

        // 7. The reading window closes while its page holds the slot: the slot comes back.
        fill(10, "reader");
        ok = co_await WaitFor(app, [&] { return Find(seen, 10) != nullptr; });
        fill(11);
        co_await app.sleep(150ms);
        const bool heldByClosing = ok && !hasFilled(11);
        reader->close();
        ok = co_await WaitFor(app, [&] { return readerClosed && hasFilled(11) && Find(received, 11); });
        checks.Check("a reading window that closes holding the slot lets it go; the sink reaches no window then",
                     heldByClosing && ok && sink->windows().empty(), std::to_string(heldByClosing) + " " + std::to_string(ok));

        std::printf("\npage to page: %d passed, %d failed\n", checks.passed, checks.failed);
        std::fflush(stdout);
        app.call("showResults", json::array({checks.lines}));
        if (!keepOpen) app.quit();
    };

    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();  // a Task runs at once, to its first co_await
    };
    // A page that never connects must not hang the test.
    int ticks = 0;
    app.every(60s, [&] {
        if (ticks++ == 0) return;  // the first call comes at once
        std::printf("the test didn't finish within 60 s\n");
        checks.failed++;
        app.quit();
    });
    app.navigate("/index.html");
    app.run();
    return checks.failed == 0 && started ? 0 : 1;
}
