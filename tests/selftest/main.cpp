// uniapi self-test: runs calls in both directions, events, streams, DOM command lists and a
// page reload against a real web view, prints each check, and exits with 0 when all pass.
//
//   uniapi_test                     streams use shared memory where supported
//   uniapi_test --no-shared-memory  streams use the fetch fallback
//   uniapi_test --exit              close by itself when done (what ctest runs)
//   uniapi_test --data-dir=<path>   keep the web engine's profile there (AppOptions::dataDirectory)
//
// Without --exit the window stays open and lists every check, grouped by step, until you
// close it. With --exit it opens without taking keyboard focus, so ctest can run while you
// work. C++ drives the test; the page half is web/selftest.js.

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

UNIAPI_ASSETS(selftest_web);

namespace {

using uniapi::json;
using namespace std::chrono_literals;

constexpr size_t kSlotBytes = 256 * 1024;
constexpr int kSlots = 3;
constexpr auto kTimeout = 30s;

uint8_t PatternByte(int seed, size_t i) {
    return uint8_t((size_t(seed) + i * 31) & 0xff);  // web/selftest.js checks the same pattern
}

// Weighted sum of the bytes; web/selftest.js computes the same to check data arrived intact.
uint64_t Checksum(const uniapi::Bytes& bytes) {
    uint64_t sum = 0;
    for (size_t i = 0; i < bytes.size(); ++i) sum += uint64_t(bytes.data()[i]) * (i % 251 + 1);
    return sum;
}

std::string Describe(const uniapi::Result& r) {
    return r.ok ? "value " + r.value.dump() : "error \"" + r.error + "\"";
}

class SelfTest {
public:
    using Done = std::function<void()>;

    SelfTest(uniapi::App& app, bool sharedRequested, bool keepOpen, std::string title)
        : app_(app), sharedRequested_(sharedRequested), keepOpen_(keepOpen), title_(std::move(title)) {}

    void Start();
    int ExitCode() const { return finished_ && failed_ == 0 ? 0 : 1; }

private:
    void Check(const std::string& name, bool ok, const std::string& detail = {});
    void Note(const std::string& text);
    void Step(std::string name, std::function<void(Done)> run) {
        steps_.emplace_back(std::move(name), std::move(run));
    }
    void RunNext();
    void WaitUntil(std::function<bool()> condition, Done next);
    void Finish(const std::string& failure = {});

    void CheckBeforeRun();
    void ExposeFunctions();
    void AddSteps();
    bool PublishSlot(const uniapi::Stream::Slot& slot, int seed, size_t size, bool hold);
    bool Publish(int seed, size_t size, bool hold);
    void CheckStreamStats(const std::string& name, Done done);
    uniapi::Task<> CoroutineChecks(Done done);
    uniapi::Task<> ThreadChecks(Done done);
    uniapi::Task<> TakeBackChecks(Done done);
    uniapi::Task<int> Seven();
    uniapi::Task<> FailLater();

