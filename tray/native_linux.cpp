// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The Linux tray: org.kde.StatusNotifierItem at /StatusNotifierItem and com.canonical.dbusmenu at
// /MenuBar, under a bus name of its own (org.kde.StatusNotifierItem-<pid>-<n>), registered with
// org.kde.StatusNotifierWatcher once both the name is ours and the watcher is there (again when
// the watcher restarts). Each icon has a session bus connection of its own: object paths belong
// to a connection, so two icons on one would have only one /MenuBar between them (the second's
// menu calls reaching the first). Closing it takes the icon away. GDBus delivers the method calls
// on the UI thread (the main context the connection was made in).
//
// Hosts: KDE Plasma, XFCE, Cinnamon, LXQt, Budgie, waybar and others; GNOME only with the
// AppIndicator extension (Ubuntu ships it on).

#include "native.hpp"

#define G_LOG_DOMAIN "uniapi"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace uniapi::tray {

namespace {

constexpr char kWatcherName[] = "org.kde.StatusNotifierWatcher";
constexpr char kWatcherPath[] = "/StatusNotifierWatcher";
constexpr char kItemInterface[] = "org.kde.StatusNotifierItem";
constexpr char kItemPath[] = "/StatusNotifierItem";
constexpr char kMenuInterface[] = "com.canonical.dbusmenu";
constexpr char kMenuPath[] = "/MenuBar";

constexpr char kItemXml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='WindowId' type='i' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='OverlayIconName' type='s' access='read'/>"
    "<property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
    "<property name='AttentionIconName' type='s' access='read'/>"
    "<property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "<property name='AttentionMovieName' type='s' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "<signal name='NewTitle'/><signal name='NewIcon'/><signal name='NewAttentionIcon'/>"
    "<signal name='NewOverlayIcon'/><signal name='NewToolTip'/>"
    "<signal name='NewStatus'><arg name='status' type='s'/></signal>"
    "</interface></node>";

constexpr char kMenuXml[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' name='parentId' direction='in'/>"
    "<arg type='i' name='recursionDepth' direction='in'/><arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='u' name='revision' direction='out'/><arg type='(ia{sv}av)' name='layout' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' name='ids' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/><arg type='a(ia{sv})' name='properties' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' name='id' direction='in'/><arg type='s' name='name' direction='in'/>"
    "<arg type='v' name='value' direction='out'/></method>"
    "<method name='Event'><arg type='i' name='id' direction='in'/><arg type='s' name='eventId' direction='in'/>"
    "<arg type='v' name='data' direction='in'/><arg type='u' name='timestamp' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' name='events' direction='in'/>"
    "<arg type='ai' name='idErrors' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' name='id' direction='in'/><arg type='b' name='needUpdate' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' name='ids' direction='in'/>"
    "<arg type='ai' name='updatesNeeded' direction='out'/><arg type='ai' name='idErrors' direction='out'/></method>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})' name='updatedProps'/>"
    "<arg type='a(ias)' name='removedProps'/></signal>"
    "<signal name='LayoutUpdated'><arg type='u' name='revision'/><arg type='i' name='parent'/></signal>"
    "<signal name='ItemActivationRequested'><arg type='i' name='id'/><arg type='u' name='timestamp'/></signal>"
    "</interface></node>";

GDBusConnection* SessionBus() {
    static GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    return bus;
}

GDBusNodeInfo* ItemInfo() {
    static GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kItemXml, nullptr);
    return info;
}

GDBusNodeInfo* MenuInfo() {
    static GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    return info;
}

std::string ProgramName() {
    char path[4096];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    if (length <= 0) return "uniapi";
    path[length] = '\0';
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

class SniNative final : public Native {
public:
    explicit SniNative(std::function<bool(int)> clicked) : clicked_(std::move(clicked)) {}
    ~SniNative() override {
        Remove();
        if (pixels_) g_bytes_unref(pixels_);
    }

    bool Create(std::string& error) override {
        if (bus_) return true;
        GError* failure = nullptr;
        gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &failure);
        if (address) {
            bus_ = g_dbus_connection_new_for_address_sync(
                address,
                GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, nullptr, &failure);
            g_free(address);
        }
        if (!bus_) {
            error = failure ? failure->message : "no D-Bus session bus";
            if (failure) g_error_free(failure);
            return false;
        }
        g_dbus_connection_set_exit_on_close(bus_, FALSE);
        static int next = 1;
        id_ = ProgramName();  // the app id, as the window's and its .desktop file's
        busName_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-" + std::to_string(next++);
        static const GDBusInterfaceVTable itemVtable = {&SniNative::ItemMethod, &SniNative::ItemProperty, nullptr, {}};
        static const GDBusInterfaceVTable menuVtable = {&SniNative::MenuMethod, &SniNative::MenuProperty, nullptr, {}};
        // One bus name per item, so each item's objects sit at the usual paths.
        itemRegistration_ = g_dbus_connection_register_object(bus_, kItemPath, ItemInfo()->interfaces[0], &itemVtable,
                                                              this, nullptr, nullptr);
        menuRegistration_ = g_dbus_connection_register_object(bus_, kMenuPath, MenuInfo()->interfaces[0], &menuVtable,
                                                              this, nullptr, nullptr);
        ownerId_ = g_bus_own_name_on_connection(
            bus_, busName_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, gpointer self) {
                auto* me = static_cast<SniNative*>(self);
                me->nameAcquired_ = true;
                me->TryRegister();
            },
            +[](GDBusConnection*, const gchar* name, gpointer self) {
                g_debug("tray: lost the bus name %s", name);
                static_cast<SniNative*>(self)->nameAcquired_ = false;
            },
            this, nullptr);
        watchId_ = g_bus_watch_name_on_connection(
            bus_, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, gpointer self) {
                auto* me = static_cast<SniNative*>(self);
                me->watcherPresent_ = true;
                me->TryRegister();  // also when it comes back (the panel restarted)
            },
            +[](GDBusConnection*, const gchar*, gpointer self) { static_cast<SniNative*>(self)->watcherPresent_ = false; },
            this, nullptr);
        Emit(kItemPath, kItemInterface, "NewIcon", nullptr);
        return true;
    }

    // ARGB32 in network byte order, not premultiplied, as the protocol wants.
    bool SetIcon(const unsigned char* png, size_t size, bool) override {
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        if (!loader) return false;
        const bool wrote = gdk_pixbuf_loader_write(loader, png, size, nullptr);
        const bool closed = gdk_pixbuf_loader_close(loader, nullptr);
        GdkPixbuf* pixbuf = wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
        if (!pixbuf || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 || gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB) {
            g_object_unref(loader);
            return false;
        }
        const int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
        const int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
        const bool alpha = gdk_pixbuf_get_has_alpha(pixbuf);
        const guchar* pixels = gdk_pixbuf_read_pixels(pixbuf);
        std::vector<unsigned char> argb;
        argb.reserve(size_t(width) * height * 4);
        for (int y = 0; y < height; ++y) {
            const guchar* row = pixels + size_t(y) * stride;
            for (int x = 0; x < width; ++x) {
                const guchar* p = row + size_t(x) * channels;
                argb.push_back(alpha ? p[3] : 255);
                argb.push_back(p[0]);
                argb.push_back(p[1]);
                argb.push_back(p[2]);
            }
        }
        g_object_unref(loader);
        if (pixels_) g_bytes_unref(pixels_);
        pixels_ = g_bytes_new(argb.data(), argb.size());
        width_ = width;
        height_ = height;
        if (bus_) Emit(kItemPath, kItemInterface, "NewIcon", nullptr);
        return true;
    }

    void SetTooltip(const std::string& text) override {
        tooltip_ = text;
        if (!bus_) return;
        Emit(kItemPath, kItemInterface, "NewToolTip", nullptr);
        Emit(kItemPath, kItemInterface, "NewTitle", nullptr);
    }

    void SetClickOpensMenu(bool on) override { itemIsMenu_ = on; }

    void SetMenu(const std::vector<std::string>& labels, const std::vector<int>& flags) override {
        labels_ = labels;
        flags_ = flags;
        ++revision_;
        if (bus_) Emit(kMenuPath, kMenuInterface, "LayoutUpdated", g_variant_new("(ui)", revision_, 0));
    }

    // Off the bus: the watcher sees the name (and the connection) go and the host drops the icon.
    void Remove() override {
        if (!bus_) return;
        g_bus_unwatch_name(watchId_);
        g_bus_unown_name(ownerId_);
        if (itemRegistration_) g_dbus_connection_unregister_object(bus_, itemRegistration_);
        if (menuRegistration_) g_dbus_connection_unregister_object(bus_, menuRegistration_);
        g_dbus_connection_flush_sync(bus_, nullptr, nullptr);
        g_dbus_connection_close_sync(bus_, nullptr, nullptr);
        g_object_unref(bus_);
        bus_ = nullptr;
    }

    bool TestPlaced() override {
        if (!bus_) return false;
        GVariant* reply = g_dbus_connection_call_sync(
            bus_, kWatcherName, kWatcherPath, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", kWatcherName, "RegisteredStatusNotifierItems"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
        if (!reply) return false;
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        bool found = false;
        if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING_ARRAY)) {
            const gchar** items = g_variant_get_strv(value, nullptr);
            // Watchers list the service as given, or as "<name>/<path>", or by the unique name.
            const gchar* unique = g_dbus_connection_get_unique_name(bus_);
            for (const gchar** item = items; *item; ++item) {
                if (g_str_has_prefix(*item, busName_.c_str()) || (unique && g_str_has_prefix(*item, unique))) found = true;
            }
            g_free(items);
        }
        if (value) g_variant_unref(value);
        g_variant_unref(reply);
        return found;
    }

    // The menu item's "clicked" event, sent to this item over the bus as a host sends it
    // (asynchronous: it comes in on the UI thread).
    void TestChoose(int index) override {
        if (!bus_) return;
        g_dbus_connection_call(bus_, busName_.c_str(), kMenuPath, kMenuInterface, "Event",
                               g_variant_new("(isvu)", index + 1, "clicked", g_variant_new_string(""), guint32(0)),
                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    }

private:
    void Emit(const char* path, const char* interface, const char* name, GVariant* parameters) {
        g_dbus_connection_emit_signal(bus_, nullptr, path, interface, name, parameters, nullptr);
    }

    void TryRegister() {
        if (!nameAcquired_ || !watcherPresent_ || !bus_) return;
        g_dbus_connection_call(
            bus_, kWatcherName, kWatcherPath, kWatcherName, "RegisterStatusNotifierItem",
            g_variant_new("(s)", busName_.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer) {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                if (reply) g_variant_unref(reply);
                if (error) {
                    g_debug("tray: the StatusNotifierWatcher refused the item: %s", error->message);
                    g_error_free(error);
                }
            },
            nullptr);
    }

    // --- The item ---

    GVariant* IconPixmaps() const {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
        if (pixels_) {
            g_variant_builder_add(&builder, "(ii@ay)", width_, height_,
                                  g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), pixels_, TRUE));
        }
        return g_variant_builder_end(&builder);
    }

    static GVariant* ItemProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**,
                                  gpointer data) {
        auto* me = static_cast<SniNative*>(data);
        const std::string property = name;
        if (property == "Category") return g_variant_new_string("ApplicationStatus");
        if (property == "Id") return g_variant_new_string(me->id_.c_str());
        if (property == "Title") return g_variant_new_string(me->tooltip_.empty() ? me->id_.c_str() : me->tooltip_.c_str());
        if (property == "Status") return g_variant_new_string("Active");
        if (property == "WindowId") return g_variant_new_int32(0);
        if (property == "IconPixmap") return me->IconPixmaps();
        if (property == "OverlayIconPixmap" || property == "AttentionIconPixmap") {
            return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
        }
        if (property == "ToolTip") {
            return g_variant_new("(s@a(iiay)ss)", "", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0),
                                 me->tooltip_.c_str(), "");
        }
        if (property == "ItemIsMenu") return g_variant_new_boolean(me->itemIsMenu_);
        if (property == "Menu") return g_variant_new_object_path(kMenuPath);
        return g_variant_new_string("");  // IconName, OverlayIconName, AttentionIconName, AttentionMovieName
    }

    static void ItemMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant*,
                           GDBusMethodInvocation* invocation, gpointer data) {
        auto* me = static_cast<SniNative*>(data);
        g_dbus_method_invocation_return_value(invocation, nullptr);  // answered first: the app may take long
        // Activate: a plain click. ContextMenu: the host opens Menu itself. SecondaryActivate (a
        // middle click) and Scroll: nothing.
        if (std::strcmp(method, "Activate") == 0) me->clicked_(-1);
    }

    // --- The menu ---

    int Count() const { return int(labels_.size()); }

    GVariant* MenuProperties(int id) const {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (id == 0) {
            g_variant_builder_add(&builder, "{sv}", "children-display", g_variant_new_string("submenu"));
        } else if (id >= 1 && id <= Count()) {
            const int flags = flags_[size_t(id - 1)];
            if (flags & kSeparator) {
                g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_string("separator"));
            } else {
                g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string(labels_[size_t(id - 1)].c_str()));
                g_variant_builder_add(&builder, "{sv}", "enabled", g_variant_new_boolean(!(flags & kDisabled)));
                if (flags & kChecked) {
                    g_variant_builder_add(&builder, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
                    g_variant_builder_add(&builder, "{sv}", "toggle-state", g_variant_new_int32(1));
                }
            }
        }
        return g_variant_builder_end(&builder);
    }

    // (ia{sv}av): the item, and its children (the root's: every item; no submenus).
    GVariant* MenuLayout(int id, int depth) const {
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (id == 0 && depth != 0) {
            for (int child = 1; child <= Count(); ++child) g_variant_builder_add(&children, "v", MenuLayout(child, 0));
        }
        return g_variant_new("(i@a{sv}av)", id, MenuProperties(id), &children);
    }

    void MenuEvent(int id, const char* event) {
        if (std::strcmp(event, "clicked") == 0 && id >= 1 && id <= Count() && !(flags_[size_t(id - 1)] & kSeparator)) {
            clicked_(id - 1);
        }
    }

    static GVariant* MenuProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**,
                                  gpointer) {
        if (std::strcmp(name, "Version") == 0) return g_variant_new_uint32(3);
        if (std::strcmp(name, "TextDirection") == 0) return g_variant_new_string("ltr");
        if (std::strcmp(name, "Status") == 0) return g_variant_new_string("normal");
        return g_variant_new_strv(nullptr, 0);  // IconThemePath
    }

    static void MenuMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                           GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
        auto* me = static_cast<SniNative*>(data);
        const std::string name = method;
        if (name == "GetLayout") {
            gint32 parent = 0, depth = -1;
            g_variant_get(parameters, "(ii@as)", &parent, &depth, nullptr);
            if (parent < 0 || parent > me->Count()) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "no item %d", parent);
                return;
            }
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(u@(ia{sv}av))", me->revision_, me->MenuLayout(parent, depth)));
        } else if (name == "GetGroupProperties") {
            GVariant* ids = nullptr;
            g_variant_get(parameters, "(@ai@as)", &ids, nullptr);
            GVariantBuilder result;
            g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
            gsize n = 0;
            const auto* wanted = static_cast<const gint32*>(g_variant_get_fixed_array(ids, &n, sizeof(gint32)));
            if (n == 0) {  // none asked for: all of them
                for (int id = 0; id <= me->Count(); ++id) g_variant_builder_add(&result, "(i@a{sv})", id, me->MenuProperties(id));
            }
            for (gsize i = 0; i < n; ++i) {
                if (wanted[i] >= 0 && wanted[i] <= me->Count()) {
                    g_variant_builder_add(&result, "(i@a{sv})", wanted[i], me->MenuProperties(wanted[i]));
                }
            }
            g_variant_unref(ids);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result)));
        } else if (name == "GetProperty") {
            gint32 id = 0;
            const gchar* property = nullptr;
            g_variant_get(parameters, "(i&s)", &id, &property);
            GVariant* properties = g_variant_ref_sink(me->MenuProperties(id));
            GVariant* value = g_variant_lookup_value(properties, property, nullptr);
            g_variant_unref(properties);
            if (!value) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "no property %s", property);
                return;
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
            g_variant_unref(value);
        } else if (name == "Event") {
            gint32 id = 0;
            const gchar* event = nullptr;
            g_variant_get(parameters, "(i&s@vu)", &id, &event, nullptr, nullptr);
            const std::string copy = event ? event : "";
            g_dbus_method_invocation_return_value(invocation, nullptr);  // answered first: the app may take long
            me->MenuEvent(id, copy.c_str());
        } else if (name == "EventGroup") {
            GVariantIter* events = nullptr;
            g_variant_get(parameters, "(a(isvu))", &events);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
            gint32 id = 0;
            gchar* event = nullptr;
            while (g_variant_iter_next(events, "(is@vu)", &id, &event, nullptr, nullptr)) {
                me->MenuEvent(id, event);
                g_free(event);
            }
            g_variant_iter_free(events);
        } else if (name == "AboutToShow") {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        } else if (name == "AboutToShowGroup") {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
                                          g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        } else {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s", method);
        }
    }

    std::function<bool(int)> clicked_;
    GDBusConnection* bus_ = nullptr;
    std::string busName_;
    std::string id_ = "uniapi";
    guint ownerId_ = 0, watchId_ = 0, itemRegistration_ = 0, menuRegistration_ = 0;
    bool nameAcquired_ = false, watcherPresent_ = false;
    std::string tooltip_;
    int width_ = 0, height_ = 0;
    GBytes* pixels_ = nullptr;  // ARGB32
    bool itemIsMenu_ = false;
    std::vector<std::string> labels_;  // menu items: ids 1..count (0 is the root)
    std::vector<int> flags_;
    guint32 revision_ = 1;
};

}  // namespace

bool NativeAvailable() {
    GDBusConnection* bus = SessionBus();
    if (!bus) return false;
    GVariant* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                  "NameHasOwner", g_variant_new("(s)", kWatcherName), G_VARIANT_TYPE("(b)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
    if (!reply) return false;
    gboolean has = FALSE;
    g_variant_get(reply, "(b)", &has);
    g_variant_unref(reply);
    return has;
}

std::unique_ptr<Native> NewNative(std::function<bool(int)> clicked) { return std::make_unique<SniNative>(std::move(clicked)); }

}  // namespace uniapi::tray
