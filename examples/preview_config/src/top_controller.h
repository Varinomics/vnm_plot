#pragma once

#include "preview_controller.h"

class Top_controller : public Example_plot_controller
{
    Q_OBJECT

public:
    explicit Top_controller(QObject* parent = nullptr)
    :
        Example_plot_controller(Preset::TOP, parent)
    {}
};
