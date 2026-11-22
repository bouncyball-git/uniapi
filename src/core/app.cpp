// Platform-independent part of uniapi: the message protocol, calls in both directions,
// events, streams and file serving, for the main window and every further window (each its
// own page connection, Window::Impl). Platform code lives behind detail::Backend.
//
// Messages are JSON text; "t" says what each one is.
//   page -> app   hello {page}           a page called uniapi.connect()
//                 ready {sharedMemory?}  it has mapped every shared region it was given, or
//                                        (sharedMemory: false) gave up waiting and fetches
//                 call {id, fn, args, uploads?}    run a C++ function
//                 result {id, ok, value | error, uploads?}   answer to a C++ -> JS call
//                 event {name, data, uploads?}
//                 release {stream, id, slot}       the page is done with a slot
//                 unshared {stream, id} | {sink, id}  a later stream's or sink's regions never
//                                        came; it fetches (a stream) or uploads (a sink)
//                 put {sink, id, slot, size, meta, uploads?, data?}   the page filled a sink's
//                                        slot (data: its bytes, uploaded, without shared memory)
//   app -> page   welcome {sharedMemory, streams, sinks, session}  answer to hello; shared
//                                        regions follow
//                 stream {stream: {name, id, slots, shared}}   created or replaced
//                 sink {sink: {name, id, slots, size, shared, free}}   created or replaced
//                 free {sink, id, slot}  C++ is done with a sink's slot (or refused it)
//                 call / result / event  as above, the other way, with downloads? for uploads?
//                 frame {stream, id, slot, size, meta}   a slot is ready (the page calls the
//                                        stream's handler in the order the frames were sent)
// A stream may reach several windows' pages (Stream::addWindow): each gets the same frames, in
// its own messages, and the slot is free again once every page it went to released it.
// Every stream gets a new id when it is created or replaced, so messages about a replaced
// stream can't touch its successor. Shared regions arrive tagged {stream, id, slot}, and not
// necessarily in order with the messages: the page waits for a region it doesn't have yet.
// Without shared memory the page fetches /uniapi/stream/<name>/<slot> instead.
//
// A sink is a stream the other way round (Window::sink): the page fills its slots and C++
// receives them. Its regions are shared writable, tagged {sink, id, slot} (ids come from the
// streams' counter, so the page keeps both kinds of region by id). Without them the page puts
// a slot's bytes in its put message as binary data (uploaded as below), and C++ keeps that
// upload as the slot's data until it releases the slot.
//
// A region that never arrives must not hang the page (shared memory is used only if every
// check passes, otherwise silently not). The page waits regionTimeout (3 s by default): at
// connect it then sends ready {sharedMemory: false} and fetches every stream; for a stream
// created later it fetches that frame and sends unshared. The core serves fetches of shared
// slots too, and App::sharedMemory() / Stream::shared() report what the page really uses.
//
// Binary data from the page (an ArrayBuffer, typed array or Blob in a call's arguments or an
// event's data) doesn't go into the JSON. The page POSTs each value to
// /uniapi/upload/<session>/<n> and puts {"$uniapi.upload": n} in its place; once the uploads
// are done it sends the message with "uploads": true. The core swaps each placeholder for
// {"$uniapi.bytes": <random token>} and keeps the data under that token until the call is
// answered (or the event handler returns); Bytes' from_json looks it up. The session (one per
// page connection) keeps a reloaded page's uploads apart from the previous page's. A result
// (page -> app) carries uploads the same way.
//
// Binary data to the page: Bytes' to_json keeps the data under a random string token and
// writes {"$uniapi.download": "<token>"}. A message containing one gets "downloads": true; the
// page fetches /uniapi/download/<token> (once) for each, replaces it with a Uint8Array, and
// only then handles the message. Messages after it wait, so their order is kept.

#include "core/asset_bytes.hpp"
#include "core/backend.hpp"
#include "core/desktop.hpp"
#include "core/ending.hpp"
#include "core/placement.hpp"
#include "core/library_files.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

UNIAPI_ASSETS(uniapi_library_js);  // js/ compiled into the library (see CMakeLists.txt)

namespace uniapi {
namespace {

constexpr size_t kDomSlotBytes = 64 * 1024;
constexpr int kDomSlots = 2;
constexpr std::string_view kDomStream = "uniapi.dom";
constexpr std::string_view kStreamPrefix = "/uniapi/stream/";
constexpr std::string_view kLibraryPrefix = "/uniapi/";
constexpr std::string_view kUploadPrefix = "/uniapi/upload/";
constexpr char kUploadKey[] = "$uniapi.upload";  // placeholder from the page: {key: n}
constexpr char kBytesKey[] = "$uniapi.bytes";    // after the core took the data: {key: token}
constexpr char kDownloadKey[] = "$uniapi.download";  // C++ to page: {key: "<token>"}
constexpr std::string_view kDownloadPrefix = "/uniapi/download/";
constexpr int kMaxSlots = 64;

const char* MimeTypeFor(std::string_view path) {
    static const struct { const char* extension; const char* type; } kTypes[] = {
        {".html", "text/html; charset=utf-8"},  {".htm", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},    {".json", "application/json"},
        {".svg", "image/svg+xml"},              {".png", "image/png"},
        {".jpg", "image/jpeg"},                 {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},                  {".webp", "image/webp"},
        {".ico", "image/x-icon"},               {".wasm", "application/wasm"},
        {".woff2", "font/woff2"},               {".txt", "text/plain; charset=utf-8"},
    };
    size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos) {
        std::string extension(path.substr(dot));
        for (char& c : extension) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const auto& entry : kTypes) {
            if (extension == entry.extension) return entry.type;
        }
    }
    return "application/octet-stream";
}

const Asset* FindAsset(const AssetTable* table, std::string_view path) {
    if (!table) return nullptr;
    for (size_t i = 0; i < table->count; ++i) {
        if (path == table->items[i].path) return &table->items[i];
    }
    return nullptr;
}

// Files optional parts of uniapi add under /uniapi/ (see library_files.hpp). A function-local
// static, so it exists whenever another object's static initializer registers into it.
struct LibraryFiles {
    std::string_view prefix;
    const AssetTable* table;
};
std::vector<LibraryFiles>& ExtraLibraryFiles() {
    static std::vector<LibraryFiles> files;
    return files;
}

// A file under /uniapi/: the page library itself, or one an optional part registered.
const Asset* FindLibraryAsset(std::string_view path) {
    if (const Asset* asset = FindAsset(&uniapi_library_js, path)) return asset;
    for (const LibraryFiles& files : ExtraLibraryFiles()) {
        if (path.substr(0, files.prefix.size()) == files.prefix) {
            if (const Asset* asset = FindAsset(files.table, path.substr(files.prefix.size()))) return asset;
        }
    }
    return nullptr;
}

// An embedded file's bytes: as stored (they live as long as the program and need no keepAlive),
// or, for a compressed one, inflated for this request alone: the response owns them, and they
// go when the platform has sent them. Nothing inflated is kept, so a compressed file costs no
// memory after it was served (assets.cpp).
void Serve(const Asset& asset, detail::Response& response) {
    response.mimeType = MimeTypeFor(asset.path);
    if (asset.packedSize == 0) {
        response.status = 200;
        response.data = asset.data;
        response.size = asset.size;
        return;
    }
    auto bytes = detail::InflateAsset(asset);
    if (!bytes) {
        response.status = 500;  // a damaged build: the file can't be inflated
        return;
    }
    response.status = 200;
    response.data = bytes->data();
    response.size = bytes->size();
    response.keepAlive = std::move(bytes);
}

// A whole decimal number from a request path ("12"); false for "", "1x", "-1" or overflow.
bool ParseNumber(const std::string& text, uint64_t& out) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
    errno = 0;
    out = std::strtoull(text.c_str(), nullptr, 10);
    return errno == 0;
}

// "%20" -> " " and so on (UTF-8 bytes stay as they are). False for a malformed escape or a
// NUL byte, which no file or stream name has.
bool PercentDecode(const std::string& text, std::string& out) {
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) return false;  // needs two hex digits after it
            const int high = hex(text[i + 1]);
            const int low = hex(text[i + 2]);
            if (high < 0 || low < 0) return false;
            c = static_cast<char>(high * 16 + low);
            i += 2;
        }
        if (c == '\0') return false;
        out += c;
    }
    return true;
}

std::string Dump(const json& message) {
    return message.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Runs code called from a platform callback (a window message, a COM or GTK callback), where
// an escaping exception would end the program: it is printed instead. `what` names the code.
template <typename F>
void RunGuarded(const char* what, F&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uniapi: %s failed: %s\n", what, e.what());
    } catch (...) {
        std::fprintf(stderr, "uniapi: %s failed (unknown exception)\n", what);
    }
}

// Binary data from the page by token, for Bytes' from_json, which has nothing but the JSON to
// go on. Tokens are random, so page data can't name one; any thread may read.
class BytesRegistry {
public:
    using Data = std::shared_ptr<const std::vector<uint8_t>>;

    uint64_t Add(Data data) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t token;
        do {
            token = random_();
        } while (token == 0 || entries_.count(token));
        entries_.emplace(token, std::move(data));
        return token;
    }
    Data Find(uint64_t token) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(token);
        return it == entries_.end() ? nullptr : it->second;
    }
    void Remove(const std::vector<uint64_t>& tokens) {
        if (tokens.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint64_t token : tokens) entries_.erase(token);
    }

private:
    std::mutex mutex_;
    std::mt19937_64 random_{std::random_device{}()};
    std::unordered_map<uint64_t, Data> entries_;
};

BytesRegistry& Registry() {
    static BytesRegistry registry;
    return registry;
}

// Binary data C++ sends to the page (a Bytes in JSON), kept under a random token until the
// page fetches /uniapi/download/<token>, once. String tokens: JavaScript numbers can't hold 64
// bits. Data nobody fetches (the page went away, or JSON built but never sent) is dropped when
// the page ends; entries older than a minute are also pruned whenever new data is added.
class Downloads {
public:
    using Data = std::shared_ptr<const std::vector<uint8_t>>;

    std::string Add(Data data) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = now - it->second.created > std::chrono::minutes(1) ? entries_.erase(it) : std::next(it);
        }
        std::string token;
        do {
            char text[33];
            std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(random_()),
                          static_cast<unsigned long long>(random_()));
            token = text;
        } while (entries_.count(token));
        entries_.emplace(token, Entry{std::move(data), now});
        return token;
    }
    Data Take(const std::string& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(token);
        if (it == entries_.end()) return nullptr;
        Data data = std::move(it->second.data);
        entries_.erase(it);
        return data;
    }
    bool Empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.empty();
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    struct Entry {
        Data data;
        std::chrono::steady_clock::time_point created;
    };
    std::mutex mutex_;
    std::mt19937_64 random_{std::random_device{}()};
    std::unordered_map<std::string, Entry> entries_;
};

Downloads& DownloadRegistry() {
    static Downloads downloads;
    return downloads;
}

bool ContainsDownload(const json& value, int depth = 0) {
    if (depth > 64) return false;
    if (value.is_object()) {
        if (value.size() == 1 && value.contains(kDownloadKey)) return true;
        for (const json& item : value) {
            if (ContainsDownload(item, depth + 1)) return true;
        }
    } else if (value.is_array()) {
        for (const json& item : value) {
            if (ContainsDownload(item, depth + 1)) return true;
        }
    }
    return false;
}

