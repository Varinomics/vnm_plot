#include "plot_render_feedback.h"

#include <vnm_plot/rhi/series_renderer.h>

namespace vnm::plot::detail {

void fill_stack_feedback(const Series_renderer& series, plot_render_feedback_t& feedback)
{
    feedback.stack_validity = series.main_stack_validity();
    feedback.stack_statuses = series.stack_view_statuses();
    feedback.stack_validity_ready = true;
}

} // namespace vnm::plot::detail
