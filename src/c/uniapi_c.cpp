// The C API (include/uniapi/uniapi.h) over the C++ library. No C++ exception may reach C code:
// functions that can fail catch them and leave the message in uniapi_last_error(). The
// wrappers around C callbacks run inside the library, which catches what they throw.

#include "c_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using uniapi::json;

struct uniapi_bytes {
    std::vector<uniapi::Bytes> items;
};

struct uniapi_reply {
    explicit uniapi_reply(uniapi::Reply r) : reply(std::move(r)) {}
    uniapi::Reply reply;
    uniapi_bytes bytes;
};

namespace {

thread_local std::string lastError;

using uniapi::c_api::Guard;
using uniapi::c_api::GuardVoid;
using uniapi::c_api::SetError;

// malloc'ed copies for C, freed with uniapi_free / uniapi_free_list. Throw when out of memory.
char* Dup(const std::string& text) {
    char* copy = static_cast<char*>(std::malloc(text.size() + 1));
    if (!copy) throw std::bad_alloc();
    std::memcpy(copy, text.c_str(), text.size() + 1);
    return copy;
}

char** DupList(const std::vector<std::string>& items) {
    auto** list = static_cast<char**>(std::calloc(items.size() + 1, sizeof(char*)));  // NULL-terminated
    if (!list) throw std::bad_alloc();
    try {
        for (size_t i = 0; i < items.size(); ++i) list[i] = Dup(items[i]);
    } catch (...) {
        for (size_t i = 0; list[i]; ++i) std::free(list[i]);
        std::free(list);
        throw;
    }
    return list;
}

std::string Text(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

// JSON from C, or null for NULL. Throws on invalid JSON.
json Parse(const char* text, const char* what) {
    if (!text) return nullptr;
    json value = json::parse(text, nullptr, false);
    if (value.is_discarded()) throw std::invalid_argument(std::string(what) + ": invalid JSON: " + text);
    return value;
}

// Swaps the C++ library's binary placeholders for {"$bytes": <index into out>}.
void ExtractBytes(json& value, std::vector<uniapi::Bytes>& out, int depth = 0) {
    if (depth > 64) return;
    if (value.is_object() && value.size() == 1 && value.contains("$uniapi.bytes")) {
        out.push_back(value.get<uniapi::Bytes>());
        value = json{{"$bytes", out.size() - 1}};
        return;
    }
    if (value.is_object() || value.is_array()) {
        for (json& item : value) ExtractBytes(item, out, depth + 1);
    }
}

std::string Str(const char* text) { return text ? text : ""; }
std::string_view View(const char* text) { return text ? std::string_view(text) : std::string_view(); }

const json* Find(const json& document, const char* pointer) {
    const json::json_pointer path(Str(pointer));
    return document.contains(path) ? &document.at(path) : nullptr;
}

uniapi::Stream* StreamOf(uniapi_stream* stream) { return reinterpret_cast<uniapi::Stream*>(stream); }
const uniapi::Stream* StreamOf(const uniapi_stream* stream) { return reinterpret_cast<const uniapi::Stream*>(stream); }
uniapi::DomCommandWriter* DomOf(uniapi_dom* dom) { return reinterpret_cast<uniapi::DomCommandWriter*>(dom); }

uniapi::Window& WindowOf(uniapi_window* window) {
    if (!window) throw std::invalid_argument("no window");
    return *reinterpret_cast<uniapi::Window*>(window);
}
const uniapi::Window* WindowOf(const uniapi_window* window) { return reinterpret_cast<const uniapi::Window*>(window); }

std::function<void(const std::string&)> PageHandler(uniapi_page_handler handler, void* user) {
    if (!handler) return nullptr;
    return [handler, user](const std::string& page) { handler(user, page.c_str()); };
}

// A list for C: the strings and a NULL-terminated array of pointers into them.
struct CList {
    explicit CList(const std::vector<std::string>& items) {
        for (const std::string& item : items) pointers.push_back(item.c_str());
        pointers.push_back(nullptr);
    }
    std::vector<const char*> pointers;
    const char* const* data() const { return pointers.data(); }
    size_t size() const { return pointers.size() - 1; }
};

std::function<void(const std::vector<std::string>&, int, int)> DropHandler(uniapi_drop_handler handler, void* user) {
    if (!handler) return nullptr;
    return [handler, user](const std::vector<std::string>& paths, int x, int y) {
        const CList list(paths);
        handler(user, list.data(), list.size(), x, y);
    };
}

std::function<bool(uniapi::CloseReason)> CloseHandler(uniapi_close_handler handler, void* user) {
    if (!handler) return nullptr;
    return [handler, user](uniapi::CloseReason reason) {
        return handler(user, reason == uniapi::CloseReason::Quit ? UNIAPI_CLOSE_QUIT : UNIAPI_CLOSE_WINDOW) != 0;
    };
}

}  // namespace

extern "C" {

// --- The app ----------------------------------------------------------------------------------

void uniapi_options_init(uniapi_options* options) {
    if (!options) return;
    const uniapi::AppOptions defaults;
    options->title = "uniapi";
    options->width = defaults.width;
    options->height = defaults.height;
    options->min_width = defaults.minWidth;
    options->min_height = defaults.minHeight;
    options->resizable = defaults.resizable;
    options->keep_aspect_ratio = defaults.keepAspectRatio;
    options->fullscreen = defaults.fullscreen;
    options->shared_memory = defaults.sharedMemory;
    options->activate = defaults.activate;
    options->host = "uniapi.example";
    options->data_directory = nullptr;
    options->browser_arguments = nullptr;
    options->title_bar = UNIAPI_TITLE_BAR_SYSTEM;
    options->show_when_ready = defaults.showWhenReady;
    options->defer_tasks_after_input = defaults.deferTasksAfterInput;
    options->dmabuf_renderer = defaults.dmabufRenderer;
    options->start_hidden = defaults.startHidden;
    options->single_instance = nullptr;
    options->remember_placement = nullptr;
    options->strict_calls = defaults.strictCalls;
    options->dev_tools = defaults.devTools;
    options->max_width = defaults.maxWidth;
    options->max_height = defaults.maxHeight;
    options->background_color = nullptr;
}

namespace {
uniapi::TitleBar TitleBarOf(int mode) {
    return mode == UNIAPI_TITLE_BAR_DARK    ? uniapi::TitleBar::Dark
           : mode == UNIAPI_TITLE_BAR_LIGHT ? uniapi::TitleBar::Light
                                            : uniapi::TitleBar::System;
}
}  // namespace

uniapi_app* uniapi_app_new(const uniapi_options* options) {
    return Guard<uniapi_app*>(nullptr, [&] {
        uniapi::AppOptions o;
        if (options) {
            if (options->title) o.title = options->title;
            o.width = options->width;
            o.height = options->height;
            o.minWidth = options->min_width;
            o.minHeight = options->min_height;
            o.resizable = options->resizable != 0;
            o.keepAspectRatio = options->keep_aspect_ratio != 0;
            o.fullscreen = options->fullscreen != 0;
            o.sharedMemory = options->shared_memory != 0;
            o.activate = options->activate != 0;
            if (options->host) o.host = options->host;
            if (options->data_directory) o.dataDirectory = options->data_directory;
            if (options->browser_arguments) o.browserArguments = options->browser_arguments;
            o.titleBar = TitleBarOf(options->title_bar);
            o.showWhenReady = options->show_when_ready != 0;
            o.deferTasksAfterInput = options->defer_tasks_after_input != 0;
            o.dmabufRenderer = options->dmabuf_renderer != 0;
            o.startHidden = options->start_hidden != 0;
            if (options->single_instance) o.singleInstance = options->single_instance;
            if (options->remember_placement) o.rememberPlacement = options->remember_placement;
            o.strictCalls = options->strict_calls != 0;
            o.devTools = options->dev_tools != 0;
            o.maxWidth = options->max_width;
            o.maxHeight = options->max_height;
            if (options->background_color) o.backgroundColor = options->background_color;
        }
        return new uniapi_app(o);
    });
}

void uniapi_app_free(uniapi_app* app) { delete app; }

int uniapi_on_ui_thread(const uniapi_app* app) { return app && app->app.onUiThread() ? 1 : 0; }

int uniapi_web_engine(const uniapi_app* app, uniapi_web_engine_info* out) {
    if (!out) return 0;
    *out = uniapi_web_engine_info{};
    return Guard(0, [&] {
        const uniapi::WebEngine engine = app->app.webEngine();
        // Copied cut to fit, always ending in '\0'.
        auto copy = [](const std::string& text, char* to, size_t size) {
            const size_t n = std::min(text.size(), size - 1);
            std::memcpy(to, text.data(), n);
            to[n] = '\0';
        };
        copy(engine.name, out->name, sizeof out->name);
        copy(engine.version, out->version, sizeof out->version);
        copy(engine.variant, out->variant, sizeof out->variant);
        return 1;
    });
}

int uniapi_run(uniapi_app* app) {
    return Guard(1, [&] { return app->app.run(); });
}

void uniapi_quit(uniapi_app* app) {
    GuardVoid([&] { app->app.quit(); });
}

void uniapi_serve(uniapi_app* app, const uniapi_assets* assets) {
    if (!assets) {
        SetError("uniapi_serve: no assets");
        return;
    }
    GuardVoid([&] { app->app.serve(*reinterpret_cast<const uniapi::AssetTable*>(assets)); });
}

void uniapi_serve_directory(uniapi_app* app, const char* path) {
    GuardVoid([&] { app->app.serveDirectory(Str(path)); });
}

void uniapi_navigate(uniapi_app* app, const char* path) {
    GuardVoid([&] { app->app.navigate(Str(path)); });
}

// --- JavaScript calls C -------------------------------------------------------------------------

namespace {
int Expose(uniapi::Window& window, const char* name, uniapi_handler handler, void* user) {
    return Guard(0, [&] {
        if (!handler) throw std::invalid_argument("uniapi_expose: no handler");
        window.exposeHandler(Str(name), [handler, user](const json& args, const uniapi::Reply& reply) {
            auto answer = std::make_unique<uniapi_reply>(reply);
            json copy = args;
            ExtractBytes(copy, answer->bytes.items);  // throws (and rejects) if the data is gone
            const std::string text = Text(copy);
            uniapi_reply* r = answer.release();      // the C code owns it now
            handler(user, text.c_str(), &r->bytes, r);
        });
        return 1;
    });
}
}  // namespace

int uniapi_expose(uniapi_app* app, const char* name, uniapi_handler handler, void* user) {
    return Expose(app->app, name, handler, user);
}

void uniapi_reply_resolve(uniapi_reply* reply, const char* value_json) {
    if (!reply) return;
    try {
        reply->reply.resolve(Parse(value_json, "uniapi_reply_resolve"));
    } catch (const std::exception& e) {
        SetError(e.what());
        reply->reply.reject(e.what());
    }
    delete reply;
}

void uniapi_reply_reject(uniapi_reply* reply, const char* message) {
    if (!reply) return;
    GuardVoid([&] { reply->reply.reject(Str(message)); });
    delete reply;
}

// --- Events ---------------------------------------------------------------------------------------

namespace {
int On(uniapi::Window& window, const char* event, uniapi_event_handler handler, void* user) {
    return Guard(0, [&] {
        if (!handler) throw std::invalid_argument("uniapi_on: no handler");
        window.on(Str(event), [handler, user](const json& data) {
            uniapi_bytes bytes;
            json copy = data;
            ExtractBytes(copy, bytes.items);
            const std::string text = Text(copy);
            handler(user, text.c_str(), &bytes);
        });
        return 1;
    });
}

int Emit(uniapi::Window& window, const char* event, const char* data_json) {
    return Guard(0, [&] {
        window.emit(Str(event), Parse(data_json, "uniapi_emit"));
        return 1;
    });
}
}  // namespace

int uniapi_on(uniapi_app* app, const char* event, uniapi_event_handler handler, void* user) {
    return On(app->app, event, handler, user);
}

void uniapi_off(uniapi_app* app, const char* event) {
    GuardVoid([&] { app->app.off(Str(event)); });
}

void uniapi_unexpose(uniapi_app* app, const char* name) {
    GuardVoid([&] { app->app.unexpose(Str(name)); });
}

int uniapi_emit(uniapi_app* app, const char* event, const char* data_json) {
    return Emit(app->app, event, data_json);
}

// --- C calls JavaScript -------------------------------------------------------------------------

namespace {
int Call(uniapi::Window& window, const char* function, const char* args_json, uniapi_result_handler done, void* user) {
    return Guard(0, [&] {
        json args = args_json ? Parse(args_json, "uniapi_call") : json::array();
        window.call(Str(function), std::move(args), [done, user](const uniapi::Result& result) {
            if (!done) return;
            uniapi_bytes bytes;
            if (!result.ok) {
                done(user, 0, result.error.c_str(), &bytes);
                return;
            }
            std::string text;
            try {
                json value = result.value;
                ExtractBytes(value, bytes.items);
                text = Text(value);
            } catch (const std::exception& e) {
                done(user, 0, e.what(), &bytes);
                return;
            }
            done(user, 1, text.c_str(), &bytes);
        });
        return 1;
    });
}
}  // namespace

int uniapi_call(uniapi_app* app, const char* function, const char* args_json, uniapi_result_handler done,
                void* user) {
    return Call(app->app, function, args_json, done, user);
}

// --- Binary data ----------------------------------------------------------------------------------

size_t uniapi_bytes_count(const uniapi_bytes* bytes) { return bytes ? bytes->items.size() : 0; }

const unsigned char* uniapi_bytes_data(const uniapi_bytes* bytes, size_t index, size_t* size) {
    if (!bytes || index >= bytes->items.size()) {
        if (size) *size = 0;
        return nullptr;
    }
    const uniapi::Bytes& item = bytes->items[index];
    if (size) *size = item.size();
    static const unsigned char kEmpty = 0;
    return item.data() ? item.data() : &kEmpty;  // never NULL for a valid index
}

char* uniapi_bytes_json(const void* data, size_t size) {
    return Guard<char*>(nullptr, [&] {
        if (!data && size > 0) throw std::invalid_argument("uniapi_bytes_json: no data");
        return Dup(Text(json(uniapi::Bytes(data, size))));
    });
}

// --- Connection ------------------------------------------------------------------------------------

void uniapi_on_connect(uniapi_app* app, uniapi_page_handler handler, void* user) {
    GuardVoid([&] { app->app.onConnect = PageHandler(handler, user); });
}

void uniapi_on_disconnect(uniapi_app* app, uniapi_page_handler handler, void* user) {
    GuardVoid([&] { app->app.onDisconnect = PageHandler(handler, user); });
}

int uniapi_connected(const uniapi_app* app) {
    return Guard(0, [&] { return app->app.connected() ? 1 : 0; });
}
int uniapi_shared_memory(const uniapi_app* app) {
    return Guard(0, [&] { return app->app.sharedMemory() ? 1 : 0; });
}

// --- Window and event loop --------------------------------------------------------------------

void uniapi_set_title(uniapi_app* app, const char* title) {
    GuardVoid([&] { app->app.setTitle(Str(title)); });
}

void uniapi_set_size(uniapi_app* app, int width, int height) {
    GuardVoid([&] { app->app.setSize(width, height); });
}

void uniapi_set_fullscreen(uniapi_app* app, int on) {
    GuardVoid([&] { app->app.setFullscreen(on != 0); });
}

int uniapi_fullscreen(const uniapi_app* app) {
    return Guard(0, [&] { return app->app.fullscreen() ? 1 : 0; });
}

void uniapi_set_title_bar(uniapi_app* app, int mode) {
    GuardVoid([&] { app->app.setTitleBar(TitleBarOf(mode)); });
}

void uniapi_show(uniapi_app* app) {
    GuardVoid([&] { app->app.show(); });
}

void uniapi_hide(uniapi_app* app) {
    GuardVoid([&] { app->app.hide(); });
}

int uniapi_visible(const uniapi_app* app) {
    return Guard(0, [&] { return app && app->app.visible() ? 1 : 0; });
}

void uniapi_on_close_requested(uniapi_app* app, uniapi_close_handler handler, void* user) {
    GuardVoid([&] { app->app.onCloseRequested = CloseHandler(handler, user); });
}

// --- The desktop ----------------------------------------------------------------------------------

void uniapi_on_external_url(uniapi_app* app, uniapi_url_handler handler, void* user) {
    GuardVoid([&] {
        app->app.onExternalURL = handler ? std::function<bool(const std::string&)>(
                                               [handler, user](const std::string& url) { return handler(user, url.c_str()) != 0; })
                                         : nullptr;
    });
}

void uniapi_on_second_instance(uniapi_app* app, uniapi_launch_handler handler, void* user) {
    GuardVoid([&] {
        if (!handler) {
            app->app.onSecondInstance = nullptr;
            return;
        }
        app->app.onSecondInstance = [handler, user](const std::vector<std::string>& args, const std::string& dir) {
            const CList list(args);
            handler(user, list.data(), list.size(), dir.c_str());
        };
    });
}

void uniapi_on_file_drop(uniapi_app* app, uniapi_drop_handler handler, void* user) {
    GuardVoid([&] { app->app.onFileDrop = DropHandler(handler, user); });
}

void uniapi_clipboard_text(uniapi_app* app, uniapi_text_handler handler, void* user) {
    GuardVoid([&] {
        if (!handler) return;
        app->app.clipboardText([handler, user](std::optional<std::string> text) {
            handler(user, text ? text->c_str() : nullptr);
        });
    });
}

int uniapi_set_clipboard_text(uniapi_app* app, const char* text) {
    return Guard(0, [&] {
        if (!app->app.setClipboardText(Str(text))) throw std::runtime_error("the clipboard didn't take the text");
        return 1;
    });
}

// --- Notifications ----------------------------------------------------------------------------

char* uniapi_notify(uniapi_app* app, const char* title, const char* body, const char* id, uniapi_notified done,
                    void* user) {
    return Guard<char*>(nullptr, [&] {
        uniapi::Notification n{Str(title), Str(body), Str(id)};
        std::function<void(const uniapi::Result&)> answer;
        if (done) {
            answer = [done, user](const uniapi::Result& result) {
                const std::string notified = result.value.is_string() ? result.value.get<std::string>() : std::string();
                done(user, notified.c_str(), result.ok ? nullptr : result.error.c_str());
            };
        }
        return Dup(app->app.notify(std::move(n), std::move(answer)));
    });
}

void uniapi_on_notification_clicked(uniapi_app* app, uniapi_id_handler handler, void* user) {
    GuardVoid([&] {
        app->app.onNotificationClicked =
            handler ? std::function<void(const std::string&)>([handler, user](const std::string& id) { handler(user, id.c_str()); })
                    : nullptr;
    });
}

int uniapi_open_url(const char* url) {
    return Guard(0, [&] {
        uniapi::openURL(Str(url));
        return 1;
    });
}

int uniapi_open_path(const char* path) {
    return Guard(0, [&] {
        uniapi::openPath(Str(path));
        return 1;
    });
}

int uniapi_reveal_path(const char* path) {
    return Guard(0, [&] {
        uniapi::revealPath(Str(path));
        return 1;
    });
}

int uniapi_title_bar(const uniapi_app* app) {
    return Guard(int(UNIAPI_TITLE_BAR_SYSTEM), [&] {
        switch (app->app.titleBar()) {
        case uniapi::TitleBar::Dark: return int(UNIAPI_TITLE_BAR_DARK);
        case uniapi::TitleBar::Light: return int(UNIAPI_TITLE_BAR_LIGHT);
        default: return int(UNIAPI_TITLE_BAR_SYSTEM);
        }
    });
}

int uniapi_set_icon(uniapi_app* app, const unsigned char* png, size_t size) {
    return Guard(0, [&] { return app->app.setIcon(png, size) ? 1 : 0; });
}

int uniapi_every(uniapi_app* app, unsigned milliseconds, uniapi_callback fn, void* user) {
    return Guard(0, [&] {
        if (!fn) throw std::invalid_argument("uniapi_every: no function");
        const int id = app->app.every(std::chrono::milliseconds(milliseconds), [fn, user] { fn(user); });
        if (id == 0) throw std::runtime_error("uniapi_every: no timer available");
        return id;
    });
}

void uniapi_cancel(uniapi_app* app, int timer) {
    GuardVoid([&] { app->app.cancel(timer); });
}

void uniapi_post(uniapi_app* app, uniapi_callback fn, void* user) {
    if (!fn) return;
    GuardVoid([&] { app->app.post([fn, user] { fn(user); }); });
}

char* uniapi_save_file_dialog(uniapi_app* app, const char* title, const char* default_name,
                              const char* filter_name, const char* filter_pattern) {
    return Guard<char*>(nullptr, [&]() -> char* {
        uniapi::SaveDialogOptions options;
        if (title) options.title = title;
        options.defaultName = Str(default_name);
        options.filterName = Str(filter_name);
        options.filterPattern = Str(filter_pattern);
        auto path = app->app.saveFileDialog(options);
        return path ? Dup(*path) : nullptr;
    });
}

char** uniapi_open_file_dialog(uniapi_app* app, const char* title, const char* filter_name,
                               const char* filter_pattern, int multiple, int folder) {
    return Guard<char**>(nullptr, [&]() -> char** {
        uniapi::OpenDialogOptions options;
        if (title) options.title = title;
        options.filterName = Str(filter_name);
        options.filterPattern = Str(filter_pattern);
        options.multiple = multiple != 0;
        options.folder = folder != 0;
        auto paths = app->app.openFileDialog(options);
        return paths.empty() ? nullptr : DupList(paths);
    });
}

namespace {

std::vector<uniapi::FileFilter> Filters(const uniapi_dialog_options& options) {
    std::vector<uniapi::FileFilter> filters;
    for (int i = 0; options.filters && i < options.filter_count; ++i) {
        filters.push_back({Str(options.filters[i].name), Str(options.filters[i].pattern)});
    }
    return filters;
}

}  // namespace

char* uniapi_save_file_dialog_with(uniapi_app* app, const uniapi_dialog_options* given) {
    return Guard<char*>(nullptr, [&]() -> char* {
        const uniapi_dialog_options none{};
        const uniapi_dialog_options& o = given ? *given : none;
        uniapi::SaveDialogOptions options;
        if (o.title) options.title = o.title;
        options.defaultName = Str(o.default_name);
        options.filters = Filters(o);
        options.directory = Str(o.directory);
        auto path = app->app.saveFileDialog(options);
        return path ? Dup(*path) : nullptr;
    });
}

char** uniapi_open_file_dialog_with(uniapi_app* app, const uniapi_dialog_options* given) {
    return Guard<char**>(nullptr, [&]() -> char** {
        const uniapi_dialog_options none{};
        const uniapi_dialog_options& o = given ? *given : none;
        uniapi::OpenDialogOptions options;
        if (o.title) options.title = o.title;
        options.filters = Filters(o);
        options.directory = Str(o.directory);
        options.multiple = o.multiple != 0;
        options.folder = o.folder != 0;
        auto paths = app->app.openFileDialog(options);
        return paths.empty() ? nullptr : DupList(paths);
    });
}

// --- Streams ------------------------------------------------------------------------------------------

uniapi_stream* uniapi_stream_create(uniapi_app* app, const char* name, size_t slot_size, int slot_count) {
    return Guard<uniapi_stream*>(nullptr, [&] {
        return reinterpret_cast<uniapi_stream*>(&app->app.stream(Str(name), slot_size, slot_count));
    });
}

int uniapi_stream_acquire(uniapi_stream* stream, uniapi_slot* slot) {
    return Guard(0, [&] {
        uniapi::Stream::Slot s;
        if (!slot || !StreamOf(stream)->acquire(s)) return 0;
        slot->index = s.index;
        slot->data = s.data;
        slot->capacity = s.capacity;
        return 1;
    });
}

int uniapi_stream_publish(uniapi_stream* stream, const uniapi_slot* slot, size_t size, const char* meta_json) {
    return Guard(0, [&] {
        if (!slot) throw std::invalid_argument("uniapi_stream_publish: no slot");
        uniapi::Stream::Slot s;
        s.index = slot->index;
        s.data = slot->data;
        s.capacity = slot->capacity;
        return StreamOf(stream)->publish(s, size, Parse(meta_json, "uniapi_stream_publish")) ? 1 : 0;
    });
}

void uniapi_stream_on_slot_freed(uniapi_stream* stream, uniapi_callback fn, void* user) {
    GuardVoid([&] {
        StreamOf(stream)->onSlotFreed = fn ? std::function<void()>([fn, user] { fn(user); }) : nullptr;
    });
}

int uniapi_stream_shared(const uniapi_stream* stream) {
    return Guard(0, [&] { return StreamOf(stream)->shared() ? 1 : 0; });
}
uint64_t uniapi_stream_delivered(const uniapi_stream* stream) {
    return Guard<uint64_t>(0, [&] { return StreamOf(stream)->delivered(); });
}

// --- DOM command lists ----------------------------------------------------------------------------

namespace {
int SendDom(uniapi::Window& window, uniapi_dom_writer write, void* user) {
    return Guard(0, [&] {
        if (!write) throw std::invalid_argument("uniapi_send_dom: no writer");
        return window.sendDom([&](uniapi::DomCommandWriter& writer) {
            write(user, reinterpret_cast<uniapi_dom*>(&writer));
        }) ? 1 : 0;
    });
}
}  // namespace

int uniapi_send_dom(uniapi_app* app, uniapi_dom_writer write, void* user) {
    return SendDom(app->app, write, user);
}

// DomCommandWriter doesn't throw or allocate (a command that doesn't fit is left out), so
// these need no guard.
void uniapi_dom_bind(uniapi_dom* dom, uint32_t id, const char* selector) { DomOf(dom)->Bind(id, View(selector)); }
void uniapi_dom_create(uniapi_dom* dom, uint32_t id, const char* tag) { DomOf(dom)->Create(id, View(tag)); }
void uniapi_dom_append(uniapi_dom* dom, uint32_t parent, uint32_t child) { DomOf(dom)->Append(parent, child); }
void uniapi_dom_remove(uniapi_dom* dom, uint32_t id) { DomOf(dom)->Remove(id); }
void uniapi_dom_clear(uniapi_dom* dom, uint32_t id) { DomOf(dom)->Clear(id); }
void uniapi_dom_set_text(uniapi_dom* dom, uint32_t id, const char* text) { DomOf(dom)->SetText(id, View(text)); }
void uniapi_dom_set_attribute(uniapi_dom* dom, uint32_t id, const char* name, const char* value) {
    DomOf(dom)->SetAttribute(id, View(name), View(value));
}
void uniapi_dom_set_style(uniapi_dom* dom, uint32_t id, const char* property, const char* value) {
    DomOf(dom)->SetStyle(id, View(property), View(value));
}
void uniapi_dom_toggle_class(uniapi_dom* dom, uint32_t id, const char* name, int on) {
    DomOf(dom)->ToggleClass(id, View(name), on != 0);
}
int uniapi_dom_overflowed(const uniapi_dom* dom) {
    return reinterpret_cast<const uniapi::DomCommandWriter*>(dom)->overflowed();
}

// --- JSON helpers -------------------------------------------------------------------------------------

char* uniapi_json_string(const char* text) {
    return Guard<char*>(nullptr, [&] { return Dup(Text(json(Str(text)))); });
}

char* uniapi_json_get(const char* json_text, const char* pointer) {
    return Guard<char*>(nullptr, [&]() -> char* {
        const json document = Parse(json_text, "uniapi_json_get");
        const json* value = Find(document, pointer);
        if (!value) throw std::invalid_argument("uniapi_json_get: nothing at " + Str(pointer));
        return Dup(Text(*value));
    });
}

char* uniapi_json_get_string(const char* json_text, const char* pointer) {
    return Guard<char*>(nullptr, [&]() -> char* {
        const json document = Parse(json_text, "uniapi_json_get_string");
        const json* value = Find(document, pointer);
        if (!value || !value->is_string()) throw std::invalid_argument("uniapi_json_get_string: no string at " + Str(pointer));
        return Dup(value->get<std::string>());
    });
}

int uniapi_json_get_number(const char* json_text, const char* pointer, double* out) {
    return Guard(0, [&] {
        const json document = Parse(json_text, "uniapi_json_get_number");
        const json* value = Find(document, pointer);
        if (!value || !value->is_number()) throw std::invalid_argument("uniapi_json_get_number: no number at " + Str(pointer));
        if (out) *out = value->get<double>();
        return 1;
    });
}

int uniapi_json_get_bool(const char* json_text, const char* pointer, int* out) {
    return Guard(0, [&] {
        const json document = Parse(json_text, "uniapi_json_get_bool");
        const json* value = Find(document, pointer);
        if (!value || !value->is_boolean()) throw std::invalid_argument("uniapi_json_get_bool: no boolean at " + Str(pointer));
        if (out) *out = value->get<bool>() ? 1 : 0;
        return 1;
    });
}

// --- Helpers --------------------------------------------------------------------------------------------

char* uniapi_executable_path(void) {
    return Guard<char*>(nullptr, [] { return Dup(uniapi::executablePath()); });
}

char** uniapi_arguments(void) {
    return Guard<char**>(nullptr, [] { return DupList(uniapi::arguments()); });
}

void uniapi_alert(const char* title, const char* message) {
    GuardVoid([&] { uniapi::alert(Str(title), Str(message)); });
}

void uniapi_free(void* memory) { std::free(memory); }

void uniapi_free_list(char** list) {
    if (!list) return;
    for (char** item = list; *item; ++item) std::free(*item);
    std::free(list);
}

const char* uniapi_last_error(void) { return lastError.c_str(); }

// --- More windows ---------------------------------------------------------------------------------

void uniapi_window_options_init(uniapi_window_options* options) {
    if (!options) return;
    const uniapi::WindowOptions defaults;
    options->title = "uniapi";
    options->width = defaults.width;
    options->height = defaults.height;
    options->min_width = defaults.minWidth;
    options->min_height = defaults.minHeight;
    options->resizable = defaults.resizable;
    options->keep_aspect_ratio = defaults.keepAspectRatio;
    options->title_bar = UNIAPI_TITLE_BAR_SYSTEM;
    options->show_when_ready = defaults.showWhenReady;
    options->activate = defaults.activate;
    options->owned = defaults.owned;
    options->modal = defaults.modal;
    options->start_hidden = defaults.startHidden;
    options->remember_placement = nullptr;
    options->max_width = defaults.maxWidth;
    options->max_height = defaults.maxHeight;
    options->background_color = nullptr;
}

uniapi_window* uniapi_open_window(uniapi_app* app, const uniapi_window_options* options) {
    return Guard<uniapi_window*>(nullptr, [&] {
        if (!app) throw std::invalid_argument("uniapi_open_window: no app");
        uniapi::WindowOptions o;
        if (options) {
            if (options->title) o.title = options->title;
            o.width = options->width;
            o.height = options->height;
            o.minWidth = options->min_width;
            o.minHeight = options->min_height;
            o.resizable = options->resizable != 0;
            o.keepAspectRatio = options->keep_aspect_ratio != 0;
            o.titleBar = TitleBarOf(options->title_bar);
            o.showWhenReady = options->show_when_ready != 0;
            o.activate = options->activate != 0;
            o.owned = options->owned != 0;
            o.modal = options->modal != 0;
            o.startHidden = options->start_hidden != 0;
            if (options->remember_placement) o.rememberPlacement = options->remember_placement;
            o.maxWidth = options->max_width;
            o.maxHeight = options->max_height;
            if (options->background_color) o.backgroundColor = options->background_color;
        }
        return reinterpret_cast<uniapi_window*>(&app->app.openWindow(o));
    });
}

uniapi_window* uniapi_main_window(uniapi_app* app) {
    return app ? reinterpret_cast<uniapi_window*>(static_cast<uniapi::Window*>(&app->app)) : nullptr;
}

void uniapi_window_close(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).close(); });
}

