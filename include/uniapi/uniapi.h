/*
 * Copyright (C) 2026 bouncyball
 * Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
 * SPDX-License-Identifier: Apache-2.0
 */

/* uniapi for C (C99 or later; also usable from other languages through their C bindings).
 *
 * The same library as uniapi.hpp, through plain functions:
 *   - objects are opaque handles (uniapi_app*, uniapi_stream*, ...);
 *   - callbacks are function pointers with a `void* user` you choose;
 *   - JSON travels as UTF-8 text ("[1, 2]"); the uniapi_json_* helpers read and write simple
 *     values, so small programs need no JSON library;
 *   - binary data from the page arrives in a uniapi_bytes list: in the JSON each value is
 *     {"$bytes": <index>}; to send binary data to the page, put uniapi_bytes_json() into JSON.
 *
 * Strings the library returns that you must free say so; free them with uniapi_free (or
 * uniapi_free_list for string lists). Functions that can fail return 0 or NULL and leave a
 * message in uniapi_last_error().
 *
 * Threads: the app belongs to the thread that made it (the UI thread), where every handler,
 * timer and posted function runs. Functions that only do something (uniapi_emit, uniapi_call,
 * uniapi_set_title, uniapi_show, uniapi_quit, uniapi_navigate, uniapi_expose, uniapi_on...) work
 * from any thread: they post themselves to the UI thread and happen a moment later. Functions
 * that answer something (uniapi_connected, uniapi_visible, uniapi_stream_acquire / _publish,
 * uniapi_open_window, uniapi_every, the dialogs...) fail off the UI thread: 0 or NULL, and
 * uniapi_last_error() names the function. Also safe from any thread: uniapi_post,
 * uniapi_notify, uniapi_reply_*, uniapi_bytes_json, the uniapi_json_* helpers, uniapi_free /
 * uniapi_free_list, uniapi_on_ui_thread and uniapi_last_error (which is per thread).
 *
 *   static void add(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
 *       double a = 0, b = 0;
 *       uniapi_json_get_number(args, "/0", &a);
 *       uniapi_json_get_number(args, "/1", &b);
 *       char result[64];
 *       snprintf(result, sizeof result, "%g", a + b);
 *       uniapi_reply_resolve(reply, result);
 *   }
 *
 *   UNIAPI_C_ASSETS(my_web);   // from uniapi_embed(myapp my_web web) in CMake
 *
 *   int main(void) {
 *       uniapi_options options;
 *       uniapi_options_init(&options);
 *       options.title = "My app";
 *       uniapi_app* app = uniapi_app_new(&options);
 *       uniapi_serve(app, uniapi_assets_my_web());
 *       uniapi_expose(app, "add", add, NULL);
 *       uniapi_navigate(app, "/index.html");
 *       int code = uniapi_run(app);
 *       uniapi_app_free(app);
 *       return code;
 *   }
 */
#ifndef UNIAPI_H
#define UNIAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uniapi_app uniapi_app;
typedef struct uniapi_window uniapi_window;  /* a window and its page: the main one or a further one */
typedef struct uniapi_reply uniapi_reply;    /* answers one call from JavaScript */
typedef struct uniapi_bytes uniapi_bytes;    /* binary values from the page in one message */
typedef struct uniapi_stream uniapi_stream;
typedef struct uniapi_sink uniapi_sink;      /* slots the page fills, C receives */
typedef struct uniapi_dom uniapi_dom;        /* writes one DOM command list */
typedef struct uniapi_assets uniapi_assets;  /* files compiled in by uniapi_embed() */

/* Declares the accessor uniapi_embed(<target> <name> <dir>) generates for C code. */
#define UNIAPI_C_ASSETS(name) const uniapi_assets* uniapi_assets_##name(void)

/* --- The app ----------------------------------------------------------------------------- */

/* The title bar and frame: the system's light or dark setting (following changes), or forced. */
typedef enum uniapi_title_bar_mode {
    UNIAPI_TITLE_BAR_SYSTEM = 0,
    UNIAPI_TITLE_BAR_DARK = 1,
    UNIAPI_TITLE_BAR_LIGHT = 2
} uniapi_title_bar_mode;

