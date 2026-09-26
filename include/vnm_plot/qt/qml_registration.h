#pragma once

namespace vnm::plot {

// Call on the GUI thread before creating a QML engine. Registers the VnmPlot
// 1.0 C++ and QML components, including their embedded resources. Repeated
// calls are harmless; no engine-specific import path is required.
void register_qml_types();

} // namespace vnm::plot
