#pragma once

#include <util/fs.h>

#include <optional>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

namespace android_driver {

bool is_safe_driver_relative_path(const fs::path &relative_path);
std::optional<std::string> read_driver_library_name_from_meta(const fs::path &meta_path);
std::optional<std::string> resolve_custom_driver_library_name(const fs::path &driver_path);
bool is_custom_driver_loaded(const std::string &driver_name, uint32_t vendor_id, uint32_t driver_version, std::string_view device_name);
PFN_vkGetInstanceProcAddr resolve_vk_get_instance_proc_addr(const std::string &driver_name);
std::string device_summary();
void log_device_identity();
bool is_adreno_6xx();

// libadrenotools installs a process-wide hook table the first time a driver is opened, so reopening a
// different driver on an already patched process is what breaks the renderer. These track which driver this
// process resolved when it actually built a renderer (never from a probe), so a later, different choice is
// detected and answered with the same in-process relaunch the stock restart-app button uses.
void bind_driver_for_this_process(const std::string &driver_name);
bool is_driver_switch_pending(const std::string &requested_driver);
bool is_driver_bound_to_this_process();

// Called from VKState::create() when the configured driver differs from the one this process already
// resolved. Clears the recorded choice so the relaunch can resolve the new driver instead of looping on the
// same mismatch, and remembers that a relaunch is owed. Returns false once too many switches happened in one
// process, in which case the caller keeps running with whatever the system loader provides.
bool request_driver_switch_relaunch(const std::string &requested_driver);

// True exactly once, right after request_driver_switch_relaunch() asked for a relaunch. The launch loop uses
// it to restart the session in-process instead of reporting a renderer failure.
bool consume_driver_switch_relaunch_request();

} // namespace android_driver