typedef struct uniapi_options {
    const char* title;          /* "uniapi" */
    int width, height;          /* the page's area in logical pixels: 1000 x 700 */
    int min_width, min_height;  /* 0: no minimum */
    int resizable;              /* 1 */
    int keep_aspect_ratio;      /* 0; 1: resizing keeps width:height (Windows, GTK 3 on X11) */
    int fullscreen;             /* 0 */
    int shared_memory;          /* 1: zero-copy streams where supported */
    int activate;               /* 1: take keyboard focus when the window opens */
    const char* host;           /* "uniapi.example" */
    const char* data_directory; /* NULL: a per-user folder named after the executable */
    /* Windows only: extra switches for the web engine (WebView2), space-separated, e.g.
     * "--proxy-server=host:8080". NULL: none. Ignored on Linux. See
     * AppOptions::browserArguments in uniapi.hpp. */
    const char* browser_arguments;
    int title_bar;              /* UNIAPI_TITLE_BAR_SYSTEM; or _DARK, _LIGHT (see uniapi_set_title_bar) */
    int show_when_ready;        /* 0: the window appears once its first page has loaded; 1: when
                                 * the page calls app.show() (or uniapi_show), 5 s at most
                                 * (on Linux counted from the page's load) */
    int defer_tasks_after_input; /* 0: C's messages reach the page at once after a click or key;
                                  * 1: Chromium's default, held until the next frame (Windows;
                                  * see AppOptions::deferTasksAfterInput) */
    int dmabuf_renderer;        /* 1; 0: Linux only, WebKit's DMA-BUF renderer off: the window
                                 * shows ~0.2 s sooner on some GPUs, but frames are copied:
                                 * WebGL slower, animation costs CPU (AppOptions::dmabufRenderer) */
    int start_hidden;           /* 0; 1: the window opens hidden, its page loading and running,
                                 * until uniapi_show (an app that starts in the background) */
    /* NULL: as many instances as are started. An id unique to the app ("org.example.mixer"):
     * a second launch hands its arguments to the running one (uniapi_on_second_instance) and
     * ends, in uniapi_app_new (exit(0)). See AppOptions::singleInstance. */
    const char* single_instance;
    /* NULL; a name: the window opens where and as it was left, saved under that name when it
     * closes (see AppOptions::rememberPlacement). */
    const char* remember_placement;
    int strict_calls;           /* 0; 1: C++'s typed exposed functions refuse extra arguments and
                                 * unknown fields (AppOptions::strictCalls; C handlers see the JSON) */
    int dev_tools;              /* 0; 1: the pages can be inspected with the engine's developer
                                 * tools (AppOptions::devTools; uniapi_window_open_dev_tools) */
    int max_width, max_height;  /* 0: no maximum (not on Linux with GTK 4; AppOptions::maxWidth) */
    const char* background_color; /* NULL; "#rrggbb" or "#rgb": the colour behind the page, before
                                   * it has drawn and at its edges (AppOptions::backgroundColor) */
    int context_menu;           /* 1; 0: the engine's right-click menu keeps only its text editing
                                 * items (cut, copy, paste...), none elsewhere
                                 * (AppOptions::contextMenu) */
} uniapi_options;

void uniapi_options_init(uniapi_options* options); /* fills in the defaults above */

uniapi_app* uniapi_app_new(const uniapi_options* options); /* NULL options: the defaults */
void uniapi_app_free(uniapi_app* app);
int uniapi_run(uniapi_app* app); /* shows the window; returns when it is closed */
int uniapi_on_ui_thread(const uniapi_app* app); /* 1 on the app's own thread; any thread */

