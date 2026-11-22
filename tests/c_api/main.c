/*
 * Copyright (C) 2026 bouncyball
 * Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
 * SPDX-License-Identifier: Apache-2.0
 */

/* The C API test: a C program (compiled as C) that drives a real page through uniapi.h and
 * exits with 0 when every check passes. The page half is web/index.html. It calls every C
 * function that can run unattended; the dialogs and uniapi_alert wait for a person, so the
 * checker app (tests/checker) covers those by hand. The behaviour behind these functions is
 * the C++ library's, which the C++ self-test checks in depth: this test checks the C layer
 * (arguments, results, memory, errors) and that each function reaches the right C++ one.
 *
 * It resizes the window and switches full screen on and off for a moment.
 *
 *   uniapi_test_c                     streams use shared memory where supported
 *   uniapi_test_c --no-shared-memory  streams use the fetch fallback
 *   uniapi_test_c --exit              close by itself when done (what ctest runs)
 *
 * Without --exit the window stays open and lists every check, grouped by step, like the C++
 * self-test does. With --exit it opens without taking keyboard focus.
 */
#include <uniapi/uniapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UNIAPI_C_ASSETS(c_api_web);

static uniapi_app* app;
static int passed, failed;
static int keep_open = 1; /* --exit clears it */
static int finished;      /* finish() ran: every step did */
static uniapi_stream* stream;
static int slots_freed;                 /* uniapi_stream_on_slot_freed calls */
static uniapi_sink* sink;               /* the page fills it (sinkFill), on_sink_data checks it */
static int sink_slots, sink_intact;     /* slots on_sink_data got, and how many held the pattern */
static int sink_meta_ok = 1;            /* each slot's meta named its seed and size */
static int sink_held_set;               /* the first slot, kept until the page is done */
static uniapi_sink_slot sink_held;
static int dom_overflow_big = -1;       /* uniapi_dom_overflowed after a too-big command */
static int dom_overflow_small = -1;     /* ...and after commands that fit */
static int connects, disconnects;       /* uniapi_on_connect / uniapi_on_disconnect calls */
static char disconnected_page[64];
static char* disk_file;                 /* written for uniapi_serve_directory; removed at the end */

/* A 4 x 4 green PNG for uniapi_set_icon. */
static const unsigned char icon_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00, 0x00, 0x00,
    0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41,
    0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00,
    0x12, 0x74, 0x00, 0x00, 0x12, 0x74, 0x01, 0xde, 0x66, 0x1f, 0x78, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41,
    0x54, 0x18, 0x57, 0x63, 0x90, 0xeb, 0xb1, 0xf9, 0x8f, 0x8c, 0x49, 0x16, 0xb0, 0xf9, 0x0f, 0x00, 0xf4, 0x79,
    0x1e, 0x51, 0x1b, 0x9f, 0x08, 0x78, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

/* The window sizes the "Window" step asks for (whole pixels at 100, 125, 150 and 200 %). */
#define TEST_WIDTH 640
#define TEST_HEIGHT 560

/* Every check as JSON, [{"step", "name", "detail", "ok"}, ...], for the results page. */
static char* records;
static size_t records_length;
static const char* current_step = "Before uniapi_run";

static void append(const char* text) {
    size_t length = strlen(text);
    char* grown = (char*)realloc(records, records_length + length + 1);
    if (!grown) return; /* out of memory: the page then misses this record */
    records = grown;
    memcpy(records + records_length, text, length + 1);
    records_length += length;
}

static void append_string(const char* text) {
    char* quoted = uniapi_json_string(text ? text : "");
    append(quoted ? quoted : "\"\"");
    uniapi_free(quoted);
}

static void begin_step(const char* name) {
    current_step = name;
    printf("%s\n", name);
    fflush(stdout);
}

static void check(const char* name, int ok, const char* detail) {
    if (ok) {
        ++passed;
        printf("  pass  %s\n", name);
    } else {
        ++failed;
        printf("  FAIL  %s%s%s\n", name, detail && *detail ? ": " : "", detail ? detail : "");
    }
    fflush(stdout);
    append(records_length > 1 ? ",{\"step\":" : "{\"step\":");
    append_string(current_step);
    append(",\"name\":");
    append_string(name);
    append(",\"detail\":");
    append_string(ok ? "" : detail);
    append(ok ? ",\"ok\":true}" : ",\"ok\":false}");
}

static unsigned char pattern_byte(int seed, size_t i) { return (unsigned char)((seed + i * 31) & 0xff); }

/* --- Functions the page calls --------------------------------------------------------------- */

static void add(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    double a = 0, b = 0;
    char text[64];
    (void)user;
    (void)bytes;
    if (!uniapi_json_get_number(args, "/0", &a) || !uniapi_json_get_number(args, "/1", &b)) {
        uniapi_reply_reject(reply, "add needs two numbers");
        return;
    }
    snprintf(text, sizeof text, "%.17g", a + b);
    uniapi_reply_resolve(reply, text);
}