void uniapi_window_open_dev_tools(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).openDevTools(); });
}

void uniapi_window_on_closed(uniapi_window* window, uniapi_callback handler, void* user) {
    GuardVoid([&] {
        WindowOf(window).onClosed = handler ? std::function<void()>([handler, user] { handler(user); }) : nullptr;
    });
}

void uniapi_window_on_close_requested(uniapi_window* window, uniapi_close_handler handler, void* user) {
    GuardVoid([&] { WindowOf(window).onCloseRequested = CloseHandler(handler, user); });
}

void uniapi_window_on_file_drop(uniapi_window* window, uniapi_drop_handler handler, void* user) {
    GuardVoid([&] { WindowOf(window).onFileDrop = DropHandler(handler, user); });
}

void uniapi_window_navigate(uniapi_window* window, const char* path) {
    GuardVoid([&] { WindowOf(window).navigate(Str(path)); });
}

int uniapi_window_expose(uniapi_window* window, const char* name, uniapi_handler handler, void* user) {
    return Guard(0, [&] { return Expose(WindowOf(window), name, handler, user); });
}

int uniapi_window_on(uniapi_window* window, const char* event, uniapi_event_handler handler, void* user) {
    return Guard(0, [&] { return On(WindowOf(window), event, handler, user); });
}

