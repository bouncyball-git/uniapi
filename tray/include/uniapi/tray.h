/* uniapi tray for C: the app's icon in the system tray, with a tooltip, a menu and a click of its
 * own (see uniapi/tray.hpp). Link uniapi_tray. Use it on the UI thread, as the app.
 *
 *   static void on_show(void* user) { uniapi_show((uniapi_app*)user); }
 *   static void on_quit(void* user) { uniapi_quit((uniapi_app*)user); }
 *
 *   uniapi_tray* tray = uniapi_tray_new(app, png, png_size, 0, "My app");
 *   uniapi_tray_item items[] = {
 *       {"Show", 0, on_show, app},
 *       {NULL, UNIAPI_TRAY_SEPARATOR, NULL, NULL},
 *       {"Quit", 0, on_quit, app},
 *   };
 *   uniapi_tray_set_menu(tray, items, 3);
 */
#ifndef UNIAPI_TRAY_H
#define UNIAPI_TRAY_H

#include <uniapi/uniapi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uniapi_tray uniapi_tray;

enum { UNIAPI_TRAY_SEPARATOR = 1, UNIAPI_TRAY_CHECKED = 2, UNIAPI_TRAY_DISABLED = 4 };

/* One menu item: its label, flags (UNIAPI_TRAY_*), and what a choice of it calls (may be NULL). */
typedef struct uniapi_tray_item {
    const char* label;
    int flags;
    uniapi_callback on_click;
    void* user;
} uniapi_tray_item;

/* Whether there is a tray to put an icon in (Linux: a StatusNotifierWatcher on the session bus). */
int uniapi_tray_available(void);
/* The icon (PNG; template_image: macOS draws it in the menu bar's colour) in the tray, once
 * uniapi_run runs. NULL, and uniapi_last_error says why, without a tray or a PNG. */
uniapi_tray* uniapi_tray_new(uniapi_app* app, const unsigned char* png, size_t size, int template_image,
                             const char* tooltip);
void uniapi_tray_free(uniapi_tray* tray); /* takes the icon away (the app ending does too) */
/* Replaces the menu (the labels are copied). */
void uniapi_tray_set_menu(uniapi_tray* tray, const uniapi_tray_item* items, int count);
int uniapi_tray_set_icon(uniapi_tray* tray, const unsigned char* png, size_t size, int template_image);
void uniapi_tray_set_tooltip(uniapi_tray* tray, const char* text);
/* A click on the icon; NULL: the click opens the menu (Linux: set it before uniapi_run). */
void uniapi_tray_on_click(uniapi_tray* tray, uniapi_callback handler, void* user);

#ifdef __cplusplus
}
#endif

#endif /* UNIAPI_TRAY_H */
