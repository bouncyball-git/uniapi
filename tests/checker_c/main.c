/*
 * Copyright (C) 2026 bouncyball
 * Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
 * SPDX-License-Identifier: Apache-2.0
 */

/* uniapi C checker: the C API's checks that need a person at the screen, one button each, like
 * the C++ checker (tests/checker) but through uniapi.h. Each result is shown on the page. Not
 * run by ctest; the C API test (tests/c_api) covers every other C function unattended.
 *
 *   uniapi_checker_c                 alert, Open and Save dialogs, icon, full screen
 *   uniapi_checker_c --dialog-first  an alert before uniapi_run first: on Linux the app must
 *                                    then start with shared memory off (and not crash)
 */
#include <uniapi/uniapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

UNIAPI_C_ASSETS(checker_c_web);

static uniapi_app* app;

/* A dialog waiting to be opened from uniapi_post: dialogs run their own event loop, so they
 * open outside the web view's event that brought the call. */
typedef struct pending {
    uniapi_reply* reply;
    int multiple, folder;
} pending;

static pending* make_pending(uniapi_reply* reply, int multiple, int folder) {
    pending* p = (pending*)malloc(sizeof *p);
    if (!p) {
        uniapi_reply_reject(reply, "out of memory");
        return NULL;
    }
    p->reply = reply;
    p->multiple = multiple;
    p->folder = folder;
    return p;
}

/* A NULL-terminated list of paths as a JSON array of strings (free it); NULL list: []. */
static char* paths_json(char** paths) {
    size_t size = 3, used = 0;
    char** path;
    char* json;
    for (path = paths; path && *path; ++path) size += strlen(*path) * 6 + 4; /* worst-case escaping */
    json = (char*)malloc(size);
    if (!json) return NULL;
    json[used++] = '[';
    for (path = paths; path && *path; ++path) {
        char* quoted = uniapi_json_string(*path);
        size_t length = quoted ? strlen(quoted) : 0;
        if (path != paths) json[used++] = ',';
        if (quoted) memcpy(json + used, quoted, length);
        used += length;
        uniapi_free(quoted);
    }
    json[used++] = ']';
    json[used] = '\0';
    return json;
}

/* The system's temporary folder, where the dialogs open. */
static const char* temp_folder(void) {
    const char* names[] = {"TMPDIR", "TEMP", "TMP"};
    for (int i = 0; i < 3; ++i) {
        const char* value = getenv(names[i]);
        if (value && *value) return value;
    }
    return "/tmp";
}

/* Opens in the temporary folder, with three file types to choose from (the icon picker below
 * uses the short uniapi_open_file_dialog). */
static void run_open(void* user) {
    pending* p = (pending*)user;
    const uniapi_file_filter filters[] = {
        {"Text or images", "*.txt;*.png;*.jpg"}, {"C files", "*.c;*.h"}, {"All files", "*"}};
    uniapi_dialog_options o = {0};
    o.title = p->folder ? "Pick folder(s)" : "Pick file(s)";
    o.filters = filters;
    o.filter_count = 3;
    o.directory = temp_folder();
    o.multiple = p->multiple;
    o.folder = p->folder;
    char** picked = uniapi_open_file_dialog_with(app, &o);
    char* json = paths_json(picked); /* cancelled: [] */
    uniapi_reply_resolve(p->reply, json ? json : "[]");
    free(json); /* from malloc in paths_json, not from the library */
    uniapi_free_list(picked);
    free(p);
}

static void open_with(uniapi_reply* reply, int multiple, int folder) {
    pending* p = make_pending(reply, multiple, folder);
    if (p) uniapi_post(app, run_open, p);
}

static void open_one(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) { (void)u; (void)a; (void)b; open_with(r, 0, 0); }
static void open_several(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) { (void)u; (void)a; (void)b; open_with(r, 1, 0); }
static void open_folder(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) { (void)u; (void)a; (void)b; open_with(r, 0, 1); }
static void open_folders(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) { (void)u; (void)a; (void)b; open_with(r, 1, 1); }

static void run_save(void* user) {
    pending* p = (pending*)user;
    const uniapi_file_filter filters[] = {{"CSV file", "*.csv"}, {"Text file", "*.txt"}};
    uniapi_dialog_options o = {0};
    o.title = "Save the results";
    o.default_name = "results.csv";
    o.filters = filters;
    o.filter_count = 2;
    o.directory = temp_folder();
    char* path = uniapi_save_file_dialog_with(app, &o);
    char* json = path ? uniapi_json_string(path) : NULL;
    uniapi_reply_resolve(p->reply, json); /* cancelled: null */
    uniapi_free(json);
    uniapi_free(path);
    free(p);
}

static void save(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) {
    pending* p = make_pending(r, 0, 0);
    (void)u;
    (void)a;
    (void)b;
    if (p) uniapi_post(app, run_save, p);
}

static void run_alert(void* user) {
    pending* p = (pending*)user;
    uniapi_alert("uniapi C checker", "uniapi_alert from C. Close it: the page then says \"closed\".");
    uniapi_reply_resolve(p->reply, "\"closed\"");
    free(p);
}