// Holds the data of one call or event under tokens until it goes out of scope.
struct UploadedBytes {
    std::vector<uint64_t> tokens;
    UploadedBytes() = default;
    UploadedBytes(const UploadedBytes&) = delete;
    UploadedBytes& operator=(const UploadedBytes&) = delete;
    ~UploadedBytes() { Registry().Remove(tokens); }
};

}  // namespace

bool detail::AddLibraryFiles(const char* prefix, const AssetTable* table) {
    if (prefix && table) ExtraLibraryFiles().push_back({prefix, table});
    return true;
}

std::string commandLine() {
    std::string joined;
    for (const std::string& argument : arguments()) {
        if (!joined.empty()) joined += ' ';
        joined += argument;
    }
    return joined;
}

// ----------------------------------------------------------------------------------------
// Bytes

Bytes::Bytes(std::vector<uint8_t> data)
    : data_(std::make_shared<const std::vector<uint8_t>>(std::move(data))) {}

Bytes::Bytes(const void* data, size_t size)
    : data_(std::make_shared<const std::vector<uint8_t>>(static_cast<const uint8_t*>(data),
                                                         static_cast<const uint8_t*>(data) + size)) {}

const std::vector<uint8_t>& Bytes::vector() const {
    static const std::vector<uint8_t> empty;
    return data_ ? *data_ : empty;
}

void to_json(json& value, const Bytes& bytes) {
    auto data = bytes.data_ ? bytes.data_ : std::make_shared<const std::vector<uint8_t>>();
    value = json{{kDownloadKey, DownloadRegistry().Add(std::move(data))}};
}

void from_json(const json& value, Bytes& bytes) {
    if (value.is_object() && value.size() == 1) {
        auto it = value.find(kBytesKey);
        if (it != value.end() && it->is_number_unsigned()) {
            if (auto data = Registry().Find(it->get<uint64_t>())) {
                bytes = Bytes(std::move(data));
                return;
            }
            throw std::invalid_argument(
                "the binary data is gone: take it before answering the call (or returning from the event handler)");
        }
    }
    throw std::invalid_argument("expected binary data (an ArrayBuffer, typed array, DataView or Blob)");
}

// ----------------------------------------------------------------------------------------
// Internal state

struct Stream::Impl {
    enum class State { Free, Writing, WithPage };
    struct SlotData {
        std::unique_ptr<detail::SharedRegion> region;  // shared memory mode
        std::shared_ptr<std::vector<uint8_t>> heap;    // fetch mode; requests keep it alive
        uint8_t* data = nullptr;
        State state = State::Free;
        size_t size = 0;
        // A page went away holding it: its fetch may still be reading the buffer on another
        // thread, so the slot gets a fresh one when it is free again (fetch mode).
        bool renew = false;
    };
    // A window whose page receives it.
    struct Target {
        Window::Impl* window = nullptr;
        bool active = true;        // false: removed, but its page still holds slots (it hands them back)
        bool pageFetches = false;  // its current page didn't get the regions and fetches instead
        uint64_t held = 0;         // the slots with its page, one bit each (64 slots at most)
    };

    Window::Impl* window = nullptr;  // the window it belongs to (its streams hold it)
    Stream* owner = nullptr;
    std::string name;
    uint64_t id = 0;  // new for every created or replaced stream
    size_t slotSize = 0;
    int slotCount = 0;
    bool shared = false;       // the slots are shared regions
    bool allocated = false;
    std::vector<SlotData> slots;
    std::vector<Target> targets;  // the windows it reaches; the one it belongs to first
    uint64_t delivered = 0;
    bool waiting = false;

    ~Impl();  // leaves the other windows it reached (after Window::Impl)
    void Allocate(detail::Backend& backend, bool useShared);
    Target* TargetOf(const Window::Impl* w) {
        for (Target& t : targets) {
            if (t.window == w) return &t;
        }
        return nullptr;
    }
    bool SharedWith(const Target& t) const { return allocated && shared && !t.pageFetches; }
    json Info(const Target& t) const {
        return {{"name", name}, {"id", id}, {"slots", slotCount}, {"shared", SharedWith(t)}};
    }
    // Frees a slot no page holds any more. True if it was with a page.
    bool FreeIfUnheld(int index) {
        SlotData& slot = slots[size_t(index)];
        const uint64_t bit = uint64_t(1) << index;
        for (const Target& t : targets) {
            if (t.held & bit) return false;
        }
        if (slot.state != State::WithPage) return false;
        slot.state = State::Free;
        if (slot.renew && slot.heap) {
            slot.heap = std::make_shared<std::vector<uint8_t>>(slotSize);
            slot.data = slot.heap->data();
        }
        slot.renew = false;
        return true;
    }
    // Drops what a window's page held without handing it back (it went away). True if a slot
    // became free.
    bool DropHeld(Target& t) {
        const uint64_t held = t.held;
        t.held = 0;
        bool freed = false;
        for (int i = 0; i < int(slots.size()); ++i) {
            if (!(held & (uint64_t(1) << i))) continue;
            slots[size_t(i)].renew = true;
            if (FreeIfUnheld(i)) freed = true;
        }
        return freed;
    }
    void ResetForNewPage(Window::Impl* w);  // after Window::Impl
    void Release(Window::Impl* w, int index);
    void RemoveTarget(Window::Impl* w);
};

struct Sink::Impl {
    enum class State { Page, App };  // the page may fill it; C++ has it (published, not released)
    struct SlotData {
        std::unique_ptr<detail::SharedRegion> region;  // shared memory mode
        std::shared_ptr<std::vector<uint8_t>> upload;  // what the page uploaded (no shared memory)
        State state = State::Page;
    };

    Window::Impl* window = nullptr;  // the window it belongs to (its sinks hold it)
    Sink* owner = nullptr;
    std::string name;
    uint64_t id = 0;  // new for every created or replaced sink (from the streams' counter)
    size_t slotSize = 0;
    int slotCount = 0;
    bool shared = false;       // the slots are shared regions
    bool allocated = false;
    bool pageUploads = true;   // the current page didn't get the regions (or has none): it uploads
    std::vector<SlotData> slots;
    uint64_t received = 0;

    void Allocate(detail::Backend& backend, bool useShared);
    bool SharedWithPage() const { return allocated && shared && !pageUploads; }
    json Info() const {
        json free = json::array();
        for (int i = 0; i < int(slots.size()); ++i) {
            if (slots[size_t(i)].state == State::Page) free.push_back(i);
        }
        return {{"name", name}, {"id", id}, {"slots", slotCount}, {"size", slotSize},
                {"shared", SharedWithPage()}, {"free", free}};
    }
    void Put(const json& message);   // after Window::Impl
    void ReleaseSlot(int index);     // C++ is done with it: back to the page
};

namespace {

// The main window as a BackendWindow: the backend's own window methods.
class MainWindow final : public detail::BackendWindow {
public:
    explicit MainWindow(detail::Backend& backend) : backend_(backend) {}
    void Navigate(const std::string& path) override { backend_.Navigate(path); }
    void PostToPage(const std::string& text) override { backend_.PostToPage(text); }
    void ShareRegion(detail::SharedRegion& region, const std::string& infoJson, bool writable) override {
        backend_.ShareRegion(region, infoJson, writable);
    }
    void SetTitle(const std::string& title) override { backend_.SetTitle(title); }
    void OpenDevTools() override { backend_.OpenDevTools(); }
    void SetFullscreen(bool on) override { backend_.SetFullscreen(on); }
    void SetSize(int width, int height) override { backend_.SetSize(width, height); }
    bool IsFullscreen() const override { return backend_.IsFullscreen(); }
    void SetTitleBar(TitleBar mode) override { backend_.SetTitleBar(mode); }
    TitleBar GetTitleBar() const override { return backend_.GetTitleBar(); }
    void Show() override { backend_.Show(); }
    void SetVisible(bool visible) override { backend_.SetVisible(visible); }
    bool IsVisible() const override { return backend_.IsVisible(); }
    bool SetIcon(const unsigned char* png, size_t size) override { return backend_.SetIcon(png, size); }
    void Close() override { backend_.Quit(); }
    Size CurrentSize() const override { return backend_.CurrentSize(); }
    std::optional<Point> Position() const override { return backend_.Position(); }
    bool SetPosition(int x, int y) override { return backend_.SetPosition(x, y); }
    void Center() override { backend_.Center(); }
    detail::WindowState State() const override { return backend_.State(); }
    void SetState(detail::WindowState state) override { backend_.SetState(state); }
    detail::Bounds NormalBounds() const override { return backend_.NormalBounds(); }
    bool SetAlwaysOnTop(bool on) override { return backend_.SetAlwaysOnTop(on); }
    bool IsAlwaysOnTop() const override { return backend_.IsAlwaysOnTop(); }
    std::vector<Screen> Screens() const override { return backend_.Screens(); }

private:
    detail::Backend& backend_;
};

}  // namespace

// One window's page connection: everything its page and C++ say to each other.
struct Window::Impl : std::enable_shared_from_this<Window::Impl> {
    enum class PageState { None, Connecting, Connected };

    std::shared_ptr<App::Impl> app;  // the program; a reply answered on another thread holds it
    Window* owner = nullptr;         // the public object (null once it is gone)
    detail::BackendWindow* window = nullptr;      // its window: the main window's adapter, or `own`
    std::unique_ptr<detail::BackendWindow> own;   // a further window's
    bool main = false;

    std::map<std::string, Handler, std::less<>> functions;
    std::map<std::string, std::function<void(const json&)>, std::less<>> events;
    std::map<uint64_t, std::function<void(const Result&)>> pendingCalls;
    std::map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> uploads;  // by the page's number
    uint64_t nextCallId = 1;
    std::vector<std::unique_ptr<Stream>> streams;
    std::vector<Stream::Impl*> received;  // other windows' streams that reach this one's page
    std::vector<std::unique_ptr<Sink>> sinks;
    Stream* dom = nullptr;

    bool ready = false;       // its web view exists
    bool useShared = false;   // shared memory works with its web view
    bool pageShared = false;  // ...and the current page got its regions (see "ready")
    std::optional<std::string> pendingNavigation;
    PageState pageState = PageState::None;
    std::string page;
    uint64_t generation = 0;  // this page connection's session (unique across windows)
    bool closed = false;