void uniapi_window_off(uniapi_window* window, const char* event) {
    GuardVoid([&] { WindowOf(window).off(Str(event)); });
}

void uniapi_window_unexpose(uniapi_window* window, const char* name) {
    GuardVoid([&] { WindowOf(window).unexpose(Str(name)); });
}

int uniapi_window_emit(uniapi_window* window, const char* event, const char* data_json) {
    return Guard(0, [&] { return Emit(WindowOf(window), event, data_json); });
}

int uniapi_window_call(uniapi_window* window, const char* function, const char* args_json,
                       uniapi_result_handler done, void* user) {
    return Guard(0, [&] { return Call(WindowOf(window), function, args_json, done, user); });
}

void uniapi_window_on_connect(uniapi_window* window, uniapi_page_handler handler, void* user) {
    GuardVoid([&] { WindowOf(window).onConnect = PageHandler(handler, user); });
}

void uniapi_window_on_disconnect(uniapi_window* window, uniapi_page_handler handler, void* user) {
    GuardVoid([&] { WindowOf(window).onDisconnect = PageHandler(handler, user); });
}

int uniapi_window_connected(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->connected() ? 1 : 0; });
}
int uniapi_window_shared_memory(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->sharedMemory() ? 1 : 0; });
}

uniapi_stream* uniapi_window_stream_create(uniapi_window* window, const char* name, size_t slot_size, int slot_count) {
    return Guard<uniapi_stream*>(nullptr, [&] {
        return reinterpret_cast<uniapi_stream*>(&WindowOf(window).stream(Str(name), slot_size, slot_count));
    });
}

