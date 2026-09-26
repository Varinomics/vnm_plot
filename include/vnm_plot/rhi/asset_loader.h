#pragma once

// VNM Plot Library - Asset Loader
// Asset loading from registered byte views.

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
// Loads named assets registered by the library or its host.
class Asset_loader
{
public:
    // Log callback for errors
    using Log_callback = std::function<void(const std::string&)>;

    Asset_loader();
    ~Asset_loader();

    // Set log callback
    void set_log_callback(Log_callback callback);

    // Register an embedded asset
    // The data must remain valid for the lifetime of the Asset_loader.
    void register_embedded(std::string_view name, std::string_view data);

    // Load an asset by name
    // Returns the asset data, or nullopt on failure.
    [[nodiscard]] std::optional<Byte_buffer> load(std::string_view name) const;

private:
    void log_error(const std::string& message) const;

    Log_callback   m_log_callback;

    // Map from asset name to embedded data view
    std::unordered_map<std::string, std::string_view> m_embedded;
};

// Initialize embedded assets into the given loader.
// The bundled font uses Qt resources; registered replacements remain supported.
void init_embedded_assets(Asset_loader& loader);

} // namespace vnm::plot