static void greet(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    char* name = uniapi_json_get_string(args, "/0");
    char text[256];
    char* json;
    (void)user;
    (void)bytes;
    snprintf(text, sizeof text, "Hello, %s", name ? name : "?");
    json = uniapi_json_string(text);
    uniapi_reply_resolve(reply, json);
    uniapi_free(json);
    uniapi_free(name);
}

static void fail(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    (void)user;
    (void)args;
    (void)bytes;
    uniapi_reply_reject(reply, "c boom");
}

static void answer_later(void* reply) { uniapi_reply_resolve((uniapi_reply*)reply, "\"later\""); }

static void later(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    (void)user;
    (void)args;
    (void)bytes;
    uniapi_post(app, answer_later, reply); /* the reply outlives this handler */
}

static void bytes_info(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    size_t size = 0, i;
    const unsigned char* data = uniapi_bytes_data(bytes, 0, &size);
    unsigned long long sum = 0;
    char* placeholder = uniapi_json_get(args, "/0");
    char* quoted = uniapi_json_string(placeholder ? placeholder : "");
    char text[512];
    (void)user;
    for (i = 0; data && i < size; ++i) sum += (unsigned long long)data[i] * (i % 251 + 1);
    snprintf(text, sizeof text, "{\"count\":%u,\"size\":%u,\"sum\":%llu,\"placeholder\":%s}",
             (unsigned)uniapi_bytes_count(bytes), (unsigned)size, sum, quoted ? quoted : "null");
    uniapi_reply_resolve(reply, text);
    uniapi_free(quoted);
    uniapi_free(placeholder);
}

static void make_bytes(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    double size = 0, seed = 0;
    unsigned char data[1024];
    size_t i;
    char* json;
    (void)user;
    (void)bytes;
    uniapi_json_get_number(args, "/0", &size);
    uniapi_json_get_number(args, "/1", &seed);
    if (size < 0) size = 0;
    if (size > sizeof data) size = sizeof data;
    for (i = 0; i < (size_t)size; ++i) data[i] = pattern_byte((int)seed, i);
    json = uniapi_bytes_json(data, (size_t)size);
    uniapi_reply_resolve(reply, json);
    uniapi_free(json);
}

static void bad_json(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    (void)user;
    (void)args;
    (void)bytes;
    uniapi_reply_resolve(reply, "{not json");
}

/* --- The C side's checks, one after another ----------------------------------------------------- */

static void next(void);
static void finish(void);

/* A further window (step "A further window"): opened, its page connects, a call into it, then
 * closed by C. */
static uniapi_window* window;
static int window_closed;
static int window_close_asked; /* its close handler: never asked by uniapi_window_close */
static int main_close_asked;   /* the main window's: never asked by uniapi_quit */

static int on_window_close_requested(void* user, int reason) {
    (void)user;
    (void)reason;
    ++window_close_asked;
    return 0; /* kept open */
}

static int on_main_close_requested(void* user, int reason) {
    (void)user;
    (void)reason;
    ++main_close_asked;
    return 1;
}

static void continue_later(void* user) {
    (void)user;
    next();
}

static void on_window_closed(void* user) {
    (void)user;
    window_closed = 1;
    window = NULL; /* destroyed once this returns */
    check("uniapi_window_close closes it without asking its close handler: uniapi_window_on_closed",
          window_close_asked == 0, "");
    uniapi_post(app, continue_later, NULL); /* out of the window's close event */
}

static void on_window_add(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("uniapi_window_call reaches that window's page", ok && strcmp(value, "42") == 0, value);
    check("uniapi_main_window is the app's window", uniapi_window_connected(uniapi_main_window(app)) == uniapi_connected(app)
              && uniapi_main_window(app) != NULL, "");
    uniapi_window_close(window);
}

static void on_window_connect(void* user, const char* page) {
    (void)user;
    uniapi_stream* both;
    int hidden, shown, hidden_again, shown_again;
    check("a further window's page connects (uniapi_window_on_connect)",
          strcmp(page, "c-api") == 0 && uniapi_window_connected(window), page);
    hidden = uniapi_window_visible(window);
    uniapi_window_show(window);
    shown = uniapi_window_visible(window);
    uniapi_window_hide(window);
    hidden_again = uniapi_window_visible(window);
    uniapi_window_show(window);
    shown_again = uniapi_window_visible(window);
    check("start_hidden: it opens hidden, its page connected; uniapi_window_show, _hide and _visible",
          hidden == 0 && shown == 1 && hidden_again == 0 && shown_again == 1 && uniapi_window_connected(window), "");
    {
        int width = 0, height = 0;
        char* placement;
        uniapi_window_size(window, &width, &height);
        check("uniapi_window_size: the page area it opened with (320x200)", width > 0 && height > 0, "");
        placement = uniapi_window_placement(window);
        check("uniapi_window_placement gives text uniapi_window_set_placement takes back",
              placement && strstr(placement, "\"width\"") && uniapi_window_set_placement(window, placement) == 1,
              placement ? placement : uniapi_last_error());
        uniapi_free(placement);
        check("uniapi_window_set_placement refuses other text", uniapi_window_set_placement(window, "{\"x\":1}") == 0,
              uniapi_last_error());
        check("uniapi_window_minimized and _maximized: neither, as opened",
              uniapi_window_minimized(window) == 0 && uniapi_window_maximized(window) == 0, "");
    }
    both = uniapi_stream_create(app, "toBoth", 64, 1);
    check("uniapi_stream_add_window: an app's stream reaches the window too; a second of that name doesn't",
          both && uniapi_stream_add_window(both, window) == 1 && uniapi_window_stream_create(window, "toBoth", 64, 1) == NULL,
          uniapi_last_error());
    uniapi_stream_remove_window(both, window);
    uniapi_window_call(window, "jsAdd", "[20, 22]", on_window_add, NULL);
}
static int step;

