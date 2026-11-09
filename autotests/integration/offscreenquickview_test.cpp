/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "backends/virtual/virtual_egl_backend.h"
#include "compositor.h"
#include "core/renderbackend.h"
#include "effect/effectframe.h"
#include "effect/effecthandler.h"
#include "effect/effectloader.h"
#include "effect/offscreenquickview.h"
#include "effect/quickeffect.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "opengl/gltexture.h"
#include "scene/item.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"

#include <KConfigGroup>
#include <KWayland/Client/surface.h>
#include <QDir>
#include <QFile>
#include <QQuickWindow>
#include <QStandardPaths>

namespace KWin
{

static QImage grabOutput(LogicalOutput *output)
{
    SceneView *view = nullptr;
    for (auto candidate : effects->scene()->views()) {
        auto sceneView = dynamic_cast<SceneView *>(candidate);
        if (sceneView && sceneView->logicalOutput() == output) {
            view = sceneView;
            break;
        }
    }
    if (!view) {
        return {};
    }
    auto layer = dynamic_cast<VirtualEglLayer *>(view->layer());
    auto backend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
    if (!layer || !layer->texture() || !backend || !backend->openglContext()->makeCurrent()) {
        return {};
    }
    if (layer->sourceRect() != RectF(view->deviceRect()) || layer->targetRect() != view->deviceRect()) {
        return {};
    }
    // The virtual output target already flips the scene into OpenGL's buffer coordinates.
    return layer->texture()->toImage();
}

class OffscreenQuickViewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void testOpacity_data();
    void testOpacity();
    void testResynchronizeOpacity();
    void testOpacityCapabilityVersion();
    void testOpaqueRegion_data();
    void testOpaqueRegion();
    void testRepaint();
    void testComposition();
    void testEffectFrame();
};

void OffscreenQuickViewTest::initTestCase()
{
    QVERIFY(waylandServer()->init(qAppName()));
    auto config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup plugins(config, QStringLiteral("Plugins"));
    for (const QString &name : EffectLoader().listOfKnownEffects()) {
        plugins.writeEntry(name + QStringLiteral("Enabled"), false);
    }
    config->sync();
    kwinApp()->setConfig(config);
    qputenv("KWIN_COMPOSE", QByteArrayLiteral("O2"));
    kwinApp()->start();
    Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
    QCOMPARE(Compositor::self()->backend()->compositingType(), KWin::OpenGLCompositing);
}

void OffscreenQuickViewTest::cleanup()
{
    Test::destroyWaylandConnection();
}

void OffscreenQuickViewTest::testOpacity_data()
{
    QTest::addColumn<qreal>("opacity");
    QTest::newRow("zero") << qreal(0);
    QTest::newRow("fractional") << qreal(0.25);
    QTest::newRow("opaque") << qreal(1);
}

void OffscreenQuickViewTest::testOpacity()
{
    QFETCH(qreal, opacity);
    QSignalSpy added(effects->scene()->overlayItem(), &Item::childAdded);
    OffscreenQuickView view(OffscreenQuickView::ExportMode::Texture, false);
    QCOMPARE(added.count(), 1);
    Item *item = added.first().first().value<Item *>();
    QVERIFY(item);
    view.setOpacity(0.5);
    view.setOpacity(opacity);
    QCOMPARE(view.opacity(), opacity);
    QCOMPARE(view.window()->opacity(), opacity);
    QCOMPARE(item->opacity(), opacity);
    QVERIFY(view.isVisible());
    QVERIFY(item->isVisible());
}

void OffscreenQuickViewTest::testResynchronizeOpacity()
{
    QSignalSpy added(effects->scene()->overlayItem(), &Item::childAdded);
    OffscreenQuickView view(OffscreenQuickView::ExportMode::Texture, false);
    QCOMPARE(added.count(), 1);
    Item *item = added.first().first().value<Item *>();
    QVERIFY(item);
    view.setOpacity(0.5);
    view.window()->setOpacity(0.25);
    view.setOpacity(0.25);
    QCOMPARE(item->opacity(), 0.25);
    QCOMPARE(view.opacity(), 0.25);
}