static void alert(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) {
    pending* p = make_pending(r, 0, 0);
    (void)u;
    (void)a;
    (void)b;
    if (p) uniapi_post(app, run_alert, p);
}

static void toggle_fullscreen(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) {
    (void)u;
    (void)a;
    (void)b;
    uniapi_set_fullscreen(app, !uniapi_fullscreen(app));
    uniapi_reply_resolve(r, uniapi_fullscreen(app) ? "true" : "false");
}

/* The title bar: the system's light or dark, then forced dark, then forced light. */
static void cycle_title_bar(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) {
    const int next = (uniapi_title_bar(app) + 1) % 3;
    (void)u;
    (void)a;
    (void)b;
    uniapi_set_title_bar(app, next);
    uniapi_reply_resolve(r, next == UNIAPI_TITLE_BAR_DARK ? "\"dark\"" : next == UNIAPI_TITLE_BAR_LIGHT ? "\"light\"" : "\"system\"");
}

/* Opens a file by its UTF-8 path (on Windows, through UTF-16, so any path works). */
static FILE* open_utf8(const char* path) {
#ifdef _WIN32
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, (int)(sizeof wide / sizeof wide[0]))) return NULL;
    return _wfopen(wide, L"rb");
#else
    return fopen(path, "rb");
#endif
}

/* Picks a PNG with the Open dialog (filtered to *.png) and makes it the window icon. */
static void run_pick_icon(void* user) {
    pending* p = (pending*)user;
    char** picked = uniapi_open_file_dialog(app, "Pick a PNG for the window icon", "PNG images", "*.png", 0, 0);
    unsigned char* png = NULL;
    size_t size = 0;
    int set = 0;
    char* quoted;
    char* answer;
    FILE* file;
    if (!picked || !picked[0]) {
        uniapi_reply_resolve(p->reply, "{\"cancelled\":true}");
        uniapi_free_list(picked);
        free(p);
        return;
    }
    file = open_utf8(picked[0]);
    if (file) {
        fseek(file, 0, SEEK_END);
        size = (size_t)ftell(file);
        fseek(file, 0, SEEK_SET);
        png = (unsigned char*)malloc(size ? size : 1);
        if (png && fread(png, 1, size, file) != size) size = 0;
        fclose(file);
    }
    if (png && size) set = uniapi_set_icon(app, png, size);
    quoted = uniapi_json_string(picked[0]);
    answer = (char*)malloc(strlen(quoted ? quoted : "\"\"") + 96);
    if (answer) {
        sprintf(answer, "{\"path\":%s,\"bytes\":%lu,\"setIcon\":%s}", quoted ? quoted : "\"\"", (unsigned long)size,
                set ? "true" : "false");
        uniapi_reply_resolve(p->reply, answer);
    } else {
        uniapi_reply_reject(p->reply, "out of memory");
    }
    free(answer);
    uniapi_free(quoted);
    free(png);
    uniapi_free_list(picked);
    free(p);
}

static void pick_icon(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) {
    pending* p = make_pending(r, 0, 0);
    (void)u;
    (void)a;
    (void)b;
    if (p) uniapi_post(app, run_pick_icon, p);
}

static void shared_memory(void* u, const char* a, const uniapi_bytes* b, uniapi_reply* r) {
    (void)u;
    (void)a;
    (void)b;
    uniapi_reply_resolve(r, uniapi_shared_memory(app) ? "true" : "false");
}

int main(void) {
    uniapi_options options;
    int dialog_first = 0, code;
    char** arguments = uniapi_arguments();
    char** argument;
    for (argument = arguments; argument && *argument; ++argument) {
        if (strcmp(*argument, "--dialog-first") == 0) dialog_first = 1;
    }
    uniapi_free_list(arguments);
    if (dialog_first) {
        uniapi_alert("uniapi C checker", "This alert ran before uniapi_run. Close it; the app then starts "
                                         "(on Linux expect shared memory OFF on the page, and no crash).");
    }

    uniapi_options_init(&options);
    options.title = "uniapi C checker";
    options.width = 760;
    options.height = 560;
    options.min_width = 500; /* the page asks you to shrink the window below this */
    options.min_height = 400;
    app = uniapi_app_new(&options);
    if (!app) {
        printf("uniapi_app_new failed: %s\n", uniapi_last_error());
        return 1;
    }
    uniapi_serve(app, uniapi_assets_checker_c_web());
    uniapi_expose(app, "openOne", open_one, NULL);
    uniapi_expose(app, "openSeveral", open_several, NULL);
    uniapi_expose(app, "openFolder", open_folder, NULL);
    uniapi_expose(app, "openFolders", open_folders, NULL);
    uniapi_expose(app, "save", save, NULL);
    uniapi_expose(app, "alert", alert, NULL);
    uniapi_expose(app, "toggleFullscreen", toggle_fullscreen, NULL);
    uniapi_expose(app, "cycleTitleBar", cycle_title_bar, NULL);
    uniapi_expose(app, "pickIcon", pick_icon, NULL);
    uniapi_expose(app, "sharedMemory", shared_memory, NULL);

    uniapi_navigate(app, "/index.html");
    code = uniapi_run(app);
    uniapi_app_free(app);
    return code;
}