/* "The desktop": the clipboard's text read, replaced, read back, put back; opening things refused
 * for bad input (nothing is opened). */
static char* clipboard_original;
static int clipboard_had_text;
static const char clipboard_probe[] = "uniapi C API test: clipboard \xc3\xa9 1";

static int is_wayland(void) {
#ifdef __linux__
    const char* display = getenv("WAYLAND_DISPLAY");
    const char* backend = getenv("GDK_BACKEND");
    return display && *display && !(backend && strncmp(backend, "x11", 3) == 0);
#else
    return 0;
#endif
}

static void on_clipboard_back(void* user, const char* text) {
    (void)user;
    if (text && strcmp(text, clipboard_probe) == 0) {
        check("uniapi_set_clipboard_text, then uniapi_clipboard_text: the same text", 1, "");
    } else if (is_wayland()) {
        /* Wayland may not take the clipboard from a window the person hasn't used. */
        printf("  n/a   uniapi_clipboard_text after uniapi_set_clipboard_text (Wayland)\n");
    } else {
        check("uniapi_set_clipboard_text, then uniapi_clipboard_text: the same text", 0, text ? text : "NULL");
    }
    if (clipboard_had_text) uniapi_set_clipboard_text(app, clipboard_original); /* put back */
    free(clipboard_original);
    clipboard_original = NULL;
    next();
}

static void on_empty_notification(void* user, const char* id, const char* error) {
    (void)user;
    check("uniapi_notify refuses a notification with neither title nor body (later, on the UI thread)",
          error != NULL && strstr(error, "title or a body") != NULL && id && strcmp(id, "c-empty") == 0, error ? error : "shown");
}

static void on_clipboard_original(void* user, const char* text) {
    (void)user;
    clipboard_had_text = text != NULL;
    if (text) {
        clipboard_original = (char*)malloc(strlen(text) + 1);
        if (clipboard_original) strcpy(clipboard_original, text);
        else clipboard_had_text = 0;
    }
    check("uniapi_set_clipboard_text takes text once the window exists",
          uniapi_set_clipboard_text(app, clipboard_probe) == 1, uniapi_last_error());
    uniapi_clipboard_text(app, on_clipboard_back, NULL);
}

static void on_page_results(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    char pointer[64];
    int i;
    (void)user;
    (void)bytes;
    if (!ok) {
        check("the page's checks ran", 0, value);
        next();
        return;
    }
    for (i = 0;; ++i) {
        char* name;
        char* detail;
        int passed_check = 0;
        snprintf(pointer, sizeof pointer, "/%d/name", i);
        name = uniapi_json_get_string(value, pointer);
        if (!name) break;
        snprintf(pointer, sizeof pointer, "/%d/ok", i);
        uniapi_json_get_bool(value, pointer, &passed_check);
        snprintf(pointer, sizeof pointer, "/%d/detail", i);
        detail = uniapi_json_get_string(value, pointer);
        check(name, passed_check, detail);
        uniapi_free(detail);
        uniapi_free(name);
    }
    next();
}

static void on_add(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("uniapi_call gets a JavaScript result", ok && strcmp(value, "5") == 0, value);
    next();
}

static void on_throws(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("uniapi_call gets a JavaScript error", !ok && strcmp(value, "js boom") == 0, value);
    next();
}

static void on_js_bytes(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    size_t size = 0;
    const unsigned char* data = uniapi_bytes_data(bytes, 0, &size);
    char* data_json = ok ? uniapi_json_get(value, "/data") : NULL;
    (void)user;
    check("binary data in a JavaScript result arrives in uniapi_bytes",
          ok && uniapi_bytes_count(bytes) == 1 && size == 3 && data && data[0] == 1 && data[2] == 3 &&
              data_json && strcmp(data_json, "{\"$bytes\":0}") == 0,
          value);
    uniapi_free(data_json);
    next();
}

static void on_bytes_info(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    double size = 0;
    int is_bytes = 0;
    (void)user;
    (void)bytes;
    check("uniapi_bytes_json in call arguments reaches the page as a Uint8Array",
          ok && uniapi_json_get_bool(value, "/isBytes", &is_bytes) && is_bytes &&
              uniapi_json_get_number(value, "/size", &size) && size == 4,
          value);
    next();
}

static void on_pong(void* user, const char* data, const uniapi_bytes* bytes) {
    double n = 0;
    (void)user;
    (void)bytes;
    check("uniapi_emit and uniapi_on carry events both ways", uniapi_json_get_number(data, "/got/n", &n) && n == 7, data);
    next();
}

