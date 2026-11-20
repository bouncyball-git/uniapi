// The uniapi logo on any background: its versions (light, dark, gray, emboss) at every icon
// size, on a background that changes colour by itself or as you pick it. The page is
// web/index.html; the logo's own files (doc/logo: its SVGs and the PNGs rendered for each size)
// are compiled in as a second table, and the page gets them from C++ as binary data.

#include <uniapi/uniapi.hpp>

#include <stdexcept>
#include <string>

UNIAPI_ASSETS(logo_web);    // the page (web/)
UNIAPI_ASSETS(logo_files);  // doc/logo: uniapi[-<version>].svg, png/uniapi[-<version>]-<size>.png

int main() {
    uniapi::AppOptions options;
    options.title = "uniapi logo";
    // A first guess: the page measures its content once the logo has loaded and asks for the
    // size that fits it (fit below), and only then is the window shown.
    options.width = 1000;
    options.height = 760;
    options.showWhenReady = true;
    uniapi::App app(options);
    app.serve(logo_web);

    // JavaScript -> C++: await app.native.fit(width, height): the page's area to fit its content
    // (logical pixels; the PNGs are drawn at real screen pixels, so it depends on the screen's
    // scale), then the window appears.
    app.expose("fit", [&app](int width, int height) {
        app.setSize(width, height);
        app.show();
    });

    // JavaScript -> C++: await app.native.logo("png/uniapi-dark-64.png") -> a Uint8Array.
    app.expose("logo", [](const std::string& path) {
        const uniapi::Asset* file = logo_files.find(path);
        if (!file) throw std::invalid_argument("no logo file " + path);
        const uniapi::AssetData bytes = uniapi::assetData(*file);
        return uniapi::Bytes(bytes.data, bytes.size);
    });

    app.navigate("/index.html");
    return app.run();
}