/* The web engine the app's pages run in, for an About box or a bug report: "WebKitGTK" 2.50.1 on
 * Linux (the library loaded at run time), "WebView2" 131.0.2903.86 on Windows (the installed
 * runtime, so the Chromium version; "" until uniapi_run has started it), "WebKit" 621.1.15 on
 * macOS (the system's). variant: on Linux which WebKitGTK the program is built for, "API 6.0,
 * GTK 4" or "API 4.1, GTK 3"; "" elsewhere. Fills out and returns 1; 0 (out emptied) on
 * failure, e.g. off the UI thread. */
typedef struct uniapi_web_engine_info {
    char name[32];
    char version[64];
    char variant[64];
} uniapi_web_engine_info;
int uniapi_web_engine(const uniapi_app* app, uniapi_web_engine_info* out);
void uniapi_quit(uniapi_app* app);

void uniapi_serve(uniapi_app* app, const uniapi_assets* assets);
void uniapi_serve_directory(uniapi_app* app, const char* path);
void uniapi_navigate(uniapi_app* app, const char* path); /* e.g. "/index.html" */

/* --- JavaScript calls C ---------------------------------------------------------------------- */

/* args_json is the array of arguments; it is valid only while the handler runs (copy what you
 * need to answer later). Answer exactly once, now or later, from any thread, with
 * uniapi_reply_resolve or uniapi_reply_reject; that also frees the reply. `bytes` (binary
 * arguments) stays valid until then. */
typedef void (*uniapi_handler)(void* user, const char* args_json, const uniapi_bytes* bytes,
                               uniapi_reply* reply);
int uniapi_expose(uniapi_app* app, const char* name, uniapi_handler handler, void* user);
void uniapi_reply_resolve(uniapi_reply* reply, const char* value_json); /* NULL: null */
void uniapi_reply_reject(uniapi_reply* reply, const char* message);

/* --- Events -------------------------------------------------------------------------------------- */

/* `bytes` is valid while the handler runs. */
typedef void (*uniapi_event_handler)(void* user, const char* data_json, const uniapi_bytes* bytes);
int uniapi_on(uniapi_app* app, const char* event, uniapi_event_handler handler, void* user);
void uniapi_off(uniapi_app* app, const char* event);   /* stops handling it */
void uniapi_unexpose(uniapi_app* app, const char* name); /* no longer callable from the page */
int uniapi_emit(uniapi_app* app, const char* event, const char* data_json); /* NULL: null */

/* --- C calls JavaScript ------------------------------------------------------------------------ */

/* ok: value_or_error is the result as JSON; otherwise the error message. Runs exactly once, on
 * the UI thread; `bytes` (binary values in the result) is valid while it runs. */
typedef void (*uniapi_result_handler)(void* user, int ok, const char* value_or_error,
                                      const uniapi_bytes* bytes);
int uniapi_call(uniapi_app* app, const char* function, const char* args_json, /* NULL: [] */
                uniapi_result_handler done, void* user);                     /* done may be NULL */

/* --- Binary data ----------------------------------------------------------------------------- */

size_t uniapi_bytes_count(const uniapi_bytes* bytes);
/* The value {"$bytes": index} stands for; NULL if out of range. */
const unsigned char* uniapi_bytes_data(const uniapi_bytes* bytes, size_t index, size_t* size);
/* JSON to put where the page should get a Uint8Array with a copy of this data (in a result,
 * call arguments or event data). Free it. It works once, for the current page: send it soon
 * (within a minute) in one message. */
char* uniapi_bytes_json(const void* data, size_t size);

/* --- Connection -------------------------------------------------------------------------------- */

typedef void (*uniapi_page_handler)(void* user, const char* page);
void uniapi_on_connect(uniapi_app* app, uniapi_page_handler handler, void* user);
void uniapi_on_disconnect(uniapi_app* app, uniapi_page_handler handler, void* user);
int uniapi_connected(const uniapi_app* app);
int uniapi_shared_memory(const uniapi_app* app);

/* --- Window and event loop ------------------------------------------------------------------- */

typedef void (*uniapi_callback)(void* user);

