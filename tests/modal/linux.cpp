// The modal test's clicks: made inside the app on GTK 3, handed to GTK as if from the mouse
// through gtk_main_do_event, where GTK sends input outside a modal window to the modal window
// instead (not through the compositor). GTK 4 has no way to make an event.

#include <gtk/gtk.h>

#include <string>

// A click at (x, y) in the window with this title: 1, 0 if there is no such window, -1 where this
// GTK can't make one. On the UI thread.
int Click(const std::string& title, int x, int y) {
#ifdef UNIAPI_GTK4
    (void)title;
    (void)x;
    (void)y;
    return -1;
#else
    GtkWindow* found = nullptr;
    GList* toplevels = gtk_window_list_toplevels();
    for (GList* item = toplevels; item; item = item->next) {
        if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(item->data)), title.c_str()) == 0) found = GTK_WINDOW(item->data);
    }
    g_list_free(toplevels);
    if (!found) return 0;
    GtkWidget* child = gtk_bin_get_child(GTK_BIN(found));
    GdkWindow* window = gtk_widget_get_window(child ? child : GTK_WIDGET(found));
    if (!window) return 0;
    GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window)));
    int originX = 0, originY = 0;
    gdk_window_get_origin(window, &originX, &originY);
    for (int release = 0; release < 2; ++release) {
        GdkEvent* event = gdk_event_new(release ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS);
        event->button.window = GDK_WINDOW(g_object_ref(window));
        event->button.time = GDK_CURRENT_TIME;
        event->button.x = x;
        event->button.y = y;
        event->button.x_root = originX + x;
        event->button.y_root = originY + y;
        event->button.button = 1;
        event->button.state = release ? GDK_BUTTON1_MASK : 0;
        gdk_event_set_device(event, pointer);
        gtk_main_do_event(event);
        gdk_event_free(event);
    }
    return 1;
#endif
}