    void Send(json message) {
        if (!window) return;
        // Bytes in the message became download placeholders; tell the page to fetch them first.
        if (!DownloadRegistry().Empty() && ContainsDownload(message)) message["downloads"] = true;
        window->PostToPage(Dump(message));
    }
    detail::BackendEvents Events();
    void OnReady(bool sharedMemorySupported);
    void OnMessage(const std::string& text);
    void Dispatch(const std::string& text);
    void OnRequest(detail::Request& request, detail::Response& response);
    bool OnCloseRequested(CloseReason reason);
    void OnClosed();
    // Placement (rememberPlacement): the name it is kept under, the latest seen, saved once.
    std::string placementName;
    std::optional<detail::SavedPlacement> lastPlacement;
    bool placementKept = false;
    std::optional<detail::SavedPlacement> CurrentPlacement() const;
    void ApplyPlacement(detail::SavedPlacement placement);
    void RestorePlacement(const std::string& name);
    void OnPlacementChanged();
    void KeepPlacement();
    void OnExternalNavigation(const std::string& url);
    void OnFileDrop(const std::vector<std::string>& paths, int x, int y);
    void OnNotificationClicked(const std::string& id);
    bool TakeUploads(json& value, std::vector<uint64_t>& tokens, int depth = 0);
    void StartPage(const std::string& name);
    void EndPage();
    void ShareStream(Stream::Impl& stream);
    void Announce(Stream::Impl& stream);  // a stream new to its connected page
    void ShareSink(Sink::Impl& sink);
    void AnnounceSink(Sink::Impl& sink);  // a sink new to its connected page
    Sink::Impl* FindSink(std::string_view name) {
        for (auto& sink : sinks) {
            if (sink->name() == name) return sink->internal();
        }
        return nullptr;
    }
    // Every stream its page receives: its own and other windows' (a copy: the loops change them).
    std::vector<Stream::Impl*> PageStreams() const {
        std::vector<Stream::Impl*> all;
        for (const auto& stream : streams) all.push_back(stream->internal());
        all.insert(all.end(), received.begin(), received.end());
        return all;
    }
    void LeaveStreams();  // it is going: out of the other windows' streams
    void HandleCall(const json& message);
    void SendResult(uint64_t id, uint64_t forGeneration, bool ok, json value, std::string error);
    Stream::Impl* FindStream(std::string_view name);
    // A further window's page reaches the main window's functions and events where its own
    // window has none of that name.
    const Handler* FindFunction(std::string_view name) const;
    const std::function<void(const json&)>* FindEvent(std::string_view name) const;
};

struct App::Impl {
    App* owner = nullptr;
    AppOptions options;
    std::unique_ptr<detail::Backend> backend;
    // Guards `backend` against Answer() and App::post() on a worker thread while ~App resets it.
    // The UI thread reads it without the lock: only the UI thread changes it.
    std::mutex backendMutex;

    const AssetTable* assets = nullptr;
    std::string assetDirectory;
    uint64_t nextStreamId = 1;
    uint64_t nextSession = 1;   // page connections, across every window
    Window::Impl* main = nullptr;
    Window* sender = nullptr;  // App::sender(): whose page's call or event is being handled
    std::unique_ptr<detail::BackendWindow> mainWindow;  // the main window as a BackendWindow
    std::vector<std::unique_ptr<Window>> windows;       // further windows, until they close
    detail::InstanceHandle instance;  // singleInstance: listening for later launches
    bool running = false;             // run() has started: the windows exist
    std::thread::id uiThread = std::this_thread::get_id();  // the App's: where it was made
    std::map<int, std::function<void()>> ending;  // AddEndingStep's, run first by ~App
    int nextEnding = 1;
    std::atomic<uint64_t> nextNotification{1};
    // Makes notification ids unique across runs: macOS keeps notifications in its list after
    // the program ends, and a click on one comes to the next run.
    std::string notificationRun = [] {
        char run[24];
        std::snprintf(run, sizeof run, "%llx",
                      static_cast<unsigned long long>(std::chrono::system_clock::now().time_since_epoch().count()));
        return std::string(run);
    }();

    // Pages connected or connecting, in every window.
    int Pages() const {
        int count = main && main->pageState != Window::Impl::PageState::None ? 1 : 0;
        for (const auto& window : windows) {
            if (window->internal()->pageState != Window::Impl::PageState::None) ++count;
        }
        return count;
    }
    // The stream with this id in any window, or nullptr (it was replaced or its window is gone).
    Stream::Impl* StreamById(uint64_t id) const {
        auto find = [id](const Window::Impl& w) -> Stream::Impl* {
            for (const auto& stream : w.streams) {
                if (stream->internal()->id == id) return stream->internal();
            }
            return nullptr;
        };
        if (main) {
            if (Stream::Impl* found = find(*main)) return found;
        }
        for (const auto& window : windows) {
            if (Stream::Impl* found = find(*window->internal())) return found;
        }
        return nullptr;
    }
    // A further window that closed: destroyed (out of the platform's close event, see OnClosed).
    void Remove(Window::Impl* link) {
        auto it = std::find_if(windows.begin(), windows.end(), [&](const auto& w) { return w->internal() == link; });
        if (it == windows.end()) return;
        std::unique_ptr<Window> gone = std::move(*it);
        windows.erase(it);
    }
};

// --- The UI thread ---
// GTK, Cocoa and WebView2 work only from the thread that started them: the App's, where every
// handler, timer and posted function runs. App, Window and Stream belong to it:
//   - Methods that only do something (emit, call, setTitle, show, hide, close, navigate, expose,
//     on, quit...) work from any thread: called elsewhere they post themselves to the UI thread
//     (Elsewhere) and return at once, so they happen a moment later, in order with other posted
//     work.
//   - Methods that answer something or must act at once (connected, visible, Stream::acquire,
//     Stream::publish, openWindow, the dialogs...) throw std::logic_error when called elsewhere
//     (UiOnly), naming themselves: a call from the wrong thread would otherwise race with the UI
//     thread, and crash, hang or be lost depending on the platform.
// post(), notify(), Reply and onUiThread() work from anywhere.
namespace {

bool OnUiThread(const App::Impl& program) { return std::this_thread::get_id() == program.uiThread; }

void PostToUi(App::Impl& program, std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(program.backendMutex);  // the App may be ending right now
    if (program.backend) program.backend->Post([fn = std::move(fn)] { RunGuarded("a call from another thread", fn); });
}

[[noreturn]] void WrongThread(const char* method) {
    throw std::logic_error(std::string("uniapi: ") + method +
                           " called from another thread: it belongs to the UI thread (call it in a handler, a timer, "
                           "or inside app.post)");
}

void UiOnly(const App::Impl* program, const char* method) {
    if (program && !OnUiThread(*program)) WrongThread(method);
}

// The posting itself, once for all methods (Elsewhere below): fn(app) on the UI thread, if the
// app is still there by then. Out of line, so that a method holds only the handing over of its
// arguments, not this.
[[gnu::noinline]] void PostToApp(const std::shared_ptr<App::Impl>& program, std::function<void(App&)> fn) {
    std::weak_ptr<App::Impl> weak = program;
    PostToUi(*program, [weak, fn = std::move(fn)] {
        if (const auto p = weak.lock(); p && p->owner) fn(*p->owner);
    });
}

// As PostToApp, for a window: fn(window), if the window is still there by then.
[[gnu::noinline]] void PostToWindow(const std::shared_ptr<Window::Impl>& impl, std::function<void(Window&)> fn) {
    std::weak_ptr<Window::Impl> weak = impl;
    PostToUi(*impl->app, [weak, fn = std::move(fn)] {
        if (const auto w = weak.lock(); w && w->owner) fn(*w->owner);
    });
}

// Off the UI thread: fn(app) is posted to it and true says the method should return; on it, false.
template <typename F>
bool Elsewhere(const std::shared_ptr<App::Impl>& program, F&& fn) {
    if (!program || OnUiThread(*program)) return false;
    PostToApp(program, std::forward<F>(fn));
    return true;
}

// As Elsewhere, for a window: fn(window) on the UI thread, if the window is still there by then.
template <typename F>
bool Elsewhere(const std::shared_ptr<Window::Impl>& impl, F&& fn) {
    App::Impl* program = impl->app.get();
    if (!program || OnUiThread(*program)) return false;
    PostToWindow(impl, std::forward<F>(fn));
    return true;
}

// The common case of Elsewhere: a method and its arguments, posted as they are (a string_view's
// text copied). The posted call depends only on the method's signature, not on the method, so
// methods alike (show, hide, close...; setTitle, navigate, off...) share one. Called as
//   if (Elsewhere(impl_, &Window::setTitle, title)) return;
template <typename T>
struct Kept {
    using type = std::decay_t<T>;
};
template <>
struct Kept<std::string_view> {
    using type = std::string;
};
template <typename T>
struct AsIs {  // keeps a parameter out of template argument deduction (C++20's type_identity)
    using type = T;
};

template <typename R, typename... Params>
[[gnu::noinline]] void PostCall(const std::shared_ptr<Window::Impl>& impl, R (Window::*method)(Params...),
                                const typename AsIs<Params>::type&... args) {
    // One function posted, holding the window weakly (as PostToWindow's), not one inside another.
    PostToUi(*impl->app, [weak = std::weak_ptr<Window::Impl>(impl), method,
                          kept = std::tuple<typename Kept<Params>::type...>(args...)]() mutable {
        if (const auto w = weak.lock(); w && w->owner) {
            std::apply([&](auto&... a) { (w->owner->*method)(std::move(a)...); }, kept);
        }
    });
}

template <typename R, typename... Params>
[[gnu::noinline]] void PostCall(const std::shared_ptr<App::Impl>& program, R (App::*method)(Params...),
                                const typename AsIs<Params>::type&... args) {
    PostToUi(*program, [weak = std::weak_ptr<App::Impl>(program), method,
                        kept = std::tuple<typename Kept<Params>::type...>(args...)]() mutable {
        if (const auto p = weak.lock(); p && p->owner) {
            std::apply([&](auto&... v) { (p->owner->*method)(std::move(v)...); }, kept);
        }
    });
}

template <typename R, typename... Params>
bool Elsewhere(const std::shared_ptr<Window::Impl>& impl, R (Window::*method)(Params...),
               const typename AsIs<Params>::type&... args) {
    App::Impl* program = impl->app.get();
    if (!program || OnUiThread(*program)) return false;
    PostCall(impl, method, args...);
    return true;
}

template <typename R, typename... Params>
bool Elsewhere(const std::shared_ptr<App::Impl>& program, R (App::*method)(Params...), const typename AsIs<Params>::type&... args) {
    if (!program || OnUiThread(*program)) return false;
    PostCall(program, method, args...);
    return true;
}

void UiOnly(const Window::Impl& impl, const char* method) { UiOnly(impl.app.get(), method); }

}  // namespace

struct Reply::State {
    std::weak_ptr<Window::Impl> window;
    uint64_t id = 0;
    uint64_t generation = 0;
    std::atomic<bool> answered{false};
    UploadedBytes bytes;  // the call's binary data, kept until it is answered
};

// ----------------------------------------------------------------------------------------
// Reply

namespace {
void Answer(const std::shared_ptr<Reply::State>& state, bool ok, json value, std::string error) {
    if (state->answered.exchange(true)) return;  // the first answer wins
    Registry().Remove(state->bytes.tokens);      // binary data is valid until the answer
    std::shared_ptr<Window::Impl> window = state->window.lock();
    if (!window) return;
    std::shared_ptr<App::Impl> app = window->app;
    auto send = [window, id = state->id, generation = state->generation, ok,
                 value = std::move(value), error = std::move(error)]() mutable {
        window->SendResult(id, generation, ok, std::move(value), std::move(error));
    };
    {
        // From another thread: the App may be shutting down on the UI thread right now.
        std::lock_guard<std::mutex> lock(app->backendMutex);
        if (!app->backend) return;
        if (!app->backend->OnUiThread()) {
            app->backend->Post(std::move(send));
            return;
        }
    }
    send();  // on the UI thread, where ~App can't be running at the same time
}
}  // namespace

bool Reply::strictCalls() const {
    const std::shared_ptr<Window::Impl> window = state_ ? state_->window.lock() : nullptr;
    return window && window->app && window->app->options.strictCalls;
}

bool detail::containsBytes(const json& value) {
    if (value.is_object()) {
        if (value.contains(kBytesKey)) return true;
        for (const auto& item : value) {
            if (containsBytes(item)) return true;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (containsBytes(item)) return true;
        }
    }
    return false;
}

