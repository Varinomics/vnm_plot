#include "test_macros.h"

#include <vnm_plot/vnm_plot.h>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QVariant>

#include <memory>

bool test_documented_plot_view()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import VnmPlot 1.0
        Item {
            width: 640
            height: 480
            readonly property bool hover_inactive: !plot.area_hover_enabled && plot.hovered_series_id === null
            PlotTimeAxis { id: axis }
            PlotView { id: plot; anchors.fill: parent; time_axis: axis }
        }
    )", QUrl("qrc:/quickstart.qml"));
    if (component.isError()) {
        std::cerr << component.errorString().toStdString();
    }
    std::unique_ptr<QObject> object(component.create());
    TEST_ASSERT(object, "documented VnmPlot import should create a PlotView with its interaction and indicator");
    TEST_ASSERT(object->property("hover_inactive").toBool(),
        "PlotView area hover must remain opt-in and expose null before a containing area has rendered");
    return true;
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    vnm::plot::register_qml_types();
    vnm::plot::register_qml_types();
    return test_documented_plot_view() ? 0 : 1;
}