/* Why the user asks a window to close (uniapi_on_close_requested). */
typedef enum uniapi_close_reason {
    UNIAPI_CLOSE_WINDOW = 0, /* its close button, Cmd+W, Alt+F4, the taskbar's or window manager's Close */
    UNIAPI_CLOSE_QUIT = 1    /* macOS: the app's Quit (Cmd+Q, the app menu, the Dock); asked of the main window */
} uniapi_close_reason;
/* Return 1 to let the window close, 0 to keep it open (to hide it instead, or to ask first and
 * close it once answered). */
typedef int (*uniapi_close_handler)(void* user, int reason);

void uniapi_set_title(uniapi_app* app, const char* title);
void uniapi_set_fullscreen(uniapi_app* app, int on);
void uniapi_set_size(uniapi_app* app, int width, int height); /* the page area, logical pixels */
int uniapi_fullscreen(const uniapi_app* app);
/* Title bar and frame: UNIAPI_TITLE_BAR_SYSTEM (follows the system), _DARK or _LIGHT. */
void uniapi_set_title_bar(uniapi_app* app, int mode);
int uniapi_title_bar(const uniapi_app* app); /* the mode set (not what the system shows) */
/* Brings the window to the front: shown again if hidden (uniapi_hide, start_hidden), restored
 * if minimised; a window kept hidden for its page (show_when_ready) appears. */
void uniapi_show(uniapi_app* app);
/* Takes the window off the screen without closing it: its page stays connected and runs. */
void uniapi_hide(uniapi_app* app);
int uniapi_visible(const uniapi_app* app); /* 0 once hidden or closed; minimised counts as visible */
/* The user asks the main window to close (see uniapi_close_handler). Not asked for uniapi_quit,
 * uniapi_window_close or the app ending. NULL: it closes. */
void uniapi_on_close_requested(uniapi_app* app, uniapi_close_handler handler, void* user);
int uniapi_set_icon(uniapi_app* app, const unsigned char* png, size_t size);
/* Calls fn once right away, then every `milliseconds` (at least 1). Returns the timer id, or 0
 * if none could be created (Windows allows 63 at once). */
int uniapi_every(uniapi_app* app, unsigned milliseconds, uniapi_callback fn, void* user);
void uniapi_cancel(uniapi_app* app, int timer);
void uniapi_post(uniapi_app* app, uniapi_callback fn, void* user); /* any thread */
/* Modal dialogs. Save: the path, or NULL if cancelled (free it). Open: a NULL-terminated list
 * of paths, or NULL if cancelled (uniapi_free_list). Patterns like "*.png;*.jpg". */
char* uniapi_save_file_dialog(uniapi_app* app, const char* title, const char* default_name,
                              const char* filter_name, const char* filter_pattern);
char** uniapi_open_file_dialog(uniapi_app* app, const char* title, const char* filter_name,
                               const char* filter_pattern, int multiple, int folder);

/* The same with all the choices: several file types, and the folder the dialog opens in. Zero
 * the struct, then set what you need (uniapi_dialog_options o = {0};). */
typedef struct uniapi_file_filter {
    const char* name;    /* e.g. "Images"; NULL or "": the pattern is shown */
    const char* pattern; /* e.g. "*.png;*.jpg"; "*" for every file; NULL or "": left out */
} uniapi_file_filter;

typedef struct uniapi_dialog_options {
    const char* title;                  /* NULL: "Save" or "Open" */
    const char* default_name;           /* save: the suggested file name */
    const uniapi_file_filter* filters;  /* the file types to choose from; the first is selected */
    int filter_count;
    const char* directory; /* the folder it opens in; NULL, or one that isn't there: the system's */
    int multiple;          /* open: let the user pick several */
    int folder;            /* open: pick folders instead of files (the filters are ignored) */
} uniapi_dialog_options;

char* uniapi_save_file_dialog_with(uniapi_app* app, const uniapi_dialog_options* options);
char** uniapi_open_file_dialog_with(uniapi_app* app, const uniapi_dialog_options* options);

/* --- The desktop -------------------------------------------------------------------------------- */

/* The page tried to leave the app's pages for url (a link, a form or window.open to another
 * site, mailto:...): the navigation is cancelled; return 1 to open url with the system, 0 not
 * to. No handler: it opens. */