int uniapi_stream_add_window(uniapi_stream* stream, uniapi_window* window) {
    return Guard(0, [&] {
        StreamOf(stream)->addWindow(WindowOf(window));
        return 1;
    });
}

void uniapi_stream_remove_window(uniapi_stream* stream, uniapi_window* window) {
    GuardVoid([&] { StreamOf(stream)->removeWindow(WindowOf(window)); });
}

int uniapi_window_send_dom(uniapi_window* window, uniapi_dom_writer write, void* user) {
    return Guard(0, [&] { return SendDom(WindowOf(window), write, user); });
}

void uniapi_window_set_title(uniapi_window* window, const char* title) {
    GuardVoid([&] { WindowOf(window).setTitle(Str(title)); });
}

void uniapi_window_set_size(uniapi_window* window, int width, int height) {
    GuardVoid([&] { WindowOf(window).setSize(width, height); });
}

void uniapi_window_set_fullscreen(uniapi_window* window, int on) {
    GuardVoid([&] { WindowOf(window).setFullscreen(on != 0); });
}

int uniapi_window_fullscreen(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->fullscreen() ? 1 : 0; });
}

void uniapi_window_set_title_bar(uniapi_window* window, int mode) {
    GuardVoid([&] { WindowOf(window).setTitleBar(TitleBarOf(mode)); });
}

