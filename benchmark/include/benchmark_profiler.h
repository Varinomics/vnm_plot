// vnm_plot Benchmark - Profiler
// Hierarchical scope timing with stable benchmark report generation

#ifndef VNM_PLOT_BENCHMARK_PROFILER_H
#define VNM_PLOT_BENCHMARK_PROFILER_H

#include "allocation_tracker.h"
#include "path_io.h"

#include <vnm_plot/core/plot_config.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace vnm::benchmark {

/// Report metadata for output file
struct Report_metadata {
    std::string session = "benchmark_run";
    std::string stream = "SIM";
    std::string data_type = "Bars";      // "Bars" or "Trades"
    std::string backend = "qrhi-offscreen";
    double target_duration = 30.0;
    std::string filename_prefix = "inspector_benchmark";
    std::filesystem::path output_directory;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point generated_at;

    // Extended metadata (only included with --extended-metadata flag)
    bool include_extended = false;
    uint64_t seed = 0;
    double volatility = 0.0;
    std::size_t ring_capacity = 0;
    std::size_t samples_generated = 0;
    std::map<std::string, std::string> reproduction;
    std::vector<std::string> invocation_args;
};

/// Profiler that builds hierarchical timing tree with scope aggregation.
/// Implements vnm::plot::Profiler interface to capture vnm_plot internal scopes.
class Benchmark_profiler : public vnm::plot::Profiler {
public:
    Benchmark_profiler()
    {
        m_root.name = "[root]";
    }

    ~Benchmark_profiler() override = default;

    /// Begin a named scope. Nested calls create child scopes.
    void begin_scope(const char* name) override
    {
        Thread_allocation_suppression suppress_instrumentation_allocations;
        auto start_time = std::chrono::steady_clock::now();
        const char* scope_name = name ? name : "";

        std::lock_guard<std::mutex> lock(m_mutex);
        auto& ctx = get_thread_context_locked();
        ctx.start_times.push(start_time);
        auto& children = ctx.current->children;
        auto it = children.find(scope_name);
        if (it == children.end()) {
            auto child = std::make_unique<Scope_stats>();
            child->name = scope_name;
            child->parent = ctx.current;
            it = children.emplace(scope_name, std::move(child)).first;
        }
        ctx.current = it->second.get();
    }

    /// End the current scope and record timing.
    void end_scope() override
    {
        Thread_allocation_suppression suppress_instrumentation_allocations;
        auto end_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_mutex);
        auto& ctx = get_thread_context_locked();
        if (ctx.start_times.empty() || !ctx.current) {
            return;
        }
        auto start_time = ctx.start_times.top();
        ctx.start_times.pop();

        double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

        // Aggregate into existing stats
        ctx.current->call_count++;
        ctx.current->total_ms += elapsed_ms;
        ctx.current->min_ms = std::min(ctx.current->min_ms, elapsed_ms);
        ctx.current->max_ms = std::max(ctx.current->max_ms, elapsed_ms);
        if (ctx.current->name == "renderer.frame.execute_passes.render_data_series.prepare") {
            m_last_series_prepare_ms = elapsed_ms;
        }

        if (ctx.current->parent) {
            ctx.current = ctx.current->parent;
        }
    }

    void record_observation(const char* name, double value) override
    {
        Thread_allocation_suppression suppress_instrumentation_allocations;
        if (!name || !std::isfinite(value)) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        record_observation_locked(name, 1, value, value, value, true);
    }

    void record_observation_summary(
        const char* name,
        uint64_t call_count,
        double total,
        double min,
        double max)
    {
        Thread_allocation_suppression suppress_instrumentation_allocations;
        if (!name || call_count == 0 ||
            !std::isfinite(total) || !std::isfinite(min) || !std::isfinite(max))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        record_observation_locked(name, call_count, total, min, max, false);
    }

    void ensure_observation(const char* name, double value = 0.0)
    {
        Thread_allocation_suppression suppress_instrumentation_allocations;
        if (!name || !std::isfinite(value)) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_observations.find(name) == m_observations.end()) {
            record_observation_locked(name, 1, value, value, value, true);
        }
    }

