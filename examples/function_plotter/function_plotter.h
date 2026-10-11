#pragma once

// Function Plotter
// A Qt Quick component that evaluates mathematical expressions using mexce
// and displays them using vnm_plot.

#include "function_sample_source.h"

#include <vnm_plot/vnm_plot.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtQuick/QQuickItem>

#include <memory>
#include <string>
#include <vector>

// Forward declaration
namespace mexce { class evaluator; }
class QAudioSink;
class QBuffer;
class Function_plotter;

// Represents a single function entry with its own expression, data, and visualization
class Function_entry : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString expression READ expression WRITE set_expression NOTIFY expression_changed)
    Q_PROPERTY(QColor color READ color WRITE set_color NOTIFY color_changed)
    Q_PROPERTY(QString errorMessage READ error_message NOTIFY error_message_changed)
    Q_PROPERTY(bool hasError READ has_error NOTIFY error_message_changed)
    Q_PROPERTY(bool is_playing READ is_playing NOTIFY playback_state_changed)
    Q_PROPERTY(int index READ index CONSTANT)
    Q_PROPERTY(int numSamples READ num_samples WRITE set_num_samples NOTIFY num_samples_changed)
    Q_PROPERTY(bool showDots READ show_dots WRITE set_show_dots NOTIFY display_style_changed)
    Q_PROPERTY(bool showLine READ show_line WRITE set_show_line NOTIFY display_style_changed)
    Q_PROPERTY(bool showArea READ show_area WRITE set_show_area NOTIFY display_style_changed)

public:
    explicit Function_entry(int id, const QColor& color, const QString& initial_expression, Function_plotter* plotter);
    ~Function_entry();

    // Properties
    QString expression() const { return m_expression; }
    void set_expression(const QString& expr);

    QColor color() const { return m_color; }
    void set_color(const QColor& color);

    QString error_message() const { return m_error_message; }
    bool has_error() const { return !m_error_message.isEmpty(); }
    bool is_playing() const { return m_is_playing; }
    int index() const;

    int num_samples() const { return m_num_samples; }
    void set_num_samples(int n);

    // Display style (DOTS=0x1, LINE=0x2, AREA=0x4)
    bool show_dots() const;
    bool show_line() const;
    bool show_area() const;
    void set_show_dots(bool v);
    void set_show_line(bool v);
    void set_show_area(bool v);

    // Internal access
    int series_id() const { return m_series_id; }
    void set_series_id(int id) { m_series_id = id; }
    vnm::plot::examples::Function_data_source* data_source() { return &m_data_source; }
    std::shared_ptr<vnm::plot::series_data_t> series() const { return m_series; }

    // Generate samples using plotter's x range and own sample count
    void generate_samples(double x_min, double x_max);
    bool compile_expression();
    bool expression_valid() const { return m_expression_valid; }
    void mark_audio_stale();

public slots:
    void play_sound();

signals:
    void expression_changed();
    void color_changed();
    void error_message_changed();
    void display_style_changed();
    void playback_state_changed();
    void num_samples_changed();
    void data_updated();

private:
    void set_error(const QString& msg);
    void clear_error();
    void set_is_playing(bool playing);
    void setup_series();
    void update_series_color();
    void push_series_update();
    void set_style_flag(vnm::plot::Display_style flag, bool enabled);

    Function_plotter* m_plotter;
    std::unique_ptr<mexce::evaluator> m_evaluator;
    vnm::plot::examples::Function_data_source m_data_source;
    std::shared_ptr<vnm::plot::series_data_t> m_series;

    int m_series_id;
    QString m_expression = "sin(x)";
    QColor m_color;
    QString m_error_message;
    int m_num_samples = 10000;
    bool m_expression_valid = false;
    bool m_is_playing = false;

    // Bound variable for mexce
    double m_x = 0.0;

    // Audio playback
    std::unique_ptr<QAudioSink> m_audio_sink;
    std::unique_ptr<QBuffer> m_audio_buffer;
    bool m_audio_stale = true;
};

