// vnm_plot Benchmark - Window Implementation

#include "benchmark_window.h"
#include "allocation_tracker.h"
#include "path_io.h"
#include "plot_frame_renderer.h"

#include <vnm_plot/core/plot_config.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QOffscreenSurface>
#include <QQuickItem>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#endif

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

namespace vnm::benchmark {

namespace {

std::string device_type_name(QRhiDriverInfo::DeviceType type)
{
    switch (type) {
    case QRhiDriverInfo::IntegratedDevice: return "integrated";
    case QRhiDriverInfo::DiscreteDevice: return "discrete";
    case QRhiDriverInfo::ExternalDevice: return "external";
    case QRhiDriverInfo::VirtualDevice: return "virtual";
    case QRhiDriverInfo::CpuDevice: return "cpu";
    case QRhiDriverInfo::UnknownDevice: return "unknown";
    }
    return "unknown";
}

std::string frame_op_result_name(QRhi::FrameOpResult result)
{
    switch (result) {
    case QRhi::FrameOpSuccess: return "success";
    case QRhi::FrameOpError: return "error";
    case QRhi::FrameOpSwapChainOutOfDate: return "swap-chain-out-of-date";
    case QRhi::FrameOpDeviceLost: return "device-lost";
    }
    return "unknown";
}

std::string surface_format_identity(const QSurfaceFormat& format)
{
    const char* profile = "none";
    if (format.profile() == QSurfaceFormat::CoreProfile) {
        profile = "core";
    }
    else
    if (format.profile() == QSurfaceFormat::CompatibilityProfile) {
        profile = "compatibility";
    }
    return std::to_string(format.majorVersion()) + "." +
        std::to_string(format.minorVersion()) + "|" + profile + "|" +
        std::to_string(format.samples());
}

Graphics_device_info graphics_device_info_from_rhi(QRhi& rhi)
{
    const QRhiDriverInfo driver = rhi.driverInfo();
    Graphics_device_info info;
    info.backend = rhi.backendName();
    info.device_name = std::string(driver.deviceName.constData(), driver.deviceName.size());
    info.device_type = device_type_name(driver.deviceType);
    info.device_id = driver.deviceId;
    info.vendor_id = driver.vendorId;
    return info;
}

std::uint64_t process_memory_high_water_bytes()
{
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)))
    {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#elif defined(Q_OS_UNIX)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(Q_OS_MACOS)
        return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
#endif
    }
#endif
    return 0;
}

double steady_clock_resolution_ns()
{
    double resolution = std::numeric_limits<double>::max();
    auto previous = std::chrono::steady_clock::now();
    for (int sample = 0; sample < 10'000; ++sample) {
        const auto current = std::chrono::steady_clock::now();
        const double delta = std::chrono::duration<double, std::nano>(
            current - previous).count();
        if (delta > 0.0) {
            resolution = std::min(resolution, delta);
        }
        previous = current;
    }
    return std::isfinite(resolution) ? resolution : 0.0;
}

void ensure_interval_observations(Benchmark_profiler& profiler)
{
    profiler.ensure_observation("benchmark.producer.lock_wait_ns");
    profiler.ensure_observation("renderer.series_window.monotonicity_scan_count");
    profiler.ensure_observation("renderer.series_window.monotonicity_scan_samples");
    profiler.ensure_observation("renderer.frame.gpu_buffer_allocation_bytes");
    profiler.ensure_observation("renderer.frame.gpu_buffer_allocation_count");
    profiler.ensure_observation("benchmark.frame.cpu_allocation_bytes");
    profiler.ensure_observation("benchmark.frame.cpu_allocation_count");
}

template<typename T>
void record_ring_statistics(
    Benchmark_profiler& profiler,
    const std::vector<std::unique_ptr<Ring_buffer<T>>>& buffers,
    double measured_seconds)
{
    if (buffers.empty()) {
        return;
    }
    typename Ring_buffer<T>::Statistics total;
    std::uint64_t producer_wait_min_ns = std::numeric_limits<std::uint64_t>::max();
    for (const auto& buffer : buffers) {
        const auto stats = buffer->statistics();
        total.occupancy += stats.occupancy;
        total.high_water_occupancy += stats.high_water_occupancy;
        total.published_samples += stats.published_samples;
        total.overwritten_samples += stats.overwritten_samples;
        total.producer_wait_count += stats.producer_wait_count;
        total.producer_wait_total_ns += stats.producer_wait_total_ns;
        if (stats.producer_wait_count > 0) {
            producer_wait_min_ns = std::min(producer_wait_min_ns, stats.producer_wait_min_ns);
            total.producer_wait_max_ns = std::max(
                total.producer_wait_max_ns,
                stats.producer_wait_max_ns);
        }
    }
    profiler.record_observation("benchmark.ring.occupancy", static_cast<double>(total.occupancy));
    profiler.record_observation(
        "benchmark.ring.high_water_occupancy",
        static_cast<double>(total.high_water_occupancy));
    profiler.record_observation(
        "benchmark.ring.published_samples",
        static_cast<double>(total.published_samples));
    profiler.record_observation(
        "benchmark.ring.overwritten_samples",
        static_cast<double>(total.overwritten_samples));
    if (measured_seconds > 0.0) {
        profiler.record_observation(
            "benchmark.ring.published_samples_per_second",
            static_cast<double>(total.published_samples) / measured_seconds);
    }
    if (total.producer_wait_count > 0) {
        profiler.record_observation_summary(
            "benchmark.producer.lock_wait_ns",
            total.producer_wait_count,
            static_cast<double>(total.producer_wait_total_ns),
            static_cast<double>(producer_wait_min_ns),
            static_cast<double>(total.producer_wait_max_ns));
    }
}