void uniapi_window_size(const uniapi_window* window, int* width, int* height) {
    const uniapi::Size size = Guard(uniapi::Size{}, [&] { return window ? WindowOf(window)->size() : uniapi::Size{}; });
    if (width) *width = size.width;
    if (height) *height = size.height;
}

int uniapi_window_position(const uniapi_window* window, int* x, int* y) {
    const std::optional<uniapi::Point> point = Guard(std::optional<uniapi::Point>(), [&] {
        return window ? WindowOf(window)->position() : std::optional<uniapi::Point>();
    });
    if (x) *x = point ? point->x : 0;
    if (y) *y = point ? point->y : 0;
    return point ? 1 : 0;
}

int uniapi_window_screens(const uniapi_window* window, uniapi_screen* out, int max) {
    return Guard(0, [&] {
        if (!window) return 0;
        const std::vector<uniapi::Screen> screens = WindowOf(window)->screens();
        for (int i = 0; out && i < max && i < int(screens.size()); ++i) {
            const uniapi::Screen& s = screens[size_t(i)];
            out[i] = {s.x, s.y, s.width, s.height, s.scale, s.primary ? 1 : 0, s.current ? 1 : 0};
        }
        return int(screens.size());
    });
}

void uniapi_window_set_position(uniapi_window* window, int x, int y) {
    GuardVoid([&] { WindowOf(window).setPosition(x, y); });
}