Q_DECLARE_METATYPE(Function_entry*)

// Main function plotter class - manages multiple function entries
class Function_plotter : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(double xMin READ x_min WRITE set_x_min NOTIFY range_changed)
    Q_PROPERTY(double xMax READ x_max WRITE set_x_max NOTIFY range_changed)
    Q_PROPERTY(vnm::plot::Plot_widget* plot_widget READ plot_widget WRITE set_plot_widget NOTIFY plot_widget_changed)
    Q_PROPERTY(int functionCount READ function_count NOTIFY function_count_changed)
    Q_PROPERTY(bool stackFunctions READ stack_functions WRITE set_stack_functions NOTIFY stack_functions_changed)

public:
    enum Roles {
        ExpressionRole = Qt::UserRole + 1,
        ColorRole,
        HasErrorRole,
        ErrorMessageRole,
        IsPlayingRole,
        NumSamplesRole,
        ShowDotsRole,
        ShowLineRole,
        ShowAreaRole,
        EntryRole  // Returns the Function_entry* directly
    };

    explicit Function_plotter(QObject* parent = nullptr);
    ~Function_plotter();

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QHash<int, QByteArray> roleNames() const override;

    // Properties
    double x_min() const { return m_x_min; }
    void set_x_min(double v);

    double x_max() const { return m_x_max; }
    void set_x_max(double v);

    int function_count() const { return static_cast<int>(m_entries.size()); }

    bool stack_functions() const { return m_stack_functions; }
    void set_stack_functions(bool stacked);

    // Plot widget connection
    vnm::plot::Plot_widget* plot_widget() const { return m_plot_widget; }
    void set_plot_widget(vnm::plot::Plot_widget* widget);

public slots:
    // Function management
    void add_function();
    void remove_function(int index);
    void move_function(int from, int to);

    // Access function entry by index (for QML)
    Function_entry* get_function(int index) const;

    // Preset expressions for demo
    void load_preset(int index);

    // Recompute all functions
    void recompute_all();

signals:
    void range_changed();
    void plot_widget_changed();
    void function_count_changed();
    void stack_functions_changed();

private:
    void regenerate_all_samples();
    void update_plot_widget();
    void configure_plot_widget();
    void refresh_series_labels(bool refresh_all = false);
    QColor get_next_color();
    QString get_unique_expression() const;
    int index_of_entry(Function_entry* entry) const;

    // Entry destructors detach their series, so the widget guard must outlive them.
    QPointer<vnm::plot::Plot_widget> m_plot_widget;
    std::vector<std::unique_ptr<Function_entry>> m_entries;
    int m_next_series_id = 1;
    int m_color_index = 0;

    double m_x_min = -10.0;
    double m_x_max = 10.0;
    bool m_stack_functions = false;

};

// Preset expressions for demo
struct function_preset_t
{
    const char* name;
    const char* expression;
    double      x_min;
    double      x_max;
};

constexpr function_preset_t k_function_presets[] = {
    {"Sine Wave",           "sin(x)",                           -10,   10},
    {"Cosine Wave",         "cos(x)",                           -10,   10},
    {"Damped Sine",         "sin(x) * exp(-abs(x)/5)",          -15,   15},
    {"Polynomial",          "x^3 - 3*x^2 + 2*x",                -2,    4},
    {"Gaussian",            "exp(-x^2/2)",                      -5,    5},
    {"Sinc Function",       "sin(x)/x",                         -20,   20},
    {"Beats",               "sin(10*x) * sin(x)",               -10,   10},
    {"Square-ish Wave",     "sin(x) + sin(3*x)/3 + sin(5*x)/5", -10,   10},
    {"Logarithm",           "log(abs(x) + 0.1)",                -5,    5},
    {"Square Root",         "sqrt(abs(x))",                    -5,    5},
};

constexpr int k_num_presets = sizeof(k_function_presets) / sizeof(k_function_presets[0]);
