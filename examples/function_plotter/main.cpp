// Function Plotter Example
// Demonstrates vnm_plot library with mexce for plotting mathematical functions.
//
// This is a proof-of-concept showing that vnm_plot can work independently
// with any data source that implements Data_source.

#include "function_plotter.h"

#include <vnm_plot/vnm_plot.h>

#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QSurfaceFormat>

int main(int argc, char* argv[])
{
    // Enable high-DPI scaling
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QSurfaceFormat surface_format = QSurfaceFormat::defaultFormat();
    surface_format.setSamples(8);
    QSurfaceFormat::setDefaultFormat(surface_format);

    QGuiApplication app(argc, argv);
    app.setApplicationName("Function Plotter");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("VNM");
    app.setWindowIcon(QIcon(":/rc/varinomics.ico"));

    // Register QML types
    vnm::plot::register_qml_types();
    qmlRegisterUncreatableType<Function_entry>("FunctionPlotter", 1, 0, "FunctionEntry",
        "Function entries are created by Function_plotter");

    // Create the function plotter backend
    Function_plotter plotter;

    // Load the default preset
    plotter.load_preset(0);  // Sine wave

    // Set up QML engine
    QQmlApplicationEngine engine;

    // Expose the plotter to QML
    engine.rootContext()->setContextProperty("functionPlotter", &plotter);

    // Expose presets to QML
    QVariantList presets;
    for (int i = 0; i < k_num_presets; ++i) {
        QVariantMap preset;
        preset["name"]       = QString::fromLatin1(k_function_presets[i].name);
        preset["expression"] = QString::fromLatin1(k_function_presets[i].expression);
        preset["xMin"]       = k_function_presets[i].x_min;
        preset["xMax"]       = k_function_presets[i].x_max;
        presets.append(preset);
    }
    engine.rootContext()->setContextProperty("functionPresets", presets);

    // Load the main QML file
    const QUrl url(QStringLiteral("qrc:/qml/main.qml"));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Failed to load QML";
        return -1;
    }

    return app.exec();
}