void OffscreenQuickViewTest::testOpacityCapabilityVersion()
{
    // The downstream capability is visible before any scene view exists and
    // describes both the opacity propagation and the occlusion behavior that
    // the tests above verify.
    const QMetaObject &metaObject = QuickSceneEffect::staticMetaObject;
    const int index = metaObject.indexOfProperty("plasmaFusionOffscreenOpacityVersion");
    QVERIFY(index >= 0);
    const QMetaProperty property = metaObject.property(index);
    QVERIFY(property.isValid());
    QVERIFY(property.isReadable());
    QVERIFY(!property.isWritable());
    QCOMPARE(property.typeId(), QMetaType::Int);

    QuickSceneEffect effect;
    QCOMPARE(effect.property("plasmaFusionOffscreenOpacityVersion").toInt(), 1);
    QCOMPARE(effect.plasmaFusionOffscreenOpacityVersion(), 1);
    QVERIFY(!effect.viewAt(QPoint(0, 0)));
}

void OffscreenQuickViewTest::testOpaqueRegion_data()
{
    QTest::addColumn<qreal>("opacity");
    QTest::addColumn<bool>("alpha");
    QTest::newRow("zero") << qreal(0) << false;
    QTest::newRow("fractional") << qreal(0.5) << false;
    QTest::newRow("opaque") << qreal(1) << false;
    QTest::newRow("alpha-zero") << qreal(0) << true;
    QTest::newRow("alpha-fractional") << qreal(0.5) << true;
    QTest::newRow("alpha-opaque") << qreal(1) << true;
}

void OffscreenQuickViewTest::testOpaqueRegion()
{
    QFETCH(qreal, opacity);
    QFETCH(bool, alpha);
    QSignalSpy added(effects->scene()->overlayItem(), &Item::childAdded);
    OffscreenQuickView view(OffscreenQuickView::ExportMode::Texture, alpha);
    QCOMPARE(added.count(), 1);
    Item *item = added.first().first().value<Item *>();
    QVERIFY(item);
    view.setGeometry(Rect(0, 0, 200, 100));
    view.window()->setColor(Qt::red);
    view.update(nullptr);
    auto surface = qobject_cast<SurfaceItem *>(item);
    QVERIFY(surface);
    QVERIFY(surface->buffer());
    QCOMPARE(surface->hasAlphaChannel(), alpha);
    QCOMPARE(item->opaque().isEmpty(), alpha);
    view.setOpacity(opacity);
    QCOMPARE(item->opaque().isEmpty(), alpha || opacity != 1);
}

void OffscreenQuickViewTest::testRepaint()
{
    QSignalSpy added(effects->scene()->overlayItem(), &Item::childAdded);
    OffscreenQuickView view(OffscreenQuickView::ExportMode::Texture, false);
    QCOMPARE(added.count(), 1);
    Item *item = added.first().first().value<Item *>();
    QVERIFY(item);
    view.setGeometry(Rect(0, 0, 200, 100));
    const auto views = effects->scene()->views();
    QVERIFY(!views.isEmpty());
    RenderView *renderView = views.first();
    item->resetRepaints(renderView);
    QVERIFY(!item->hasRepaints(renderView));
    view.setOpacity(0.5);
    QVERIFY(item->hasRepaints(renderView));
}

