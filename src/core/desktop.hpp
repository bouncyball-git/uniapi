#pragma once

// One instance per user (AppOptions::singleInstance): see desktop.cpp.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uniapi::detail {

// A later launch's arguments and working folder, on the listener's own thread.
using LaunchHandler = std::function<void(std::vector<std::string> arguments, std::string workingDirectory)>;

class InstanceListener;  // listening for later launches until destroyed
struct InstanceListenerDeleter {
    void operator()(InstanceListener* listener) const;  // stops it: joins its thread
};
using InstanceHandle = std::unique_ptr<InstanceListener, InstanceListenerDeleter>;

// A running instance takes this launch (handedOver: this program should end); else this one
// listens for later launches (null if it can't: the reason is printed, the app runs anyway).
InstanceHandle StartSingleInstance(const std::string& id, LaunchHandler received, bool& handedOver);

}  // namespace uniapi::detail