typedef int (*uniapi_url_handler)(void* user, const char* url);
void uniapi_on_external_url(uniapi_app* app, uniapi_url_handler handler, void* user);
/* A later launch (single_instance): its arguments (without the program; `count` of them, the
 * list also NULL-terminated) and working folder. No handler: the main window is shown. */
typedef void (*uniapi_launch_handler)(void* user, const char* const* arguments, size_t count,
                                      const char* working_directory);
void uniapi_on_second_instance(uniapi_app* app, uniapi_launch_handler handler, void* user);
/* macOS: the files the system asks the app to open (a double-click in the Finder on a type the
 * bundle declares, uniapi_install's FILE_TYPES; Open With; a drop on the Dock icon; open -a):
 * their full paths (`count`, the list also NULL-terminated), valid while the handler runs. They
 * never come as the program's arguments there: launched by a double-click, the app gets them once
 * uniapi_run has started (maybe before the page has connected), later ones while it runs. No
 * handler: the main window is shown. Windows and Linux pass files as arguments (and to
 * uniapi_on_second_instance): never called there. See App::onOpenFiles. */
typedef void (*uniapi_paths_handler)(void* user, const char* const* paths, size_t count);
void uniapi_on_open_files(uniapi_app* app, uniapi_paths_handler handler, void* user);
/* Files dropped on a window: their paths (`count`, also NULL-terminated) and where, in the
 * page's CSS pixels. */
typedef void (*uniapi_drop_handler)(void* user, const char* const* paths, size_t count, int x, int y);
void uniapi_on_file_drop(uniapi_app* app, uniapi_drop_handler handler, void* user);
/* The clipboard's text: handler gets it, or NULL when it holds none; later, on the UI thread
 * (valid while it runs). Set: 0 if the system refused it (also before uniapi_run, except on
 * macOS). */
typedef void (*uniapi_text_handler)(void* user, const char* text);
void uniapi_clipboard_text(uniapi_app* app, uniapi_text_handler handler, void* user);
int uniapi_set_clipboard_text(uniapi_app* app, const char* text);
/* Open things with the system, from any thread: a URL with a scheme (the default browser, the
 * mail app...); a file with its default app, a folder in the file manager; the file manager
 * showing a file or folder, selected where it can. 1, or 0 and uniapi_last_error says why. */
int uniapi_open_url(const char* url);
int uniapi_open_path(const char* path);
int uniapi_reveal_path(const char* path);

/* --- Notifications ------------------------------------------------------------------------------ */

/* error NULL: the system has the notification (id is its id); otherwise why it couldn't. Runs on
 * the UI thread, after uniapi_notify has returned. */
typedef void (*uniapi_notified)(void* user, const char* id, const char* error);
/* Shows a system notification (see App::notify in uniapi.hpp: Linux's notification server, macOS's
 * Notification Center from an .app bundle, a balloon on Windows). id: NULL makes one, unique across
 * runs. Returns its id (free it). Safe from any thread; done may be NULL (a failure is printed). */
char* uniapi_notify(uniapi_app* app, const char* title, const char* body, const char* id,
                    uniapi_notified done, void* user);
/* A notification clicked: its id. No handler: the main window is shown. */
typedef void (*uniapi_id_handler)(void* user, const char* id);
void uniapi_on_notification_clicked(uniapi_app* app, uniapi_id_handler handler, void* user);

/* --- Streams ------------------------------------------------------------------------------------- */

typedef struct uniapi_slot {
    int index;
    unsigned char* data;
    size_t capacity;
} uniapi_slot;

/* Creates or replaces a stream; the handle is valid until the stream is replaced or the app
 * is freed. NULL on bad arguments. */
uniapi_stream* uniapi_stream_create(uniapi_app* app, const char* name, size_t slot_size, int slot_count);
int uniapi_stream_acquire(uniapi_stream* stream, uniapi_slot* slot);
int uniapi_stream_publish(uniapi_stream* stream, const uniapi_slot* slot, size_t size,
                          const char* meta_json); /* NULL: null */