void uniapi_window_center(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).center(); });
}

void uniapi_window_minimize(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).minimize(); });
}

void uniapi_window_maximize(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).maximize(); });
}

void uniapi_window_restore(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).restore(); });
}

int uniapi_window_minimized(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->minimized() ? 1 : 0; });
}
int uniapi_window_maximized(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->maximized() ? 1 : 0; });
}

int uniapi_window_set_always_on_top(uniapi_window* window, int on) {
    return Guard(0, [&] { return WindowOf(window).setAlwaysOnTop(on != 0) ? 1 : 0; });
}

int uniapi_window_always_on_top(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->alwaysOnTop() ? 1 : 0; });
}

char* uniapi_window_placement(const uniapi_window* window) {
    return Guard<char*>(nullptr, [&]() -> char* {
        if (!window) return nullptr;
        const std::string text = WindowOf(window)->placement();
        return text.empty() ? nullptr : Dup(text);
    });
}

int uniapi_window_set_placement(uniapi_window* window, const char* placement) {
    return Guard(0, [&] {
        if (!WindowOf(window).setPlacement(Str(placement))) throw std::invalid_argument("not a window placement");
        return 1;
    });
}

void uniapi_window_on_placement_changed(uniapi_window* window, uniapi_callback handler, void* user) {
    GuardVoid([&] {
        WindowOf(window).onPlacementChanged = handler ? std::function<void()>([handler, user] { handler(user); }) : nullptr;
    });
}

void uniapi_window_show(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).show(); });
}

void uniapi_window_hide(uniapi_window* window) {
    GuardVoid([&] { WindowOf(window).hide(); });
}

int uniapi_window_visible(const uniapi_window* window) {
    return Guard(0, [&] { return window && WindowOf(window)->visible() ? 1 : 0; });
}

int uniapi_window_set_icon(uniapi_window* window, const unsigned char* png, size_t size) {
    return Guard(0, [&] { return WindowOf(window).setIcon(png, size) ? 1 : 0; });
}

}  // extern "C"

void uniapi::c_api::SetError(std::string message) { lastError = std::move(message); }