template<typename T>
void reset_ring_measurement_statistics(
    std::vector<std::unique_ptr<Ring_buffer<T>>>& buffers)
{
    for (auto& buffer : buffers) {
        buffer->reset_measurement_statistics();
    }
}


std::string format_benchmark_timestamp(std::int64_t ts_ns, std::int64_t /*step_ns*/)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(ts_ns) / 1.0e9);
    return buf;
}

void update_time_range_from_source(
    vnm::plot::Data_source* source,
    const std::string& data_type,
    std::int64_t& t_min,
    std::int64_t& t_max,
    std::int64_t& t_available_min)
{
    if (!source) {
        return;
    }

    auto result = source->try_snapshot();
    if (result.status != vnm::plot::snapshot_result_t::Snapshot_status::READY) {
        return;
    }

    const auto& snapshot = result.snapshot;
    if (snapshot.count == 0) {
        return;
    }

    const void* first_sample = snapshot.at(0);
    const void* last_sample = snapshot.at(snapshot.count - 1);
    if (!first_sample || !last_sample) {
        return;
    }

    std::int64_t t_first = 0;
    std::int64_t t_last = 0;
    if (data_type == "Trades") {
        t_first = reinterpret_cast<const Trade_sample*>(first_sample)->timestamp;
        t_last = reinterpret_cast<const Trade_sample*>(last_sample)->timestamp;
    }
    else {
        t_first = reinterpret_cast<const Bar_sample*>(first_sample)->timestamp;
        t_last = reinterpret_cast<const Bar_sample*>(last_sample)->timestamp;
    }

    t_available_min = t_first;

    constexpr std::int64_t k_window_ns = std::int64_t{10} * 1'000'000'000;
    t_max = t_last;
    t_min = std::max(t_first, t_last - k_window_ns);
}

} // anonymous namespace


Benchmark_rhi_window::Benchmark_rhi_window(const Benchmark_config& config)
:
    QQuickWindow(),
    m_config(config),
    m_generator([&config]() {
        Brownian_generator::Config gen_config;
        gen_config.seed = config.seed;
        gen_config.volatility = config.volatility;
        gen_config.initial_price = 100.0;
        gen_config.time_step = 1.0 / config.rate;
        return gen_config;
    }())
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(0);
    setFormat(format);

    setTitle("vnm_plot Benchmark QRhi");
    resize(m_config.framebuffer_width, m_config.framebuffer_height);

    if (m_config.data_type == "Trades") {
        m_trade_buffers.reserve(m_config.series_count);
        m_trade_sources.reserve(m_config.series_count);
        for (std::size_t index = 0; index < m_config.series_count; ++index) {
            auto buffer = std::make_unique<Ring_buffer<Trade_sample>>(m_config.ring_capacity);
            buffer->set_profiler(&m_profiler);
            auto source = std::make_unique<Benchmark_data_source<Trade_sample>>(*buffer);
            source->set_profiler(&m_profiler);
            m_trade_buffers.push_back(std::move(buffer));
            m_trade_sources.push_back(std::move(source));
        }
    }
    else {
        m_bar_buffers.reserve(m_config.series_count);
        m_bar_sources.reserve(m_config.series_count);
        for (std::size_t index = 0; index < m_config.series_count; ++index) {
            auto buffer = std::make_unique<Ring_buffer<Bar_sample>>(m_config.ring_capacity);
            buffer->set_profiler(&m_profiler);
            auto source = std::make_unique<Benchmark_data_source<Bar_sample>>(*buffer);
            source->set_profiler(&m_profiler);
            m_bar_buffers.push_back(std::move(buffer));
            m_bar_sources.push_back(std::move(source));
        }
    }

    m_render_config.dark_mode = true;
    m_render_config.show_text = m_config.show_text;
    m_render_config.snap_lines_to_pixels = false;
    m_render_config.line_width_px = m_config.line_width_px;
    m_render_config.point_diameter_px = m_config.point_diameter_px;
    m_render_config.font_size_px = k_adjusted_font_px;
    m_render_config.base_label_height_px = k_base_label_height_px;
    m_render_config.preview_height_px = k_adjusted_preview_height;
    m_render_config.format_timestamp = format_benchmark_timestamp;
    m_render_config.profiler =
        std::shared_ptr<vnm::plot::Profiler>(&m_profiler, [](vnm::plot::Profiler*) {});

    m_plot = new vnm::plot::Plot_widget();
    m_plot->setParentItem(contentItem());
    m_plot->setWidth(width());
    m_plot->setHeight(height());
    m_plot->set_config(m_render_config);
    m_plot->set_grid_visibility(1.0);
    m_plot->set_preview_visibility(1.0);
    m_plot->set_line_width_px(m_config.line_width_px);
    m_plot->set_preview_height(k_adjusted_preview_height);
    m_plot->set_vbar_width(k_vbar_width_pixels);
    m_plot->set_visible_info(vnm::plot::k_visible_info_none);
    m_plot->set_v_auto(true);

    connect(this, &QQuickWindow::widthChanged, this, [this](int w) {
        m_plot->setWidth(w);
    });
    connect(this, &QQuickWindow::heightChanged, this, [this](int h) {
        m_plot->setHeight(h);
    });
    connect(&m_render_timer, &QTimer::timeout, this, &Benchmark_rhi_window::on_render_timer);
    connect(&m_benchmark_timer, &QTimer::timeout, this, &Benchmark_rhi_window::on_benchmark_timeout);
    connect(
        this,
        &QQuickWindow::sceneGraphInitialized,
        this,
        [this]() {
            auto* rhi = static_cast<QRhi*>(rendererInterface()->getResource(
                this,
                QSGRendererInterface::RhiResource));
            if (!rhi) {
                return;
            }
            std::lock_guard<std::mutex> lock(m_graphics_info_mutex);
            m_graphics_info = graphics_device_info_from_rhi(*rhi);
        },
        Qt::DirectConnection);

    setup_series();

    if (m_config.static_data) {
        fill_static_data();
    }
    else {
        m_stop_generator.store(false);
        m_generator_thread = std::thread(&Benchmark_rhi_window::generator_thread_func, this);
    }

    m_warmup_frames_remaining = m_config.warmup_frames;
    if (m_warmup_frames_remaining == 0) {
        m_started_at = std::chrono::system_clock::now();
        if (m_config.measured_frames == 0) {
            m_benchmark_timer.setSingleShot(true);
            m_benchmark_timer.start(static_cast<int>(m_config.duration_seconds * 1000));
        }
    }
    m_render_timer.start(0);
}

