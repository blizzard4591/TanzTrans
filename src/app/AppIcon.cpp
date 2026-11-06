#include "AppIcon.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace tt::app {

QIcon makeAppIcon()
{
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(0.5, 0.5, size - 1.0, size - 1.0);
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0, QColor(0x7b, 0x3f, 0xe4));
        g.setColorAt(1, QColor(0x1e, 0x88, 0xe5));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(r, size * 0.22, size * 0.22);

        // play triangle
        QPainterPath tri;
        tri.moveTo(size * 0.30, size * 0.22);
        tri.lineTo(size * 0.30, size * 0.62);
        tri.lineTo(size * 0.64, size * 0.42);
        tri.closeSubpath();
        p.setBrush(Qt::white);
        p.drawPath(tri);

        // "shrink" bar: wide on the left, narrow on the right
        QPainterPath bar;
        bar.moveTo(size * 0.20, size * 0.70);
        bar.lineTo(size * 0.80, size * 0.74);
        bar.lineTo(size * 0.80, size * 0.78);
        bar.lineTo(size * 0.20, size * 0.84);
        bar.closeSubpath();
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawPath(bar);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

} // namespace tt::app
