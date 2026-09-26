#include <vnm_plot/rhi/asset_loader.h>

#if defined(VNM_PLOT_ENABLE_TEXT)
#include <QByteArray>
#include <QFile>
#include <QResource>

// Q_INIT_RESOURCE must reference the generated symbol in the global namespace.
static QByteArray load_bundled_font()
{
    Q_INIT_RESOURCE(vnm_plot_fonts);
    QFile file(QStringLiteral(":/vnm_plot/fonts/monospace.ttf"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
#endif

namespace vnm::plot {

void init_embedded_assets(Asset_loader& loader)
{
#if defined(VNM_PLOT_ENABLE_TEXT)
    // Asset_loader borrows registered bytes. Retain the resource's uncompressed
    // contents for the process lifetime, including after this loader is destroyed.
    static const QByteArray font = load_bundled_font();
    if (!font.isEmpty()) {
        loader.register_embedded("fonts/monospace.ttf",
            std::string_view(font.constData(), static_cast<std::size_t>(font.size())));
    }
#else
    (void)loader;
#endif
}

} // namespace vnm::plot