static void on_frame_seen(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    int frame_ok = 0;
    (void)user;
    (void)bytes;
    check("a stream frame published from C arrives intact", ok && uniapi_json_get_bool(value, "/ok", &frame_ok) && frame_ok, value);
    next();
}

static void on_slot_freed(void* user) {
    (void)user;
    ++slots_freed;
}

static int sink_bytes_ok(const unsigned char* data, size_t size, int seed) {
    size_t i;
    for (i = 0; i < size; ++i) {
        if (data[i] != pattern_byte(seed, i)) return 0;
    }
    return 1;
}

/* The page published a sink slot: the pattern for its seed, size bytes. The first is held (the
 * page then has one slot left and must wait for it), the others go back at once. */
static void on_sink_data(void* user, const uniapi_sink_slot* slot, const char* meta) {
    double seed = -1, size = -1;
    (void)user;
    ++sink_slots;
    if (!uniapi_json_get_number(meta, "/seed", &seed) || !uniapi_json_get_number(meta, "/size", &size)
        || (size_t)size != slot->size) {
        sink_meta_ok = 0;
    }
    if (sink_bytes_ok(slot->data, slot->size, (int)seed)) ++sink_intact;
    if (!sink_held_set) {
        sink_held = *slot; /* kept: its data stays valid until released */
        sink_held_set = 1;
        return;
    }
    uniapi_sink_release(sink, slot);
}

static void on_sink_filled(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    double sent = 0;
    int shared = -1;
    char detail[160];
    (void)user;
    (void)bytes;
    snprintf(detail, sizeof detail, "page: %s; C got %d slots, %d intact", ok ? value : "failed", sink_slots, sink_intact);
    check("the page fills a sink (app.sink), C gets every slot in uniapi_sink_on_data, every byte as written",
          ok && uniapi_json_get_number(value, "/sent", &sent) && sent == 3 && sink_slots == 3 && sink_intact == 3, detail);
    check("each slot's meta reaches C as JSON text, its size the slot's", sink_meta_ok, "");
    check("a held slot's data stays as the page wrote it until uniapi_sink_release",
          sink_held_set && sink_bytes_ok(sink_held.data, sink_held.size, 40), "");
    uniapi_sink_release(sink, &sink_held);
    uniapi_sink_release(sink, &sink_held); /* released already: nothing happens */
    check("uniapi_sink_received counts them; uniapi_sink_shared matches the page's and uniapi_shared_memory",
          uniapi_sink_received(sink) == 3 && uniapi_json_get_bool(value, "/shared", &shared)
              && shared == uniapi_sink_shared(sink) && uniapi_sink_shared(sink) == uniapi_shared_memory(app),
          value);
    next();
}

/* Waits (up to 3 s) until both frames are delivered and a freed slot was reported. */
static void wait_for_frame(void* user) {
    static int timer, ticks;
    if (user == NULL) { /* first call: start polling */
        ticks = 0;
        timer = uniapi_every(app, 5, wait_for_frame, (void*)1);
        return;
    }
    if ((uniapi_stream_delivered(stream) < 2 || slots_freed < 1) && ++ticks < 600) return;
    uniapi_cancel(app, timer);
    check("uniapi_stream_on_slot_freed fires when a slot comes back after a failed acquire", slots_freed >= 1,
          "it never fired");
    uniapi_stream_on_slot_freed(stream, NULL, NULL); /* NULL: no callback any more */
    uniapi_call(app, "frameSeen", NULL, on_frame_seen, NULL);
}

static void write_dom(void* user, uniapi_dom* dom) {
    char* big = (char*)malloc(70000);
    (void)user;
    uniapi_dom_create(dom, 1, "p");
    uniapi_dom_set_attribute(dom, 1, "id", "from-c");
    uniapi_dom_set_attribute(dom, 1, "hidden", ""); /* only read by the check, not shown */
    uniapi_dom_set_text(dom, 1, "written from C");
    uniapi_dom_append(dom, UNIAPI_DOM_BODY, 1);
    /* A text bigger than a whole list (64 KB) is left out; the commands before it still go. */
    if (big) {
        memset(big, 'x', 69999);
        big[69999] = '\0';
        uniapi_dom_create(dom, 9, "p");
        uniapi_dom_set_text(dom, 9, big);
        free(big);
    }
    dom_overflow_big = uniapi_dom_overflowed(dom);
}

static void on_dom_text(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("a DOM command list written from C changes the page", ok && strcmp(value, "\"written from C\"") == 0, value);
    check("uniapi_dom_overflowed says a command didn't fit (and the rest still went)", dom_overflow_big == 1, "");
    next();
}

/* Edits elements the page already has (index.html, #dom-area). */
static void write_dom_edits(void* user, uniapi_dom* dom) {
    (void)user;
    uniapi_dom_bind(dom, 2, "#dom-style");
    uniapi_dom_set_style(dom, 2, "color", "rgb(1, 2, 3)");
    uniapi_dom_bind(dom, 3, "#dom-class");
    uniapi_dom_toggle_class(dom, 3, "on", 1);
    uniapi_dom_toggle_class(dom, 3, "off", 0);
    uniapi_dom_bind(dom, 4, "#dom-remove");
    uniapi_dom_remove(dom, 4);
    uniapi_dom_bind(dom, 5, "#dom-clear");
    uniapi_dom_clear(dom, 5);
    dom_overflow_small = uniapi_dom_overflowed(dom);
}