Benchmark_rhi_window::~Benchmark_rhi_window()
{
    stop_generator_thread();
    delete m_plot;
    m_plot = nullptr;
}

Graphics_device_info Benchmark_rhi_window::graphics_device_info() const
{
    std::lock_guard<std::mutex> lock(m_graphics_info_mutex);
    return m_graphics_info;
}

void Benchmark_rhi_window::stop_generator_thread()
{
    if (m_generator_thread.joinable()) {
        m_stop_generator.store(true);
        m_generator_thread.join();
    }
}

void Benchmark_rhi_window::generator_thread_func()
{
    const double samples_per_second = m_config.rate;
    const double ns_per_sample = 1e9 / samples_per_second;

    auto start_time = std::chrono::steady_clock::now();

    while (!m_stop_generator.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(now - start_time).count();
        double target_samples = elapsed_ns / ns_per_sample;
        std::size_t current_count = m_samples_generated.load();

        while (current_count < static_cast<std::size_t>(target_samples) && !m_stop_generator.load()) {
            if (m_config.data_type == "Trades") {
                const Trade_sample sample = m_generator.next_trade();
                for (auto& buffer : m_trade_buffers) {
                    buffer->push(sample);
                }
            }
            else {
                const Bar_sample sample = m_generator.next_bar();
                for (auto& buffer : m_bar_buffers) {
                    buffer->push(sample);
                }
            }
            ++m_samples_generated;
            ++current_count;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void Benchmark_rhi_window::fill_static_data()
{
    if (m_config.data_type == "Trades") {
        std::vector<Trade_sample> samples(m_config.static_sample_count);
        m_generator.generate_trades(samples.data(), samples.size());
        for (auto& buffer : m_trade_buffers) {
            buffer->push_batch(samples.data(), samples.size());
        }
    }
    else {
        std::vector<Bar_sample> samples(m_config.static_sample_count);
        m_generator.generate_bars(samples.data(), samples.size());
        for (auto& buffer : m_bar_buffers) {
            buffer->push_batch(samples.data(), samples.size());
        }
    }
    m_samples_generated.store(m_config.static_sample_count);
}

void Benchmark_rhi_window::setup_series()
{
    const std::string& style = !m_config.style.empty()
        ? m_config.style
        : (m_config.data_type == "Trades" ? std::string("Dots") : std::string("Area"));
    m_series.reserve(m_config.series_count);
    for (std::size_t index = 0; index < m_config.series_count; ++index) {
        auto series = std::make_shared<vnm::plot::series_data_t>();
        series->enabled = true;
        const float blend = static_cast<float>(index % 8) / 8.0f;
        series->color = glm::vec4(0.2f + 0.5f * blend, 0.7f, 0.9f - 0.5f * blend, 1.0f);

        if (m_config.data_type == "Trades") {
            series->set_data_source_ref(*m_trade_sources[index]);
            series->access = make_trade_access_policy();
        }
        else {
            series->set_data_source_ref(*m_bar_sources[index]);
            series->access = make_bar_access_policy();
        }

        if (style == "Dots") {
            series->style = vnm::plot::Display_style::DOTS;
        }
        else
        if (style == "Line") {
            series->style = vnm::plot::Display_style::LINE;
        }
        else {
            series->style = vnm::plot::Display_style::AREA;
        }

        m_plot->add_series(static_cast<int>(index + 1), series);
        m_series.push_back(std::move(series));
    }
}

void Benchmark_rhi_window::update_plot_view()
{
    vnm::plot::Data_source* source = m_config.data_type == "Trades"
        ? static_cast<vnm::plot::Data_source*>(m_trade_sources.front().get())
        : static_cast<vnm::plot::Data_source*>(m_bar_sources.front().get());

    update_time_range_from_source(
        source,
        m_config.data_type,
        m_t_min,
        m_t_max,
        m_t_available_min);

    vnm::plot::Plot_view view;
    view.t_range = std::pair<qint64, qint64>{m_t_min, m_t_max};
    view.t_available_range = std::pair<qint64, qint64>{m_t_available_min, m_t_max};
    view.v_auto = true;
    m_plot->set_view(view);
}

void Benchmark_rhi_window::on_render_timer()
{
    if (m_measurement_finished) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_last_timer_tick.time_since_epoch().count() != 0) {
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(now - m_last_timer_tick).count();
        m_timer_interval_count++;
        m_timer_interval_total_ms += elapsed_ms;
        m_timer_interval_min_ms = std::min(m_timer_interval_min_ms, elapsed_ms);
        m_timer_interval_max_ms = std::max(m_timer_interval_max_ms, elapsed_ms);
        if (m_warmup_frames_remaining == 0) {
            m_profiler.record_observation("benchmark.frame.total_ms", elapsed_ms);
        }
    }
    m_last_timer_tick = now;

    update_plot_view();
    m_plot->update();
    update();

    if (m_warmup_frames_remaining > 0) {
        --m_warmup_frames_remaining;
        if (m_warmup_frames_remaining == 0) {
            reset_ring_measurement_statistics(m_bar_buffers);
            reset_ring_measurement_statistics(m_trade_buffers);
            m_profiler.reset();
            m_started_at = std::chrono::system_clock::now();
            m_last_timer_tick = {};
            m_timer_interval_count = 0;
            m_timer_interval_total_ms = 0.0;
            m_timer_interval_min_ms = std::numeric_limits<double>::max();
            m_timer_interval_max_ms = 0.0;
            if (m_config.measured_frames == 0) {
                m_benchmark_timer.setSingleShot(true);
                m_benchmark_timer.start(static_cast<int>(m_config.duration_seconds * 1000));
            }
        }
        return;
    }

    ++m_measured_frame_count;
    m_profiler.record_counter("benchmark.frame.output_count");
    if (m_config.measured_frames > 0 &&
        m_measured_frame_count >= m_config.measured_frames)
    {
        on_benchmark_timeout();
    }
}

void Benchmark_rhi_window::on_benchmark_timeout()
{
    if (m_measurement_finished) {
        return;
    }
    m_measurement_finished = true;
    stop_generator_thread();
    m_render_timer.stop();
    m_benchmark_timer.stop();
    m_profiler.record_observation_summary(
        "qrhi.scheduler.timer_interval",
        m_timer_interval_count,
        m_timer_interval_total_ms,
        m_timer_interval_min_ms,
        m_timer_interval_max_ms);
    record_final_statistics();
    emit benchmark_finished();
}

void Benchmark_rhi_window::record_final_statistics()
{
    const double measured_seconds = m_timer_interval_total_ms > 0.0
        ? m_timer_interval_total_ms / 1000.0
        : m_config.duration_seconds;
    record_ring_statistics(m_profiler, m_bar_buffers, measured_seconds);
    record_ring_statistics(m_profiler, m_trade_buffers, measured_seconds);
    m_profiler.record_observation(
        "benchmark.memory.process_high_water_bytes",
        static_cast<double>(process_memory_high_water_bytes()));
    ensure_interval_observations(m_profiler);
}

Benchmark_rhi_offscreen_runner::Benchmark_rhi_offscreen_runner(const Benchmark_config& config)
:
    m_config(config),
    m_generator([&config]() {
        Brownian_generator::Config gen_config;
        gen_config.seed = config.seed;
        gen_config.volatility = config.volatility;
        gen_config.initial_price = 100.0;
        gen_config.time_step = 1.0 / config.rate;
        return gen_config;
    }())
{
    m_frame_renderer = std::make_unique<vnm::plot::detail::Plot_frame_renderer>();
    m_snapshot = std::make_unique<vnm::plot::detail::Plot_render_snapshot>();
}

Benchmark_rhi_offscreen_runner::~Benchmark_rhi_offscreen_runner()
{
    stop_generator_thread();
}

void Benchmark_rhi_offscreen_runner::setup_data_source()
{
    if (m_config.data_type == "Trades") {
        m_trade_buffers.reserve(m_config.series_count);
        m_trade_sources.reserve(m_config.series_count);
        for (std::size_t index = 0; index < m_config.series_count; ++index) {
            auto buffer = std::make_unique<Ring_buffer<Trade_sample>>(m_config.ring_capacity);
            buffer->set_profiler(&m_profiler);
            auto source = std::make_unique<Benchmark_data_source<Trade_sample>>(*buffer);
            source->set_profiler(&m_profiler);
            m_trade_buffers.push_back(std::move(buffer));
            m_trade_sources.push_back(std::move(source));
        }
    }
    else {
        m_bar_buffers.reserve(m_config.series_count);
        m_bar_sources.reserve(m_config.series_count);
        for (std::size_t index = 0; index < m_config.series_count; ++index) {
            auto buffer = std::make_unique<Ring_buffer<Bar_sample>>(m_config.ring_capacity);
            buffer->set_profiler(&m_profiler);
            auto source = std::make_unique<Benchmark_data_source<Bar_sample>>(*buffer);
            source->set_profiler(&m_profiler);
            m_bar_buffers.push_back(std::move(buffer));
            m_bar_sources.push_back(std::move(source));
        }
    }
}

void Benchmark_rhi_offscreen_runner::setup_rendering()
{
    m_frame_renderer->initialize();
    m_snapshot->adjusted_font_px        = k_adjusted_font_px;
    m_snapshot->base_label_height_px    = k_base_label_height_px;
    m_snapshot->adjusted_preview_height = k_adjusted_preview_height;
    m_snapshot->vbar_width_pixels       = k_vbar_width_pixels;

    m_snapshot->config.dark_mode = true;
    m_snapshot->config.show_text =
#if defined(VNM_PLOT_ENABLE_TEXT)
        m_config.show_text;
#else
        false;
#endif
    m_snapshot->config.snap_lines_to_pixels = false;
    m_snapshot->config.line_width_px = m_config.line_width_px;
    m_snapshot->config.point_diameter_px = m_config.point_diameter_px;
    m_snapshot->config.font_size_px = k_adjusted_font_px;
    m_snapshot->config.base_label_height_px = k_base_label_height_px;
    m_snapshot->config.preview_height_px = k_adjusted_preview_height;
    m_snapshot->config.format_timestamp = format_benchmark_timestamp;
    m_snapshot->config.profiler =
        std::shared_ptr<vnm::plot::Profiler>(&m_profiler, [](vnm::plot::Profiler*) {});
}

void Benchmark_rhi_offscreen_runner::setup_series()
{
    const std::string& style = !m_config.style.empty()
        ? m_config.style
        : (m_config.data_type == "Trades" ? std::string("Dots") : std::string("Area"));
    for (std::size_t index = 0; index < m_config.series_count; ++index) {
        auto series = std::make_shared<vnm::plot::series_data_t>();
        series->enabled = true;
        series->stack_group = m_config.stack_series ? 1 : 0;
        const float blend = static_cast<float>(index % 8) / 8.0f;
        series->color = glm::vec4(0.2f + 0.5f * blend, 0.7f, 0.9f - 0.5f * blend, 1.0f);

        if (m_config.data_type == "Trades") {
            series->set_data_source_ref(*m_trade_sources[index]);
            series->access = make_trade_access_policy();
        }
        else {
            series->set_data_source_ref(*m_bar_sources[index]);
            series->access = make_bar_access_policy();
        }

        if (style == "Dots") {
            series->style = vnm::plot::Display_style::DOTS;
        }
        else
        if (style == "Line") {
            series->style = vnm::plot::Display_style::LINE;
        }
        else {
            series->style = vnm::plot::Display_style::AREA;
        }

        m_snapshot->series[static_cast<int>(index + 1)] = std::move(series);
    }
}

void Benchmark_rhi_offscreen_runner::generator_thread_func()
{
    Publication_rate_clock rate_clock;

    while (!m_stop_generator.load()) {
        Publication_rate_clock::duration paused_duration{};
        if (!wait_for_generator_permission(paused_duration)) {
            break;
        }
        rate_clock.exclude_pause(paused_duration);
        auto now = std::chrono::steady_clock::now();
        if (m_generator_rate_rebase_requested.exchange(false, std::memory_order_acq_rel)) {
            rate_clock.rebase(now, m_samples_generated.load());
        }
        std::size_t target_samples = rate_clock.target_samples(now, m_config.rate);
        std::size_t current_count = m_samples_generated.load();

        while (current_count < target_samples && !m_stop_generator.load()) {
            paused_duration = {};
            if (!wait_for_generator_permission(paused_duration)) {
                break;
            }
            rate_clock.exclude_pause(paused_duration);
            now = std::chrono::steady_clock::now();
            if (m_generator_rate_rebase_requested.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                current_count = m_samples_generated.load();
                rate_clock.rebase(now, current_count);
                target_samples = rate_clock.target_samples(now, m_config.rate);
                continue;
            }
            if (m_config.data_type == "Trades") {
                const Trade_sample sample = m_generator.next_trade();
                for (auto& buffer : m_trade_buffers) {
                    buffer->push(sample);
                }
            }
            else {
                const Bar_sample sample = m_generator.next_bar();
                for (auto& buffer : m_bar_buffers) {
                    buffer->push(sample);
                }
            }
            ++m_samples_generated;
            ++current_count;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void Benchmark_rhi_offscreen_runner::stop_generator_thread()
{
    if (m_generator_thread.joinable()) {
        m_stop_generator.store(true);
        m_generator_pause_requested.store(false);
        m_generator_control_cv.notify_all();
        m_generator_thread.join();
    }
}

void Benchmark_rhi_offscreen_runner::pause_generator_publication()
{
    if (!m_generator_thread.joinable()) {
        return;
    }
    m_generator_pause_requested.store(true, std::memory_order_release);
    m_generator_control_cv.notify_all();
    std::unique_lock lock(m_generator_control_mutex);
    m_generator_control_cv.wait(lock, [&]() {
        return m_generator_paused || m_stop_generator.load();
    });
}

void Benchmark_rhi_offscreen_runner::resume_generator_publication()
{
    if (!m_generator_thread.joinable()) {
        return;
    }
    m_generator_rate_rebase_requested.store(true, std::memory_order_release);
    m_generator_pause_requested.store(false, std::memory_order_release);
    m_generator_control_cv.notify_all();
    std::unique_lock lock(m_generator_control_mutex);
    m_generator_control_cv.wait(lock, [&]() {
        return !m_generator_paused || m_stop_generator.load();
    });
}

bool Benchmark_rhi_offscreen_runner::wait_for_generator_permission(
    Publication_rate_clock::duration& paused_duration)
{
    if (!m_generator_pause_requested.load(std::memory_order_acquire)) {
        return !m_stop_generator.load();
    }
    const auto pause_started = Publication_rate_clock::clock::now();
    std::unique_lock lock(m_generator_control_mutex);
    m_generator_paused = true;
    m_generator_control_cv.notify_all();
    m_generator_control_cv.wait(lock, [&]() {
        return !m_generator_pause_requested.load(std::memory_order_acquire) ||
            m_stop_generator.load();
    });
    m_generator_paused = false;
    m_generator_control_cv.notify_all();
    paused_duration += Publication_rate_clock::clock::now() - pause_started;
    return !m_stop_generator.load();
}

void Benchmark_rhi_offscreen_runner::fill_static_data()
{
    if (m_config.data_type == "Trades") {
        std::vector<Trade_sample> samples(m_config.static_sample_count);
        m_generator.generate_trades(samples.data(), samples.size());
        const std::int64_t timestamp_offset =
            (samples[1].timestamp - samples[0].timestamp) /
            static_cast<std::int64_t>(m_trade_buffers.size());
        for (std::size_t index = 0; index < m_trade_buffers.size(); ++index) {
            auto& buffer = m_trade_buffers[index];
            buffer->push_batch(samples.data(), samples.size());
            if (m_config.stack_series && index + 1 < m_trade_buffers.size()) {
                for (auto& sample : samples) {
                    sample.timestamp += timestamp_offset;
                }
            }
        }
    }
    else {
        std::vector<Bar_sample> samples(m_config.static_sample_count);
        m_generator.generate_bars(samples.data(), samples.size());
        const std::int64_t timestamp_offset =
            (samples[1].timestamp - samples[0].timestamp) /
            static_cast<std::int64_t>(m_bar_buffers.size());
        for (std::size_t index = 0; index < m_bar_buffers.size(); ++index) {
            auto& buffer = m_bar_buffers[index];
            buffer->push_batch(samples.data(), samples.size());
            if (m_config.stack_series && index + 1 < m_bar_buffers.size()) {
                for (auto& sample : samples) {
                    sample.timestamp += timestamp_offset;
                }
            }
        }
    }
    m_samples_generated.store(m_config.static_sample_count);
}

bool Benchmark_rhi_offscreen_runner::initialize_rhi(std::string& error_message)
{
    std::string fallback_surface_requested_format = "not-applicable";
    std::string fallback_surface_resolved_format = "not-applicable";
    const auto create_null_rhi = [&]() {
        QRhiNullInitParams params;
        m_rhi.reset(QRhi::create(QRhi::Null, &params));
        return m_rhi != nullptr;
    };

    const auto create_opengl_rhi = [&]() {
#if QT_CONFIG(opengl)
        QRhiGles2InitParams params;
        fallback_surface_requested_format = surface_format_identity(params.format);
        m_fallback_surface.reset(QRhiGles2InitParams::newFallbackSurface());
        if (!m_fallback_surface || !m_fallback_surface->isValid()) {
            error_message = "cold.fallback_surface: failed to create a valid offscreen surface";
            m_fallback_surface.reset();
            return false;
        }
        fallback_surface_resolved_format = surface_format_identity(m_fallback_surface->format());
        params.fallbackSurface = m_fallback_surface.get();
        m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
        if (!m_rhi) {
            m_fallback_surface.reset();
        }
        return m_rhi != nullptr;
#else
        return false;
#endif
    };

    const auto create_d3d11_rhi = [&]() {
#ifdef Q_OS_WIN
        QRhiD3D11InitParams params;
        m_rhi.reset(QRhi::create(QRhi::D3D11, &params));
        return m_rhi != nullptr;
#else
        return false;
#endif
    };

    const auto create_metal_rhi = [&]() {
#if defined(Q_OS_MACOS) && QT_CONFIG(metal)
        QRhiMetalInitParams params;
        m_rhi.reset(QRhi::create(QRhi::Metal, &params));
        return m_rhi != nullptr;
#else
        return false;
#endif
    };

    const auto create_vulkan_rhi = [&]() {
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
        m_vulkan_instance = std::make_unique<QVulkanInstance>();
        if (!m_vulkan_instance->create()) {
            m_vulkan_instance.reset();
            return false;
        }
        QRhiVulkanInitParams params;
        params.inst = m_vulkan_instance.get();
        m_rhi.reset(QRhi::create(QRhi::Vulkan, &params));
        if (!m_rhi) {
            m_vulkan_instance.reset();
        }
        return m_rhi != nullptr;
#else
        return false;
#endif
    };

    const std::string& requested = m_config.graphics_backend;
    bool created = false;
    if (requested == "null") {
        created = create_null_rhi();
    }
    else
    if (requested == "opengl") {
        created = create_opengl_rhi();
    }
    else
    if (requested == "d3d11") {
        created = create_d3d11_rhi();
    }
    else
    if (requested == "metal") {
        created = create_metal_rhi();
    }
    else
    if (requested == "vulkan") {
        created = create_vulkan_rhi();
    }
    else
    if (requested == "native") {
#if defined(Q_OS_WIN)
        created = create_d3d11_rhi();
#elif defined(Q_OS_MACOS)
        created = create_metal_rhi();
#else
        created = create_opengl_rhi();
#endif
    }

    if (!created || !m_rhi) {
        error_message = "cold.backend_create: failed to create requested offscreen QRhi backend '" +
            requested + "'";
        return false;
    }
    if (m_rhi->backend() == QRhi::Null && requested != "null") {
        error_message = "cold.backend_validate: requested '" + requested +
            "' but QRhi created the Null backend";
        return false;
    }

    m_graphics_info = graphics_device_info_from_rhi(*m_rhi);
    m_graphics_info.fallback_surface_requested_format =
        fallback_surface_requested_format;
    m_graphics_info.fallback_surface_resolved_format =
        fallback_surface_resolved_format;

    const QSize size(m_config.framebuffer_width, m_config.framebuffer_height);
    const int sample_count = static_cast<int>(m_config.sample_count);
    m_color_buffer.reset(m_rhi->newRenderBuffer(
        QRhiRenderBuffer::Color,
        size,
        sample_count,
        {},
        QRhiTexture::RGBA8));
    if (!m_color_buffer) {
        error_message = "Failed to allocate QRhi offscreen color buffer";
        return false;
    }
    if (!m_color_buffer->create()) {
        error_message = "Failed to create QRhi offscreen color buffer";
        return false;
    }

    if (m_config.capture_pixel_checksum) {
        m_resolve_texture.reset(m_rhi->newTexture(
            QRhiTexture::RGBA8,
            size,
            1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_resolve_texture || !m_resolve_texture->create()) {
            error_message = "cold.render_target: failed to create pixel readback resolve texture";
            return false;
        }
    }

    QRhiColorAttachment color_attachment;
    color_attachment.setRenderBuffer(m_color_buffer.get());
    if (m_resolve_texture) {
        color_attachment.setResolveTexture(m_resolve_texture.get());
    }
    QRhiTextureRenderTargetDescription rt_desc(color_attachment);
    m_render_target.reset(m_rhi->newTextureRenderTarget(rt_desc));
    if (!m_render_target) {
        error_message = "Failed to allocate QRhi offscreen render target";
        return false;
    }
    m_render_pass_descriptor.reset(m_render_target->newCompatibleRenderPassDescriptor());
    if (!m_render_pass_descriptor) {
        error_message = "Failed to allocate QRhi offscreen render pass descriptor";
        return false;
    }
    m_render_target->setRenderPassDescriptor(m_render_pass_descriptor.get());
    if (!m_render_target->create()) {
        error_message = "Failed to create QRhi offscreen render target";
        return false;
    }
    return true;
}

bool Benchmark_rhi_offscreen_runner::render_frame(
    std::string& error_message,
    bool measured)
{
    Thread_allocation_scope allocation_scope(measured);
    const auto frame_started = std::chrono::steady_clock::now();
    const auto submission_started = frame_started;
    QRhiCommandBuffer* cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        error_message = "QRhi beginOffscreenFrame failed";
        return false;
    }

    vnm::plot::Data_source* source = m_config.data_type == "Trades"
        ? static_cast<vnm::plot::Data_source*>(m_trade_sources.front().get())
        : static_cast<vnm::plot::Data_source*>(m_bar_sources.front().get());
    {
        VNM_PLOT_PROFILE_SCOPE(&m_profiler, "benchmark.update_time_range");
        update_time_range_from_source(
            source,
            m_config.data_type,
            m_snapshot->data_cfg.t_min,
            m_snapshot->data_cfg.t_max,
            m_snapshot->data_cfg.t_available_min);
        m_snapshot->data_cfg.t_available_max = m_snapshot->data_cfg.t_max;
    }

    m_frame_renderer->render(*m_snapshot, m_rhi.get(), m_render_target.get(), cb);
    m_last_prepare_ms = m_profiler.last_series_prepare_ms();
    if (measured) {
        m_profiler.record_observation("benchmark.planning.time_ms", m_last_prepare_ms);
    }

    QRhiReadbackResult readback;
    if (measured && m_config.capture_pixel_checksum && m_resolve_texture) {
        QRhiResourceUpdateBatch* readback_updates = m_rhi->nextResourceUpdateBatch();
        readback_updates->readBackTexture(
            QRhiReadbackDescription(m_resolve_texture.get()),
            &readback);
        cb->resourceUpdate(readback_updates);
    }

    const QRhi::FrameOpResult end_result = m_rhi->endOffscreenFrame();
    if (end_result != QRhi::FrameOpSuccess) {
        error_message = "measure.end_offscreen_frame: " + frame_op_result_name(end_result);
        return false;
    }
    if (measured) {
        const double submission_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - submission_started).count();
        m_profiler.record_observation("benchmark.frame.submission_ms", submission_ms);
    }
    if (m_config.finish || (measured && m_config.capture_pixel_checksum)) {
        VNM_PLOT_PROFILE_SCOPE(&m_profiler, "renderer.frame.finish");
        const auto finish_started = std::chrono::steady_clock::now();
        const QRhi::FrameOpResult finish_result = m_rhi->finish();
        if (finish_result != QRhi::FrameOpSuccess) {
            error_message = "measure.finish: " + frame_op_result_name(finish_result);
            return false;
        }
        if (measured) {
            const double finish_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - finish_started).count();
            m_profiler.record_observation("benchmark.frame.gpu_finish_ms", finish_ms);
        }
    }
    if (measured && m_config.capture_pixel_checksum) {
        if (readback.data.isEmpty()) {
            error_message = "measure.pixel_readback: QRhi returned no pixel data";
            return false;
        }
        std::uint64_t checksum = 14'695'981'039'346'656'037ull;
        for (const unsigned char byte : readback.data) {
            checksum ^= byte;
            checksum *= 1'099'511'628'211ull;
        }
        m_pixel_checksum = checksum;
        m_pixel_nonuniform_count = 0;
        m_stack_sum_pixel_count = 0;
        if (readback.data.size() >= 4) {
            const auto* pixels = reinterpret_cast<const unsigned char*>(
                readback.data.constData());
            for (qsizetype offset = 4; offset + 4 <= readback.data.size(); offset += 4) {
                if (std::memcmp(pixels, pixels + offset, 4) != 0) {
                    ++m_pixel_nonuniform_count;
                }
            }
            for (qsizetype offset = 0; offset + 4 <= readback.data.size(); offset += 4) {
                const bool rgba =
                    pixels[offset]     == 230 &&
                    pixels[offset + 1] == 223 &&
                    pixels[offset + 2] == 204;
                const bool bgra =
                    pixels[offset]     == 204 &&
                    pixels[offset + 1] == 223 &&
                    pixels[offset + 2] == 230;
                if (rgba || bgra) {
                    ++m_stack_sum_pixel_count;
                }
            }
        }
        m_profiler.record_observation(
            "benchmark.frame.pixel_readback_bytes",
            static_cast<double>(readback.data.size()));
        m_profiler.record_observation(
            "benchmark.frame.pixel_checksum_low32",
            static_cast<double>(checksum & 0xffff'ffffull));
        m_profiler.record_observation(
            "benchmark.frame.pixel_nonuniform_count",
            static_cast<double>(m_pixel_nonuniform_count));
        m_profiler.record_observation(
            "benchmark.frame.stack_sum_pixel_count",
            static_cast<double>(m_stack_sum_pixel_count));
    }
    if (measured) {
        const Thread_allocation_measurement allocations = allocation_scope.finish();
        const double frame_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - frame_started).count();
        m_profiler.record_observation(
            "benchmark.frame.cpu_allocation_count",
            static_cast<double>(allocations.count));
        m_profiler.record_observation(
            "benchmark.frame.cpu_allocation_bytes",
            static_cast<double>(allocations.bytes));
        m_profiler.record_observation("benchmark.frame.total_ms", frame_ms);
        m_profiler.record_counter("benchmark.frame.output_count");
        ++m_measured_frame_count;
    }
    return true;
}

bool Benchmark_rhi_offscreen_runner::run(std::string& error_message)
{
    m_phase_trace_started = std::chrono::steady_clock::now();
    const auto trace_identity = std::chrono::system_clock::now().time_since_epoch().count();
    m_phase_trace_path = (
        std::filesystem::path(m_config.output_directory) /
        ("benchmark_phase_trace_" + std::to_string(trace_identity) + "_" +
            std::to_string(QCoreApplication::applicationPid()) + ".jsonl")).string();
    m_phase_trace.open(
        path_for_file_io(m_phase_trace_path),
        std::ios::out | std::ios::trunc);
    if (!m_phase_trace) {
        error_message = "phase_trace.open: failed to open " + m_phase_trace_path;
        return false;
    }
    error_message.reserve(384);
    record_phase("cold.setup.begin");
    const auto cold_started = std::chrono::steady_clock::now();
    const auto setup_started = cold_started;
    setup_data_source();
    setup_rendering();
    setup_series();
    record_phase("cold.setup.end");
    m_cold_setup_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - setup_started).count();

    const auto backend_started = std::chrono::steady_clock::now();
    record_phase("cold.backend_create.begin");
    try {
        if (!initialize_rhi(error_message)) {
            record_phase("cold.backend_create.failed");
            return false;
        }
    }
    catch (const std::bad_alloc&) {
        record_phase("cold.backend_create.failed");
        error_message = "cold.backend_create: std::bad_alloc";
        return false;
    }
    record_phase("cold.backend_create.end");
    m_cold_backend_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - backend_started).count();

    if (m_config.static_data) {
        fill_static_data();
    }
    else {
        m_stop_generator.store(false);
        m_generator_thread = std::thread(&Benchmark_rhi_offscreen_runner::generator_thread_func, this);
    }

    record_phase("warmup.begin");
    for (std::size_t frame = 0; frame < m_config.warmup_frames; ++frame) {
        const auto warmup_started = std::chrono::steady_clock::now();
        if (!render_frame(error_message, false)) {
            record_phase("warmup.frame.failed", frame);
            error_message = "warmup.frame: " + error_message;
            stop_generator_thread();
            return false;
        }
        if (frame == 0) {
            m_cold_first_submission_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - warmup_started).count();
            m_cold_shader_pipeline_prepare_ms = m_last_prepare_ms;
        }
    }
    record_phase("warmup.end");

    m_cold_total_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cold_started).count();
    const double clock_resolution_ns = steady_clock_resolution_ns();
    pause_generator_publication();
    reset_ring_measurement_statistics(m_bar_buffers);
    reset_ring_measurement_statistics(m_trade_buffers);
    m_profiler.reset();
    m_profiler.record_observation("benchmark.cold.setup_ms", m_cold_setup_ms);
    m_profiler.record_observation("benchmark.cold.backend_create_ms", m_cold_backend_ms);
    if (m_config.warmup_frames > 0) {
        m_profiler.record_observation(
            "benchmark.cold.first_submission_ms",
            m_cold_first_submission_ms);
        m_profiler.record_observation(
            "benchmark.cold.series_shader_pipeline_prepare_ms",
            m_cold_shader_pipeline_prepare_ms);
    }
    m_profiler.record_observation("benchmark.cold.total_ms", m_cold_total_ms);
    m_profiler.record_observation(
        "benchmark.clock.steady_resolution_ns",
        clock_resolution_ns);
    m_profiler.record_observation(
        "benchmark.warmup.frame_count",
        static_cast<double>(m_config.warmup_frames));

    record_phase("measure.begin");
    m_started_at = std::chrono::system_clock::now();
    const auto measured_started = std::chrono::steady_clock::now();
    resume_generator_publication();
    const auto duration = std::chrono::duration<double>(m_config.duration_seconds);

    const auto render_measured_frame = [&](std::size_t frame) {
        if (render_frame(error_message, true)) {
            return true;
        }
        record_phase("measure.frame.failed", frame);
        error_message = "measure.frame: " + error_message;
        return false;
    };

    if (m_config.measured_frames > 0) {
        for (std::size_t frame = 0; frame < m_config.measured_frames; ++frame) {
            if (!render_measured_frame(frame)) {
                stop_generator_thread();
                return false;
            }
        }
    }
    else {
        std::size_t frame = 0;
        while (std::chrono::steady_clock::now() - measured_started < duration) {
            if (!render_measured_frame(frame++)) {
                stop_generator_thread();
                return false;
            }
        }
    }

    pause_generator_publication();
    const auto measured_ended = std::chrono::steady_clock::now();
    record_phase("measure.end");
    const double measured_seconds = std::chrono::duration<double>(
        measured_ended - measured_started).count();
    record_final_statistics(measured_seconds);

    const auto shutdown_started = std::chrono::steady_clock::now();
    record_phase("shutdown.generator.begin");
    stop_generator_thread();
    record_phase("shutdown.generator.end");
    const double shutdown_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - shutdown_started).count();
    m_profiler.record_observation("benchmark.shutdown.generator_ms", shutdown_ms);
    record_phase("complete");
    return true;
}

void Benchmark_rhi_offscreen_runner::record_phase(
    const char* phase,
    std::size_t frame) const
{
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - m_phase_trace_started).count();
    if (m_phase_trace) {
        m_phase_trace << "{\"elapsed_ms\":" << std::setprecision(17) << elapsed_ms
                      << ",\"phase\":\"" << phase << "\",\"frame\":" << frame << "}\n";
        m_phase_trace.flush();
    }
}

void Benchmark_rhi_offscreen_runner::record_final_statistics(double measured_seconds)
{
    record_ring_statistics(m_profiler, m_bar_buffers, measured_seconds);
    record_ring_statistics(m_profiler, m_trade_buffers, measured_seconds);
    m_profiler.record_observation(
        "benchmark.memory.process_high_water_bytes",
        static_cast<double>(process_memory_high_water_bytes()));
    ensure_interval_observations(m_profiler);
}

}  // namespace vnm::benchmark