void uniapi_stream_on_slot_freed(uniapi_stream* stream, uniapi_callback fn, void* user);
int uniapi_stream_shared(const uniapi_stream* stream);
uint64_t uniapi_stream_delivered(const uniapi_stream* stream);

/* --- Sinks: large data from the page ------------------------------------------------------------ */

/* A slot the page published (uniapi_sink_on_data): what it wrote, size bytes. The data stays valid,
 * and the page can't use the slot again, until uniapi_sink_release: keep a copy of this struct to
 * release it later (from another thread too). sink: internal (which sink; a replaced one's slots
 * are ignored). What the page wrote is untrusted: a size larger than the slot is refused, but the
 * bytes are whatever the page put there. */
typedef struct uniapi_sink_slot {
    int index;
    const unsigned char* data;
    size_t size;
    uint64_t sink;
} uniapi_sink_slot;
/* A slot the page published, on the UI thread, in the order the page published them; meta_json is
 * the page's meta as JSON text, and slot the slot (both valid during the call only). */
typedef void (*uniapi_sink_handler)(void* user, const uniapi_sink_slot* slot, const char* meta_json);

/* Creates or replaces a sink: slots the page fills (app.sink(name): acquire, write, publish) and C
 * receives (uniapi_sink_on_data). With shared memory C reads the very memory the page wrote, no
 * copy; without it (macOS, shared_memory 0) the page uploads each slot as it publishes it. See
 * App::sink in uniapi.hpp. The handle is valid until the sink is replaced or the app is freed;
 * replacing drops the page's slots of the old one. Its names share nothing with streams'. NULL on
 * bad arguments. */
uniapi_sink* uniapi_sink_create(uniapi_app* app, const char* name, size_t slot_size, int slot_count);
/* The handler for published slots. NULL: each slot goes back to the page at once. */
void uniapi_sink_on_data(uniapi_sink* sink, uniapi_sink_handler handler, void* user);
/* Hands a slot back to the page. Any thread, while the sink exists; a slot released already, or one
 * of a sink since replaced: nothing happens. */
void uniapi_sink_release(uniapi_sink* sink, const uniapi_sink_slot* slot);
int uniapi_sink_shared(const uniapi_sink* sink);        /* 1: the page writes into shared memory */
uint64_t uniapi_sink_received(const uniapi_sink* sink); /* slots the page has published so far */

/* --- DOM command lists ------------------------------------------------------------------------- */

#define UNIAPI_DOM_BODY 0u
typedef void (*uniapi_dom_writer)(void* user, uniapi_dom* dom);
/* Calls write to fill one list and sends it; 0 if no page or the previous lists are pending. */
int uniapi_send_dom(uniapi_app* app, uniapi_dom_writer write, void* user);
void uniapi_dom_bind(uniapi_dom* dom, uint32_t id, const char* selector);
void uniapi_dom_create(uniapi_dom* dom, uint32_t id, const char* tag);
void uniapi_dom_append(uniapi_dom* dom, uint32_t parent, uint32_t child);
void uniapi_dom_remove(uniapi_dom* dom, uint32_t id);
void uniapi_dom_clear(uniapi_dom* dom, uint32_t id);
void uniapi_dom_set_text(uniapi_dom* dom, uint32_t id, const char* text);
void uniapi_dom_set_attribute(uniapi_dom* dom, uint32_t id, const char* name, const char* value);
void uniapi_dom_set_style(uniapi_dom* dom, uint32_t id, const char* property, const char* value);
void uniapi_dom_toggle_class(uniapi_dom* dom, uint32_t id, const char* name, int on);
int uniapi_dom_overflowed(const uniapi_dom* dom);

/* --- More windows ------------------------------------------------------------------------------ */

/* A further window with its own page (see App::openWindow in uniapi.hpp). The uniapi_app
 * functions above act on the main window; uniapi_window_* act on any window, the main one too
 * (uniapi_main_window). A further window's page calls its window's functions, and the app's
 * where its window has none of that name; the same for events. Windows, Linux and macOS. */