static void on_dom_edits(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    char* color = ok ? uniapi_json_get_string(value, "/color") : NULL;
    int on = 0, off = 1, removed = 0, cleared = 0;
    (void)user;
    (void)bytes;
    if (ok) {
        uniapi_json_get_bool(value, "/on", &on);
        uniapi_json_get_bool(value, "/off", &off);
        uniapi_json_get_bool(value, "/removed", &removed);
        uniapi_json_get_bool(value, "/cleared", &cleared);
    }
    check("uniapi_dom_bind finds an element, uniapi_dom_set_style styles it", color && strcmp(color, "rgb(1, 2, 3)") == 0, value);
    check("uniapi_dom_toggle_class adds and removes classes", on && !off, value);
    check("uniapi_dom_remove removes an element", removed, value);
    check("uniapi_dom_clear empties an element", cleared, value);
    check("uniapi_dom_overflowed is 0 when every command fits", dom_overflow_small == 0, "");
    uniapi_free(color);
    next();
}

static void on_disk_text(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("uniapi_serve_directory serves a file from disk", ok && strcmp(value, "\"from disk\"") == 0, value);
    next();
}

/* [innerWidth, innerHeight] from the page, after it waited for the size it was told. */
static int page_size_is(const char* value, int width, int height) {
    double w = 0, h = 0;
    return uniapi_json_get_number(value, "/0", &w) && uniapi_json_get_number(value, "/1", &h) && w == width &&
           h == height;
}

static void on_sized(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("uniapi_set_size gives the page that size", ok && page_size_is(value, TEST_WIDTH, TEST_HEIGHT), value);
    next();
}

static void on_full_screen(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    int full = 0;
    (void)user;
    (void)bytes;
    check("...and the page fills the screen", ok && uniapi_json_get_bool(value, "/full", &full) && full, value);
    next();
}

static void on_left_full_screen(void* user, int ok, const char* value, const uniapi_bytes* bytes) {
    (void)user;
    (void)bytes;
    check("...and the window gets its size back", ok && page_size_is(value, TEST_WIDTH, TEST_HEIGHT), value);
    uniapi_set_size(app, 720, 820); /* the size the results are shown at */
    next();
}

