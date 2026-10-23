import QtQuick
import VnmPlot 1.0

Item {
    id: root

    property alias plot_widget: plot
    // Optional override; when unset (null), PlotWidget retains its own setting.
    property var dark_mode: null
    property bool interaction_enabled: true
    property bool link_indicator: false
    property PlotTimeAxis time_axis: null
    property string indicator_x_label: "x"
    property string indicator_y_label: "y"
    property alias indicator_max_series_rows: indicator.max_series_rows
    property alias stacked_marker_note: indicator.stacked_marker_note

    signal main_plot_clicked(real timestamp_ms)
    signal main_plot_sample_hovered(real timestamp_ms)
    signal main_plot_sample_exited()

    function nearest_sample_timestamp_at(x, y) {
        const usableWidth = plot.width - plot.vbar_width_qml
        const usableHeight = plot.height - plot.reserved_height
        if (usableWidth <= 0 || usableHeight <= 0) {
            return null
        }
        if (x < 0 || x > usableWidth || y < 0 || y >= usableHeight) {
            return null
        }

        const nearestSamples = plot.get_nearest_samples(0, usableWidth, usableHeight, x)
        if (nearestSamples.length <= 0) {
            return null
        }

        const timestampMs = nearestSamples[0].x
        if (timestampMs === undefined || timestampMs === null || !isFinite(timestampMs)) {
            return null
        }

        return timestampMs
    }

    function update_hover_sample(x, y) {
        const timestampMs = nearest_sample_timestamp_at(x, y)
        if (timestampMs === null) {
            clear_hover_sample()
            return
        }

        if (!internal.has_hover_sample || internal.hover_timestamp_ms !== timestampMs) {
            internal.has_hover_sample = true
            internal.hover_timestamp_ms = timestampMs
            root.main_plot_sample_hovered(timestampMs)
        }
    }

    function clear_hover_sample() {
        if (!internal.has_hover_sample) {
            return
        }
        internal.has_hover_sample = false
        internal.hover_timestamp_ms = 0
        root.main_plot_sample_exited()
    }

    QtObject {
        id: internal
        property bool has_hover_sample: false
        property real hover_timestamp_ms: 0
    }

    PlotWidget {
        id: plot
        anchors.fill: parent
        time_axis: root.time_axis
    }

    PlotIndicator {
        id: indicator
        anchors.fill: parent
        plot_widget: plot
        link_indicator: root.link_indicator
        x_value_label: root.indicator_x_label
        y_value_label: root.indicator_y_label
    }

    PlotInteractionItem {
        id: interaction
        anchors.fill: parent
        plot_widget: plot
        interaction_enabled: root.interaction_enabled

        onMouse_position_changed: (x, y) => {
            indicator.update_mouse_position(x, y)
            root.update_hover_sample(x, y)
        }
        onMouse_exited: {
            indicator.set_mouse_in_plot(false)
            root.clear_hover_sample()
        }
        onMouse_clicked: (x, y) => {
            const timestampMs = root.nearest_sample_timestamp_at(x, y)
            if (timestampMs === null) {
                return
            }

            root.main_plot_clicked(timestampMs)
        }
    }

    PinchHandler {
        target: null
        enabled: root.interaction_enabled
        minimumPointCount: 2
        maximumPointCount: 2

        onScaleChanged: delta => {
            if (!active || !isFinite(delta) || delta <= 0) {
                return
            }

            const usableWidth = plot.width - plot.vbar_width_qml
            if (usableWidth <= 0) {
                return
            }

            const pivot = Math.max(
                0,
                Math.min(1, centroid.position.x / usableWidth))
            plot.adjust_t_from_pivot_and_scale(pivot, 1 / delta)
        }

        onActiveChanged: {
            if (active) {
                indicator.set_mouse_in_plot(false)
                root.clear_hover_sample()
            }
        }
    }

    Component.onCompleted: {
        if (root.dark_mode !== null) {
            plot.dark_mode = root.dark_mode
        }
    }

    onDark_modeChanged: {
        if (root.dark_mode !== null) {
            plot.dark_mode = root.dark_mode
        }
    }
}