typedef struct uniapi_window_options {
    const char* title;          /* "uniapi" */
    int width, height;          /* the page's area in logical pixels: 800 x 600 */
    int min_width, min_height;  /* 0: no minimum */
    int resizable;              /* 1 */
    int keep_aspect_ratio;      /* 0 */
    int title_bar;              /* UNIAPI_TITLE_BAR_SYSTEM */
    int show_when_ready;        /* 0 */
    int activate;               /* 1: take keyboard focus when it opens */
    int owned;                  /* 1: above the main window, minimised and closed with it, centred over it */
    int modal;                  /* 0; 1: the main window takes no input until it closes */
    int start_hidden;           /* 0; 1: opens hidden, its page running, until uniapi_window_show */
    const char* remember_placement; /* NULL; as uniapi_options' (a name of its own) */
    int max_width, max_height;      /* 0: no maximum, as uniapi_options' */
    const char* background_color;   /* NULL, as uniapi_options' */
} uniapi_window_options;

void uniapi_window_options_init(uniapi_window_options* options); /* fills in the defaults above */
/* Opens it (before uniapi_run: with the main window); navigate it to its page. The handle is
 * valid until its on_closed handler has returned (the window is destroyed then) or the app is
 * freed. NULL where the platform can't open more windows yet. */
uniapi_window* uniapi_open_window(uniapi_app* app, const uniapi_window_options* options);
uniapi_window* uniapi_main_window(uniapi_app* app);
/* As its close button, without asking its close handler; the main one: the app. */
void uniapi_window_close(uniapi_window* window);
/* The engine's developer tools on the window's page, with dev_tools on (Window::openDevTools);
 * the main window's: uniapi_window_open_dev_tools(uniapi_main_window(app)). */
void uniapi_window_open_dev_tools(uniapi_window* window);
void uniapi_window_on_closed(uniapi_window* window, uniapi_callback handler, void* user);
/* The user asks the window to close: as uniapi_on_close_requested. */
void uniapi_window_on_close_requested(uniapi_window* window, uniapi_close_handler handler, void* user);
/* Files dropped on this window: as uniapi_on_file_drop. */
void uniapi_window_on_file_drop(uniapi_window* window, uniapi_drop_handler handler, void* user);
void uniapi_window_navigate(uniapi_window* window, const char* path);
int uniapi_window_expose(uniapi_window* window, const char* name, uniapi_handler handler, void* user);
int uniapi_window_on(uniapi_window* window, const char* event, uniapi_event_handler handler, void* user);
void uniapi_window_off(uniapi_window* window, const char* event);
void uniapi_window_unexpose(uniapi_window* window, const char* name);
int uniapi_window_emit(uniapi_window* window, const char* event, const char* data_json);
int uniapi_window_call(uniapi_window* window, const char* function, const char* args_json,
                       uniapi_result_handler done, void* user);
void uniapi_window_on_connect(uniapi_window* window, uniapi_page_handler handler, void* user);
void uniapi_window_on_disconnect(uniapi_window* window, uniapi_page_handler handler, void* user);
int uniapi_window_connected(const uniapi_window* window);
int uniapi_window_shared_memory(const uniapi_window* window);
uniapi_stream* uniapi_window_stream_create(uniapi_window* window, const char* name, size_t slot_size, int slot_count);
uniapi_sink* uniapi_window_sink_create(uniapi_window* window, const char* name, size_t slot_size, int slot_count);
/* Sends a stream to another window's page too (one set of slots; a slot comes back once every
 * page it went to handed it back). 0 on error: that window has a stream of this name already. */
int uniapi_stream_add_window(uniapi_stream* stream, uniapi_window* window);
void uniapi_stream_remove_window(uniapi_stream* stream, uniapi_window* window);
/* Page to page: what a sink's page publishes reaches another window's page too, as a stream of the
 * same name, the very slots (no copy; with shared memory that page maps them read-only); a slot goes
 * back to the sink's page once C released it and every page it went to handed it back. See
 * Sink::addWindow in uniapi.hpp. 0 on error: the sink's own window, or that window has a stream of
 * this name already. */
