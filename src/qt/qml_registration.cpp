#include <vnm_plot/qt/qml_registration.h>

#include "qml_resources.h"
#include <vnm_plot/qt/plot_interaction_item.h>
#include <vnm_plot/qt/plot_time_axis.h>
#include <vnm_plot/qt/plot_widget.h>

#include <QQmlEngine>
#include <QUrl>

// The reference keeps the resource object in statically linked applications.
int qInitResources_vnm_plot();

namespace vnm::plot {

void detail::initialize_qml_resources()
{
    static const int initialized = qInitResources_vnm_plot();
    Q_UNUSED(initialized);
}

void register_qml_types()
{
    static const bool registered = [] {
        detail::initialize_qml_resources();
        qmlRegisterType<Plot_widget>("VnmPlot", 1, 0, "PlotWidget");
        qmlRegisterType<Plot_interaction_item>("VnmPlot", 1, 0, "PlotInteractionItem");
        qmlRegisterType<Plot_time_axis>("VnmPlot", 1, 0, "PlotTimeAxis");
        qmlRegisterType(QUrl("qrc:/vnm_plot/qml/VnmPlot/PlotView.qml"),
            "VnmPlot", 1, 0, "PlotView");
        qmlRegisterType(QUrl("qrc:/vnm_plot/qml/VnmPlot/PlotIndicator.qml"),
            "VnmPlot", 1, 0, "PlotIndicator");
        return true;
    }();
    Q_UNUSED(registered);
}

} // namespace vnm::plot