static void next(void) {
    switch (step++) {
    case 0:
        begin_step("The page calls C");
        uniapi_call(app, "runTests", NULL, on_page_results, NULL);
        break;
    case 1:
        begin_step("C calls the page");
        uniapi_call(app, "jsAdd", "[2, 3]", on_add, NULL);
        break;
    case 2:
        uniapi_call(app, "jsThrows", NULL, on_throws, NULL);
        break;
    case 3:
        uniapi_call(app, "jsBytes", NULL, on_js_bytes, NULL);
        break;
    case 4: {
        const unsigned char four[4] = {1, 2, 3, 4};
        char* placeholder = uniapi_bytes_json(four, sizeof four);
        char args[256];
        snprintf(args, sizeof args, "[%s]", placeholder);
        uniapi_call(app, "jsBytesInfo", args, on_bytes_info, NULL);
        uniapi_free(placeholder);
        break;
    }
    case 5:
        begin_step("Events, streams, DOM");
        uniapi_emit(app, "ping", "{\"n\": 7}");
        break; /* on_pong continues */
    case 6: {
        /* Both slots go out at once (the page can't give one back while this runs), so a third
         * acquire fails; uniapi_stream_on_slot_freed then says when one is free again. */
        uniapi_slot slot;
        size_t i;
        int ok = 1, frames;
        for (frames = 0; ok && frames < 2; ++frames) {
            ok = uniapi_stream_acquire(stream, &slot);
            if (ok) {
                for (i = 0; i < 100; ++i) slot.data[i] = pattern_byte(5, i);
                ok = uniapi_stream_publish(stream, &slot, 100, "{\"seed\": 5, \"size\": 100}");
            }
        }
        check("uniapi_stream_acquire and uniapi_stream_publish", ok, uniapi_last_error());
        uniapi_stream_on_slot_freed(stream, on_slot_freed, NULL);
        check("uniapi_stream_acquire fails while every slot is with the page", !uniapi_stream_acquire(stream, &slot), "");
        check("uniapi_stream_shared matches uniapi_shared_memory", uniapi_stream_shared(stream) == uniapi_shared_memory(app), "");
        wait_for_frame(NULL);
        break;
    }
    case 7:
        check("uniapi_send_dom sends a list", uniapi_send_dom(app, write_dom, NULL), uniapi_last_error());
        uniapi_call(app, "domText", NULL, on_dom_text, NULL);
        break;
    case 8:
        check("uniapi_send_dom sends a list of edits", uniapi_send_dom(app, write_dom_edits, NULL), uniapi_last_error());
        uniapi_call(app, "domEdits", NULL, on_dom_edits, NULL);
        break;
    case 9:
        begin_step("A sink: the page to C");
        uniapi_call(app, "sinkFill", "[\"c-sink\", 3, 40, [200, 256, 17]]", on_sink_filled, NULL);
        break;
    case 10:
        begin_step("Files and window");
        uniapi_call(app, "diskText", NULL, on_disk_text, NULL);
        break;
    case 11: {
        char args[64];
        uniapi_set_size(app, TEST_WIDTH, TEST_HEIGHT);
        snprintf(args, sizeof args, "[%d, %d]", TEST_WIDTH, TEST_HEIGHT);
        uniapi_call(app, "waitForSize", args, on_sized, NULL);
        break;
    }
    case 12:
        check("the title bar follows the system by default", uniapi_title_bar(app) == UNIAPI_TITLE_BAR_SYSTEM, "");
        check("uniapi_visible is 1 for the shown main window", uniapi_visible(app) == 1, "");
        check("uniapi_on_ui_thread is 1 in a handler (the app's own thread)", uniapi_on_ui_thread(app) == 1, "");
        {
            uniapi_screen screens[8];
            const int count = uniapi_window_screens(uniapi_main_window(app), screens, 8);
            int current = 0, i;
            for (i = 0; i < count && i < 8; ++i) current += screens[i].current && screens[i].width > 0 && screens[i].scale >= 1;
            check("uniapi_window_screens: the screens, the window on one of them", count > 0 && current == 1, "");
        }
        /* dev_tools is off here: nothing opens, and nothing fails (no new error). */
        {
            char before[256];
            snprintf(before, sizeof before, "%s", uniapi_last_error());
            uniapi_window_open_dev_tools(uniapi_main_window(app));
            check("uniapi_window_open_dev_tools with dev_tools off: nothing happens, no error",
                  strcmp(before, uniapi_last_error()) == 0, uniapi_last_error());
        }
        {
            uniapi_web_engine_info engine;
            const int got = uniapi_web_engine(app, &engine);
            const int numbers = engine.version[0] != '\0' && strspn(engine.version, "0123456789.") == strlen(engine.version)
                                && strchr(engine.version, '.') != NULL;
            check("uniapi_web_engine: a name and a version of numbers", got == 1 && engine.name[0] != '\0' && numbers,
                  engine.version);
        }
        uniapi_show(app);  /* already shown: in front again (no focus taken under --exit), no harm */
        uniapi_set_title_bar(app, UNIAPI_TITLE_BAR_DARK);
        {
            const int dark = uniapi_title_bar(app);
            uniapi_set_title_bar(app, UNIAPI_TITLE_BAR_LIGHT);
            const int light = uniapi_title_bar(app);
            uniapi_set_title_bar(app, UNIAPI_TITLE_BAR_SYSTEM);
            check("uniapi_set_title_bar: dark, light, back to the system's",
                  dark == UNIAPI_TITLE_BAR_DARK && light == UNIAPI_TITLE_BAR_LIGHT
                      && uniapi_title_bar(app) == UNIAPI_TITLE_BAR_SYSTEM, "");
        }
        check("uniapi_fullscreen is 0 in a normal window", uniapi_fullscreen(app) == 0, "");
        uniapi_set_fullscreen(app, 1);
        check("uniapi_set_fullscreen(1): uniapi_fullscreen says 1", uniapi_fullscreen(app) == 1, "");
        uniapi_call(app, "waitForFullScreen", "[true]", on_full_screen, NULL);
        break;
    case 13: {
        char args[64];
        uniapi_set_fullscreen(app, 0);
        check("uniapi_set_fullscreen(0): uniapi_fullscreen says 0", uniapi_fullscreen(app) == 0, "");
        snprintf(args, sizeof args, "[%d, %d]", TEST_WIDTH, TEST_HEIGHT);
        uniapi_call(app, "waitForSize", args, on_left_full_screen, NULL);
        break;
    }
    case 14:
        begin_step("Reloading the page");
        disconnects = 0;
        uniapi_navigate(app, "/index.html?reloaded=1");
        break; /* on_disconnect, then on_connect continues */
    case 15:
        begin_step("The desktop");
        check("uniapi_open_url refuses text that isn't a URL with a scheme",
              uniapi_open_url("not a url") == 0 && strstr(uniapi_last_error(), "scheme") != NULL, uniapi_last_error());
        check("uniapi_open_path refuses a path that isn't there",
              uniapi_open_path("no such folder/no such file.txt") == 0 && strstr(uniapi_last_error(), "no such") != NULL,
              uniapi_last_error());
        check("uniapi_reveal_path refuses an empty path", uniapi_reveal_path("") == 0, uniapi_last_error());
        {
            char* id = uniapi_notify(app, "", "", "c-empty", on_empty_notification, NULL);
            check("uniapi_notify returns the notification's id", id && strcmp(id, "c-empty") == 0, id ? id : "NULL");
            uniapi_free(id);
        }
        uniapi_clipboard_text(app, on_clipboard_original, NULL);
        break; /* on_clipboard_original, on_clipboard_back continue */
    case 16: {
        uniapi_window_options options;
        begin_step("A further window");
        uniapi_window_options_init(&options);
        options.title = "uniapi C API test: a further window";
        options.width = 320;
        options.height = 200;
        options.activate = keep_open;
        check("uniapi_window_options_init: owned, not modal, not hidden",
              options.owned == 1 && options.modal == 0 && options.start_hidden == 0, "");
        options.start_hidden = 1;
        window = uniapi_open_window(app, &options);
        check("uniapi_open_window opens one", window != NULL, uniapi_last_error());
        if (!window) {
            next();
            break;
        }
        uniapi_window_on_connect(window, on_window_connect, NULL);
        uniapi_window_on_closed(window, on_window_closed, NULL);
        uniapi_window_on_close_requested(window, on_window_close_requested, NULL);
        uniapi_window_navigate(window, "/index.html?window=1");
        break; /* on_window_connect continues */
    }
    default:
        finish();
        break;
    }
}