void OffscreenQuickViewTest::testComposition()
{
    QVERIFY(Test::setupWaylandConnection());
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(window);
    QTRY_VERIFY(window->readyForPainting());
    QTRY_VERIFY(window->windowItem()->isVisible());
    QVERIFY(window->windowItem()->surfaceItem()->buffer());
    QSignalSpy initialFrame(effects->scene(), &WorkspaceScene::frameRendered);
    effects->scene()->addRepaintFull();
    QVERIFY(initialFrame.wait());
    const auto grab = [window]() {
        return grabOutput(window->output());
    };
    const QPoint sample = window->clientGeometry().center().toPoint();
    QTRY_VERIFY(!grab().isNull());
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::blue));

    QSignalSpy frameRendered(effects->scene(), &WorkspaceScene::frameRendered);
    OffscreenQuickView view(OffscreenQuickView::ExportMode::Texture, false);
    view.setAutomaticRepaint(false);
    view.setGeometry(window->clientGeometry().rounded());
    view.window()->setColor(Qt::red);
    // Zero opacity must apply to the first composed frame, not only a later update.
    view.setOpacity(0);
    view.update(nullptr);
    QVERIFY(frameRendered.wait());
    QCOMPARE(grab().pixelColor(sample), QColor(Qt::blue));

    view.setOpacity(1);
    QVERIFY(frameRendered.wait());
    QCOMPARE(grab().pixelColor(sample), QColor(Qt::red));

    view.setOpacity(0.5);
    QVERIFY(frameRendered.wait());
    const QColor mixed = grab().pixelColor(sample);
    QVERIFY(mixed.red() > 0 && mixed.red() < 255);
    QVERIFY(mixed.blue() > 0 && mixed.blue() < 255);
    QVERIFY(std::abs(mixed.red() - mixed.blue()) <= 1);
    QCOMPARE(mixed.green(), 0);

    // Only the client changes: an unchanged translucent overlay must not occlude its damage.
    Test::render(surface.get(), QSize(200, 100), Qt::green);
    QTRY_VERIFY(grab().pixelColor(sample).green() > 0);
    const QColor updated = grab().pixelColor(sample);
    QVERIFY(updated.red() > 0 && updated.red() < 255);
    QCOMPARE(updated.blue(), 0);

    view.setOpacity(0);
    QVERIFY(frameRendered.wait());
    QCOMPARE(grab().pixelColor(sample), QColor(Qt::green));
    Test::render(surface.get(), QSize(200, 100), Qt::blue);
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::blue));

    view.setOpacity(1);
    QVERIFY(frameRendered.wait());
    QCOMPARE(grab().pixelColor(sample), QColor(Qt::red));
}

void OffscreenQuickViewTest::testEffectFrame()
{
    const QString framePath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/kwin/frames/plasma");
    QVERIFY(QDir().mkpath(framePath));
    QVERIFY(QFile::copy(QFINDTESTDATA("../../src/qml/frames/plasma/frame_unstyled.qml"),
                        framePath + QStringLiteral("/frame_unstyled.qml")));
    QVERIFY(Test::setupWaylandConnection());
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(window);
    QTRY_VERIFY(window->readyForPainting());
    QTRY_VERIFY(window->windowItem()->isVisible());
    const QPoint sample = window->clientGeometry().center().toPoint();
    const auto grab = [window]() {
        return grabOutput(window->output());
    };
    QSignalSpy frameRendered(effects->scene(), &WorkspaceScene::frameRendered);
    effects->scene()->addRepaintFull();
    QVERIFY(frameRendered.wait());
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::blue));

    EffectFrame frame(EffectFrameUnstyled, false, sample);
    QPixmap icon(32, 32);
    icon.fill(Qt::red);
    frame.setIcon(QIcon(icon));
    frame.setIconSize(icon.size());
    QImage targetImage(1280, 1024, QImage::Format_ARGB32_Premultiplied);
    const RenderTarget target(&targetImage);
    const RenderViewport viewport(RectF(0, 0, 1280, 1024), 1, target, QPoint());
    frame.render(target, viewport, Region::infinite(), 1, 1);
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::red));
    const QPoint edge(frame.geometry().left() + 2, sample.y());
    QTRY_COMPARE(grab().pixelColor(edge), QColor(Qt::black));

    frame.render(target, viewport, Region::infinite(), 0.5, 1);
    QVERIFY(frameRendered.wait());
    const QColor mixed = grab().pixelColor(sample);
    QVERIFY(mixed.red() > 0 && mixed.red() < 255);
    QVERIFY(std::abs(mixed.red() - mixed.blue()) <= 1);
    const QColor fadedFrame = grab().pixelColor(edge);
    QVERIFY(fadedFrame.blue() > 0 && fadedFrame.blue() < 255);

    frame.render(target, viewport, Region::infinite(), 0, 1);
    QVERIFY(frameRendered.wait());
    QCOMPARE(grab().pixelColor(sample), QColor(Qt::blue));
    QCOMPARE(grab().pixelColor(edge), QColor(Qt::blue));

    frame.render(target, viewport, Region::infinite(), 1, 0);
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::red));
    QTRY_COMPARE(grab().pixelColor(edge), QColor(Qt::blue));
    frame.render(target, viewport, Region::infinite(), 1, 1);
    QTRY_COMPARE(grab().pixelColor(edge), QColor(Qt::black));
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::OffscreenQuickViewTest)
#include "offscreenquickview_test.moc"
