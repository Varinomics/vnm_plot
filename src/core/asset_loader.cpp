#include <vnm_plot/rhi/asset_loader.h>

#include <utility>

namespace vnm::plot {

Asset_loader::Asset_loader()  = default;
Asset_loader::~Asset_loader() = default;

void Asset_loader::set_log_callback(Log_callback callback)
{
    m_log_callback = std::move(callback);
}

void Asset_loader::log_error(const std::string& message) const
{
    if (m_log_callback) {
        m_log_callback(message);
    }
}

void Asset_loader::register_embedded(std::string_view name, std::string_view data)
{
    m_embedded[std::string(name)] = data;
}

std::optional<Byte_buffer> Asset_loader::load(std::string_view name) const
{
    auto it = m_embedded.find(std::string(name));
    if (it != m_embedded.end()) {
        return Byte_buffer(it->second);
    }

    log_error("Asset not found: " + std::string(name));
    return std::nullopt;
}

} // namespace vnm::plot