namespace {
// Every key of `given` is in `back` (the decoded value converted to JSON again), at any depth.
void CompareFields(const json& given, const json& back, const std::string& path, size_t index) {
    if (given.is_object() && back.is_object()) {
        for (auto it = given.begin(); it != given.end(); ++it) {
            const std::string field = path.empty() ? it.key() : path + "." + it.key();  // e.g. channels[0].pan
            const auto found = back.find(it.key());
            if (found == back.end()) {
                throw std::invalid_argument("argument " + std::to_string(index + 1) + ": unknown field \"" + field + "\"");
            }
            CompareFields(it.value(), *found, field, index);
        }
    } else if (given.is_array() && back.is_array()) {
        for (size_t i = 0; i < given.size() && i < back.size(); ++i) {
            CompareFields(given[i], back[i], path + "[" + std::to_string(i) + "]", index);
        }
    }
}
}  // namespace

void detail::checkKnownFields(const json& given, const json& decoded, size_t index) {
    CompareFields(given, decoded, "", index);
}

void Reply::resolve(json value) const {
    Answer(state_, true, std::move(value), {});
}

void Reply::reject(std::string message) const {
    Answer(state_, false, nullptr, std::move(message));
}

// ----------------------------------------------------------------------------------------
// Stream

Stream::Impl::~Impl() {
    for (const Target& t : targets) {
        if (t.window == window) continue;
        auto& list = t.window->received;
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
    }
}

void Stream::Impl::ResetForNewPage(Window::Impl* w) {
    Target* t = TargetOf(w);
    if (!t) return;
    const bool freed = DropHeld(*t);
    t->pageFetches = !w->useShared;  // a web view without shared memory fetches
    if (!t->active) {
        RemoveTarget(w);
        return;
    }
    if (w == window) {
        waiting = false;  // its own page starts again (onConnect)
    } else if (freed && waiting) {
        // Another window's page went away holding slots the others had handed back: tell C++,
        // later (this runs inside the platform's navigation or close event, and onSlotFreed may
        // replace this very stream).
        waiting = false;
        App::Impl* program = window->app.get();
        const uint64_t streamId = id;
        if (program->backend) {
            program->backend->Post([program, streamId] {
                Stream::Impl* stream = program->StreamById(streamId);
                if (!stream || !stream->owner) return;
                auto onSlotFreed = stream->owner->onSlotFreed;
                if (onSlotFreed) RunGuarded("onSlotFreed", onSlotFreed);
            });
        }
    }
}

void Stream::Impl::Release(Window::Impl* w, int index) {
    Target* t = TargetOf(w);
    if (!t || index < 0 || size_t(index) >= slots.size()) return;  // slots: empty until allocated
    const uint64_t bit = uint64_t(1) << index;
    if (!(t->held & bit)) return;
    t->held &= ~bit;
    const bool freed = FreeIfUnheld(index);
    if (!t->active && t->held == 0) RemoveTarget(w);  // removed, and now it has handed everything back
    if (!freed) return;
    ++delivered;
    if (waiting) {
        waiting = false;
        auto onSlotFreed = owner->onSlotFreed;  // it may replace this very stream
        if (onSlotFreed) onSlotFreed();
    }
}

// Out of the stream for good: what its page held is dropped.
void Stream::Impl::RemoveTarget(Window::Impl* w) {
    auto it = std::find_if(targets.begin(), targets.end(), [&](const Target& t) { return t.window == w; });
    if (it == targets.end()) return;
    const uint64_t held = it->held;
    targets.erase(it);
    for (int i = 0; i < int(slots.size()); ++i) {
        if (!(held & (uint64_t(1) << i))) continue;
        slots[size_t(i)].renew = true;
        FreeIfUnheld(i);
    }
    if (w != window) {
        auto& list = w->received;
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
    }
}

Stream::Stream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    impl_->owner = this;
}

Stream::~Stream() = default;

const std::string& Stream::name() const { return impl_->name; }
size_t Stream::slotSize() const { return impl_->slotSize; }
int Stream::slotCount() const { return impl_->slotCount; }
bool Stream::shared() const {
    const Impl& s = *impl_;
    if (s.window) UiOnly(*s.window, "Stream::shared");
    if (!s.allocated || !s.shared) return false;
    for (const Impl::Target& t : s.targets) {
        if (t.active && t.window->pageState != Window::Impl::PageState::None && t.pageFetches) return false;
    }
    return true;
}
uint64_t Stream::delivered() const {
    if (impl_->window) UiOnly(*impl_->window, "Stream::delivered");
    return impl_->delivered;
}

// Throws (std::bad_alloc) if the memory can't be had, leaving the stream unallocated: it then
// refuses acquire() and isn't shared with the page.
void Stream::Impl::Allocate(detail::Backend& backend, bool useShared) {
    allocated = false;
    shared = false;
    slots.clear();
    try {
        slots.resize(slotCount);
        if (useShared) {
            bool all = true;
            for (SlotData& slot : slots) {
                slot.region = backend.CreateSharedRegion(slotSize);
                if (!slot.region) {
                    all = false;  // fall back for the whole stream
                    break;
                }
                slot.data = slot.region->data;
            }
            shared = all;
        }
        if (!shared) {
            for (SlotData& slot : slots) {
                slot.region.reset();
                slot.heap = std::make_shared<std::vector<uint8_t>>(slotSize);
                slot.data = slot.heap->data();
            }
        }
    } catch (...) {
        shared = false;
        slots.clear();
        throw;
    }
    allocated = true;
}

namespace {
bool AnyPageConnected(const Stream::Impl& s) {
    for (const Stream::Impl::Target& t : s.targets) {
        if (t.active && t.window->pageState == Window::Impl::PageState::Connected) return true;
    }
    return false;
}
}  // namespace

bool Stream::acquire(Slot& slot) {
    Impl& s = *impl_;
    if (s.window) UiOnly(*s.window, "Stream::acquire");
    if (!s.allocated || !AnyPageConnected(s)) return false;
    for (int i = 0; i < s.slotCount; ++i) {
        if (s.slots[i].state == Impl::State::Free) {
            s.slots[i].state = Impl::State::Writing;
            slot = {i, s.slots[i].data, s.slotSize};
            return true;
        }
    }
    s.waiting = true;
    return false;
}

bool Stream::publish(const Slot& slot, size_t size, const json& meta) {
    Impl& s = *impl_;
    if (s.window) UiOnly(*s.window, "Stream::publish");
    // slots stays empty until the web view exists; a slot that wasn't acquired can't publish.
    if (slot.index < 0 || size_t(slot.index) >= s.slots.size()) return false;
    Impl::SlotData& data = s.slots[slot.index];
    if (data.state != Impl::State::Writing) return false;
    if (size > s.slotSize || !AnyPageConnected(s)) {
        data.state = Impl::State::Free;
        return false;
    }
    data.state = Impl::State::WithPage;
    data.size = size;
    const json message = {{"t", "frame"}, {"stream", s.name}, {"id", s.id}, {"slot", slot.index},
                          {"size", size}, {"meta", meta}};
    const uint64_t bit = uint64_t(1) << slot.index;
    for (Impl::Target& t : s.targets) {
        if (!t.active || t.window->pageState != Window::Impl::PageState::Connected) continue;
        t.held |= bit;
        t.window->Send(message);
    }
    return true;
}

void Stream::addWindow(Window& other) {
    Impl& s = *impl_;
    if (s.window) UiOnly(*s.window, "Stream::addWindow");
    Window::Impl* w = other.internal();
    if (w->app != s.window->app) throw std::invalid_argument("that window belongs to another App");
    if (Impl::Target* t = s.TargetOf(w)) {
        if (t->active) return;
        t->active = true;  // removed and added again before its page handed everything back
        return;
    }
    for (Impl* stream : w->PageStreams()) {
        if (stream->name == s.name) {
            throw std::invalid_argument("that window has a stream named \"" + s.name + "\" already");
        }
    }
    s.targets.push_back({w});
    s.targets.back().pageFetches = !w->useShared;
    w->received.push_back(&s);
    if (w->pageState != Window::Impl::PageState::None) w->Announce(s);
}

void Stream::removeWindow(Window& other) {
    Impl& s = *impl_;
    if (s.window && s.window->app && !OnUiThread(*s.window->app)) {
        // Posted: this stream by its id (it may have been replaced by then), that window weakly.
        App::Impl* program = s.window->app.get();
        std::weak_ptr<App::Impl> weakProgram = s.window->app;
        std::weak_ptr<Window::Impl> weakOther = other.internal()->shared_from_this();
        PostToUi(*program, [weakProgram, weakOther, id = s.id] {
            const auto p = weakProgram.lock();
            const auto w = weakOther.lock();
            Stream::Impl* stream = p ? p->StreamById(id) : nullptr;
            if (stream && stream->owner && w && w->owner) stream->owner->removeWindow(*w->owner);
        });
        return;
    }
    Impl::Target* t = s.TargetOf(other.internal());
    if (!t || !t->active) return;
    if (t->held == 0) {
        if (t->window == s.window) {
            t->active = false;  // it stays in its own window's list; added back with addWindow
        } else {
            s.RemoveTarget(t->window);
        }
        return;
    }
    t->active = false;  // out once its page has handed its slots back (or went away)
}

std::vector<Window*> Stream::windows() const {
    if (impl_->window) UiOnly(*impl_->window, "Stream::windows");
    std::vector<Window*> result;
    for (const Impl::Target& t : impl_->targets) {
        if (t.active && t.window->owner) result.push_back(t.window->owner);
    }
    return result;
}

// ----------------------------------------------------------------------------------------
// Sink

Sink::Sink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    impl_->owner = this;
}

Sink::~Sink() = default;

const std::string& Sink::name() const { return impl_->name; }
size_t Sink::slotSize() const { return impl_->slotSize; }
int Sink::slotCount() const { return impl_->slotCount; }
bool Sink::shared() const {
    if (impl_->window) UiOnly(*impl_->window, "Sink::shared");
    return impl_->SharedWithPage() && impl_->window->pageState != Window::Impl::PageState::None;
}
uint64_t Sink::received() const {
    if (impl_->window) UiOnly(*impl_->window, "Sink::received");
    return impl_->received;
}

// Throws (std::bad_alloc) if the memory can't be had, leaving the sink unallocated: its page then
// can't fill it (every put is refused). Without shared memory nothing is allocated: each slot's
// data is the page's upload.
void Sink::Impl::Allocate(detail::Backend& backend, bool useShared) {
    allocated = false;
    shared = false;
    slots.clear();
    try {
        slots.resize(slotCount);
        if (useShared) {
            bool all = true;
            for (SlotData& slot : slots) {
                slot.region = backend.CreateSharedRegion(slotSize);
                if (!slot.region) {
                    all = false;  // uploads for the whole sink
                    break;
                }
            }
            shared = all;
            if (!shared) {
                for (SlotData& slot : slots) slot.region.reset();
            }
        }
    } catch (...) {
        shared = false;
        slots.clear();
        throw;
    }
    allocated = true;
}