private:
    static constexpr std::size_t k_max_retained_samples = 8192;

    void record_observation_locked(
        const char* name,
        uint64_t call_count,
        double total,
        double min,
        double max,
        bool retain_sample)
    {
        auto [stats_it, inserted] = m_observations.try_emplace(name);
        auto& stats = stats_it->second;
        if (inserted) {
            std::uint64_t seed = 1'469'598'103'934'665'603ull;
            for (const unsigned char byte : std::string(name)) {
                seed ^= byte;
                seed *= 1'099'511'628'211ull;
            }
            stats.reservoir_rng.seed(seed);
        }
        stats.name = name;
        stats.call_count += call_count;
        stats.total += total;
        stats.min = std::min(stats.min, min);
        stats.max = std::max(stats.max, max);
        if (retain_sample) {
            if (stats.samples.size() < k_max_retained_samples) {
                stats.samples.push_back(total);
            }
            else {
                std::uniform_int_distribution<std::uint64_t> distribution(
                    0,
                    stats.call_count - 1);
                const std::uint64_t candidate = distribution(stats.reservoir_rng);
                if (candidate < k_max_retained_samples) {
                    stats.samples[static_cast<std::size_t>(candidate)] = total;
                }
            }
        }
    }

public:
    /// Generate benchmark report string
    std::string generate_report(const Report_metadata& meta) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;

        // Header
        oss << "vnm_plot profiling report\n";
        oss << "Session: " << meta.session << "\n";
        oss << "Started at (UTC): " << format_utc_time(meta.started_at) << "\n";
        oss << "Target duration (s): " << std::fixed << std::setprecision(3) << meta.target_duration << "\n";
        oss << "Report generated at (UTC): " << format_utc_time(meta.generated_at) << "\n";
        oss << "Built with " << get_compiler_string() << "\n";
        oss << "\n";

        // Metadata section
        oss << "Metadata:\n";
        write_metadata(oss, meta);
        oss << "\n";

        write_observations(oss);

        // Table header (108 characters total)
        // Column widths: Section=58, Calls=5, Total=9, Avg=10, Min=6, Max=6, Percent=8 (+6 spaces)
        oss << std::left << std::setw(58) << "Section" << " "
            << std::right << std::setw(5) << "Calls" << " "
            << std::setw(9) << "Total ms" << " "
            << std::setw(10) << "Average ms" << " "
            << std::setw(6) << "Min ms" << " "
            << std::setw(6) << "Max ms" << " "
            << std::setw(8) << "Percent" << "\n";
        oss << std::string(108, '-') << "\n";

        // Calculate total time for percentage
        double total_root_ms = 0.0;
        for (const auto& [name, child] : m_root.children) {
            total_root_ms += child->total_ms;
        }

        // Write tree (disambiguate duplicate short names at the root level)
        std::map<std::string, std::size_t> root_name_counts;
        for (const auto& [name, child] : m_root.children) {
            root_name_counts[short_name(child->name)]++;
        }
        std::map<std::string, std::size_t> root_name_seen;

        for (auto it = m_root.children.begin(); it != m_root.children.end(); ++it) {
            bool is_last = (std::next(it) == m_root.children.end());
            std::string display_name = short_name(it->second->name);
            if (root_name_counts[display_name] > 1) {
                std::size_t idx = ++root_name_seen[display_name];
                display_name += "_" + std::to_string(idx);
            }
            write_scope_tree(oss, *it->second, "", is_last, total_root_ms, display_name);
        }

        oss << "\n";
        return oss.str();
    }

    /// Write report to file in output directory
    std::filesystem::path write_report(const Report_metadata& meta) const
    {
        // Generate filename: <prefix>_YYYYMMDD_HHMMSS_<STREAM>_<DataType>.txt
        std::string filename = generate_filename(meta);
        std::filesystem::path output_path = meta.output_directory / filename;

        // Ensure directory exists
        std::filesystem::create_directories(path_for_file_io(meta.output_directory));

        // Write report
        std::ofstream ofs(path_for_file_io(output_path));
        if (!ofs) {
            throw std::runtime_error("cannot open benchmark report: " + output_path.string());
        }
        ofs << generate_report(meta);
        if (!ofs) {
            throw std::runtime_error("cannot write benchmark report: " + output_path.string());
        }

        write_raw_report(meta);

        return output_path;
    }

    std::filesystem::path raw_report_path(const Report_metadata& meta) const
    {
        return meta.output_directory / generate_raw_filename(meta);
    }

    std::filesystem::path write_raw_report(const Report_metadata& meta) const
    {
        const std::filesystem::path output_path = raw_report_path(meta);
        std::filesystem::create_directories(path_for_file_io(meta.output_directory));

        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream ofs(path_for_file_io(output_path));
        if (!ofs) {
            throw std::runtime_error("cannot open raw benchmark report: " + output_path.string());
        }

        ofs << "{\n  \"schema_version\": 1,\n";
        ofs << "  \"retained_sample_limit_per_metric\": "
            << k_max_retained_samples << ",\n";
        ofs << "  \"metadata\": {\n";
        write_json_string_field(ofs, "session", meta.session, true, 4);
        write_json_string_field(ofs, "stream", meta.stream, true, 4);
        write_json_string_field(ofs, "data_type", meta.data_type, true, 4);
        write_json_string_field(ofs, "backend", meta.backend, true, 4);
        write_json_number_field(ofs, "target_duration_seconds", meta.target_duration, true, 4);
        write_json_string_field(ofs, "started_at_utc", format_utc_time(meta.started_at), true, 4);
        write_json_string_field(ofs, "generated_at_utc", format_utc_time(meta.generated_at),
            !meta.reproduction.empty(), 4);
        std::size_t reproduction_index = 0;
        for (const auto& [name, value] : meta.reproduction) {
            write_json_string_field(
                ofs,
                name,
                value,
                ++reproduction_index < meta.reproduction.size(),
                4);
        }
        ofs << "  },\n  \"invocation_args\": [";
        for (std::size_t i = 0; i < meta.invocation_args.size(); ++i) {
            if (i > 0) {
                ofs << ", ";
            }
            ofs << "\"" << json_escape(meta.invocation_args[i]) << "\"";
        }
        ofs << "],\n  \"observations\": {\n";

        std::size_t observation_index = 0;
        for (const auto& [name, stats] : m_observations) {
            ofs << "    \"" << json_escape(name) << "\": {\n";
            ofs << "      \"count\": " << stats.call_count << ",\n";
            ofs << "      \"retained_sample_count\": " << stats.samples.size() << ",\n";
            ofs << "      \"total\": " << json_number(stats.total) << ",\n";
            const double mean = stats.call_count > 0
                ? stats.total / static_cast<double>(stats.call_count)
                : 0.0;
            ofs << "      \"mean\": " << json_number(mean) << ",\n";
            ofs << "      \"min\": " << json_number(stats.min) << ",\n";
            ofs << "      \"max\": " << json_number(stats.max) << ",\n";
            write_json_percentile(ofs, "p50", stats.samples, 0.50);
            write_json_percentile(ofs, "p95", stats.samples, 0.95);
            write_json_percentile(ofs, "p99", stats.samples, 0.99);
            ofs << "      \"samples\": [";
            for (std::size_t i = 0; i < stats.samples.size(); ++i) {
                if (i > 0) {
                    ofs << ", ";
                }
                ofs << json_number(stats.samples[i]);
            }
            ofs << "]\n    }";
            if (++observation_index < m_observations.size()) {
                ofs << ",";
            }
            ofs << "\n";
        }
        ofs << "  }\n}\n";
        if (!ofs) {
            throw std::runtime_error("cannot write raw benchmark report: " + output_path.string());
        }
        return output_path;
    }

    /// Reset profiler for next run
    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_root.children.clear();
        m_root.call_count = 0;
        m_root.total_ms = 0.0;
        m_root.min_ms = std::numeric_limits<double>::max();
        m_root.max_ms = 0.0;
        m_thread_contexts.clear();
        m_observations.clear();
        m_last_series_prepare_ms = 0.0;
    }

    // Capture the shipping renderer's existing scope instead of timing a
    // benchmark-owned copy of series preparation.
    double last_series_prepare_ms() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_series_prepare_ms;
    }

    /// Get root scope for inspection
    const auto& root() const { return m_root; }

