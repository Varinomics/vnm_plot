#include "font_atlas_cache.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace vnm::plot::detail {

bool operator==(const font_atlas_key_t& lhs, const font_atlas_key_t& rhs)
{
    return lhs.baked_pixel_height == rhs.baked_pixel_height && lhs.font_digest == rhs.font_digest;
}

std::size_t font_atlas_key_hash_t::operator()(const font_atlas_key_t& key) const
{
    // The digest already is a cryptographic hash, so its leading bytes make a
    // good bucket index; the pixel height separates bake sizes of one font.
    std::size_t hash = 0;
    for (std::size_t i = 0; i < sizeof(std::size_t) && i < key.font_digest.size(); ++i) {
        hash = (hash << 8) | static_cast<std::size_t>(key.font_digest[i]);
    }
    return hash ^ (static_cast<std::size_t>(key.baked_pixel_height) * 0x9e3779b9u);
}

Font_atlas_cache::Font_atlas_cache(std::size_t max_retained_bytes)
    : m_max_retained_bytes(max_retained_bytes)
{
}

std::shared_ptr<cached_font_data_t> Font_atlas_cache::get_or_build(
    const font_atlas_key_t&    key,
    bool                       force_rebuild,
    const Producer&            produce)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    for (;;) {
        const auto found = m_entries.find(key);
        if (found == m_entries.end()) {
            break;
        }
        if (found->second.producing) {
            // Single flight: one producer per key, every other caller waits for
            // it rather than repeating a multi-second bake.
            m_production_finished.wait(lock);
            continue;
        }
        if (force_rebuild) {
            m_entries.erase(found);
            break;
        }
        found->second.last_used = ++m_use_counter;
        return found->second.font;
    }

    m_entries[key].producing = true;
    lock.unlock();

    std::shared_ptr<cached_font_data_t> produced;
    try {
        produced = produce();
    }
    catch (...) {
        lock.lock();
        finish_production(key, nullptr);
        throw;
    }

    lock.lock();
    finish_production(key, produced);
    return produced;
}

void Font_atlas_cache::finish_production(
    const font_atlas_key_t&                key,
    std::shared_ptr<cached_font_data_t>    produced)
{
    if (!produced) {
        m_entries.erase(key);
        m_production_finished.notify_all();
        return;
    }

    auto& entry     = m_entries[key];
    entry.font      = std::move(produced);
    entry.producing = false;
    entry.last_used = ++m_use_counter;
    evict_least_recently_used();
    m_production_finished.notify_all();
}

void Font_atlas_cache::evict_least_recently_used()
{
    struct candidate_t
    {
        font_atlas_key_t   key;
        std::uint64_t      last_used = 0;
        std::size_t        bytes     = 0;
    };

    std::vector<candidate_t>    candidates;
    std::size_t                 retained = 0;
    candidates.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries) {
        if (!entry.font) {
            // Still in production, so it retains no bitmap yet and must survive
            // eviction until its producer publishes it.
            continue;
        }
        const std::size_t bytes = entry.font->atlas.rgba.size();
        retained += bytes;
        candidates.push_back({key, entry.last_used, bytes});
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const candidate_t& lhs, const candidate_t& rhs) {
            return lhs.last_used < rhs.last_used;
        });

    for (const auto& candidate : candidates) {
        if (retained <= m_max_retained_bytes) {
            break;
        }
        retained -= candidate.bytes;
        m_entries.erase(candidate.key);
    }
}

} // namespace vnm::plot::detail
