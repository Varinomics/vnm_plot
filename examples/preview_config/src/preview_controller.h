#pragma once

#include "function_sample_source.h"

#include <vnm_plot/vnm_plot.h>

#include <QtCore/QObject>

#include <memory>

class Example_plot_controller : public QObject
{
    Q_OBJECT

    Q_PROPERTY(vnm::plot::Plot_widget* plot_widget READ plot_widget WRITE set_plot_widget NOTIFY plot_widget_changed)

public:
    vnm::plot::Plot_widget* plot_widget() const { return m_plot_widget; }
    void set_plot_widget(vnm::plot::Plot_widget* widget);

signals:
    void plot_widget_changed();

protected:
    enum class Preset { PREVIEW, TOP };
    Example_plot_controller(Preset preset, QObject* parent);

private:
    void setup_series();
    void configure_plot_widget();
    void generate_samples();

    const Preset m_preset;
    const int m_series_id;
    vnm::plot::Plot_widget* m_plot_widget = nullptr;
    std::shared_ptr<vnm::plot::examples::Function_data_source> m_main_source;
    std::shared_ptr<vnm::plot::examples::Function_data_source> m_preview_source;
    std::shared_ptr<vnm::plot::series_data_t> m_series;
    QMetaObject::Connection m_time_axis_connection;
};

class Preview_controller : public Example_plot_controller
{
    Q_OBJECT

public:
    explicit Preview_controller(QObject* parent = nullptr)
    :
        Example_plot_controller(Preset::PREVIEW, parent)
    {}
};