/* Prints the summary and puts it in the title; then closes (--exit) or shows every check. */
static void finish(void) {
    char summary[128];
    char title[192];
    char* quoted;
    finished = 1;
    snprintf(summary, sizeof summary, "%s: %d passed, %d failed",
             uniapi_shared_memory(app) ? "shared memory" : "fetch fallback", passed, failed);
    printf("\nC API, %s\n", summary);
    snprintf(title, sizeof title, "uniapi C API test - %s", summary);
    uniapi_set_title(app, title);
    if (!keep_open) {
        fflush(stdout);
        uniapi_quit(app);
        return;
    }
    printf("Close the window to exit.\n");
    fflush(stdout);
    append("]"); /* closes the records array opened in main() */
    /* showResults(summary, ok, checks) */
    quoted = uniapi_json_string(summary);
    {
        size_t size = strlen(quoted ? quoted : "\"\"") + records_length + 32;
        char* args = (char*)malloc(size);
        if (args) {
            snprintf(args, size, "[%s,%s,%s]", quoted ? quoted : "\"\"", failed == 0 ? "true" : "false",
                     records ? records : "[]");
            uniapi_call(app, "showResults", args, NULL, NULL);
            free(args);
        }
    }
    uniapi_free(quoted);
}

static void on_connect(void* user, const char* page) {
    char detail[160];
    (void)user;
    if (++connects == 1) {
        begin_step("Connecting");
        check("the page connects", strcmp(page, "c-api") == 0, page);
        check("uniapi_connected is 1 once it has", uniapi_connected(app) == 1, "");
        next();
        return;
    }
    /* After the reload ("Reloading the page" in next()). */
    snprintf(detail, sizeof detail, "fired %d times, for page \"%s\"", disconnects, disconnected_page);
    check("uniapi_on_disconnect fires once when the page goes away",
          disconnects == 1 && strcmp(disconnected_page, "c-api") == 0, detail);
    check("the reloaded page connects again", strcmp(page, "c-api") == 0 && uniapi_connected(app) == 1, page);
    next();
}

static void on_disconnect(void* user, const char* page) {
    (void)user;
    ++disconnects;
    snprintf(disconnected_page, sizeof disconnected_page, "%s", page ? page : "");
}

/* Writes a file next to the program and serves that folder with uniapi_serve_directory (the
 * page's own files still come from the compiled-in assets, which go first). */
static void serve_from_disk(void) {
    char* path = uniapi_executable_path();
    char* slash;
    char* folder_end;
    FILE* file;
    if (!path) return;
    slash = strrchr(path, '/');
    folder_end = strrchr(path, '\\');
    if (folder_end == NULL || (slash && slash > folder_end)) folder_end = slash;
    if (!folder_end) {
        uniapi_free(path);
        return;
    }
    *folder_end = '\0';
    disk_file = (char*)malloc(strlen(path) + 32);
    if (disk_file) {
        sprintf(disk_file, "%s/c_api_disk.txt", path);
        file = fopen(disk_file, "wb");
        if (file) {
            fputs("from disk", file);
            fclose(file);
        }
    }
    uniapi_serve_directory(app, path);
    uniapi_free(path);
}

