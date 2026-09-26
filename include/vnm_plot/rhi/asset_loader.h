#pragma once

// VNM Plot Library - Asset Loader
// Asset loading with embedded defaults and optional file overrides.

#include <vnm_plot/core/types.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vnm::plot {

// -----------------------------------------------------------------------------
// Asset_loader
// -----------------------------------------------------------------------------
// Loads named assets with support for:
// - Embedded defaults (compiled into the binary)
// - Optional file system overrides (for development/debugging)
class Asset_loader
{
public:
    // Log callback for errors
    using Log_callback = std::function<void(const std::string&)>;

    Asset_loader();
    ~Asset_loader();

    // Set log callback
    void set_log_callback(Log_callback callback);

    // Configure file system override directory (empty to disable)
    // If set, assets will first be searched in this directory.
    void set_override_directory(std::string_view path);

    // Register an embedded asset
    // The data must remain valid for the lifetime of the Asset_loader.
    void register_embedded(std::string_view name, std::string_view data);

    // Load an asset by name
    // Returns the asset data, or nullopt on failure.
    // First checks override directory, then embedded assets.
    [[nodiscard]] std::optional<Byte_buffer> load(std::string_view name) const;

private:
    bool load_file(std::string_view path, Byte_buffer& out) const;
    void log_error(const std::string& message) const;

    Log_callback   m_log_callback;
    std::string    m_override_dir;

    // Map from asset name to embedded data view
    std::unordered_map<std::string, std::string_view> m_embedded;
};

// Initialize embedded assets into the given loader.
// The bundled font uses Qt resources; registered replacements remain supported.
void init_embedded_assets(Asset_loader& loader);

} // namespace vnm::plot
