/*
 *  Copyright (c) 2007 Boudewijn Rempt <boud@valdyas.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_shape_layer_canvas.h"

#include <QPainter>
#include <QMutexLocker>

#include <KoShapeManager.h>
#include <KoSelectedShapesProxySimple.h>
#include <KoViewConverter.h>
#include <KoColorSpace.h>

#include <kis_paint_device.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_painter.h>
#include <flake/kis_shape_layer.h>
#include <KoCompositeOpRegistry.h>
#include <KoSelection.h>
#include <KoUnit.h>
#include "kis_image_view_converter.h"

#include <kis_debug.h>

#include <QThread>
#include <QApplication>

#include <kis_spontaneous_job.h>
#include "kis_image.h"
#include "kis_global.h"

//#define DEBUG_REPAINT

KisShapeLayerCanvas::KisShapeLayerCanvas(KisShapeLayer *parent, KisImageWSP image)
        : KoCanvasBase(0)
        , m_isDestroying(false)
        , m_viewConverter(new KisImageViewConverter(image))
        , m_shapeManager(new KoShapeManager(this))
        , m_selectedShapesProxy(new KoSelectedShapesProxySimple(m_shapeManager.data()))
        , m_projection(0)
        , m_parentLayer(parent)
        , m_image(image)
{
    /**
     * The layour should also add itself to its own shape manager, so that the canvas
     * would track its changes/transformations
     */
    m_shapeManager->addShape(parent, KoShapeManager::AddWithoutRepaint);
    m_shapeManager->selection()->setActiveLayer(parent);

    connect(this, SIGNAL(forwardRepaint()), SLOT(repaint()), Qt::QueuedConnection);
}

KisShapeLayerCanvas::~KisShapeLayerCanvas()
{
    m_shapeManager->remove(m_parentLayer);
}

void KisShapeLayerCanvas::setImage(KisImageWSP image)
{
    m_viewConverter->setImage(image);
}

void KisShapeLayerCanvas::prepareForDestroying()
{
    m_isDestroying = true;
}

void KisShapeLayerCanvas::gridSize(QPointF *offset, QSizeF *spacing) const
{
    Q_ASSERT(false); // This should never be called as this canvas should have no tools.
    Q_UNUSED(offset);
    Q_UNUSED(spacing);
}

bool KisShapeLayerCanvas::snapToGrid() const
{
    Q_ASSERT(false); // This should never be called as this canvas should have no tools.
    return false;
}

void KisShapeLayerCanvas::addCommand(KUndo2Command *)
{
    Q_ASSERT(false); // This should never be called as this canvas should have no tools.
}

KoShapeManager *KisShapeLayerCanvas::shapeManager() const
{
    return m_shapeManager.data();
}

KoSelectedShapesProxy *KisShapeLayerCanvas::selectedShapesProxy() const
{
    return m_selectedShapesProxy.data();
}

#ifdef DEBUG_REPAINT
# include <stdlib.h>
#endif


class KisRepaintShapeLayerLayerJob : public KisSpontaneousJob
{
public:
    KisRepaintShapeLayerLayerJob(KisShapeLayerSP layer) : m_layer(layer) {}

    bool overrides(const KisSpontaneousJob *_otherJob) override {
        const KisRepaintShapeLayerLayerJob *otherJob =
            dynamic_cast<const KisRepaintShapeLayerLayerJob*>(_otherJob);

        return otherJob && otherJob->m_layer == m_layer;
    }

    void run() override {
        m_layer->forceUpdateTimedNode();
    }

    int levelOfDetail() const override {
        return 0;
    }

private:
    KisShapeLayerSP m_layer;
};


void KisShapeLayerCanvas::updateCanvas(const QRectF& rc)
{
    dbgUI << "KisShapeLayerCanvas::updateCanvas()" << rc;
    //image is 0, if parentLayer is being deleted so don't update
    if (!m_parentLayer->image() || m_isDestroying) {
        return;
    }

    // grow for antialiasing
    const QRect r = kisGrowRect(m_viewConverter->documentToView(rc).toAlignedRect(), 2);

    {
        QMutexLocker locker(&m_dirtyRegionMutex);
        m_dirtyRegion += r;
        qreal x, y;
        m_viewConverter->zoom(&x, &y);
    }

    /**
     * HACK ALERT!
     *
     * The shapes may be accessed from both, GUI and worker threads! And we have no real
     * guard against this until the vector tools will be ported to the strokes framework.
     *
     * Here we just avoid the most obvious conflict of threads:
     *
     * 1) If the layer if modified by a non-gui (worker) thread, use a spontaneous jobs
     *    to rerender the canvas. The job will be executed (almost) exclusively and it is
     *    the responsibility of the worker thread to add a barrier to wait until this job is
     *    completed, and not try to access the shapes concurrently.
     *
     * 2) If the layer is modified by a gui thread, it means that we are being accessed by
     *    a legacy vector tool. It this case just emit a queued signal to make sure the updates
     *    are compressed a little bit (TODO: add a compressor?)
     */

    if (qApp->thread() == QThread::currentThread()) {
        emit forwardRepaint();
    } else {
        m_image->addSpontaneousJob(new KisRepaintShapeLayerLayerJob(m_parentLayer));
    }
}

void KisShapeLayerCanvas::repaint()
{

    QRect r;

    {
        QMutexLocker locker(&m_dirtyRegionMutex);
        r = m_dirtyRegion.boundingRect();
        m_dirtyRegion = QRegion();
    }

    if (r.isEmpty()) return;

    r = r.intersected(m_parentLayer->image()->bounds());
    QImage image(r.width(), r.height(), QImage::Format_ARGB32);
    image.fill(0);
    QPainter p(&image);

    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.translate(-r.x(), -r.y());
    p.setClipRect(r);
#ifdef DEBUG_REPAINT
    QColor color = QColor(random() % 255, random() % 255, random() % 255);
    p.fillRect(r, color);
#endif

    m_shapeManager->paint(p, *m_viewConverter, false);
    p.end();

    KisPaintDeviceSP dev = new KisPaintDevice(m_projection->colorSpace());
    dev->convertFromQImage(image, 0);

    KisPainter::copyAreaOptimized(r.topLeft(), dev, m_projection, QRect(QPoint(), r.size()));

    m_parentLayer->setDirty(r);
}

KoToolProxy * KisShapeLayerCanvas::toolProxy() const
{
//     Q_ASSERT(false); // This should never be called as this canvas should have no tools.
    return 0;
}

KoViewConverter* KisShapeLayerCanvas::viewConverter() const
{
    return m_viewConverter.data();
}

QWidget* KisShapeLayerCanvas::canvasWidget()
{
    return 0;
}

const QWidget* KisShapeLayerCanvas::canvasWidget() const
{
    return 0;
}

KoUnit KisShapeLayerCanvas::unit() const
{
    Q_ASSERT(false); // This should never be called as this canvas should have no tools.
    return KoUnit(KoUnit::Point);
}

void KisShapeLayerCanvas::forceRepaint()
{
    /**
     * WARNING! Although forceRepaint() may be called from different threads, it is
     * not entirely safe. If the user plays with shapes at the same time (vector tools are
     * not ported to strokes yet), the shapes my be accessed from two different places at
     * the same time, which will cause a crash.
     *
     * The only real solution to this is to port vector tools to strokes framework.
     */

    repaint();
}