/* Checks that need no page. */
static void checks_before_run(void) {
    char* text;
    double number = 0;
    int flag = 0;
    char** list;
    begin_step("Before uniapi_run");
    text = uniapi_json_string("a\"b\\c\n\xc3\xa9");
    check("uniapi_json_string quotes text", text && strcmp(text, "\"a\\\"b\\\\c\\n\xc3\xa9\"") == 0, text);
    uniapi_free(text);
    check("uniapi_json_get_number", uniapi_json_get_number("{\"a\": [1, 2.5]}", "/a/1", &number) && number == 2.5, "");
    check("uniapi_json_get_bool", uniapi_json_get_bool("[true]", "/0", &flag) && flag == 1, "");
    text = uniapi_json_get_string("{\"s\": \"x\"}", "/s");
    check("uniapi_json_get_string", text && strcmp(text, "x") == 0, text);
    uniapi_free(text);
    check("uniapi_json_get_string refuses a number", uniapi_json_get_string("[1]", "/0") == NULL, "");
    check("uniapi_last_error says why", strstr(uniapi_last_error(), "no string") != NULL, uniapi_last_error());
    text = uniapi_json_get("{\"o\": {\"k\": [1]}}", "/o");
    check("uniapi_json_get returns a value's JSON", text && strcmp(text, "{\"k\":[1]}") == 0, text);
    uniapi_free(text);
    check("uniapi_call with invalid JSON fails", !uniapi_call(app, "x", "[oops", NULL, NULL), "");
    check("uniapi_stream_create refuses a bad name", uniapi_stream_create(app, "bad/name", 16, 1) == NULL, "");
    text = uniapi_executable_path();
    check("uniapi_executable_path", text && strlen(text) > 0, text);
    uniapi_free(text);
    list = uniapi_arguments();
    check("uniapi_arguments", list != NULL, "");
    uniapi_free_list(list);
    check("uniapi_connected is 0 before uniapi_run", uniapi_connected(app) == 0, "");
    {
        uniapi_options defaults;
        uniapi_options_init(&defaults);
        check("uniapi_options_init: Chromium's deferral after input off (defer_tasks_after_input 0)",
              defaults.defer_tasks_after_input == 0, "");
        check("uniapi_options_init: the window opens shown (start_hidden 0)", defaults.start_hidden == 0, "");
        check("uniapi_options_init: as many instances as are started (single_instance NULL)",
              defaults.single_instance == NULL, "");
        check("uniapi_options_init: calls not checked strictly (strict_calls 0)", defaults.strict_calls == 0, "");
        check("uniapi_options_init: the developer tools off (dev_tools 0)", defaults.dev_tools == 0, "");
        check("uniapi_options_init: the engine's right-click menu in full (context_menu 1)", defaults.context_menu == 1, "");
        check("uniapi_options_init: no maximum size, the system's background (max_width/max_height 0, background_color NULL)",
              defaults.max_width == 0 && defaults.max_height == 0 && defaults.background_color == NULL, "");
    }
    check("uniapi_set_icon refuses bytes that aren't a PNG", uniapi_set_icon(app, (const unsigned char*)"not a png", 9) == 0, "");
#if defined(_WIN32) || defined(__APPLE__)
    /* macOS: the app's icon in the Dock. */
    check("uniapi_set_icon takes a PNG", uniapi_set_icon(app, icon_png, sizeof icon_png) == 1, uniapi_last_error());
#else
    /* GTK 4 takes window icons only from the app's .desktop file: there it returns 0 by design. */
    uniapi_set_icon(app, icon_png, sizeof icon_png);
#endif
}

int main(void) {
    uniapi_options options;
    int shared = 1;
    char** arguments = uniapi_arguments(); /* UTF-8, unlike argv on Windows */
    char** argument;
    for (argument = arguments; argument && *argument; ++argument) {
        if (strcmp(*argument, "--no-shared-memory") == 0) {
            shared = 0;
        } else if (strcmp(*argument, "--exit") == 0) {
            keep_open = 0;
        } else {
            printf("unknown option %s\nusage: uniapi_test_c [--no-shared-memory] [--exit]\n", *argument);
            uniapi_free_list(arguments);
            return 2;
        }
    }
    uniapi_free_list(arguments);
    append("[");

    uniapi_options_init(&options);
    options.title = "uniapi C API test";
    options.width = 720;
    options.height = 820;
    options.shared_memory = shared;
    options.activate = keep_open; /* run by ctest (--exit): don't take keyboard focus */
    app = uniapi_app_new(&options);
    if (!app) {
        printf("uniapi_app_new failed: %s\n", uniapi_last_error());
        return 1;
    }
    uniapi_serve(app, uniapi_assets_c_api_web());
    uniapi_expose(app, "add", add, NULL);
    uniapi_expose(app, "greet", greet, NULL);
    uniapi_expose(app, "fail", fail, NULL);
    uniapi_expose(app, "later", later, NULL);
    uniapi_expose(app, "bytesInfo", bytes_info, NULL);
    uniapi_expose(app, "makeBytes", make_bytes, NULL);
    uniapi_expose(app, "badJson", bad_json, NULL);
    uniapi_on(app, "pong", on_pong, NULL);
    uniapi_on_connect(app, on_connect, NULL);
    uniapi_on_disconnect(app, on_disconnect, NULL);
    uniapi_on_close_requested(app, on_main_close_requested, NULL);
    stream = uniapi_stream_create(app, "c-stream", 256, 2);
    sink = uniapi_sink_create(app, "c-sink", 256, 2);
    uniapi_sink_on_data(sink, on_sink_data, NULL);
    serve_from_disk();
    checks_before_run();

    uniapi_navigate(app, "/index.html");
    uniapi_run(app);
    if (!keep_open) {
        /* Ended by uniapi_quit (finish), which doesn't ask. */
        check("uniapi_quit ends the app without asking its close handler", main_close_asked == 0, "");
        printf("C API, after the app ended: %d passed, %d failed\n", passed, failed);
    }
    uniapi_app_free(app);
    if (disk_file) remove(disk_file);
    free(disk_file);
    free(records);
    return failed == 0 && finished ? 0 : 1;
}
