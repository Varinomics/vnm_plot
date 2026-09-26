import QtQuick 2.15
import QtQuick.Window 2.15
import VnmPlot 1.0
import Example 1.0

Window {
    id: window
    width: 960
    height: 640
    visible: true
    title: "vnm_plot preview config"
    color: "#0b0d12"

    PlotTimeAxis {
        id: sharedAxis
        sync_vbar_width: true
    }

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8
        property real contentHeight: height - header.implicitHeight - spacing * 2

        Text {
            id: header
            text: "Shared time axis. Top plot: separate style and range, no preview bar. Bottom plot: preview source + AREA style."
            color: "#c8d0d8"
            font.pixelSize: 14
        }

        PlotView {
            id: plotViewTop
            width: parent.width
            height: parent.contentHeight * 0.5
            dark_mode: true
            link_indicator: true
            time_axis: sharedAxis
        }

        PlotView {
            id: plotViewBottom
            width: parent.width
            height: parent.contentHeight * 0.5
            dark_mode: true
            link_indicator: true
            time_axis: sharedAxis
        }
    }

    TopController {
        plot_widget: plotViewTop.plot_widget
    }

    PreviewController {
        plot_widget: plotViewBottom.plot_widget
    }
}