int uniapi_sink_add_window(uniapi_sink* sink, uniapi_window* window);
void uniapi_sink_remove_window(uniapi_sink* sink, uniapi_window* window);
int uniapi_window_send_dom(uniapi_window* window, uniapi_dom_writer write, void* user);
void uniapi_window_set_title(uniapi_window* window, const char* title);
void uniapi_window_set_size(uniapi_window* window, int width, int height);
void uniapi_window_set_fullscreen(uniapi_window* window, int on);
int uniapi_window_fullscreen(const uniapi_window* window);
void uniapi_window_set_title_bar(uniapi_window* window, int mode);
/* Placement (see Window in uniapi.hpp): logical pixels from the primary screen's top-left
 * corner; a position is the frame's top-left corner, a size the page area's. The main window:
 * uniapi_main_window(app). Linux: no positions on Wayland or with GTK 4 (position returns 0,
 * set_position and center do nothing, always on top returns 0). */
void uniapi_window_size(const uniapi_window* window, int* width, int* height);
int uniapi_window_position(const uniapi_window* window, int* x, int* y); /* 0: not known here */
/* One of the desktop's screens: its whole area (logical pixels from the primary screen's
 * top-left corner), its scale, whether it is the primary one and whether the window is on it. */
typedef struct uniapi_screen {
    int x, y, width, height;
    double scale;
    int primary, current;
} uniapi_screen;
/* The desktop's screens, up to max of them into out: how many there are (Linux: 0 before
 * uniapi_run). */
int uniapi_window_screens(const uniapi_window* window, uniapi_screen* out, int max);
void uniapi_window_set_position(uniapi_window* window, int x, int y);
void uniapi_window_center(uniapi_window* window);
void uniapi_window_minimize(uniapi_window* window);
void uniapi_window_maximize(uniapi_window* window);
void uniapi_window_restore(uniapi_window* window);
int uniapi_window_minimized(const uniapi_window* window);
int uniapi_window_maximized(const uniapi_window* window);
int uniapi_window_set_always_on_top(uniapi_window* window, int on); /* 0 where the platform can't */
int uniapi_window_always_on_top(const uniapi_window* window);
/* Its placement as text to keep (free it; NULL if not known yet), and put back: 0 for text it
 * didn't make. */
char* uniapi_window_placement(const uniapi_window* window);
int uniapi_window_set_placement(uniapi_window* window, const char* placement);
/* Moved, resized, minimised, maximised or restored (often, while dragged). */
void uniapi_window_on_placement_changed(uniapi_window* window, uniapi_callback handler, void* user);
void uniapi_window_show(uniapi_window* window);  /* as uniapi_show */
void uniapi_window_hide(uniapi_window* window);  /* as uniapi_hide: only this window hides */
int uniapi_window_visible(const uniapi_window* window);
int uniapi_window_set_icon(uniapi_window* window, const unsigned char* png, size_t size);

/* --- JSON helpers (pointers like "/0" or "/user/name", RFC 6901) ------------------------------ */

char* uniapi_json_string(const char* text);                     /* "a\"b" for a"b; free it */
char* uniapi_json_get(const char* json, const char* pointer);   /* the value's JSON; free it */
char* uniapi_json_get_string(const char* json, const char* pointer); /* free it; NULL if not a string */
int uniapi_json_get_number(const char* json, const char* pointer, double* value);
int uniapi_json_get_bool(const char* json, const char* pointer, int* value);

/* --- Helpers --------------------------------------------------------------------------------------- */

char* uniapi_executable_path(void); /* free it */
char** uniapi_arguments(void);      /* without the program; NULL-terminated; uniapi_free_list */
void uniapi_alert(const char* title, const char* message);
void uniapi_free(void* memory);
void uniapi_free_list(char** list);
const char* uniapi_last_error(void); /* why the last failing call on this thread failed */

#ifdef __cplusplus
}
#endif

#endif /* UNIAPI_H */
