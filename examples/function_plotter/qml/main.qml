import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import FunctionPlotter 1.0
import "." as Local

ApplicationWindow {
    id: window
    visible: true
    width: 1200
    height: 800
    title: "Function Plotter - vnm_plot + mexce"
    color: "#181818"

    // Styling constants
    readonly property int controlHeight: 30
    readonly property int fontSize: 14
    readonly property color textColor: "#e6e6e6"
    readonly property color dimTextColor: "#858585"
    readonly property color surfaceBright: "#2c2c2c"
    readonly property color surfaceLight: "#353535"
    readonly property color buttonHover: "#505050"
    readonly property color buttonPressed: "#434343"
    readonly property color accentBlue: "#8094b0"

    FontLoader { id: iconFont; source: "qrc:/rc/FontAwesome7Free-Solid.otf" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // Row 1: Title
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                text: "Function Plotter"
                font.pixelSize: 18
                font.bold: true
                color: textColor
            }

            Item { Layout.fillWidth: true }

            Text {
                text: "Stack functions"
                font.pixelSize: fontSize
                color: dimTextColor
            }

            Local.StyleToggleButton {
                glyph: "\u03a3"
                tooltip: "Stack functions cumulatively"
                active: functionPlotter.stackFunctions
                onActivated: functionPlotter.stackFunctions = !functionPlotter.stackFunctions
            }
        }

        // Row 2: Function entries (dynamic list)
        ColumnLayout {
            id: functionList
            Layout.fillWidth: true
            spacing: 4

            Repeater {
                model: functionPlotter

                delegate: RowLayout {
                    id: functionDelegate
                    Layout.fillWidth: true
                    spacing: 8

                    required property int index
                    required property var entry
                    required property string expression
                    required property color color
                    required property bool hasError
                    required property string errorMessage
                    required property bool isPlaying
                    required property int numSamples
                    required property bool showDots
                    required property bool showLine
                    required property bool showArea

                    Item {
                        Layout.preferredWidth: dragHandleContent.implicitWidth
                        Layout.preferredHeight: controlHeight

                        RowLayout {
                            id: dragHandleContent
                            anchors.fill: parent
                            spacing: 8

                            Rectangle {
                                width: 8
                                height: controlHeight
                                color: functionDelegate.color
                                radius: 2
                            }

                            Text {
                                text: "f(x" + (functionDelegate.index + 1) + ") ="
                                font.pixelSize: fontSize
                                font.family: "Consolas"
                                color: dimTextColor
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                            hoverEnabled: true
                            preventStealing: true

                            onPositionChanged: function(mouse) {
                                if (!(mouse.buttons & Qt.LeftButton)) {
                                    return
                                }
                                const point = mapToItem(functionList, mouse.x, mouse.y)
                                const rowHeight = controlHeight + functionList.spacing
                                const target = Math.max(0, Math.min(
                                    functionPlotter.functionCount - 1,
                                    Math.floor(point.y / rowHeight)))
                                if (target !== functionDelegate.index) {
                                    functionPlotter.move_function(functionDelegate.index, target)
                                }
                            }
                        }
                    }

                    ComboBox {
                        id: expressionCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: controlHeight
                        model: functionPresets
                        editable: true
                        textRole: "expression"
                        currentIndex: -1
                        font.pixelSize: fontSize
                        font.family: "Consolas"
                        property bool show_error_tooltip: false

                        function update_from_expression() {
                            var idx = -1
                            if (functionPresets) {
                                for (var i = 0; i < functionPresets.length; ++i) {
                                    if (functionPresets[i].expression === functionDelegate.expression) {
                                        idx = i
                                        break
                                    }
                                }
                            }
                            expressionCombo.currentIndex = idx
                            if (idx < 0) {
                                expressionCombo.editText = functionDelegate.expression
                            }
                        }

                        function refresh_error_tooltip() {
                            if (!expressionCombo.hovered) {
                                expressionCombo.show_error_tooltip = false
                                return
                            }
                            if (functionDelegate.hasError && functionDelegate.errorMessage.length > 0) {
                                expressionCombo.show_error_tooltip = false
                                expressionCombo.show_error_tooltip = true
                            }
                            else {
                                expressionCombo.show_error_tooltip = false
                            }
                        }

                        // Error tooltip - only shown when there's an error
                        ToolTip.visible: expressionCombo.show_error_tooltip
                        ToolTip.delay: 200
                        ToolTip.text: functionDelegate.errorMessage

                        // Set initial text from model
                        Component.onCompleted: update_from_expression()

                        // Update text when model changes (but not while user is editing)
                        Connections {
                            target: functionDelegate
                            function onExpressionChanged() {
                                if (!expressionCombo.activeFocus) {
                                    expressionCombo.update_from_expression()
                                }
                            }
                            function onErrorMessageChanged() {
                                expressionCombo.refresh_error_tooltip()
                            }
                        }

                        onHoveredChanged: {
                            expressionCombo.refresh_error_tooltip()
                        }

                        background: Rectangle {
                            color: surfaceBright
                            border.color: functionDelegate.hasError ? "#ff4444" : (expressionCombo.activeFocus ? accentBlue : surfaceLight)
                            border.width: functionDelegate.hasError ? 2 : (expressionCombo.activeFocus ? 2 : 1)
                            radius: 2
                        }

                        contentItem: TextInput {
                            text: expressionCombo.editText
                            font: expressionCombo.font
                            color: textColor
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 8
                            rightPadding: controlHeight
                            selectByMouse: true
                            inputMethodHints: Qt.ImhNone

                            onTextEdited: {
                                if (expressionCombo.editText !== text) {
                                    expressionCombo.editText = text
                                }
                            }
                        }

                        indicator: Rectangle {
                            id: dropdownHotspot
                            width: controlHeight
                            height: expressionCombo.height
                            x: expressionCombo.width - width
                            y: 0
                            color: indicatorArea.pressed ? buttonPressed : indicatorArea.containsMouse ? buttonHover : "transparent"
                            radius: 2

                            Text {
                                anchors.centerIn: parent
                                text: "\u25BC"
                                font.pixelSize: 8
                                color: dimTextColor
                            }

                            MouseArea {
                                id: indicatorArea
                                anchors.fill: parent
                                hoverEnabled: true

                                onClicked: {
                                    if (expressionCombo.popup.visible) {
                                        expressionCombo.popup.close()
                                    }
                                    else {
                                        expressionCombo.popup.open()
                                    }
                                }
                            }
                        }

                        popup: Popup {
                            y: expressionCombo.height
                            width: expressionCombo.width
                            padding: 1

                            background: Rectangle {
                                color: surfaceBright
                                border.color: surfaceLight
                                border.width: 1
                            }

                            contentItem: ListView {
                                implicitHeight: contentHeight
                                model: expressionCombo.popup.visible ? expressionCombo.delegateModel : null
                                clip: true
                                ScrollIndicator.vertical: ScrollIndicator {}
                            }
                        }

                        delegate: ItemDelegate {
                            width: expressionCombo.width
                            height: controlHeight

                            contentItem: Text {
                                text: modelData && modelData.name
                                    ? modelData.name + "  (" + modelData.expression + ")"
                                    : (modelData && modelData.expression ? modelData.expression : "")
                                font.pixelSize: fontSize
                                color: textColor
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 8
                                elide: Text.ElideRight
                            }

                            background: Rectangle {
                                color: highlighted ? buttonHover : "transparent"
                            }

                            highlighted: expressionCombo.highlightedIndex === index
                        }

                        onEditTextChanged: {
                            if (!expressionCombo.activeFocus) {
                                return
                            }
                            if (functionDelegate.entry && functionDelegate.entry.expression !== editText) {
                                functionDelegate.entry.expression = editText
                            }
                        }

                        onActivated: function(preset_index) {
                            if (preset_index < 0 || !functionDelegate.entry) {
                                return
                            }
                            const preset = functionPresets[preset_index]
                            if (preset && preset.expression) {
                                expressionCombo.editText = preset.expression
                                functionDelegate.entry.expression = preset.expression
                            }
                            if (preset && preset.xMin !== undefined) {
                                functionPlotter.xMin = preset.xMin
                            }
                            if (preset && preset.xMax !== undefined) {
                                functionPlotter.xMax = preset.xMax
                            }
                        }
                    }

                    // Samples spinbox for this function
                    ValidatingSpinBox {
                        id: samplesSpin
                        from: 10
                        to: 1000000
                        value: functionDelegate.numSamples
                        stepSize: 1
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: controlHeight
                        font.pixelSize: fontSize - 2
                        textColor: window.textColor
                        surfaceBright: window.surfaceBright
                        surfaceLight: window.surfaceLight
                        buttonHover: window.buttonHover
                        buttonPressed: window.buttonPressed
                        indicatorWidth: 18
                        indicatorFontPixelSize: 9

                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: "Number of samples"

                        onValueModified: {
                            if (functionDelegate.entry) {
                                functionDelegate.entry.numSamples = value
                            }
                        }
                    }

                    // Display style toggle buttons
                    RowLayout {
                        spacing: 2

                        StyleToggleButton {
                            glyph: "\u2022"  // Bullet for dots
                            tooltip: "Show as dots"
                            active: functionDelegate.showDots
                            glyphSize: fontSize
                            Layout.preferredHeight: controlHeight
                            onActivated: if (functionDelegate.entry) functionDelegate.entry.showDots = !functionDelegate.showDots
                        }

                        StyleToggleButton {
                            glyph: "\u2014"  // Em dash for line
                            tooltip: "Show as line"
                            active: functionDelegate.showLine
                            glyphSize: fontSize
                            Layout.preferredHeight: controlHeight
                            onActivated: if (functionDelegate.entry) functionDelegate.entry.showLine = !functionDelegate.showLine
                        }

                        StyleToggleButton {
                            glyph: "\u25a0"  // Filled square for area
                            tooltip: "Show as filled area"
                            active: functionDelegate.showArea
                            glyphSize: fontSize - 2
                            Layout.preferredHeight: controlHeight
                            onActivated: if (functionDelegate.entry) functionDelegate.entry.showArea = !functionDelegate.showArea
                        }
                    }

                    Button {
                        id: playButton
                        text: functionDelegate.isPlaying ? "\uf04c" : "\uf04b"
                        Layout.preferredWidth: controlHeight
                        Layout.preferredHeight: controlHeight
                        font.family: iconFont.name
                        font.pixelSize: fontSize

                        contentItem: Text {
                            text: playButton.text
                            font: playButton.font
                            color: textColor
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            color: playButton.pressed ? buttonPressed : playButton.hovered ? buttonHover : surfaceBright
                            border.color: surfaceLight
                            border.width: 1
                            radius: playButton.height / 2
                        }

                        onClicked: {
                            if (functionDelegate.entry) {
                                functionDelegate.entry.play_sound()
                            }
                        }
                    }

                    Button {
                        id: removeButton
                        text: "\uf00d"  // X icon
                        Layout.preferredWidth: controlHeight
                        Layout.preferredHeight: controlHeight
                        font.family: iconFont.name
                        font.pixelSize: fontSize
                        visible: functionPlotter.functionCount > 1

                        contentItem: Text {
                            text: removeButton.text
                            font: removeButton.font
                            color: removeButton.hovered ? "#ff6666" : dimTextColor
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            color: removeButton.pressed ? buttonPressed : removeButton.hovered ? buttonHover : "transparent"
                            border.color: removeButton.hovered ? surfaceLight : "transparent"
                            border.width: 1
                            radius: 2
                        }

                        onClicked: functionPlotter.remove_function(functionDelegate.index)
                    }
                }
            }
        }

        // Row 3: Range controls with Fit Vertical and Add Function on the right
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                text: "x min:"
                font.pixelSize: fontSize
                color: dimTextColor
                Layout.alignment: Qt.AlignVCenter
            }

            ValidatingSpinBox {
                id: xMinSpin
                from: -1000
                to: 1000
                value: functionPlotter.xMin
                Layout.preferredWidth: 90
                Layout.preferredHeight: controlHeight
                font.pixelSize: fontSize
                textColor: window.textColor
                surfaceBright: window.surfaceBright
                surfaceLight: window.surfaceLight
                buttonHover: window.buttonHover
                buttonPressed: window.buttonPressed

                onValueChanged: functionPlotter.xMin = value
            }

            Text {
                text: "x max:"
                font.pixelSize: fontSize
                color: dimTextColor
                Layout.alignment: Qt.AlignVCenter
            }

            ValidatingSpinBox {
                id: xMaxSpin
                from: -1000
                to: 1000
                value: functionPlotter.xMax
                Layout.preferredWidth: 90
                Layout.preferredHeight: controlHeight
                font.pixelSize: fontSize
                textColor: window.textColor
                surfaceBright: window.surfaceBright
                surfaceLight: window.surfaceLight
                buttonHover: window.buttonHover
                buttonPressed: window.buttonPressed

                onValueChanged: functionPlotter.xMax = value
            }

            Item { Layout.fillWidth: true }

            Button {
                id: autoVButton
                text: "Fit Vertical"
                Layout.preferredWidth: 70
                Layout.preferredHeight: controlHeight
                font.pixelSize: fontSize

                contentItem: Text {
                    text: autoVButton.text
                    font: autoVButton.font
                    color: textColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    color: autoVButton.pressed ? buttonPressed : autoVButton.hovered ? buttonHover : surfaceBright
                    border.color: surfaceLight
                    border.width: 1
                    radius: 2
                }

                onClicked: {
                    if (plotView && plotView.plot_widget) {
                        plotView.plot_widget.auto_adjust_view(false, 0.5, false)
                    }
                }
            }

            Button {
                id: addFunctionButton
                text: "\uf067 Add Function"  // Plus icon
                Layout.preferredHeight: controlHeight
                font.pixelSize: fontSize

                contentItem: RowLayout {
                    spacing: 6
                    Text {
                        text: "\uf067"
                        font.family: iconFont.name
                        font.pixelSize: fontSize - 2
                        color: textColor
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        text: "Add Function"
                        font.pixelSize: fontSize
                        color: textColor
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                background: Rectangle {
                    color: addFunctionButton.pressed ? buttonPressed : addFunctionButton.hovered ? buttonHover : surfaceBright
                    border.color: accentBlue
                    border.width: 1
                    radius: 2
                }

                onClicked: functionPlotter.add_function()
            }
        }

        // Row 4: Plot area (takes remaining space)
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#202020"
            border.color: surfaceLight
            border.width: 1
            radius: 4

            Local.PlotView {
                id: plotView
                anchors.fill: parent
                anchors.margins: 8

                Component.onCompleted: {
                    functionPlotter.plot_widget = plotView.plot_widget
                }
            }

        }

        // Row 5: Footer
        RowLayout {
            Layout.fillWidth: true

            Item { Layout.fillWidth: true }

            Text {
                text: "vnm_plot v0.1.0"
                color: "#666666"
                font.pixelSize: 10
            }
        }
    }
}
