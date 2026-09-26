#pragma once

// VNM Plot Library - MSDF Font Atlas Cache
// Process-wide memo of built MSDF atlases, keyed by font identity and bake size.

#include <vnm_msdf_text/rhi/font_snapshot.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace vnm::plot::detail {

struct cached_font_data_t
{
    std::shared_ptr<const vnm::msdf_text::rhi::Baked_font> font;
    std::uint64_t                  cache_epoch       = 0;
    std::array<std::uint8_t, 32>   font_digest{};
};

// Identity of a cached atlas: the digest of the exact font bytes it was built
// from plus its bake pixel height. Draw sizes within that bucket share the
// same bitmap and font-unit metrics. The digest is part of the
// key because a process can hold several asset loaders that supply different
// fonts; keying on the height alone made whichever loader rendered first decide
// the font for every other one.
struct font_atlas_key_t
{
    std::array<std::uint8_t, 32>   font_digest{};
    int                            baked_pixel_height = 0;
};

[[nodiscard]] bool operator==(const font_atlas_key_t& lhs, const font_atlas_key_t& rhs);

struct font_atlas_key_hash_t
{
    [[nodiscard]] std::size_t operator()(const font_atlas_key_t& key) const;
};

// Memoizes built atlases under a byte budget, evicting the least recently used
// entries first. Entries are handed out as shared_ptr, so eviction only drops
// the memo: renderers still holding an evicted atlas keep using it.
class Font_atlas_cache
{
public:
    using Producer = std::function<std::shared_ptr<cached_font_data_t>()>;

    explicit Font_atlas_cache(std::size_t max_retained_bytes);

    Font_atlas_cache(const Font_atlas_cache&)            = delete;
    Font_atlas_cache& operator=(const Font_atlas_cache&) = delete;

    // Returns the atlas memoized for key, running produce at most once per key
    // across all threads: concurrent callers for the same key wait for the
    // single in-flight production instead of duplicating a multi-second bake.
    // force_rebuild discards any memoized entry first, so the returned atlas
    // always comes from a production started after the call.
    // Returns nullptr when produce fails; failures are not memoized.
    std::shared_ptr<cached_font_data_t> get_or_build(
        const font_atlas_key_t&    key,
        bool                       force_rebuild,
        const Producer&            produce);

private:
    struct entry_t
    {
        // Exactly one of these two states holds: producing is true and font is
        // null while a producer runs, or producing is false and font is set.
        std::shared_ptr<cached_font_data_t>
                       font;
        bool           producing = false;
        std::uint64_t  last_used = 0;
    };

    // Both require m_mutex to be held.
    void evict_least_recently_used();
    void finish_production(const font_atlas_key_t& key, std::shared_ptr<cached_font_data_t> produced);

    const std::size_t          m_max_retained_bytes;
    std::mutex                 m_mutex;
    std::condition_variable    m_production_finished;
    std::unordered_map<font_atlas_key_t, entry_t, font_atlas_key_hash_t>
                               m_entries;
    std::uint64_t              m_use_counter = 0;
};

} // namespace vnm::plot::detail
