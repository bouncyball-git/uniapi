// Linux helpers for the lifecycle test: uniapi's GTK windows, found by title. Call them on the UI
// thread.

#include <gtk/gtk.h>

#include <string>

namespace {

GtkWindow* Find(const std::string& title) {
    GtkWindow* found = nullptr;
    GList* toplevels = gtk_window_list_toplevels();
    for (GList* item = toplevels; item; item = item->next) {
        if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(item->data)), title.c_str()) == 0) found = GTK_WINDOW(item->data);
    }
    g_list_free(toplevels);
    return found;
}

}  // namespace

// Whether the window is shown (GTK's visible): 1, 0, or -1 if there is none.
int OnScreen(const std::string& title) {
    GtkWindow* window = Find(title);
    return window ? (gtk_widget_get_visible(GTK_WIDGET(window)) ? 1 : 0) : -1;
}

// As its close button: gtk_window_close, which asks (close-request on GTK 4, delete-event on 3).
bool CloseAsUser(const std::string& title) {
    GtkWindow* window = Find(title);
    if (!window) return false;
    gtk_window_close(window);
    return true;
}

bool QuitAsUser() { return false; }  // Linux has no app-wide Quit

// The developer tools' windows shown: WebKit's Web Inspector, detached, is a window of this
// process (a WebKitInspectorWindow; GTK 4's has no title). close: closes them.
int DevToolsWindows(bool close) {
    int count = 0;
    GList* toplevels = gtk_window_list_toplevels();
    for (GList* item = toplevels; item; item = item->next) {
        GtkWindow* window = GTK_WINDOW(item->data);
        if (g_strcmp0(G_OBJECT_TYPE_NAME(window), "WebKitInspectorWindow") == 0 && gtk_widget_get_visible(GTK_WIDGET(window))) {
            ++count;
            if (close) gtk_window_close(window);
        }
    }
    g_list_free(toplevels);
    return count;
}

// Not checked here: GTK keeps input from the main window itself (see tests/windows).
int TakesInput(const std::string&) { return -1; }