private:
    double m_last_series_prepare_ms = 0.0;

    struct Scope_stats {
        std::string name;
        uint64_t call_count = 0;
        double total_ms = 0.0;
        double min_ms = std::numeric_limits<double>::max();
        double max_ms = 0.0;
        std::map<std::string, std::unique_ptr<Scope_stats>> children;
        Scope_stats* parent = nullptr;
    };

    struct Observation_stats {
        std::string name;
        uint64_t call_count = 0;
        double total = 0.0;
        double min = std::numeric_limits<double>::max();
        double max = 0.0;
        std::vector<double> samples;
        std::mt19937_64 reservoir_rng;
    };

    struct Thread_context {
        Scope_stats* current = nullptr;
        std::stack<std::chrono::steady_clock::time_point> start_times;
    };

    Scope_stats m_root;
    std::map<std::string, Observation_stats> m_observations;
    mutable std::mutex m_mutex;
    std::map<std::thread::id, Thread_context> m_thread_contexts;

    // Format UTC timestamp as ISO 8601
    static std::string format_utc_time(std::chrono::system_clock::time_point tp)
    {
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::tm tm_val{};
#ifdef _WIN32
        gmtime_s(&tm_val, &time_t_val);
#else
        gmtime_r(&time_t_val, &tm_val);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    // Format timestamp for filename
    static std::string format_filename_time(std::chrono::system_clock::time_point tp)
    {
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::tm tm_val{};
#ifdef _WIN32
        gmtime_s(&tm_val, &time_t_val);
#else
        gmtime_r(&time_t_val, &tm_val);
#endif
        std::ostringstream oss;
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()) % 1000;
        oss << std::put_time(&tm_val, "%Y%m%d_%H%M%S_")
            << std::setw(3) << std::setfill('0') << milliseconds.count();
        return oss.str();
    }

    // Get compiler identification string
    static std::string get_compiler_string()
    {
#if defined(__GNUC__) && !defined(__clang__)
        std::ostringstream oss;
        oss << "GNU v" << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
        return oss.str();
#elif defined(__clang__)
        std::ostringstream oss;
        oss << "Clang v" << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
        return oss.str();
#elif defined(_MSC_VER)
        std::ostringstream oss;
        oss << "MSVC v" << (_MSC_VER / 100) << "." << (_MSC_VER % 100);
        return oss.str();
#else
        return "Unknown compiler";
#endif
    }

    // Write metadata section (alphabetically sorted)
    static void write_metadata(std::ostream& os, const Report_metadata& meta)
    {
        // Standard metadata
        os << "  - backend: " << meta.backend << "\n";
        os << "  - data_type: " << meta.data_type << "\n";
        os << "  - duration_seconds: " << static_cast<int>(meta.target_duration) << "\n";
        os << "  - output_directory: " << meta.output_directory.generic_string() << "\n";
        os << "  - report_filename: " << generate_filename(meta) << "\n";

        // Extended metadata (if enabled)
        if (meta.include_extended) {
            os << "  - ring_capacity: " << meta.ring_capacity << "\n";
            os << "  - samples_generated: " << meta.samples_generated << "\n";
            os << "  - seed: " << meta.seed << "\n";
        }

        os << "  - started_at_utc: " << format_utc_time(meta.started_at) << "\n";
        os << "  - stream: " << meta.stream << "\n";

        if (meta.include_extended) {
            os << "  - volatility: " << std::fixed << std::setprecision(4) << meta.volatility << "\n";
        }

        for (const auto& [name, value] : meta.reproduction) {
            os << "  - " << name << ": " << value << "\n";
        }
    }

    void write_observations(std::ostream& os) const
    {
        if (m_observations.empty()) {
            return;
        }

        os << "Observations:\n";
        os << std::left << std::setw(46) << "Name" << " "
            << std::right << std::setw(5) << "Calls" << " "
            << std::setw(12) << "Total" << " "
            << std::setw(12) << "Average" << " "
            << std::setw(8) << "Min" << " "
            << std::setw(8) << "Max" << "\n";
        os << std::string(96, '-') << "\n";

        for (const auto& [name, stats] : m_observations) {
            const double average = stats.call_count > 0
                ? stats.total / static_cast<double>(stats.call_count)
                : 0.0;
            os << std::left << std::setw(46) << name << " "
                << std::right << std::setw(5) << stats.call_count << " "
                << std::fixed << std::setprecision(3)
                << std::setw(12) << stats.total << " "
                << std::setw(12) << average << " "
                << std::setw(8) << stats.min << " "
                << std::setw(8) << stats.max << "\n";
        }

        os << "\n";
    }

    // Generate filename
    static std::string generate_filename(const Report_metadata& meta)
    {
        const std::string prefix = meta.filename_prefix.empty()
            ? "inspector_benchmark"
            : meta.filename_prefix;
        std::ostringstream oss;
        oss << prefix << "_" << format_filename_time(meta.started_at)
            << "_" << meta.stream << "_" << meta.data_type << ".txt";
        return oss.str();
    }

    static std::string generate_raw_filename(const Report_metadata& meta)
    {
        std::filesystem::path filename(generate_filename(meta));
        filename.replace_extension(".json");
        return filename.generic_string();
    }

    static std::string json_escape(const std::string& value)
    {
        std::ostringstream oss;
        for (const unsigned char c : value) {
            switch (c) {
            case '\"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(c) << std::dec << std::setfill(' ');
                }
                else {
                    oss << static_cast<char>(c);
                }
                break;
            }
        }
        return oss.str();
    }

    static std::string json_number(double value)
    {
        if (!std::isfinite(value)) {
            return "null";
        }
        std::ostringstream oss;
        oss << std::setprecision(17) << value;
        return oss.str();
    }

    static double percentile(std::vector<double> values, double fraction)
    {
        if (values.empty()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        std::sort(values.begin(), values.end());
        const double index = fraction * static_cast<double>(values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(std::floor(index));
        const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
        const double weight = index - static_cast<double>(lower);
        return values[lower] * (1.0 - weight) + values[upper] * weight;
    }

    static void write_json_string_field(
        std::ostream& os,
        const std::string& name,
        const std::string& value,
        bool comma,
        int indent)
    {
        os << std::string(static_cast<std::size_t>(indent), ' ')
            << "\"" << json_escape(name) << "\": \"" << json_escape(value) << "\""
            << (comma ? "," : "") << "\n";
    }

    static void write_json_number_field(
        std::ostream& os,
        const std::string& name,
        double value,
        bool comma,
        int indent)
    {
        os << std::string(static_cast<std::size_t>(indent), ' ')
            << "\"" << json_escape(name) << "\": " << json_number(value)
            << (comma ? "," : "") << "\n";
    }

    static void write_json_percentile(
        std::ostream& os,
        const char* name,
        const std::vector<double>& samples,
        double fraction)
    {
        os << "      \"" << name << "\": "
            << json_number(percentile(samples, fraction)) << ",\n";
    }

    // Extract short name from fully qualified scope name (e.g., "renderer.frame.foo" -> "foo")
    static std::string short_name(const std::string& full_name)
    {
        auto pos = full_name.rfind('.');
        if (pos != std::string::npos && pos + 1 < full_name.length()) {
            return full_name.substr(pos + 1);
        }
        return full_name;
    }

    // Count display width (ASCII chars = 1, UTF-8 sequences = 1 per character)
    static std::size_t display_width(const std::string& s)
    {
        std::size_t width = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c & 0xC0) != 0x80) {  // Not a continuation byte
                ++width;
            }
        }
        return width;
    }

    // Pad string to target display width
    static std::string pad_to_width(const std::string& s, std::size_t target_width)
    {
        std::size_t current_width = display_width(s);
        if (current_width >= target_width) {
            return s;
        }
        return s + std::string(target_width - current_width, ' ');
    }

    // Write scope tree recursively with proper indentation and glyphs
    void write_scope_tree(
        std::ostream& os,
        const Scope_stats& scope,
        const std::string& prefix,
        bool is_last,
        double total_ms,
        const std::string& display_override = "") const
    {
        // Glyph for this node
        std::string glyph = is_last ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ";  // └─ or ├─

        // Use short name (strip parent prefix since tree shows hierarchy)
        std::string display_name = display_override.empty()
            ? short_name(scope.name)
            : display_override;

        // Build section with tree prefix
        std::string section = prefix + glyph + display_name;

        // Truncate if too long (by display width)
        std::size_t section_width = display_width(section);
        if (section_width > 58) {
            // Truncate display_name to fit
            std::size_t prefix_width = display_width(prefix + glyph);
            std::size_t max_name_width = 55 - prefix_width;
            if (display_name.length() > max_name_width) {
                display_name = display_name.substr(0, max_name_width - 3) + "...";
            }
            section = prefix + glyph + display_name;
        }

        // Pad to 58 display characters
        section = pad_to_width(section, 58);

        // Calculate stats
        double avg_ms = scope.call_count > 0 ? scope.total_ms / scope.call_count : 0.0;
        double percent = total_ms > 0 ? (scope.total_ms / total_ms) * 100.0 : 0.0;

        // Write row
        os << section << " "
           << std::right << std::setw(5) << scope.call_count << " "
           << std::fixed << std::setprecision(3)
           << std::setw(9) << scope.total_ms << " "
           << std::setw(10) << avg_ms << " "
           << std::setw(6) << scope.min_ms << " "
           << std::setw(6) << scope.max_ms << " "
           << std::setw(7) << percent << "%\n";

        // Calculate children prefix
        std::string child_prefix = prefix + (is_last ? "   " : "\xe2\x94\x82  ");  // │ for continuation

        // Calculate unattributed time
        double children_total = 0.0;
        for (const auto& [name, child] : scope.children) {
            children_total += child->total_ms;
        }

        // Prepare child display names (disambiguate duplicate short names)
        std::map<std::string, std::size_t> short_name_counts;
        for (const auto& [child_name, child] : scope.children) {
            short_name_counts[short_name(child->name)]++;
        }
        std::map<std::string, std::size_t> short_name_seen;

        // Write children
        auto it = scope.children.begin();
        while (it != scope.children.end()) {
            bool child_is_last = (std::next(it) == scope.children.end()) && (children_total >= scope.total_ms - 0.001);
            std::string child_display = short_name(it->second->name);
            if (short_name_counts[child_display] > 1) {
                std::size_t idx = ++short_name_seen[child_display];
                child_display += "_" + std::to_string(idx);
            }
            write_scope_tree(os, *it->second, child_prefix, child_is_last, total_ms, child_display);
            ++it;
        }

        // Write unattributed row if there's unaccounted time
        double unattributed = scope.total_ms - children_total;
        if (unattributed > 0.001 && !scope.children.empty()) {
            std::string unattr_glyph = "\xe2\x94\x94\xe2\x94\x80 ";  // └─
            std::string unattr_section = child_prefix + unattr_glyph + "[unattributed]";
            unattr_section = pad_to_width(unattr_section, 58);

            double unattr_avg = scope.call_count > 0 ? unattributed / scope.call_count : 0.0;
            double unattr_percent = total_ms > 0 ? (unattributed / total_ms) * 100.0 : 0.0;

            os << unattr_section << " "
               << std::right << std::setw(5) << scope.call_count << " "
               << std::fixed << std::setprecision(3)
               << std::setw(9) << unattributed << " "
               << std::setw(10) << unattr_avg << " "
               << std::setw(6) << "-" << " "
               << std::setw(6) << "-" << " "
               << std::setw(7) << unattr_percent << "%\n";
        }
    }

    Thread_context& get_thread_context_locked()
    {
        auto& ctx = m_thread_contexts[std::this_thread::get_id()];
        if (!ctx.current) {
            ctx.current = &m_root;
        }
        return ctx;
    }
};

/// RAII scope guard for automatic begin/end
class Profile_scope {
public:
    Profile_scope(Benchmark_profiler& profiler, const char* name)

    :
        m_profiler(profiler)
    {
        m_profiler.begin_scope(name);
    }

    ~Profile_scope() { m_profiler.end_scope(); }

    Profile_scope(const Profile_scope&) = delete;
    Profile_scope& operator=(const Profile_scope&) = delete;

private:
    Benchmark_profiler& m_profiler;
};

// Helper macros for unique variable names
#define BENCHMARK_CONCAT_IMPL(x, y) x##y
#define BENCHMARK_CONCAT(x, y) BENCHMARK_CONCAT_IMPL(x, y)

#define BENCHMARK_SCOPE(profiler, name) \
    vnm::benchmark::Profile_scope BENCHMARK_CONCAT(profile_scope, __COUNTER__)(profiler, name)

}  // namespace vnm::benchmark

#endif  // VNM_PLOT_BENCHMARK_PROFILER_H
