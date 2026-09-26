#include "test_macros.h"

#include <vnm_plot/vnm_plot.h>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>

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
            PlotTimeAxis { id: axis }
            PlotView { anchors.fill: parent; time_axis: axis }
        }
    )", QUrl("qrc:/quickstart.qml"));
    if (component.isError()) {
        std::cerr << component.errorString().toStdString();
    }
    std::unique_ptr<QObject> object(component.create());
    TEST_ASSERT(object, "documented VnmPlot import should create a PlotView with its interaction and indicator");
    return true;
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    vnm::plot::register_qml_types();
    vnm::plot::register_qml_types();
    return test_documented_plot_view() ? 0 : 1;
}