// The page filled a slot (put): checked, then onData (or straight back without one). A put that
// doesn't fit (a slot it doesn't have, too large, its data missing) is refused: the slot goes
// back to the page.
void Sink::Impl::Put(const json& message) {
    Window::Impl& w = *window;
    const int index = message.value("slot", -1);
    if (index < 0 || index >= int(slots.size())) return;
    SlotData& slot = slots[size_t(index)];
    if (slot.state != State::Page) return;  // C++ has it already: not the page's to put
    const uint64_t size = message.value("size", uint64_t(0));
    const uint8_t* data = nullptr;  // (null for an empty upload: fine with size 0)
    bool accepted = false;
    if (message.value("uploads", false)) {
        // No shared memory: its bytes came as an upload, {"$uniapi.upload": n} in data.
        const json placeholder = message.value("data", json());
        const uint64_t number = placeholder.is_object() ? placeholder.value(kUploadKey, uint64_t(0)) : 0;
        auto upload = w.uploads.find(number);
        if (upload != w.uploads.end()) {
            slot.upload = std::move(upload->second);
            w.uploads.erase(upload);
            accepted = slot.upload->size() == size && size <= slotSize;
            data = slot.upload->data();
        }
    } else if (slot.region && SharedWithPage()) {
        accepted = size <= slotSize;
        data = slot.region->data;
    }
    if (!accepted) {
        slot.upload.reset();
        w.Send({{"t", "free"}, {"sink", name}, {"id", id}, {"slot", index}, {"refused", true}});
        return;
    }
    slot.state = State::App;
    ++received;
    auto onData = owner ? owner->onData : nullptr;  // a copy: it may replace this very sink
    if (!onData) {
        ReleaseSlot(index);
        return;
    }
    const Sink::Slot given{index, data, size_t(size), id};
    const json meta = message.value("meta", json());
    RunGuarded("Sink::onData", [&] { onData(given, meta); });
}

// Back to the page: told at once if it is connected; a page that connects later learns it from
// the sink's free list.
void Sink::Impl::ReleaseSlot(int index) {
    if (index < 0 || index >= int(slots.size())) return;
    SlotData& slot = slots[size_t(index)];
    if (slot.state != State::App) return;
    slot.state = State::Page;
    slot.upload.reset();
    if (window->pageState != Window::Impl::PageState::None) {
        window->Send({{"t", "free"}, {"sink", name}, {"id", id}, {"slot", index}});
    }
}

void Sink::release(const Slot& slot) {
    Impl& s = *impl_;
    if (slot.sink != s.id) return;  // another sink's (or one this has replaced)
    if (s.window && s.window->app && !OnUiThread(*s.window->app)) {
        // Posted: the sink by its id (it may be replaced or gone by then), its window weakly.
        std::weak_ptr<Window::Impl> weakWindow = s.window->shared_from_this();
        PostToUi(*s.window->app, [weakWindow, id = s.id, index = slot.index] {
            const auto w = weakWindow.lock();
            if (!w) return;
            for (auto& sink : w->sinks) {
                if (sink->internal()->id == id) sink->internal()->ReleaseSlot(index);
            }
        });
        return;
    }
    s.ReleaseSlot(slot.index);
}

// ----------------------------------------------------------------------------------------
// Window

