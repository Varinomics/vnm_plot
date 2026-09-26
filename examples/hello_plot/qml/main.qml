import QtQuick 2.15
import QtQuick.Window 2.15
import VnmPlot 1.0
import Example 1.0

Window {
    id: window
    width: 900
    height: 600
    visible: true
    title: "vnm_plot hello"
    color: "#0b0d12"

    PlotTimeAxis {
        id: sharedAxis
    }

    Column {
        anchors.fill: parent
        spacing: 8

        PlotView {
            id: plotViewTop
            height: parent.height * 0.5 - parent.spacing * 0.5
            width: parent.width
            time_axis: sharedAxis
        }

        PlotView {
            id: plotViewBottom
            height: parent.height * 0.5 - parent.spacing * 0.5
            width: parent.width
            time_axis: sharedAxis
        }
    }

    PlotController {
        plot_widget: plotViewTop.plot_widget
    }

    PlotController {
        plot_widget: plotViewBottom.plot_widget
    }
}
