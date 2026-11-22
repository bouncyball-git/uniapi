// uniapi's WebKitGTK web process extension: gives the page zero-copy access to stream slots.
//
// WebKitGTK loads this library into each web process (the app points it at the directory
// with webkit_web_context_set_web_extensions_directory, or ..._web_process_extensions_directory
// on webkitgtk-6.0, where it's called a web process extension). It answers two messages:
//
//   "uniapi.hello"   handshake; the reply (a boolean) tells the app whether shared memory is
//                    safe here: JavaScriptCore's Gigacage must be off in this process (see
//                    backend_linux.cpp), otherwise wrapping outside memory aborts the process
//   "uniapi.region"  parameters (info JSON, size, writable) plus one memfd: the region is mapped
//                    and handed to the page as an ArrayBuffer over that memory, through
//                    window.__uniapi.region(info, buffer) (see js/uniapi.js)
//
// A stream's region (writable false) is mapped private copy-on-write: the page sees what the app
// writes, but if page code writes into the buffer it gets its own copy of that memory page
// instead of crashing or changing the app's data. A sink's (writable true) is mapped shared: the
// page writes into it and the app reads what it wrote. The buffer is unmapped when JavaScript
// garbage-collects it.
//
// Kept apart from the uniapi library on purpose: it only needs WebKitGTK's extension API.

#ifdef UNIAPI_GTK4
#include <webkit/webkit-web-process-extension.h>
using Extension = WebKitWebProcessExtension;
#define UNIAPI_EXTENSION_ENTRY webkit_web_process_extension_initialize
#else
#include <webkit2/webkit-web-extension.h>
using Extension = WebKitWebExtension;
#define UNIAPI_EXTENSION_ENTRY webkit_web_extension_initialize
#endif

#include <gio/gunixfdlist.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr char kContextKey[] = "uniapi-js-context";

struct Mapping {
    void* data;
    size_t size;
};

void Unmap(gpointer data) {
    auto* mapping = static_cast<Mapping*>(data);
    munmap(mapping->data, mapping->size);
    delete mapping;
}

void HandleRegion(WebKitWebPage* page, WebKitUserMessage* message) {
    GVariant* parameters = webkit_user_message_get_parameters(message);
    if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(stb)"))) return;
    const char* info = nullptr;
    guint64 size = 0;
    gboolean writable = FALSE;
    g_variant_get(parameters, "(&stb)", &info, &size, &writable);
    GUnixFDList* fds = webkit_user_message_get_fd_list(message);
    if (size == 0 || !fds || g_unix_fd_list_get_length(fds) < 1) return;

    int fd = g_unix_fd_list_get(fds, 0, nullptr);  // a duplicate we own
    if (fd < 0) return;
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, writable ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return;

    // The main frame's JavaScript context, kept by OnWindowObjectCleared for each new document.
    auto* context = static_cast<JSCContext*>(g_object_get_data(G_OBJECT(page), kContextKey));
    if (!context) {
        munmap(data, size);
        return;
    }
    g_object_ref(context);
    JSCValue* bridge = jsc_context_get_value(context, "__uniapi");
    if (!jsc_value_is_object(bridge)) {  // uniapi.js isn't loaded (yet); nobody to give it to
        munmap(data, size);
    } else {
        JSCValue* infoValue = jsc_value_new_from_json(context, info);
        JSCValue* buffer = jsc_value_new_array_buffer(context, data, size, Unmap, new Mapping{data, size});
        if (buffer) {
            JSCValue* result = jsc_value_object_invoke_method(bridge, "region", JSC_TYPE_VALUE, infoValue,
                                                              JSC_TYPE_VALUE, buffer, G_TYPE_NONE);
            if (result) g_object_unref(result);
            g_object_unref(buffer);
        }
        // No munmap when it returns NULL: JavaScriptCore may already have handed the memory to
        // an ArrayBuffer whose destructor (Unmap) runs anyway. Leaking one mapping on that rare
        // failure (out of memory) is safer than unmapping it twice.
        if (infoValue) g_object_unref(infoValue);
        jsc_context_clear_exception(context);
    }
    g_object_unref(bridge);
    g_object_unref(context);
}

gboolean OnUserMessage(WebKitWebPage* page, WebKitUserMessage* message, gpointer) {
    const char* name = webkit_user_message_get_name(message);
    if (std::strcmp(name, "uniapi.hello") == 0) {
        const char* gigacage = std::getenv("GIGACAGE_ENABLED");
        const bool gigacageOff = gigacage && (std::strcmp(gigacage, "no") == 0 || std::strcmp(gigacage, "0") == 0 ||
                                              std::strcmp(gigacage, "false") == 0);
        webkit_user_message_send_reply(message,
                                       webkit_user_message_new("uniapi.hello", g_variant_new_boolean(gigacageOff)));
        return TRUE;
    }
    if (std::strcmp(name, "uniapi.region") == 0) {
        HandleRegion(page, message);
        return TRUE;
    }
    return FALSE;
}

void OnWindowObjectCleared(WebKitScriptWorld*, WebKitWebPage* page, WebKitFrame* frame, gpointer) {
    if (!webkit_frame_is_main_frame(frame)) return;
    JSCContext* context = webkit_frame_get_js_context_for_script_world(frame, webkit_script_world_get_default());
    g_object_set_data_full(G_OBJECT(page), kContextKey, context, g_object_unref);
}

}  // namespace

extern "C" G_MODULE_EXPORT void UNIAPI_EXTENSION_ENTRY(Extension* extension) {
    g_signal_connect(extension, "page-created",
                     G_CALLBACK(+[](Extension*, WebKitWebPage* page, gpointer) {
                         g_signal_connect(page, "user-message-received", G_CALLBACK(OnUserMessage), nullptr);
                     }),
                     nullptr);
    g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
                     G_CALLBACK(OnWindowObjectCleared), nullptr);
}