    uniapi::App& app_;
    bool sharedRequested_;
    bool keepOpen_;
    std::string title_;
    json records_ = json::array();  // every check, for the results page
    std::vector<std::pair<std::string, std::function<void(Done)>>> steps_;
    size_t nextStep_ = 0;
    std::string currentStep_ = "Before run()";
    int passed_ = 0;
    int failed_ = 0;
    bool finished_ = false;
    int connections_ = 0;
    int disconnects_ = 0;
    uniapi::Stream* stream_ = nullptr;
    uniapi::Stream* hugeStream_ = nullptr;  // too large to allocate
    int published_ = 0;  // frames published on stream_ to the current page
    Done onReconnect_;
    bool bytesGoneAfterAnswer_ = false;
    bool iframeReachedCpp_ = false;  // see "calledFromIframe"
    bool sharedAtStart_ = false;  // shared memory was in use on the first page
    std::thread::id uiThread_ = std::this_thread::get_id();
    std::vector<std::string> eventLog_;  // the "log" events, for the ordering check
};

void SelfTest::Check(const std::string& name, bool ok, const std::string& detail) {
    if (ok) {
        ++passed_;
        std::printf("  pass  %s\n", name.c_str());
    } else {
        ++failed_;
        std::printf("  FAIL  %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
    std::fflush(stdout);
    records_.push_back({{"step", currentStep_}, {"name", name}, {"detail", detail}, {"ok", ok}});
}

void SelfTest::Note(const std::string& text) {
    std::printf("  note  %s\n", text.c_str());
    std::fflush(stdout);
    records_.push_back({{"step", currentStep_}, {"name", text}, {"detail", ""}, {"ok", true}, {"note", true}});
}

void SelfTest::RunNext() {
    if (finished_) return;
    if (nextStep_ == steps_.size()) {
        Finish();
        return;
    }
    const auto& step = steps_[nextStep_++];
    currentStep_ = step.first;
    std::printf("%s\n", step.first.c_str());
    auto run = step.second;
    // Each step starts from post(), outside any web view event (and that tests post()).
    app_.post([this, run] {
        if (!finished_) run([this] { RunNext(); });
    });
}

void SelfTest::WaitUntil(std::function<bool()> condition, Done next) {
    auto timer = std::make_shared<int>(0);
    *timer = app_.every(2ms, [this, timer, condition, next] {
        if (finished_) {
            app_.cancel(*timer);
        } else if (condition()) {
            app_.cancel(*timer);
            next();
        }
    });
}

void SelfTest::Finish(const std::string& failure) {
    if (finished_) return;
    if (!failure.empty()) Check("finish in time", false, failure);
    finished_ = true;
    const std::string summary = std::string(app_.sharedMemory() ? "shared memory" : "fetch fallback") +
                                ": " + std::to_string(passed_) + " passed, " +
                                std::to_string(failed_) + " failed";
    std::printf("\n%s\n", summary.c_str());
    app_.setTitle(title_ + " - " + summary);
    if (!keepOpen_) {
        std::fflush(stdout);
        app_.quit();
        return;
    }
    std::printf("Close the window to exit.\n");
    std::fflush(stdout);
    if (app_.connected()) app_.call("showResults", json::array({summary, failed_ == 0, records_}));
}

uniapi::Task<int> SelfTest::Seven() {
    co_await app_.sleep(1ms);
    co_return 7;
}

uniapi::Task<> SelfTest::FailLater() {
    co_await app_.sleep(1ms);
    throw std::runtime_error("task boom");
}

// The C++ side of the coroutine checks; the page side (exposed coroutines) is in runTests().
uniapi::Task<> SelfTest::CoroutineChecks(Done done) {
    const uniapi::Result sum = co_await app_.callAsync("jsAdd", {2, 3});
    Check("co_await app.callAsync() gives the JavaScript result", sum.ok && sum.value == json(5), Describe(sum));
    const uniapi::Result failed = co_await app_.callAsync("jsThrows");
    Check("co_await app.callAsync() gives a JavaScript error", !failed.ok && failed.error == "js boom",
          Describe(failed));

    const auto start = std::chrono::steady_clock::now();
    co_await app_.sleep(30ms);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    Check("co_await app.sleep(30ms) waits about that long", waited >= 28ms && waited < 1000ms,
          std::to_string(waited.count()) + " ms");

    Check("a Task can be awaited by another coroutine", co_await Seven() == 7);
    bool caught = false;
    try {
        co_await FailLater();
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "task boom";
    }
    Check("an exception in an awaited Task reaches the awaiting coroutine", caught);

    co_await uniapi::switchToBackground();
    const bool offUi = std::this_thread::get_id() != uiThread_;
    co_await app_.switchToUi();
    Check("switchToBackground() and app.switchToUi() change threads",
          offUi && std::this_thread::get_id() == uiThread_);
    done();
}

// The UI thread's rule: from a worker thread, methods that only do something post themselves
// (in order), and those that answer something throw, naming themselves.
uniapi::Task<> SelfTest::ThreadChecks(Done done) {
    struct Seen {
        json echoed;
        bool echoOnUi = false;
        json sum;
        bool callOnUi = false;
        std::vector<std::string> order;
    };
    auto seen = std::make_shared<Seen>();
    const bool uiSays = app_.onUiThread();

    co_await uniapi::switchToBackground();
    const bool workerSays = !app_.onUiThread();
    // Actions: posted to the UI thread, in the order given; their handlers and results run there.
    app_.on("echo-back", [this, seen](const json& data) {
        seen->echoed = data;
        seen->echoOnUi = std::this_thread::get_id() == uiThread_;
        seen->order.push_back("echo");
    });
    app_.emit("ping", {{"from", "a worker thread"}});
    app_.call("jsAdd", {20, 22}, [this, seen](const uniapi::Result& r) {
        seen->sum = r.ok ? r.value : json(r.error);
        seen->callOnUi = std::this_thread::get_id() == uiThread_;
        seen->order.push_back("call");
    });
    app_.setTitle(title_ + " - threads");
    // Answers: they throw, naming themselves.
    auto names = [](const std::function<void()>& fn, const std::string& method) {
        try {
            fn();
        } catch (const std::logic_error& e) {
            return std::string(e.what()).find(method) != std::string::npos;
        }
        return false;
    };
    uniapi::Stream::Slot slot;
    const bool connectedThrows = names([this] { (void)app_.connected(); }, "Window::connected");
    const bool acquireThrows = names([this, &slot] { (void)stream_->acquire(slot); }, "Stream::acquire");
    const bool everyThrows = names([this] { app_.every(1s, [] {}); }, "App::every");
    co_await app_.switchToUi();

    const auto start = std::chrono::steady_clock::now();
    while ((seen->echoed.is_null() || seen->sum.is_null()) && std::chrono::steady_clock::now() - start < 5s) {
        co_await app_.sleep(5ms);
    }
    Check("onUiThread() is true on the UI thread, false on another", uiSays && workerSays);
    Check("from another thread, on() and emit() post themselves: the event comes back, its handler on the UI thread",
          seen->echoed == json{{"from", "a worker thread"}} && seen->echoOnUi, seen->echoed.dump());
    Check("from another thread, call() posts itself: its result comes, on the UI thread", seen->sum == json(42) && seen->callOnUi,
          seen->sum.dump());
    Check("from another thread, methods that answer something throw std::logic_error naming themselves "
          "(connected, Stream::acquire, every)",
          connectedThrows && acquireThrows && everyThrows,
          std::to_string(connectedThrows) + std::to_string(acquireThrows) + std::to_string(everyThrows));
    done();
}

// Taking things back: app.off in the page (one handler, or all) and off() in C++; app.unexpose
// and unexpose(); app.unstream (frames that still come are handed back unread).
uniapi::Task<> SelfTest::TakeBackChecks(Done done) {
    // app.off in the page: the event has two handlers, 'a' and 'b' (see selftest.js); each jsOff
    // answers which ran since the last one.
    app_.emit("off-test");
    uniapi::Result r = co_await app_.callAsync("jsOff", {"a"});
    Check("an event reaches every handler of the page", r.ok && r.value == json::array({"a", "b"}), Describe(r));
    app_.emit("off-test");
    r = co_await app_.callAsync("jsOff", {"all"});
    Check("app.off(event, handler) stops that handler only", r.ok && r.value == json::array({"b"}), Describe(r));
    app_.emit("off-test");
    r = co_await app_.callAsync("jsOff", {"all"});
    Check("app.off(event) stops every handler", r.ok && r.value == json::array(), Describe(r));
    // off() in C++: the page's event is ignored again.
    auto echoed = std::make_shared<bool>(false);
    app_.on("echo-back", [echoed](const json&) { *echoed = true; });
    app_.off("echo-back");
    app_.emit("ping", json{{"n", 1}});
    co_await app_.callAsync("jsAdd", {1, 1});  // the page has handled the ping by now
    Check("off() stops handling the page's event", !*echoed);

    // app.unexpose and unexpose().
    r = co_await app_.callAsync("jsGone");
    const bool gone = r.ok && r.value == json("here");
    co_await app_.callAsync("jsUnexpose");
    r = co_await app_.callAsync("jsGone");
    Check("app.unexpose takes a JS function back",
          gone && !r.ok && r.error == "there is no JavaScript function named \"jsGone\"", Describe(r));
    app_.expose("cppGone", [] {});
    r = co_await app_.callAsync("jsTryNative", {"cppGone"});
    const bool exposed = r.ok && r.value == json("ok");
    app_.unexpose("cppGone");
    r = co_await app_.callAsync("jsTryNative", {"cppGone"});
    Check("unexpose() takes a C++ function back",
          exposed && r.ok && r.value == json("there is no C++ function named \"cppGone\""), Describe(r));

    // app.unstream: the handler isn't called again; the frames that still come are handed back.
    uniapi::Stream& goneStream = app_.stream("gone", 64, 1);
    auto publishGone = [&goneStream] {
        uniapi::Stream::Slot slot;
        return goneStream.acquire(slot) && goneStream.publish(slot, 8);
    };
    co_await app_.sleep(50ms);  // the page has the new stream
    bool published = publishGone();
    auto delivered = [&goneStream](uint64_t n) { return goneStream.delivered() >= n; };
    for (int i = 0; i < 200 && !delivered(1); ++i) co_await app_.sleep(10ms);
    r = co_await app_.callAsync("jsGoneFrames");
    const bool received = published && r.ok && r.value == json(1);
    co_await app_.callAsync("jsUnstream");
    published = publishGone();
    for (int i = 0; i < 200 && !delivered(2); ++i) co_await app_.sleep(10ms);
    r = co_await app_.callAsync("jsGoneFrames");
    Check("app.unstream stops the handler; frames are handed back unread",
          received && published && delivered(2) && r.ok && r.value == json(1), Describe(r));
    done();
}

void SelfTest::Start() {
    std::printf("Before run()\n");
    CheckBeforeRun();
    ExposeFunctions();
    AddSteps();
    currentStep_ = "Connecting";
    std::printf("%s\n", currentStep_.c_str());

    app_.onConnect = [this](const std::string& page) {
        ++connections_;
        if (connections_ == 1) {
            Check("the page connects under its name", page == "selftest", page);
            uniapi::Stream::Slot slot;
            if (hugeStream_) {
                Check("a stream that couldn't be allocated doesn't stop the page, and refuses acquire()",
                      !hugeStream_->acquire(slot));
            } else {
                Note("built with AddressSanitizer: the stream too large to allocate is left out");
            }
            sharedAtStart_ = app_.sharedMemory();
            if (!sharedRequested_) {
                Check("--no-shared-memory turns shared memory off", !app_.sharedMemory());
            } else if (!app_.sharedMemory()) {
                Note("shared memory isn't supported here; streams use the fetch fallback");
            }
            RunNext();
        } else if (onReconnect_) {
            Done next = std::move(onReconnect_);
            onReconnect_ = nullptr;
            next();
        }
    };
    app_.onDisconnect = [this](const std::string&) { ++disconnects_; };

    const auto start = std::chrono::steady_clock::now();
    app_.every(500ms, [this, start] {
        if (!finished_ && std::chrono::steady_clock::now() - start > kTimeout) {
            Finish("timed out in step: " + currentStep_);
        }
    });
}

void SelfTest::CheckBeforeRun() {
    bool answered = false;
    app_.call("jsAdd", json::array({1, 2}), [&](const uniapi::Result& r) {
        answered = true;
        Check("call() without a page fails at once", !r.ok && r.error == "no page is connected", Describe(r));
    });
    if (!answered) Check("call() without a page fails at once", false, "done was not called");
    app_.emit("ping", 1);  // dropped without a page
    Check("connected() is false before run()", !app_.connected());

    stream_ = &app_.stream("test", kSlotBytes, kSlots);
    // Allocated when the web view exists, where it fails: that must not stop the page from
    // loading (it did: the navigation after the allocations never ran). 2^60 bytes is beyond
    // any address space; 1 TB isn't on Linux, where a sparse memfd maps fine. AddressSanitizer
    // ends the program on such an allocation instead of throwing, so not under it.
#if !defined(__SANITIZE_ADDRESS__)
    hugeStream_ = &app_.stream("huge", size_t(1) << 60, 1);
#endif
    uniapi::Stream::Slot slot;
    Check("acquire() fails without a page", !stream_->acquire(slot));

    auto throws = [](const std::function<void()>& fn) {
        try {
            fn();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    Check("stream() refuses a name with /", throws([&] { app_.stream("bad/name", 16, 1); }));
    Check("stream() refuses 0 slots", throws([&] { app_.stream("zero", 16, 0); }));
    Check("stream() refuses the library's own name", throws([&] { app_.stream("uniapi.dom", 16, 1); }));

    // Embedded files (uniapi_embed): a text file is stored compressed (when the embed tool
    // compressed this build) and comes back whole; a file of a compressed type (.png) is stored
    // as it is; a damaged compressed file gives no bytes.
    {
        std::string expected;
        for (int i = 0; i < 300; ++i) expected += "uniapi embeds this line, compressed and back.\n";
        const uniapi::Asset* text = selftest_web.find("embed/pattern.txt");
        const uniapi::AssetData bytes = text ? uniapi::assetData(*text) : uniapi::AssetData{nullptr, 0};
        const bool whole = bytes.data && std::string(reinterpret_cast<const char*>(bytes.data), bytes.size) == expected;
        const bool packed = text && (UNIAPI_TEST_EMBED_COMPRESSED ? text->packedSize > 0 && text->packedSize < text->size / 4
                                                                  : text->packedSize == 0);
        Check("an embedded text file is stored compressed and assetData() gives it back whole", text && whole && packed,
              text ? "size " + std::to_string(text->size) + ", stored " + std::to_string(text->packedSize) : "not found");
        const uniapi::Asset* image = selftest_web.find("embed/pattern.png");
        Check("an embedded file of a compressed type (.png) is stored as it is",
              image && image->packedSize == 0 && uniapi::assetData(*image).data == image->data);
        // A .js file is minified (when this build minifies): no comment, short local names.
        const uniapi::Asset* script = selftest_web.find("embed/minify.js");
        const uniapi::AssetData code = script ? uniapi::assetData(*script) : uniapi::AssetData{nullptr, 0};
        const std::string js = code.data ? std::string(reinterpret_cast<const char*>(code.data), code.size) : std::string();
        const bool minified = js.find("MINIFY-CHECK-COMMENT") == std::string::npos &&
                              js.find("aRatherLongLocalVariableName") == std::string::npos && js.find("answer") != std::string::npos;
        Check("an embedded .js file is minified (no comment, short local names)",
              script && (UNIAPI_TEST_EMBED_MINIFIED ? minified : js.find("MINIFY-CHECK-COMMENT") != std::string::npos), js);
        // .svg and .json are minified too (tdewolff/minify, when this build has it): no comment,
        // no indentation; what they say stays (the page checks that).
        auto embeddedText = [](const char* path) {
            const uniapi::Asset* asset = selftest_web.find(path);
            const uniapi::AssetData content = asset ? uniapi::assetData(*asset) : uniapi::AssetData{nullptr, 0};
            return content.data ? std::string(reinterpret_cast<const char*>(content.data), content.size) : std::string();
        };
        const std::string svg = embeddedText("embed/icon.svg");
        const std::string config = embeddedText("embed/config.json");
        const bool markupMinified = svg.find("MINIFY-CHECK-SVG") == std::string::npos && svg.find("<svg") != std::string::npos &&
                                    config.find("\n") == std::string::npos && config.find("    ") == std::string::npos &&
                                    config.find("spaces  inside  strings  stay") != std::string::npos;
        Check("embedded .svg and .json files are minified (no comment, no indentation; strings kept)",
              UNIAPI_TEST_EMBED_MINIFIED_MARKUP ? markupMinified : svg.find("MINIFY-CHECK-SVG") != std::string::npos,
              svg + " | " + config);
        static const unsigned char junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
        const uniapi::Asset damaged{"damaged.txt", junk, 100, sizeof junk};
        Check("a damaged compressed file gives no bytes (assetData)", uniapi::assetData(damaged).data == nullptr);
    }

    uint8_t buffer[16];
    uniapi::DomCommandWriter writer(buffer, sizeof buffer);
    writer.Create(1, "div");        // 12 bytes
    writer.SetText(1, "too long");  // 17 more: doesn't fit
    Check("DomCommandWriter leaves out a command that doesn't fit",
          writer.overflowed() && writer.size() == 12 && writer.commands() == 1,
          "size " + std::to_string(writer.size()) + ", commands " + std::to_string(writer.commands()));
}

void SelfTest::ExposeFunctions() {
    app_.expose("add", [](int a, int b) { return a + b; });
    app_.expose("concat", [](const std::string& a, const std::string& b) { return a + b; });
    app_.expose("echo", [](const json& value) { return value; });
    app_.expose("sum", [](const std::vector<double>& values) {
        return std::accumulate(values.begin(), values.end(), 0.0);
    });
    app_.expose("nothing", [] {});
    app_.expose("connectedWhenCalled", [this] { return app_.connected(); });
    // A cross-origin iframe in the page tries every way to call this; none may get through.
    app_.expose("calledFromIframe", [this] { iframeReachedCpp_ = true; });
    app_.expose("wasCalledFromIframe", [this] { return iframeReachedCpp_; });
    app_.expose("fail", [](const std::string& message) -> int { throw std::runtime_error(message); });
    app_.expose("failOddly", []() -> int { throw 42; });
    app_.expose("doubleLater", [](const json& args, uniapi::Reply reply) {
        const int value = args.at(0).get<int>();
        std::thread([value, reply] {
            std::this_thread::sleep_for(5ms);
            reply.resolve(value * 2);
        }).detach();
    });
    app_.expose("rejectLater", [](const json&, uniapi::Reply reply) {
        std::thread([reply] { reply.reject("later"); }).detach();
    });
    app_.expose("answerTwice", [](const json&, uniapi::Reply reply) {
        reply.resolve(1);
        reply.resolve(2);
        reply.reject("ignored");
    });
    app_.expose("postBack", [this](const json&, uniapi::Reply reply) {
        std::thread([this, reply] { app_.post([reply] { reply.resolve("posted"); }); }).detach();
    });

    // Binary data from the page.
    app_.expose("bytesInfo", [](const uniapi::Bytes& bytes) {
        return json{{"size", bytes.size()}, {"sum", Checksum(bytes)}};
    });
    app_.expose("bytesPair", [](uniapi::Bytes a, const std::string& label, uniapi::Bytes b) {
        return label + ":" + std::to_string(a.size()) + "+" + std::to_string(b.size());
    });
    app_.expose("bytesNested", [](const json& value) {
        const auto bytes = value.at("items").at(1).at("data").get<uniapi::Bytes>();
        return value.at("name").get<std::string>() + ":" + std::to_string(bytes.size());
    });
    app_.expose("bytesLater", [](const json& args, uniapi::Reply reply) {
        uniapi::Bytes bytes = args.at(0).get<uniapi::Bytes>();  // kept by the worker thread
        std::thread([bytes, reply] {
            std::this_thread::sleep_for(5ms);
            reply.resolve(Checksum(bytes));
        }).detach();
    });
    app_.expose("bytesAfterAnswer", [this](const json& args, uniapi::Reply reply) {
        reply.resolve(true);
        try {
            args.at(0).get<uniapi::Bytes>();
            bytesGoneAfterAnswer_ = false;
        } catch (const std::invalid_argument&) {
            bytesGoneAfterAnswer_ = true;
        }
    });
    app_.expose("bytesGoneAfterAnswer", [this] { return bytesGoneAfterAnswer_; });
    app_.on("log", [this](const json& data) {
        std::string entry = std::to_string(data.at("n").get<int>());
        if (data.contains("data")) entry += ":" + std::to_string(data.at("data").get<uniapi::Bytes>().size());
        eventLog_.push_back(entry);
    });
    app_.expose("eventLog", [this] { return eventLog_; });

    // Binary data to the page.
    app_.expose("makeBytes", [](size_t size, int seed) {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i) data[i] = PatternByte(seed, i);
        return uniapi::Bytes(std::move(data));
    });
    app_.expose("makeNested", [] {
        return json{{"name", "n"}, {"data", uniapi::Bytes("abc", 3)}, {"list", json::array({1, uniapi::Bytes("xy", 2)})}};
    });
    app_.expose("makeEmptyBytes", [] { return uniapi::Bytes(); });
    app_.expose("makeBytesLater", [](const json& args, uniapi::Reply reply) {
        const size_t size = args.at(0).get<size_t>();
        std::thread([size, reply] { reply.resolve(uniapi::Bytes(std::vector<uint8_t>(size, 7))); }).detach();
    });
    app_.expose("echoBytes", [](uniapi::Bytes bytes) { return bytes; });

    // Coroutines (C++20): exposed functions that return uniapi::Task.
    app_.expose("coroAdd", [this](int a, int b) -> uniapi::Task<int> {
        co_await app_.sleep(5ms);
        co_return a + b;
    });
    app_.expose("coroFail", [this](std::string message) -> uniapi::Task<int> {
        co_await app_.sleep(1ms);
        if (!message.empty()) throw std::runtime_error(message);
        co_return 0;
    });
    app_.expose("coroThrowsAtOnce", [](std::string message) -> uniapi::Task<int> {
        throw std::runtime_error(message);
        co_return 0;
    });
    app_.expose("coroVoid", [this]() -> uniapi::Task<> { co_await app_.sleep(1ms); });
    app_.expose("coroBackground", [this](int n) -> uniapi::Task<json> {
        co_await uniapi::switchToBackground();
        const bool offUi = std::this_thread::get_id() != uiThread_;
        long long sum = 0;
        for (int i = 0; i < n; ++i) sum += i;
        co_await app_.switchToUi();
        co_return json{{"sum", sum}, {"offUi", offUi}, {"backOnUi", std::this_thread::get_id() == uiThread_}};
    });
    // Finishes on the background thread: the call is answered from there (the Task race path).
    app_.expose("coroFinishOffUi", [](int n) -> uniapi::Task<int> {
        co_await uniapi::switchToBackground();
        co_return n * 2;
    });
    // Re-registers its own name while running: the running function and its captures must
    // stay alive until it returns (AddressSanitizer would catch a use after free).
    app_.expose("reexpose", [this, text = std::string("first version")]() {
        app_.expose("reexpose", [] { return std::string("second version"); });
        return text;
    });
    // A throwing event handler must not take the app down (it is called from a platform callback).
    app_.on("throwing", [](const json&) { throw std::runtime_error("handler boom"); });
    app_.expose("coroCallsJs", [this](int x) -> uniapi::Task<int> {
        const uniapi::Result r = co_await app_.callAsync("jsAdd", {x, 1});
        if (!r.ok) throw std::runtime_error(r.error);
        co_return r.value.get<int>();
    });
}

bool SelfTest::PublishSlot(const uniapi::Stream::Slot& slot, int seed, size_t size, bool hold) {
    for (size_t i = 0; i < size; ++i) slot.data[i] = PatternByte(seed, i);
    const bool ok = stream_->publish(slot, size, {{"seed", seed}, {"size", size}, {"hold", hold}});
    if (ok) ++published_;
    return ok;
}

bool SelfTest::Publish(int seed, size_t size, bool hold) {
    uniapi::Stream::Slot slot;
    return stream_->acquire(slot) && PublishSlot(slot, seed, size, hold);
}

void SelfTest::CheckStreamStats(const std::string& name, Done done) {
    app_.call("streamStats", json::array(), [this, name, done](const uniapi::Result& r) {
        const bool ok = r.ok && r.value.value("frames", -1) == published_ && r.value.value("bad", -1) == 0;
        Check(name, ok, Describe(r) + ", published " + std::to_string(published_));
        done();
    });
}

void SelfTest::AddSteps() {
    Step("C++ calls JavaScript", [this](Done done) {
        auto remaining = std::make_shared<int>(6);
        auto one = [remaining, done] {
            if (--*remaining == 0) done();
        };
        app_.call("jsAdd", json::array({2, 3}), [this, one](const uniapi::Result& r) {
            Check("a JS function returns a value", r.ok && r.value == json(5), Describe(r));
            one();
        });
        app_.call("jsAsync", json::array({7}), [this, one](const uniapi::Result& r) {
            Check("an async JS function resolves", r.ok && r.value == json(14), Describe(r));
            one();
        });
        app_.call("jsThrows", json::array(), [this, one](const uniapi::Result& r) {
            Check("a JS exception becomes Result::error", !r.ok && r.error == "js boom", Describe(r));
            one();
        });
        app_.call("nope", json::array(), [this, one](const uniapi::Result& r) {
            Check("an unknown JS function fails",
                  !r.ok && r.error == "there is no JavaScript function named \"nope\"", Describe(r));
            one();
        });
        app_.call("jsEcho", json{{"a", 1}}, [this, one](const uniapi::Result& r) {
            Check("a non-array argument is sent as one argument", r.ok && r.value == json{{"a", 1}},
                  Describe(r));
            one();
        });
        app_.call("jsUndefined", json::array(), [this, one](const uniapi::Result& r) {
            Check("undefined arrives as null", r.ok && r.value.is_null(), Describe(r));
            one();
        });
    });

    Step("Events", [this](Done done) {
        const json data = {{"n", 42}, {"text", "héllo ✓"}};
        auto answered = std::make_shared<bool>(false);
        app_.on("echo-back", [this, data, answered, done](const json& received) {
            if (*answered) return;
            *answered = true;
            Check("an event goes to the page and comes back", received == data, received.dump());
            done();
        });
        app_.emit("ping", data);
    });

    // Timer and post() callbacks run from the platform's message loop; an exception there
    // used to leave Run() and end the program. These print "uniapi: ... failed" to stderr.
    Step("Timers and post()", [this](Done done) {
        auto posted = std::make_shared<bool>(false);
        app_.post([posted] {
            *posted = true;
            throw std::runtime_error("post boom (expected)");
        });
        auto ticks = std::make_shared<int>(0);
        auto timer = std::make_shared<int>(0);
        *timer = app_.every(0ms, [this, ticks, timer] {
            if (++*ticks == 3) app_.cancel(*timer);
            throw std::runtime_error("timer boom (expected)");
        });
        Check("every() gives a timer id", *timer != 0);
        // State inside a mutable callback lasts from tick to tick (the backends used to call a
        // fresh copy of the callback each time, so `counted` was 1 on every tick).
        auto seen = std::make_shared<int>(0);
        auto counter = std::make_shared<int>(0);
        *counter = app_.every(1ms, [this, seen, counter, counted = 0]() mutable {
            *seen = ++counted;
            if (counted == 3) app_.cancel(*counter);
        });
        const auto started = std::chrono::steady_clock::now();
        WaitUntil([posted, ticks, seen, started] {
            return (*posted && *ticks >= 3 && *seen >= 3) || std::chrono::steady_clock::now() - started > 2s;
        }, [this, ticks, seen, done] {
            Check("every(0ms) keeps ticking, and throwing doesn't stop it or the app", *ticks == 3,
                  std::to_string(*ticks) + " ticks");
            Check("a mutable timer callback keeps its state from tick to tick", *seen == 3,
                  "it counted to " + std::to_string(*seen));
            done();
        });
    });

    Step("JavaScript calls C++ (checks run in the page)", [this](Done done) {
        app_.call("runTests", json::array(), [this, done](const uniapi::Result& r) {
            if (!r.ok || !r.value.is_array()) {
                Check("the page's checks ran", false, Describe(r));
            } else {
                for (const json& check : r.value) {
                    Check(check.value("name", "?"), check.value("ok", false), check.value("detail", ""));
                }
            }
            done();
        });
    });

    Step("Binary data from C++ to JavaScript", [this](Done done) {
        auto remaining = std::make_shared<int>(3);
        auto one = [remaining, done] {
            if (--*remaining == 0) done();
        };
        std::vector<uint8_t> data(5000);
        for (size_t i = 0; i < data.size(); ++i) data[i] = PatternByte(9, i);
        const uniapi::Bytes sent(std::move(data));
        app_.call("jsBytesInfo", json::array({sent}), [this, sent, one](const uniapi::Result& r) {
            const json expected = {{"isBytes", true}, {"size", 5000}, {"sum", Checksum(sent)}};
            Check("App::call() sends Bytes, which arrive as a Uint8Array", r.ok && r.value == expected, Describe(r));
            one();
        });
        app_.call("jsMakeBytes", json::array({300, 5}), [this, one](const uniapi::Result& r) {
            bool ok = false;
            std::string detail = Describe(r);
            try {
                const uniapi::Bytes bytes = r.value.get<uniapi::Bytes>();
                ok = bytes.size() == 300;
                for (size_t i = 0; ok && i < bytes.size(); ++i) ok = bytes.data()[i] == PatternByte(5, i);
                detail = "size " + std::to_string(bytes.size());
            } catch (const std::exception& e) {
                detail = e.what();
            }
            Check("a JavaScript function returns binary data to C++", ok, detail);
            one();
        });
        app_.emit("binlog", {{"n", 1}, {"data", uniapi::Bytes(std::vector<uint8_t>(1 << 20, 1))}});
        app_.emit("binlog", {{"n", 2}});
        app_.emit("binlog", {{"n", 3}, {"data", uniapi::Bytes("abc", 3)}});
        app_.call("binLog", json::array(), [this, one](const uniapi::Result& r) {
            Check("events with Bytes stay in order with the rest",
                  r.ok && r.value == json::array({"1:1048576", "2", "3:3"}), Describe(r));
            one();
        });
    });

    Step("Coroutines (C++20)", [this](Done done) { CoroutineChecks(done); });
    Step("Threads", [this](Done done) { ThreadChecks(done); });
    Step("Taking things back: off, unexpose, unstream", [this](Done done) { TakeBackChecks(done); });

    Step("Streams: backpressure", [this](Done done) {
        Check("Stream::shared() matches App::sharedMemory()", stream_->shared() == app_.sharedMemory());
        int sent = 0;
        while (sent < kSlots && Publish(100 + sent, 1000 + sent, true)) ++sent;
        Check("every slot can be published", sent == kSlots, std::to_string(sent));
        uniapi::Stream::Slot extra;
        Check("acquire() fails while every slot is with the page", !stream_->acquire(extra));
        auto freed = std::make_shared<bool>(false);
        stream_->onSlotFreed = [freed] { *freed = true; };
        app_.call("releaseHeld", json::array({kSlots}), [this, freed, done](const uniapi::Result& r) {
            Check("the page holds slots while its handler is pending", r.ok && r.value == json(kSlots),
                  Describe(r));
            WaitUntil([this] { return stream_->delivered() >= uint64_t(kSlots); }, [this, freed, done] {
                Check("onSlotFreed fires when a slot comes back", *freed);
                stream_->onSlotFreed = nullptr;
                done();
            });
        });
    });

    Step("Streams: 60 frames of mixed sizes", [this](Done done) {
        const uint64_t target = stream_->delivered() + 60;
        auto sent = std::make_shared<int>(0);
        auto pump = [this, sent] {
            static const size_t kSizes[] = {0, 1, 4096, kSlotBytes, 77777};
            while (*sent < 60 && Publish(200 + *sent, kSizes[*sent % 5], false)) ++*sent;
        };
        stream_->onSlotFreed = pump;
        pump();
        WaitUntil([this, target] { return stream_->delivered() >= target; }, [this, done] {
            stream_->onSlotFreed = nullptr;
            CheckStreamStats("every frame arrives intact (sizes 0 to 256 KB)", done);
        });
    });

    Step("Streams: refused publish", [this](Done done) {
        uniapi::Stream::Slot slot;
        const bool acquired = stream_->acquire(slot);
        Check("publish() refuses more bytes than the slot holds",
              acquired && !stream_->publish(slot, kSlotBytes + 1));
        std::vector<uniapi::Stream::Slot> slots(kSlots);
        int count = 0;
        while (count < kSlots && stream_->acquire(slots[count])) ++count;
        Check("the refused slot is free again", count == kSlots, std::to_string(count) + " free");
        for (int i = 0; i < count; ++i) PublishSlot(slots[i], 300 + i, 16, false);
        const uint64_t target = stream_->delivered() + uint64_t(count);
        WaitUntil([this, target] { return stream_->delivered() >= target; }, done);
    });

    Step("Streams: replaced while connected", [this](Done done) {
        stream_ = &app_.stream("test", 4096, 2);
        Check("a replaced stream starts empty",
              stream_->delivered() == 0 && stream_->slotSize() == 4096 && stream_->slotCount() == 2);
        int sent = 0;
        while (sent < 2 && Publish(400 + sent, 4096 - sent, false)) ++sent;
        Check("the replaced stream accepts frames", sent == 2, std::to_string(sent));
        WaitUntil([this] { return stream_->delivered() >= 2; }, [this, done] {
            CheckStreamStats("frames on the replaced stream arrive intact", done);
        });
    });

    Step("Streams: a name with a space and non-ASCII", [this](Done done) {
        uniapi::Stream& named = app_.stream("two words ✓", 1024, 1);
        uniapi::Stream::Slot slot;
        bool sent = named.acquire(slot);
        if (sent) {
            for (size_t i = 0; i < 100; ++i) slot.data[i] = PatternByte(800, i);
            sent = named.publish(slot, 100, {{"seed", 800}, {"size", 100}});
        }
        Check("a frame goes out on it", sent);
        WaitUntil([&named] { return named.delivered() >= 1; }, [this, done] {
            app_.call("spacedStats", json::array(), [this, done](const uniapi::Result& r) {
                Check("its frame reaches the page's handler intact",
                      r.ok && r.value.value("frames", 0) == 1 && r.value.value("ok", false), Describe(r));
                done();
            });
        });
    });

    Step("DOM command lists", [this](Done done) {
        const bool first = app_.sendDom([](uniapi::DomCommandWriter& dom) {
            dom.Bind(10, "#bind-target");
            dom.SetText(10, "bound");
            dom.Create(1, "div");
            dom.SetAttribute(1, "id", "dom-test");
            dom.SetAttribute(1, "data-from", "C++");
            dom.SetStyle(1, "color", "rgb(1, 2, 3)");
            dom.ToggleClass(1, "on", true);
            dom.ToggleClass(1, "off", false);
            dom.Create(2, "span");
            dom.SetText(2, "héllo ✓");
            dom.Append(1, 2);
            dom.Create(3, "b");
            dom.Append(1, 3);
            dom.Remove(3);
            dom.Create(4, "ul");
            dom.Create(5, "li");
            dom.Append(4, 5);
            dom.Create(6, "li");
            dom.Append(4, 6);
            dom.Clear(4);
            dom.Append(1, 4);
            dom.Append(uniapi::DomCommandWriter::kBody, 1);
        });
        const bool second = app_.sendDom([](uniapi::DomCommandWriter& dom) { dom.SetText(10, "bound again"); });
        const bool third = app_.sendDom([](uniapi::DomCommandWriter& dom) { dom.SetText(10, "too soon"); });
        Check("sendDom() sends lists", first && second);
        Check("sendDom() refuses a third list while two are pending", !third);
        app_.call("inspectDom", json::array(), [this, done](const uniapi::Result& r) {
            const json expected = {{"text", "héllo ✓"}, {"from", "C++"},   {"color", "rgb(1, 2, 3)"},
                                   {"className", "on"}, {"children", 2},   {"listItems", 0},
                                   {"inBody", true},    {"bound", "bound again"}};
            Check("the page shows what the lists built", r.ok && r.value == expected, Describe(r));
            done();
        });
    });

    Step("Reloading the page", [this](Done done) {
        auto result = std::make_shared<uniapi::Result>();
        auto answered = std::make_shared<bool>(false);
        app_.call("never", json::array(), [result, answered](const uniapi::Result& r) {
            *result = r;
            *answered = true;
        });
        const int disconnectsBefore = disconnects_;
        onReconnect_ = [this, result, answered, disconnectsBefore, done] {
            Check("pending calls fail when the page goes away",
                  *answered && !result->ok && result->error == "the page closed", Describe(*result));
            Check("onDisconnect fires once", disconnects_ == disconnectsBefore + 1,
                  std::to_string(disconnects_ - disconnectsBefore));
            Check("the reloaded page connects again", app_.connected() && app_.page() == "selftest");
            published_ = 0;
            app_.call("pageInfo", json::array(), [this, done](const uniapi::Result& r) {
                Check("calls reach the new page", r.ok && r.value.value("search", "") == "?reloaded=1",
                      Describe(r));
                done();
            });
        };
        app_.navigate("/index.html?reloaded=1");
    });

    Step("Streams on the reloaded page", [this](Done done) {
        const uint64_t target = stream_->delivered() + 2;
        int sent = 0;
        while (sent < 2 && Publish(500 + sent, 2048, false)) ++sent;
        Check("the stream works on the new page", sent == 2, std::to_string(sent));
        WaitUntil([this, target] { return stream_->delivered() >= target; }, [this, done] {
            CheckStreamStats("frames reach the new page intact", done);
        });
    });

    // Shared regions that never reach the page must not hang it: it falls back to fetching.
    // ?regions=... makes the page ignore its regions (a test hook in web/selftest.js).
    Step("Shared memory that never arrives", [this](Done done) {
        onReconnect_ = [this, done] {
            Check("a page that gets no shared regions still connects", app_.connected());
            if (sharedAtStart_) {
                Check("App::sharedMemory() then reports the fetch fallback", !app_.sharedMemory());
                Check("so does Stream::shared()", !stream_->shared());
            } else {
                Note("shared memory wasn't in use, so there was nothing to fall back from");
            }
            published_ = 0;
            const uint64_t target = stream_->delivered() + 2;
            int sent = 0;
            while (sent < 2 && Publish(600 + sent, 4000, false)) ++sent;
            WaitUntil([this, target] { return stream_->delivered() >= target; }, [this, done] {
                CheckStreamStats("frames arrive intact through the fallback", done);
            });
        };
        app_.navigate("/index.html?regions=all");
    });

    Step("Shared memory for a later stream that never arrives", [this](Done done) {
        onReconnect_ = [this, done] {
            if (sharedAtStart_) Check("the page connects with shared memory", app_.sharedMemory());
            stream_ = &app_.stream("test", 4096, 2);  // this page ignores the new regions
            published_ = 0;
            int sent = 0;
            while (sent < 2 && Publish(700 + sent, 3000, false)) ++sent;
            Check("the new stream accepts frames", sent == 2, std::to_string(sent));
            WaitUntil([this] { return stream_->delivered() >= 2; }, [this, done] {
                CheckStreamStats("its frames arrive intact through the fallback", [this, done] {
                    if (sharedAtStart_) {
                        Check("Stream::shared() reports the fallback for that stream", !stream_->shared());
                        Check("App::sharedMemory() stays true for the page", app_.sharedMemory());
                    }
                    done();
                });
            });
        };
        app_.navigate("/index.html?regions=later");
    });
}

}  // namespace

int main() {
    bool shared = true;
    bool keepOpen = true;
    std::string dataDirectory;
    const std::string kDataDir = "--data-dir=";
    for (const std::string& argument : uniapi::arguments()) {  // UTF-8, unlike argv on Windows
        if (argument == "--no-shared-memory") {
            shared = false;
        } else if (argument == "--exit") {
            keepOpen = false;
        } else if (argument.rfind(kDataDir, 0) == 0) {
            dataDirectory = argument.substr(kDataDir.size());
        } else {
            std::printf("unknown option %s\n"
                        "usage: uniapi_test [--no-shared-memory] [--exit] [--data-dir=<path>]\n",
                        argument.c_str());
            return 2;
        }
    }

    uniapi::AppOptions options;
    options.dataDirectory = dataDirectory;  // empty: the per-user default
    options.title = shared ? "uniapi self-test (shared memory)" : "uniapi self-test (fetch)";
    options.width = 720;
    options.height = 820;
    options.sharedMemory = shared;
    // Run by ctest (--exit): don't take keyboard focus from whatever you're doing.
    options.activate = keepOpen;
    uniapi::App app(options);
    app.serve(selftest_web);

    SelfTest test(app, shared, keepOpen, options.title);
    test.Start();
    app.navigate("/index.html");
    app.run();
    return test.ExitCode();
}