Window::Window(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Window::~Window() {
    // Its streams first: their shared regions are web view objects, released here on the UI
    // thread while the backend is up; then a further window's own window (closed without events).
    Impl& impl = *impl_;
    impl.LeaveStreams();
    impl.dom = nullptr;
    impl.streams.clear();
    impl.sinks.clear();
    impl.own.reset();
    impl.window = nullptr;
    impl.pageState = Impl::PageState::None;
    impl.owner = nullptr;
}

void Window::navigate(std::string_view path) {
    if (Elsewhere(impl_, &Window::navigate, path)) return;
    if (impl_->ready && impl_->window) {
        impl_->window->Navigate(std::string(path));
    } else {
        impl_->pendingNavigation = std::string(path);
    }
}

void Window::exposeHandler(std::string name, Handler handler) {
    if (Elsewhere(impl_, &Window::exposeHandler, name, handler)) return;
    impl_->functions[std::move(name)] = std::move(handler);
}

void Window::unexpose(std::string_view name) {
    if (Elsewhere(impl_, &Window::unexpose, name)) return;
    const auto it = impl_->functions.find(name);
    if (it != impl_->functions.end()) impl_->functions.erase(it);
}

void Window::off(std::string_view event) {
    if (Elsewhere(impl_, &Window::off, event)) return;
    const auto it = impl_->events.find(event);
    if (it != impl_->events.end()) impl_->events.erase(it);
}

void Window::on(std::string event, std::function<void(const json& data)> handler) {
    if (Elsewhere(impl_, &Window::on, event, handler)) return;
    impl_->events[std::move(event)] = std::move(handler);
}

void Window::call(std::string_view function, json args, std::function<void(const Result&)> done) {
    // From another thread: done runs on the UI thread, as always.
    if (Elsewhere(impl_, &Window::call, function, args, done)) return;
    Impl& impl = *impl_;
    if (impl.pageState != Impl::PageState::Connected) {
        if (done) done(Result{false, nullptr, "no page is connected"});
        return;
    }
    if (!args.is_array()) args = json::array({std::move(args)});
    uint64_t id = impl.nextCallId++;
    if (done) impl.pendingCalls[id] = std::move(done);
    impl.Send({{"t", "call"}, {"id", id}, {"fn", function}, {"args", std::move(args)}});
}

void Window::emit(std::string_view event, json data) {
    if (Elsewhere(impl_, &Window::emit, event, data)) return;
    if (impl_->pageState != Impl::PageState::Connected) return;
    impl_->Send({{"t", "event"}, {"name", event}, {"data", std::move(data)}});
}

Stream& Window::stream(const std::string& name, size_t slotSize, int slotCount) {
    UiOnly(*impl_, "Window::stream");
    if (name.empty() || name.find_first_of("/?#\"\\") != std::string::npos || name.find('\0') != std::string::npos) {
        throw std::invalid_argument("stream names may not be empty or contain / ? # \" \\ or a NUL character");
    }
    if (slotSize == 0 || slotCount < 1 || slotCount > kMaxSlots) {
        throw std::invalid_argument("streams need a slot size above 0 and 1 to 64 slots");
    }
    Impl& impl = *impl_;
    if (impl.dom && name == kDomStream) {
        throw std::invalid_argument("the stream name \"uniapi.dom\" is used by the library");
    }
    for (const Stream::Impl* stream : impl.received) {
        if (stream->name == name) {
            throw std::invalid_argument("another window's stream named \"" + name + "\" reaches this window already");
        }
    }
    auto data = std::make_unique<Stream::Impl>();
    data->window = &impl;
    data->targets.push_back({&impl});
    data->name = name;
    data->id = impl.app->nextStreamId++;
    data->slotSize = slotSize;
    data->slotCount = slotCount;
    if (impl.ready) data->Allocate(*impl.app->backend, impl.useShared);
    auto created = std::make_unique<Stream>(std::move(data));
    Stream* result = created.get();

    Stream::Impl& fresh = *result->internal();
    fresh.targets.front().pageFetches = !impl.useShared;

    auto existing = std::find_if(impl.streams.begin(), impl.streams.end(),
                                 [&](const auto& s) { return s->name() == name; });
    std::vector<std::pair<Impl*, bool>> reached;  // the windows the replaced one reached
    if (existing != impl.streams.end()) {
        for (const Stream::Impl::Target& t : (*existing)->internal()->targets) {
            if (t.window == &impl) {
                fresh.targets.front().active = t.active;
            } else {
                reached.emplace_back(t.window, t.active);
            }
        }
        *existing = std::move(created);  // leaves those windows
    } else {
        impl.streams.push_back(std::move(created));
    }
    if (impl.pageState != Impl::PageState::None) impl.Announce(fresh);
    for (const auto& [w, active] : reached) {
        if (!active) continue;
        fresh.targets.push_back({w});
        fresh.targets.back().pageFetches = !w->useShared;
        w->received.push_back(&fresh);
        if (w->pageState != Impl::PageState::None) w->Announce(fresh);
    }
    return *result;
}

Sink& Window::sink(const std::string& name, size_t slotSize, int slotCount) {
    UiOnly(*impl_, "Window::sink");
    if (name.empty() || name.find_first_of("/?#\"\\") != std::string::npos || name.find('\0') != std::string::npos) {
        throw std::invalid_argument("sink names may not be empty or contain / ? # \" \\ or a NUL character");
    }
    if (slotSize == 0 || slotCount < 1 || slotCount > kMaxSlots) {
        throw std::invalid_argument("sinks need a slot size above 0 and 1 to 64 slots");
    }
    Impl& impl = *impl_;
    auto data = std::make_unique<Sink::Impl>();
    data->window = &impl;
    data->name = name;
    data->id = impl.app->nextStreamId++;
    data->slotSize = slotSize;
    data->slotCount = slotCount;
    data->pageUploads = !impl.pageShared;
    if (impl.ready) data->Allocate(*impl.app->backend, impl.useShared);
    auto created = std::make_unique<Sink>(std::move(data));
    Sink* result = created.get();
    auto existing = std::find_if(impl.sinks.begin(), impl.sinks.end(),
                                 [&](const auto& s) { return s->name() == name; });
    if (existing != impl.sinks.end()) {
        *existing = std::move(created);
    } else {
        impl.sinks.push_back(std::move(created));
    }
    if (impl.pageState != Impl::PageState::None) impl.AnnounceSink(*result->internal());
    return *result;
}

bool Window::sendDom(const std::function<void(DomCommandWriter&)>& write) {
    UiOnly(*impl_, "Window::sendDom");
    if (!impl_->dom) return false;
    Stream::Slot slot;
    if (!impl_->dom->acquire(slot)) return false;
    DomCommandWriter writer(slot.data, slot.capacity);
    try {
        write(writer);
    } catch (...) {
        // Give the slot back; otherwise it stays "being written" for good, and with two slots
        // a couple of failing writers would stop DOM lists for the rest of the run.
        impl_->dom->internal()->slots[size_t(slot.index)].state = Stream::Impl::State::Free;
        throw;
    }
    return impl_->dom->publish(slot, writer.size());
}

bool Window::connected() const {
    UiOnly(*impl_, "Window::connected");
    return impl_->pageState == Impl::PageState::Connected;
}
const std::string& Window::page() const {
    UiOnly(*impl_, "Window::page");
    return impl_->page;
}
bool Window::sharedMemory() const {
    UiOnly(*impl_, "Window::sharedMemory");
    return impl_->useShared && impl_->pageShared;
}

void Window::setTitle(std::string_view title) {
    if (Elsewhere(impl_, &Window::setTitle, title)) return;
    if (impl_->window) impl_->window->SetTitle(std::string(title));
}
void Window::setFullscreen(bool on) {
    if (Elsewhere(impl_, &Window::setFullscreen, on)) return;
    if (impl_->window) impl_->window->SetFullscreen(on);
}
void Window::setSize(int width, int height) {
    if (Elsewhere(impl_, &Window::setSize, width, height)) return;
    if (impl_->window && width > 0 && height > 0) impl_->window->SetSize(width, height);
}
bool Window::fullscreen() const {
    UiOnly(*impl_, "Window::fullscreen");
    return impl_->window && impl_->window->IsFullscreen();
}
void Window::openDevTools() {
    if (Elsewhere(impl_, &Window::openDevTools)) return;
    if (impl_->window) impl_->window->OpenDevTools();
}
void Window::setTitleBar(TitleBar mode) {
    if (Elsewhere(impl_, &Window::setTitleBar, mode)) return;
    if (impl_->window) impl_->window->SetTitleBar(mode);
}
TitleBar Window::titleBar() const {
    UiOnly(*impl_, "Window::titleBar");
    return impl_->window ? impl_->window->GetTitleBar() : TitleBar::System;
}
void Window::show() {
    if (Elsewhere(impl_, &Window::show)) return;
    if (!impl_->window || impl_->closed) return;
    impl_->window->Show();
    impl_->window->SetVisible(true);
}
void Window::hide() {
    if (Elsewhere(impl_, &Window::hide)) return;
    if (impl_->window && !impl_->closed) impl_->window->SetVisible(false);
}
bool Window::visible() const {
    UiOnly(*impl_, "Window::visible");
    return impl_->window && !impl_->closed && impl_->window->IsVisible();
}
bool Window::setIcon(const unsigned char* png, size_t size) {
    UiOnly(*impl_, "Window::setIcon");
    return impl_->window && png && size > 0 && impl_->window->SetIcon(png, size);
}
void Window::close() {
    if (Elsewhere(impl_, &Window::close)) return;
    if (impl_->window && !impl_->closed) impl_->window->Close();
}
App& Window::app() const { return *impl_->app->owner; }

// --- Placement ---

Size Window::size() const {
    UiOnly(*impl_, "Window::size");
    return impl_->window ? impl_->window->CurrentSize() : Size{};
}
std::vector<Screen> Window::screens() const {
    UiOnly(*impl_, "Window::screens");
    return impl_->window ? impl_->window->Screens() : std::vector<Screen>{};
}
std::optional<Point> Window::position() const {
    UiOnly(*impl_, "Window::position");
    return impl_->window && !impl_->closed ? impl_->window->Position() : std::nullopt;
}
void Window::setPosition(int x, int y) {
    if (Elsewhere(impl_, &Window::setPosition, x, y)) return;
    if (impl_->window && !impl_->closed) impl_->window->SetPosition(x, y);
}
void Window::center() {
    if (Elsewhere(impl_, &Window::center)) return;
    if (impl_->window && !impl_->closed) impl_->window->Center();
}
void Window::minimize() {
    if (Elsewhere(impl_, &Window::minimize)) return;
    if (impl_->window && !impl_->closed) impl_->window->SetState(detail::WindowState::Minimized);
}
void Window::maximize() {
    if (Elsewhere(impl_, &Window::maximize)) return;
    if (impl_->window && !impl_->closed) impl_->window->SetState(detail::WindowState::Maximized);
}
void Window::restore() {
    if (Elsewhere(impl_, &Window::restore)) return;
    if (impl_->window && !impl_->closed) impl_->window->SetState(detail::WindowState::Normal);
}
bool Window::minimized() const {
    UiOnly(*impl_, "Window::minimized");
    return impl_->window && !impl_->closed && impl_->window->State() == detail::WindowState::Minimized;
}
bool Window::maximized() const {
    UiOnly(*impl_, "Window::maximized");
    return impl_->window && !impl_->closed && impl_->window->State() == detail::WindowState::Maximized;
}
bool Window::setAlwaysOnTop(bool on) {
    // From another thread: asked (true); whether the platform can is known on the UI thread.
    if (Elsewhere(impl_, &Window::setAlwaysOnTop, on)) return true;
    return impl_->window && !impl_->closed && impl_->window->SetAlwaysOnTop(on);
}
bool Window::alwaysOnTop() const {
    UiOnly(*impl_, "Window::alwaysOnTop");
    return impl_->window && !impl_->closed && impl_->window->IsAlwaysOnTop();
}

std::string Window::placement() const {
    UiOnly(*impl_, "Window::placement");
    const std::optional<detail::SavedPlacement> p = impl_->CurrentPlacement();
    return p ? detail::PlacementText(*p) : std::string();
}

bool Window::setPlacement(std::string_view text) {
    const std::optional<detail::SavedPlacement> p = detail::ParsePlacement(text);
    if (!p) return false;
    if (Elsewhere(impl_, &Window::setPlacement, text)) return true;
    impl_->ApplyPlacement(*p);
    return true;
}

std::optional<detail::SavedPlacement> Window::Impl::CurrentPlacement() const {
    if (!window) return std::nullopt;
    const detail::Bounds b = window->NormalBounds();
    if (b.width <= 0 || b.height <= 0) return std::nullopt;
    detail::SavedPlacement p;
    p.x = b.x;
    p.y = b.y;
    p.width = b.width;
    p.height = b.height;
    p.hasPosition = b.hasPosition;
    p.maximized = window->State() == detail::WindowState::Maximized;
    return p;
}

// Before run() the screens may not be known yet (Linux: no display): the position is kept, and
// the backend checks it against the screens when it makes the window.
void Window::Impl::ApplyPlacement(detail::SavedPlacement p) {
    if (!window || closed) return;
    const std::vector<detail::Rect> screens = app->backend ? app->backend->ScreenAreas() : std::vector<detail::Rect>{};
    p = detail::FitPlacement(p, screens, !app->running);
    window->SetSize(p.width, p.height);
    if (p.hasPosition) {
        window->SetPosition(p.x, p.y);
    } else if (!screens.empty()) {
        window->Center();
    }
    window->SetState(p.maximized ? detail::WindowState::Maximized : detail::WindowState::Normal);
}

void Window::Impl::RestorePlacement(const std::string& name) {
    placementName = name;
    if (const std::optional<detail::SavedPlacement> p = detail::RememberedPlacement(name)) ApplyPlacement(*p);
}

// The latest placement kept (to save when the window goes), then onPlacementChanged.
void Window::Impl::OnPlacementChanged() {
    if (!placementName.empty()) {
        if (std::optional<detail::SavedPlacement> p = CurrentPlacement()) lastPlacement = p;
    }
    if (owner && owner->onPlacementChanged) {
        auto handler = owner->onPlacementChanged;  // a copy: it may replace itself
        RunGuarded("onPlacementChanged", handler);
    }
}

// Saves the window's placement under its name, once, as it goes.
void Window::Impl::KeepPlacement() {
    if (placementName.empty() || placementKept) return;
    placementKept = true;
    if (std::optional<detail::SavedPlacement> p = CurrentPlacement()) lastPlacement = p;
    if (lastPlacement) detail::RememberPlacement(placementName, *lastPlacement);
}

// ----------------------------------------------------------------------------------------
// App

App::App(AppOptions options) : Window(std::make_shared<Window::Impl>()), app_(std::make_shared<Impl>()) {
    Impl& program = *app_;
    program.owner = this;
    program.options = std::move(options);
    Window::Impl& main = *impl_;
    main.app = app_;
    main.owner = this;
    main.main = true;
    program.main = &main;
    program.backend = detail::CreateBackend(program.options, main.Events());
    program.mainWindow = std::make_unique<MainWindow>(*program.backend);
    main.window = program.mainWindow.get();
    main.dom = &stream(std::string(kDomStream), kDomSlotBytes, kDomSlots);
    if (!program.options.rememberPlacement.empty()) main.RestorePlacement(program.options.rememberPlacement);  // it opens there
    if (!program.options.singleInstance.empty()) {
        // Later launches arrive on the listener's thread; onSecondInstance runs on the UI thread
        // (posted: the listener stops before the backend goes, in ~App).
        App* self = this;
        bool handedOver = false;
        program.instance = detail::StartSingleInstance(
            program.options.singleInstance,
            [self](std::vector<std::string> args, std::string dir) {
                self->post([self, args = std::move(args), dir = std::move(dir)] {
                    if (auto handler = self->onSecondInstance) {
                        RunGuarded("onSecondInstance", [&] { handler(args, dir); });
                    } else {
                        self->show();
                    }
                });
            },
            handedOver);
        if (handedOver) std::exit(0);  // the running instance has this launch
    }
}

App::~App() {
    // Further windows first (their streams, then their windows), then the main window's streams:
    // shared regions are web view objects, released here on the UI thread while the backend (and
    // on Windows, COM) is still up. The Impls may outlive the App (a worker thread answering a
    // Reply holds one) and then end on that thread.
    Impl& program = *app_;
    // Optional parts' own steps (the tray's icon), newest first, while the platform is up.
    while (!program.ending.empty()) {
        auto last = std::prev(program.ending.end());
        std::function<void()> step = std::move(last->second);
        program.ending.erase(last);
        RunGuarded("ending the app", step);
    }
    program.instance.reset();  // stopped: no more posts from its thread
    // Windows ending with the App have no close event: their placements are kept now.
    for (const auto& window : program.windows) window->internal()->KeepPlacement();
    impl_->KeepPlacement();
    program.windows.clear();
    Window::Impl& main = *impl_;
    main.LeaveStreams();
    main.dom = nullptr;
    main.streams.clear();
    main.window = nullptr;
    main.pageState = Window::Impl::PageState::None;
    std::lock_guard<std::mutex> lock(program.backendMutex);
    program.mainWindow.reset();
    program.backend.reset();  // no more events into a half-destroyed App
}

void App::serve(const AssetTable& assets) {
    if (Elsewhere(app_, [table = &assets](App& a) { a.serve(*table); })) return;
    app_->assets = &assets;
}
void App::serveDirectory(std::string path) {
    if (Elsewhere(app_, &App::serveDirectory, path)) return;
    app_->assetDirectory = std::move(path);
}

Window& App::openWindow(WindowOptions options) {
    Impl& program = *app_;
    UiOnly(&program, "App::openWindow");
    if (options.modal) options.owned = true;
    auto link = std::make_shared<Window::Impl>();
    link->app = app_;
    std::unique_ptr<Window> window(new Window(link));
    link->owner = window.get();
    link->own = program.backend->OpenWindow(options, link->Events());
    if (!link->own) throw std::runtime_error("this platform can't open more windows yet");
    link->window = link->own.get();
    link->dom = &window->stream(std::string(kDomStream), kDomSlotBytes, kDomSlots);
    if (!options.rememberPlacement.empty()) link->RestorePlacement(options.rememberPlacement);
    Window& result = *window;
    program.windows.push_back(std::move(window));
    return result;
}

Window* App::sender() const {
    UiOnly(app_.get(), "App::sender");
    return app_->sender;
}

bool App::onUiThread() const { return OnUiThread(*app_); }

WebEngine App::webEngine() const {
    UiOnly(app_.get(), "App::webEngine");
    return app_->backend ? app_->backend->Engine() : WebEngine{};
}

std::vector<Window*> App::windows() const {
    UiOnly(app_.get(), "App::windows");
    std::vector<Window*> open;
    for (const auto& window : app_->windows) {
        if (!window->internal()->closed) open.push_back(window.get());
    }
    return open;
}

// Timer and post() callbacks run from the platform's message loop: nothing may escape them.
int App::every(std::chrono::milliseconds interval, std::function<void()> fn) {
    UiOnly(app_.get(), "App::every");
    if (!fn) return 0;
    return app_->backend->AddTimer(interval, [fn = std::move(fn)] { RunGuarded("a timer callback", fn); });
}
void App::cancel(int timer) {
    if (Elsewhere(app_, &App::cancel, timer)) return;
    app_->backend->RemoveTimer(timer);
}
void App::post(std::function<void()> fn) {
    if (!fn) return;
    // From another thread: the App may be shutting down on the UI thread right now.
    std::lock_guard<std::mutex> lock(app_->backendMutex);
    if (app_->backend) app_->backend->Post([fn = std::move(fn)] { RunGuarded("a posted function", fn); });
}
std::vector<std::string> App::openFileDialog(const OpenDialogOptions& options) {
    UiOnly(app_.get(), "App::openFileDialog");
    return app_->backend->OpenFileDialog(options);
}
std::optional<std::string> App::saveFileDialog(const SaveDialogOptions& options) {
    UiOnly(app_.get(), "App::saveFileDialog");
    return app_->backend->SaveFileDialog(options);
}
int App::run() {
    UiOnly(app_.get(), "App::run");
    app_->running = true;
    return app_->backend->Run();
}
void App::quit() {
    if (Elsewhere(app_, &App::quit)) return;
    app_->backend->Quit();
}

void App::clipboardText(std::function<void(std::optional<std::string>)> done) {
    if (!done) return;
    if (Elsewhere(app_, &App::clipboardText, done)) return;
    detail::Backend* backend = app_->backend.get();
    // Never inside this call, on any platform: posted, then the platform's answer (GTK 4's later).
    backend->Post([backend, done = std::move(done)]() mutable {
        auto answered = std::make_shared<bool>(false);
        backend->ClipboardText([answered, done = std::move(done)](std::optional<std::string> text) {
            if (*answered) return;
            *answered = true;
            RunGuarded("a clipboardText callback", [&] { done(std::move(text)); });
        });
    });
}

bool App::setClipboardText(std::string_view text) {
    // From another thread: asked (true); whether the system took it is known on the UI thread.
    if (Elsewhere(app_, &App::setClipboardText, text)) return true;
    return app_->backend->SetClipboardText(std::string(text));
}

std::string App::notify(Notification n, std::function<void(const Result&)> done) {
    Impl& program = *app_;
    if (n.id.empty()) n.id = program.notificationRun + "-" + std::to_string(program.nextNotification++);
    const std::string id = n.id;
    // done, guarded; without it a failure is printed.
    auto finish = [title = n.title, id, done = std::move(done)](const std::string& error) {
        Result result;
        result.ok = error.empty();
        result.value = id;  // its id, shown or not
        result.error = error;
        if (done) {
            RunGuarded("a notify callback", [&] { done(result); });
        } else if (!result.ok) {
            std::fprintf(stderr, "uniapi: notification \"%s\" not shown: %s\n", title.c_str(), error.c_str());
        }
    };
    // From any thread: the App may be shutting down on the UI thread right now.
    std::lock_guard<std::mutex> lock(program.backendMutex);
    detail::Backend* backend = program.backend.get();
    if (!backend) return id;  // the app has ended: nobody to tell
    // On the UI thread once the event loop runs (posted functions wait for run()), and never
    // inside the caller: done always comes later.
    backend->Post([backend, n = std::move(n), finish = std::move(finish)]() mutable {
        if (n.title.empty() && n.body.empty()) {
            finish("a notification needs a title or a body");
            return;
        }
        auto answered = std::make_shared<bool>(false);
        backend->Notify(n, [answered, finish](const std::string& error) {
            if (*answered) return;
            *answered = true;
            finish(error);
        });
    });
    return id;
}

int detail::AddEndingStep(App& app, std::function<void()> step) {
    App::Impl& program = *app.app_;
    const int id = program.nextEnding++;
    program.ending[id] = std::move(step);
    return id;
}

void detail::RemoveEndingStep(App& app, int id) { app.app_->ending.erase(id); }

// ----------------------------------------------------------------------------------------
// Window::Impl

detail::BackendEvents Window::Impl::Events() {
    // The Impl outlives its window: a further window's owns it, the main window's backend goes first.
    Window::Impl* self = this;
    detail::BackendEvents backendEvents;
    backendEvents.onReady = [self](bool supported) {
        RunGuarded("starting the web view", [&] { self->OnReady(supported); });
    };
    backendEvents.onMessage = [self](const std::string& text) { self->OnMessage(text); };
    backendEvents.onRequest = [self](detail::Request& request, detail::Response& response) {
        // If it throws (std::filesystem on a path that isn't valid UTF-8, say) the answer is a 404.
        RunGuarded("serving a request", [&] { self->OnRequest(request, response); });
    };
    backendEvents.onNavigationStarting = [self] { self->EndPage(); };
    backendEvents.onCloseRequested = [self](CloseReason reason) { return self->OnCloseRequested(reason); };
    backendEvents.onExternalNavigation = [self](const std::string& url) { self->OnExternalNavigation(url); };
    backendEvents.onFileDrop = [self](const std::vector<std::string>& paths, int x, int y) {
        self->OnFileDrop(paths, x, y);
    };
    backendEvents.onPlacementChanged = [self] { self->OnPlacementChanged(); };
    backendEvents.onNotificationClicked = [self](const std::string& id) { self->OnNotificationClicked(id); };
    backendEvents.onClosed = [self] { self->OnClosed(); };
    return backendEvents;
}

void Window::Impl::OnReady(bool sharedMemorySupported) {
    ready = true;
    useShared = app->options.sharedMemory && sharedMemorySupported;
    pageShared = useShared;
    // One stream that can't get its memory must not keep the others, or the page, from starting.
    for (auto& stream : streams) {
        Stream::Impl& s = *stream->internal();
        RunGuarded("allocating a stream's memory", [&] { s.Allocate(*app->backend, useShared); });
        // Other windows' pages that connected before this web view existed: the stream again,
        // now with its slots.
        for (Stream::Impl::Target& t : s.targets) {
            if (t.window != this && t.active && t.window->pageState != PageState::None) t.window->Announce(s);
        }
    }
    for (auto& sink : sinks) {
        Sink::Impl& s = *sink->internal();
        RunGuarded("allocating a sink's memory", [&] { s.Allocate(*app->backend, useShared); });
    }
    if (pendingNavigation && window) {
        window->Navigate(*pendingNavigation);
        pendingNavigation.reset();
    }
}

// The user asks the window to close; onCloseRequested decides. A handler that throws lets it
// close: a broken handler mustn't leave a window that can't be closed.
bool Window::Impl::OnCloseRequested(CloseReason reason) {
    if (closed || !owner || !owner->onCloseRequested) return true;
    auto handler = owner->onCloseRequested;  // a copy: it may replace itself
    bool allow = true;
    RunGuarded("onCloseRequested", [&] { allow = handler(reason); });
    return allow;
}

// The page tried to leave the app's pages (the platform cancelled it): onExternalURL decides;
// without it, the URL opens with the system, on a thread of its own (the system may take a
// moment).
void Window::Impl::OnExternalNavigation(const std::string& url) {
    App* program = app->owner;
    if (!program) return;
    if (auto handler = program->onExternalURL) {
        bool open = false;
        RunGuarded("onExternalURL", [&] { open = handler(url); });
        if (!open) return;
    }
    std::thread([url] {
        try {
            openURL(url);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "uniapi: %s\n", e.what());
        }
    }).detach();
}

