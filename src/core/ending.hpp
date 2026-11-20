#pragma once

// What optional parts of uniapi must undo when the App ends (the tray removes its icon: on Windows
// one left behind stays in the notification area until the mouse passes over it). ~App runs the
// steps first, newest first, on the UI thread while the platform is still up.

#include <uniapi/uniapi.hpp>

#include <functional>

namespace uniapi::detail {

int AddEndingStep(App& app, std::function<void()> step);  // its id, for RemoveEndingStep
void RemoveEndingStep(App& app, int id);                  // what it undoes went earlier

}  // namespace uniapi::detail
