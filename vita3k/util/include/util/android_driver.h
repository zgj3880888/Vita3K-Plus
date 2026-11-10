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

// libadrenotools keeps the loaded driver in a process-wide hook table and has no way to unload it, so a
// process can only ever serve one driver choice. These track the choice this process made the first time it
// resolved a driver, so a later, different choice can be detected instead of producing a broken renderer.
bool is_driver_switch_pending(const std::string &requested_driver);
void set_current_launch_title_id(std::string title_id);
bool relaunch_process_for_driver_switch(const std::string &requested_driver);

} // namespace android_driver