// onNotificationClicked, or without one, the main window shown.
void Window::Impl::OnNotificationClicked(const std::string& id) {
    App* program = app->owner;
    if (!program) return;
    if (auto handler = program->onNotificationClicked) {
        RunGuarded("onNotificationClicked", [&] { handler(id); });
    } else {
        program->show();
    }
}

void Window::Impl::OnFileDrop(const std::vector<std::string>& paths, int x, int y) {
    if (paths.empty() || !owner || !owner->onFileDrop) return;
    auto handler = owner->onFileDrop;  // a copy: it may replace itself
    RunGuarded("onFileDrop", [&] { handler(paths, x, y); });
}

// The window closed: its page has gone; onClosed; a further window is then destroyed, outside
// the platform's close event (posted).
void Window::Impl::OnClosed() {
    KeepPlacement();  // while the platform's window is still there
    EndPage();
    if (closed) return;
    closed = true;
    if (owner && owner->onClosed) {
        auto handler = owner->onClosed;  // a copy: it may replace itself
        RunGuarded("onClosed", handler);
    }
    if (main) return;
    App::Impl* program = app.get();
    Window::Impl* self = this;
    if (program->backend) program->backend->Post([program, self] { program->Remove(self); });
}

// A stream its page receives, its own or another window's.
Stream::Impl* Window::Impl::FindStream(std::string_view name) {
    for (auto& stream : streams) {
        if (stream->name() == name) return stream->internal();
    }
    for (Stream::Impl* stream : received) {
        if (stream->name == name) return stream;
    }
    return nullptr;
}

const Window::Handler* Window::Impl::FindFunction(std::string_view name) const {
    auto it = functions.find(name);
    if (it != functions.end()) return &it->second;
    if (!main && app->main) {
        auto fallback = app->main->functions.find(name);
        if (fallback != app->main->functions.end()) return &fallback->second;
    }
    return nullptr;
}

const std::function<void(const json&)>* Window::Impl::FindEvent(std::string_view name) const {
    auto it = events.find(name);
    if (it != events.end()) return &it->second;
    if (!main && app->main) {
        auto fallback = app->main->events.find(name);
        if (fallback != app->main->events.end()) return &fallback->second;
    }
    return nullptr;
}

void Window::Impl::ShareStream(Stream::Impl& stream) {
    const Stream::Impl::Target* t = stream.TargetOf(this);
    if (!t || !stream.SharedWith(*t) || !window) return;
    for (int i = 0; i < stream.slotCount; ++i) {
        window->ShareRegion(*stream.slots[i].region,
                            Dump({{"stream", stream.name}, {"id", stream.id}, {"slot", i}}), false);
    }
}

void Window::Impl::ShareSink(Sink::Impl& sink) {
    if (!sink.SharedWithPage() || !window) return;
    for (int i = 0; i < sink.slotCount; ++i) {
        window->ShareRegion(*sink.slots[size_t(i)].region,
                            Dump({{"sink", sink.name}, {"id", sink.id}, {"slot", i}}), true);
    }
}

void Window::Impl::AnnounceSink(Sink::Impl& sink) {
    sink.pageUploads = !pageShared;  // this page gave up on regions (or never had them)
    Send({{"t", "sink"}, {"sink", sink.Info()}});
    ShareSink(sink);
}

void Window::Impl::Announce(Stream::Impl& stream) {
    Stream::Impl::Target* t = stream.TargetOf(this);
    if (!t) return;
    if (!pageShared) t->pageFetches = true;  // this page gave up on regions (or never had them)
    Send({{"t", "stream"}, {"stream", stream.Info(*t)}});
    ShareStream(stream);
}

void Window::Impl::LeaveStreams() {
    for (Stream::Impl* stream : std::vector<Stream::Impl*>(received)) stream->RemoveTarget(this);
    received.clear();
}

void Window::Impl::StartPage(const std::string& name) {
    if (pageState != PageState::None) EndPage();  // e.g. reloaded without a navigation event
    generation = app->nextSession++;
    page = name;
    pageState = PageState::Connecting;
    pageShared = useShared;  // each page tries shared memory afresh
    json infos = json::array();
    std::vector<Stream::Impl*> sent;
    for (Stream::Impl* stream : PageStreams()) {
        stream->ResetForNewPage(this);
        const Stream::Impl::Target* t = stream->TargetOf(this);
        if (!t || !t->active) continue;
        infos.push_back(stream->Info(*t));
        sent.push_back(stream);
    }
    json sinkInfos = json::array();
    for (auto& sink : sinks) {
        sink->internal()->pageUploads = !useShared;
        sinkInfos.push_back(sink->internal()->Info());
    }
    Send({{"t", "welcome"}, {"sharedMemory", useShared}, {"streams", infos}, {"sinks", sinkInfos},
          {"session", generation}});
    for (Stream::Impl* stream : sent) ShareStream(*stream);
    for (auto& sink : sinks) ShareSink(*sink->internal());
}

