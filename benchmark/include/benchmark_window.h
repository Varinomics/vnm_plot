// vnm_plot Benchmark - Window
// QRhi benchmark windows and offscreen runner.

#ifndef VNM_PLOT_BENCHMARK_WINDOW_H
#define VNM_PLOT_BENCHMARK_WINDOW_H

#include "benchmark_constants.h"
#include "benchmark_data_source.h"
#include "benchmark_profiler.h"
#include "publication_rate_clock.h"
#include "brownian_generator.h"
#include "ring_buffer.h"
#include "sample_types.h"

#include <vnm_plot/qt/plot_widget.h>
#include <vnm_plot/vnm_plot.h>

#include <QQuickWindow>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QRhi;
class QRhiRenderBuffer;
class QRhiRenderPassDescriptor;
class QRhiTextureRenderTarget;
class QRhiTexture;
class QOffscreenSurface;
class QVulkanInstance;

namespace vnm::plot::detail {
class Plot_frame_renderer;
struct Plot_render_snapshot;
}

namespace vnm::benchmark {

/// Configuration for the benchmark window
struct Benchmark_config {
    double duration_seconds = 30.0;
    std::string session = "benchmark_run";
    std::string stream = "SIM";
    std::string data_type = "Bars";  // "Bars" or "Trades"
    // "" => default per data type (Trades=Dots, Bars=Area).
    // Otherwise one of: "Dots", "Line", "Area".
    std::string style = "";
    std::string output_directory = ".";
    uint64_t seed = 0;
    double volatility = 0.02;
    double rate = 1000.0;  // samples per second
    std::size_t ring_capacity = 100000;
    std::size_t series_count = 1;
    bool stack_series = false;
    bool extended_metadata = false;
    bool quiet = false;  // Suppress console output during benchmark
    bool show_text = true;  // Text/font rendering (default: on)
    std::string backend = "qrhi-offscreen";  // "qrhi" or "qrhi-offscreen"
    // Requested graphics API. "native" selects the platform's normal QRhi
    // backend; Null is used only when explicitly requested.
    std::string graphics_backend = "native";
    bool finish = false;  // Wait for GPU completion after offscreen frames
    int framebuffer_width = 1200;
    int framebuffer_height = 720;
    std::size_t sample_count = 4;
    std::size_t warmup_frames = 2;
    // Zero keeps duration-based execution. Nonzero runs exactly this many
    // measured frames after warmup and is preferred for repeatable measurements.
    std::size_t measured_frames = 0;
    std::string scenario = "ad-hoc";
    bool capture_pixel_checksum = false;
    // Static mode pre-fills a representative deterministic data window and
    // skips the producer thread.
    bool static_data = false;
    std::size_t static_sample_count = 10'000;
    // Line thickness in pixels (default 1.5 for animated runs; bumped via
    // --line-px when comparing visuals so segments are easy to inspect).
    double line_width_px = 1.5;
    // Dot diameter in pixels for DOTS rendering (default 1.0 matches the
    // animated benchmark; bumped via --point-px so static-mode dots are
    // visible in side-by-side visual comparisons).
    double point_diameter_px = 1.0;
};

struct Graphics_device_info {
    std::string backend = "uninitialized";
    std::string device_name = "unknown";
    std::string device_type = "unknown";
    std::string fallback_surface_requested_format = "not-applicable";
    std::string fallback_surface_resolved_format = "not-applicable";
    std::uint64_t device_id = 0;
    std::uint64_t vendor_id = 0;
};

/// QRhi benchmark window using the public QQuickRhiItem Plot_widget path.
class Benchmark_rhi_window : public QQuickWindow {
    Q_OBJECT

public:
    explicit Benchmark_rhi_window(const Benchmark_config& config);
    ~Benchmark_rhi_window() override;

    Benchmark_profiler& profiler() { return m_profiler; }
    const Benchmark_profiler& profiler() const { return m_profiler; }
    const Benchmark_config& config() const { return m_config; }
    std::size_t samples_generated() const { return m_samples_generated.load(); }
    std::chrono::system_clock::time_point started_at() const { return m_started_at; }
    Graphics_device_info graphics_device_info() const;
    std::size_t measured_frame_count() const { return m_measured_frame_count; }
    std::uint64_t pixel_checksum() const { return 0; }
    std::size_t pixel_nonuniform_count() const { return 0; }
    std::size_t stack_sum_pixel_count() const { return 0; }
    std::string phase_trace_path() const { return {}; }

signals:
    void benchmark_finished();

private slots:
    void on_render_timer();
    void on_benchmark_timeout();

private:
    void setup_series();
    void generator_thread_func();
    void stop_generator_thread();
    void fill_static_data();
    void update_plot_view();
    void record_final_statistics();

    Benchmark_config m_config;
    Benchmark_profiler m_profiler;

    std::vector<std::unique_ptr<Ring_buffer<Bar_sample>>> m_bar_buffers;
    std::vector<std::unique_ptr<Ring_buffer<Trade_sample>>> m_trade_buffers;
    std::vector<std::unique_ptr<Benchmark_data_source<Bar_sample>>> m_bar_sources;
    std::vector<std::unique_ptr<Benchmark_data_source<Trade_sample>>> m_trade_sources;
    Brownian_generator m_generator;

