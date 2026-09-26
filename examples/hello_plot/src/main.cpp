#include "plot_controller.h"

#include <vnm_plot/vnm_plot.h>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>
#include <QSurfaceFormat>

#include <cstdlib>

int main(int argc, char* argv[])
{
    QSurfaceFormat surface_format = QSurfaceFormat::defaultFormat();
    surface_format.setSamples(8);
    QSurfaceFormat::setDefaultFormat(surface_format);

    QGuiApplication app(argc, argv);

    vnm::plot::register_qml_types();
    qmlRegisterType<Plot_controller>("Example", 1, 0, "PlotController");

    QQmlApplicationEngine engine;
    const QUrl url("qrc:/qml/main.qml");

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreated,
        &app,
        [url](QObject* obj, const QUrl& obj_url) {
            if (!obj && url == obj_url) {
                QCoreApplication::exit(EXIT_FAILURE);
            }
        },
        Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
