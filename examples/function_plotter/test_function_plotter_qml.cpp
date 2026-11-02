#include "function_plotter.h"

#include "../../tests/test_macros.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QVariant>

#include <memory>

namespace {

bool test_main_qml()
{
    Function_plotter plotter;
    plotter.load_preset(0);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty("functionPlotter", &plotter);
    QVariantList presets;
    for (const auto& entry : k_function_presets) {
        QVariantMap preset;
        preset["name"]       = QString::fromLatin1(entry.name);
        preset["expression"] = QString::fromLatin1(entry.expression);
        preset["xMin"]       = entry.x_min;
        preset["xMax"]       = entry.x_max;
        presets.append(preset);
    }
    engine.rootContext()->setContextProperty("functionPresets", presets);

    // Instantiate the application's actual resource while keeping the test window hidden.
    QQmlComponent component(&engine, QUrl("qrc:/qml/main.qml"));
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("visible"), false}
    }));
    if (component.isError()) {
        std::cerr << component.errorString().toStdString();
    }
    TEST_ASSERT(root, "the function plotter's main QML resource should instantiate");
    TEST_ASSERT(!root->property("visible").toBool(), "the fixture window should stay hidden");
    TEST_ASSERT(plotter.plot_widget(),
        "main.qml should assign the shared PlotView's plot_widget to the backend");
    auto* view = plotter.plot_widget()->parentItem();
    TEST_ASSERT(view && view->property("dark_mode").toBool() && plotter.plot_widget()->dark_mode(),
        "the shared PlotView should retain the example's dark-mode setting");
    TEST_ASSERT(view->property("plot_widget").value<vnm::plot::Plot_widget*>() == plotter.plot_widget(),
        "the backend should receive the widget exposed by the shared PlotView alias");
    const auto series = plotter.plot_widget()->get_series_snapshot();
    TEST_ASSERT(series.size() == static_cast<std::size_t>(plotter.function_count()) &&
        series.count(plotter.get_function(0)->series_id()) == 1,
        "the QML-assigned widget should receive the actual function series");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    vnm::plot::register_qml_types();
    qmlRegisterUncreatableType<Function_entry>("FunctionPlotter", 1, 0, "FunctionEntry",
        "Function entries are created by Function_plotter");
    return test_main_qml() ? 0 : 1;
}