    std::thread m_generator_thread;
    std::atomic<bool> m_stop_generator{false};

    vnm::plot::Plot_widget* m_plot = nullptr;
    std::vector<std::shared_ptr<vnm::plot::series_data_t>> m_series;

    vnm::plot::Plot_config m_render_config;

    std::int64_t m_t_min = 0;
    std::int64_t m_t_max = std::int64_t{10} * 1'000'000'000;
    std::int64_t m_t_available_min = 0;

    QTimer m_render_timer;
    QTimer m_benchmark_timer;
    std::chrono::steady_clock::time_point m_last_timer_tick;
    std::uint64_t m_timer_interval_count = 0;
    double m_timer_interval_total_ms = 0.0;
    double m_timer_interval_min_ms = std::numeric_limits<double>::max();
    double m_timer_interval_max_ms = 0.0;
    std::chrono::system_clock::time_point m_started_at;
    std::atomic<std::size_t> m_samples_generated{0};
    mutable std::mutex m_graphics_info_mutex;
    Graphics_device_info m_graphics_info;
    std::size_t m_measured_frame_count = 0;
    std::size_t m_warmup_frames_remaining = 0;
    bool m_measurement_finished = false;
};

/// Direct QRhi benchmark that renders into an offscreen target without Qt Quick.
class Benchmark_rhi_offscreen_runner
{
public:
    explicit Benchmark_rhi_offscreen_runner(const Benchmark_config& config);
    ~Benchmark_rhi_offscreen_runner();

    Benchmark_profiler& profiler() { return m_profiler; }
    const Benchmark_profiler& profiler() const { return m_profiler; }
    const Benchmark_config& config() const { return m_config; }
    std::size_t samples_generated() const { return m_samples_generated.load(); }
    std::chrono::system_clock::time_point started_at() const { return m_started_at; }
    Graphics_device_info graphics_device_info() const { return m_graphics_info; }
    std::size_t measured_frame_count() const { return m_measured_frame_count; }
    std::uint64_t pixel_checksum() const { return m_pixel_checksum; }
    std::size_t pixel_nonuniform_count() const { return m_pixel_nonuniform_count; }
    std::size_t stack_sum_pixel_count() const { return m_stack_sum_pixel_count; }
    std::string phase_trace_path() const { return m_phase_trace_path; }

    bool run(std::string& error_message);

private:
    void setup_data_source();
    void setup_rendering();
    void setup_series();
    void generator_thread_func();
    void stop_generator_thread();
    void pause_generator_publication();
    void resume_generator_publication();
    bool wait_for_generator_permission(
        Publication_rate_clock::duration& paused_duration);
    void fill_static_data();
    bool initialize_rhi(std::string& error_message);
    bool render_frame(std::string& error_message, bool measured);
    void record_final_statistics(double measured_seconds);
    void record_phase(const char* phase, std::size_t frame = 0) const;

    Benchmark_config m_config;
    Benchmark_profiler m_profiler;

    std::vector<std::unique_ptr<Ring_buffer<Bar_sample>>> m_bar_buffers;
    std::vector<std::unique_ptr<Ring_buffer<Trade_sample>>> m_trade_buffers;
    std::vector<std::unique_ptr<Benchmark_data_source<Bar_sample>>> m_bar_sources;
    std::vector<std::unique_ptr<Benchmark_data_source<Trade_sample>>> m_trade_sources;
    Brownian_generator m_generator;

    std::thread m_generator_thread;
    std::atomic<bool> m_stop_generator{false};
    std::atomic<bool> m_generator_pause_requested{false};
    std::atomic<bool> m_generator_rate_rebase_requested{false};
    std::mutex m_generator_control_mutex;
    std::condition_variable m_generator_control_cv;
    bool m_generator_paused = false;

    std::unique_ptr<QOffscreenSurface> m_fallback_surface;
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    std::unique_ptr<QVulkanInstance> m_vulkan_instance;
#endif
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiRenderBuffer> m_color_buffer;
    std::unique_ptr<QRhiTexture> m_resolve_texture;
    std::unique_ptr<QRhiTextureRenderTarget> m_render_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_render_pass_descriptor;

    std::unique_ptr<vnm::plot::detail::Plot_frame_renderer> m_frame_renderer;
    std::unique_ptr<vnm::plot::detail::Plot_render_snapshot> m_snapshot;

    std::chrono::system_clock::time_point m_started_at;
    std::atomic<std::size_t> m_samples_generated{0};
    Graphics_device_info m_graphics_info;
    std::size_t m_measured_frame_count = 0;
    double m_cold_setup_ms = 0.0;
    double m_cold_backend_ms = 0.0;
    double m_cold_first_submission_ms = 0.0;
    double m_cold_shader_pipeline_prepare_ms = 0.0;
    double m_cold_total_ms = 0.0;
    double m_last_prepare_ms = 0.0;
    std::uint64_t m_pixel_checksum = 0;
    std::size_t m_pixel_nonuniform_count = 0;
    std::size_t m_stack_sum_pixel_count = 0;
    std::chrono::steady_clock::time_point m_phase_trace_started;
    std::string m_phase_trace_path;
    mutable std::ofstream m_phase_trace;
};

}  // namespace vnm::benchmark

#endif  // VNM_PLOT_BENCHMARK_WINDOW_H