void Window::Impl::EndPage() {
    if (pageState == PageState::None) return;
    const bool wasConnected = pageState == PageState::Connected;
    const std::string previous = page;
    pageState = PageState::None;
    page.clear();
    auto pending = std::move(pendingCalls);
    pendingCalls.clear();
    uploads.clear();  // sent for a call or event that never came
    // Binary data no page fetched; kept while another window's page may still fetch its own.
    if (app->Pages() == 0) DownloadRegistry().Clear();
    for (Stream::Impl* stream : PageStreams()) stream->ResetForNewPage(this);
    // User code, called from the platform's navigation and close events: nothing may escape.
    for (auto& entry : pending) {
        RunGuarded("a call's callback when the page went away",
                   [&] { entry.second(Result{false, nullptr, "the page closed"}); });
    }
    if (wasConnected && owner && owner->onDisconnect) {
        auto disconnected = owner->onDisconnect;
        RunGuarded("onDisconnect", [&] { disconnected(previous); });
    }
}

// App::sender() while a page's call or event is handled (nested: a handler that runs a nested
// message loop, a dialog, may handle another page's).
struct SenderScope {
    App::Impl& app;
    Window* saved;
    SenderScope(App::Impl& program, Window* window) : app(program), saved(program.sender) { app.sender = window; }
    ~SenderScope() { app.sender = saved; }
    SenderScope(const SenderScope&) = delete;
    SenderScope& operator=(const SenderScope&) = delete;
};

// Called from the platform's message callback (COM, GTK): nothing may throw out of it, neither
// a malformed message (json's value() throws on a field of the wrong type) nor user code.
void Window::Impl::OnMessage(const std::string& text) {
    RunGuarded("handling a message from the page", [&] { Dispatch(text); });
}

// Handlers are copied before they run, so one that re-registers itself (or replaces its
// stream) doesn't destroy the code that is running.
void Window::Impl::Dispatch(const std::string& text) {
    json message = json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    const std::string type = message.value("t", "");

    if (type == "hello") {
        StartPage(message.value("page", "page"));
        return;
    }
    if (pageState == PageState::None) return;  // only a hello starts a conversation

    if (type == "ready") {
        if (pageState == PageState::Connecting) {
            // sharedMemory: false means the page's regions didn't all arrive in time, so it
            // fetches every slot instead (the silent fallback; see uniapi.js).
            if (!message.value("sharedMemory", true) && pageShared) {
                pageShared = false;
                for (Stream::Impl* stream : PageStreams()) {
                    if (Stream::Impl::Target* t = stream->TargetOf(this)) t->pageFetches = true;
                }
                for (auto& sink : sinks) sink->internal()->pageUploads = true;
            }
            pageState = PageState::Connected;
            const std::string name = page;  // a copy: a handler that navigates ends the page
            if (owner && owner->onConnect) {
                auto connected = owner->onConnect;
                connected(name);
            }
        }
    } else if (type == "show") {
        if (window) window->Show();  // the page is drawn (app.show(); see showWhenReady)
    } else if (type == "call") {
        HandleCall(message);
    } else if (type == "result") {
        auto it = pendingCalls.find(message.value("id", uint64_t(0)));
        if (it == pendingCalls.end()) return;
        auto done = std::move(it->second);
        pendingCalls.erase(it);
        const bool ok = message.value("ok", false);
        json value = ok ? message.value("value", json()) : json();
        UploadedBytes bytes;  // binary data in the result: valid while done runs
        if (ok && message.value("uploads", false) && !TakeUploads(value, bytes.tokens)) {
            done(Result{false, nullptr, "binary data for this result didn't arrive"});
            return;
        }
        done(Result{ok, std::move(value), message.value("error", "")});
    } else if (type == "event") {
        const auto* found = FindEvent(message.value("name", ""));
        json data = message.value("data", json());
        UploadedBytes bytes;  // valid while the handler runs
        if (message.value("uploads", false) && !TakeUploads(data, bytes.tokens)) return;
        if (!found) return;
        auto handler = *found;
        SenderScope scope(*app, owner);
        if (handler) handler(data);
    } else if (type == "put") {
        Sink::Impl* sink = FindSink(message.value("sink", ""));
        if (!sink || message.value("id", uint64_t(0)) != sink->id) {  // replaced since: dropped
            uploads.erase(message.value("data", json()).is_object()
                              ? message["data"].value(kUploadKey, uint64_t(0)) : 0);
            return;
        }
        sink->Put(message);
    } else if (type == "unshared" && message.contains("sink")) {
        // A sink created or replaced after connecting whose regions never arrived; the page
        // uploads its slots from now on.
        Sink::Impl* sink = FindSink(message.value("sink", ""));
        if (sink && message.value("id", uint64_t(0)) == sink->id) sink->pageUploads = true;
    } else if (type == "unshared") {
        // A stream created or replaced after connecting whose regions never arrived; the page
        // fetches its slots from now on.
        Stream::Impl* stream = FindStream(message.value("stream", ""));
        if (stream && message.value("id", uint64_t(0)) == stream->id) {
            if (Stream::Impl::Target* t = stream->TargetOf(this)) t->pageFetches = true;
        }
    } else if (type == "release") {
        Stream::Impl* stream = FindStream(message.value("stream", ""));
        if (!stream || message.value("id", uint64_t(0)) != stream->id) return;  // replaced since
        stream->Release(this, message.value("slot", -1));
    }
}

void Window::Impl::HandleCall(const json& message) {
    auto state = std::make_shared<Reply::State>();
    state->window = weak_from_this();
    state->id = message.value("id", uint64_t(0));
    state->generation = generation;
    Reply reply(state);
    const std::string name = message.value("fn", "");
    json args = message.value("args", json::array());
    if (message.value("uploads", false) && !TakeUploads(args, state->bytes.tokens)) {
        reply.reject("binary data for this call didn't arrive");
        return;
    }
    const Handler* found = FindFunction(name);
    if (!found) {
        reply.reject("there is no C++ function named \"" + name + "\"");
        return;
    }
    Handler handler = *found;  // cheap: expose() keeps the function in a shared_ptr
    SenderScope scope(*app, owner);
    try {
        handler(args, reply);
    } catch (const std::exception& e) {
        reply.reject(e.what());
    } catch (...) {
        reply.reject("unknown C++ exception");
    }
}

void Window::Impl::SendResult(uint64_t id, uint64_t forGeneration, bool ok, json value,
                              std::string error) {
    if (forGeneration != generation || pageState == PageState::None) return;
    json message = {{"t", "result"}, {"id", id}, {"ok", ok}};
    if (ok) {
        message["value"] = std::move(value);
    } else {
        message["error"] = std::move(error);
    }
    Send(std::move(message));
}

// Swaps the page's upload placeholders in `value` for registry tokens.
bool Window::Impl::TakeUploads(json& value, std::vector<uint64_t>& tokens, int depth) {
    if (depth > 64) return true;
    if (value.is_object()) {
        auto placeholder = value.find(kUploadKey);
        if (value.size() == 1 && placeholder != value.end()) {
            auto upload = uploads.find(placeholder->is_number_unsigned() ? placeholder->get<uint64_t>() : 0);
            if (upload == uploads.end()) return false;
            const uint64_t token = Registry().Add(std::move(upload->second));
            uploads.erase(upload);
            tokens.push_back(token);
            value = json{{kBytesKey, token}};
            return true;
        }
    }
    if (value.is_object() || value.is_array()) {
        for (json& item : value) {
            if (!TakeUploads(item, tokens, depth + 1)) return false;
        }
    }
    return true;
}

void Window::Impl::OnRequest(detail::Request& request, detail::Response& response) {
    const std::string& path = request.path;
    // The browser percent-encodes the path ("two%20words" for a stream or file name with a
    // space). Decoded before any check below, so "%2e%2e" is caught as "..".
    std::string clean;
    if (!PercentDecode(path.substr(0, path.find_first_of("?#")), clean)) return;

    if (request.method == "POST") {  // /uniapi/upload/<session>/<n>; nothing else takes POSTs
        if (clean.rfind(kUploadPrefix, 0) != 0 || pageState == PageState::None) return;
        const std::string rest = clean.substr(kUploadPrefix.size());
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return;
        uint64_t session = 0, number = 0;
        if (!ParseNumber(rest.substr(0, slash), session) || !ParseNumber(rest.substr(slash + 1), number)) return;
        if (session != generation || number == 0) return;  // from a page that has gone
        uploads[number] = std::make_shared<std::vector<uint8_t>>(std::move(request.body));
        response.status = 200;
        response.mimeType = "text/plain";
        return;
    }
    if (request.method != "GET" && request.method != "HEAD") return;

    if (clean.rfind(kDownloadPrefix, 0) == 0) {  // /uniapi/download/<token>: once
        auto data = DownloadRegistry().Take(clean.substr(kDownloadPrefix.size()));
        if (!data) return;
        response.status = 200;
        response.mimeType = "application/octet-stream";
        response.data = data->data();
        response.size = data->size();
        response.keepAlive = std::move(data);
        return;
    }

    if (clean.rfind(kStreamPrefix, 0) == 0) {  // /uniapi/stream/<name>/<slot>: one its page has
        const std::string rest = clean.substr(kStreamPrefix.size());
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return;
        Stream::Impl* stream = FindStream(rest.substr(0, slash));
        uint64_t index = 0;
        if (!stream || !ParseNumber(rest.substr(slash + 1), index) || index >= stream->slots.size()) return;
        const Stream::Impl::Target* target = stream->TargetOf(this);
        if (!target || !(target->held & (uint64_t(1) << index))) return;  // not with this page
        Stream::Impl::SlotData& slot = stream->slots[size_t(index)];
        if (slot.state != Stream::Impl::State::WithPage) return;
        response.status = 200;
        response.mimeType = "application/octet-stream";
        std::shared_ptr<std::vector<uint8_t>> bytes = slot.heap;
        if (!bytes) {
            // A shared slot the page couldn't map (the fallback). Serve a copy: the platform may
            // read the response on another thread after the region is gone, and shared regions
            // must be released on the UI thread.
            bytes = std::make_shared<std::vector<uint8_t>>(slot.data, slot.data + slot.size);
        }
        response.data = bytes->data();
        response.size = slot.size;
        response.keepAlive = std::move(bytes);
        return;
    }

    if (clean.rfind(kLibraryPrefix, 0) == 0) {
        if (const Asset* asset = FindLibraryAsset(std::string_view(clean).substr(kLibraryPrefix.size()))) {
            Serve(*asset, response);
        }
        return;
    }

    std::string relative = clean;
    while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
    if (relative.empty() || relative.back() == '/') relative += "index.html";
    if (relative.find("..") != std::string::npos) return;
    // A path with a drive or root ("C:/Windows/win.ini") would replace serveDirectory's folder
    // when joined to it, and serve any file on the machine.
    if (std::filesystem::u8path(relative).has_root_name() || std::filesystem::u8path(relative).has_root_directory()) {
        return;
    }

    if (const Asset* asset = FindAsset(app->assets, relative)) {
        Serve(*asset, response);
        return;
    }
    if (!app->assetDirectory.empty()) {
        std::filesystem::path file =
            std::filesystem::u8path(app->assetDirectory) / std::filesystem::u8path(relative);
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) return;  // a folder opens as a stream on Linux
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        auto bytes = std::make_shared<std::vector<uint8_t>>(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        response.status = 200;
        response.mimeType = MimeTypeFor(relative);
        response.data = bytes->data();
        response.size = bytes->size();
        response.keepAlive = bytes;
    }
}

}  // namespace uniapi
