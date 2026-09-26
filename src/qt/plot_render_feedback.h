#pragma once

#include <vnm_plot/core/series_window.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace vnm::plot::detail {

struct plot_render_feedback_t
{
    double                 measured_vbar_width = 0.0;
    float                  v_min                = 0.0f;
    float                  v_max                = 1.0f;
    std::int64_t           t_min                = 0;
    std::int64_t           t_max                = 1;
    std::int64_t           t_available_min      = 0;
    std::int64_t           t_available_max      = 1;
    std::uint64_t          series_revision      = 0;
    std::uint64_t          generation           = 0;
    std::map<int, std::vector<Rendered_stack_source>>
                           stack_validity;
    std::map<std::pair<int, Series_view_kind>, Rendered_stack_status>
                           stack_statuses;
    bool                   stack_validity_ready = false;
};

// The stack half of this record has exactly one producer, detail::fill_stack_feedback:
// declared in vnm_plot/rhi/series_renderer.h, which befriends it so that it can read a
// completed pass, and defined in plot_render_feedback.cpp.

// Shared by the scene-graph renderer and its item without either retaining
// the other. Render publishes into latest; the after-rendering marker moves
// only a frame-complete generation into completed for GUI-thread delivery.
class plot_render_feedback_channel_t
{
public:
    void publish(plot_render_feedback_t feedback)
    {
        std::lock_guard lock(m_mutex);
        feedback.generation = ++m_generation;
        m_latest            = std::move(feedback);
    }

    void mark_latest_completed()
    {
        std::lock_guard lock(m_mutex);
        if (!m_latest) {
            return;
        }

        m_completed = std::move(m_latest);
        m_latest.reset();
    }

    std::optional<plot_render_feedback_t> take_completed_after(
        std::uint64_t generation)
    {
        std::lock_guard lock(m_mutex);
        if (!m_completed || m_completed->generation <= generation) {
            return std::nullopt;
        }

        auto feedback = std::move(m_completed);
        m_completed.reset();
        return feedback;
    }

private:
    std::mutex                            m_mutex;
    std::optional<plot_render_feedback_t> m_latest;
    std::optional<plot_render_feedback_t> m_completed;
    std::uint64_t                         m_generation = 0;
};

inline std::optional<plot_render_feedback_t> take_completed_feedback(
    const std::shared_ptr<plot_render_feedback_channel_t>& current_channel,
    const std::shared_ptr<plot_render_feedback_channel_t>& callback_channel,
    std::uint64_t                                          generation)
{
    if (!callback_channel || current_channel != callback_channel) {
        return std::nullopt;
    }
    return callback_channel->take_completed_after(generation);
}

} // namespace vnm::plot::detail
