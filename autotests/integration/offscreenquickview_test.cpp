/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "backends/virtual/virtual_egl_backend.h"
#include "compositor.h"
#include "core/renderbackend.h"
#include "core/renderloop_p.h"
#include "effect/effectframe.h"
#include "effect/effecthandler.h"
#include "effect/effectloader.h"
#include "effect/offscreenquickview.h"
#include "effect/quickeffect.h"
#include "input.h"
#include "opengl/eglbackend.h"
#include "opengl/eglcontext.h"
#include "opengl/glframebuffer.h"
#include "opengl/gltexture.h"
#include "pointer_input.h"
#include "scene/item.h"
#include "scene/itemrenderer_opengl.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"

#include <KConfigGroup>
#include <KWayland/Client/pointer.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/shm_pool.h>
#include <KWayland/Client/surface.h>
#include <QDir>
#include <QFile>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QStandardPaths>

#include <optional>
#include <type_traits>
#include <utility>

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

// The layer selector reads buffer metadata only. This buffer is never imported
// or sent to an output; its item exists only for synchronous candidate selection.
class LayerCandidateBuffer : public GraphicsBuffer
{
public:
    explicit LayerCandidateBuffer(const QSize &size)
        : m_attributes{.planeCount = 1, .width = size.width(), .height = size.height(), .format = DRM_FORMAT_XRGB8888}
    {
    }

    QSize size() const override
    {
        return QSize(m_attributes.width, m_attributes.height);
    }
    bool hasAlphaChannel() const override
    {
        return false;
    }
    const DmaBufAttributes *dmabufAttributes() const override
    {
        return &m_attributes;
    }
    uint32_t format() const override
    {
        return m_attributes.format;
    }
    uint64_t modifier() const override
    {
        return m_attributes.modifier;
    }
    uint32_t pitch() const override
    {
        return uint32_t(m_attributes.width) * 4;
    }

private:
    const DmaBufAttributes m_attributes;
};

class LayerCandidateSurface : public SurfaceItem
{
public:
    LayerCandidateSurface(Item *parent, GraphicsBuffer *buffer)
        : SurfaceItem(parent)
    {
        setBuffer(buffer, FileDescriptor{});
        setDestinationSize(buffer->size());
    }

    RegionF opaque() const override
    {
        return RegionF(rect());
    }
};

// Plasma/6.8 constructs a RenderTarget from a QImage; master only wraps
// framebuffers. Detect once which shape this build provides.
template<typename Image, typename = void>
constexpr bool ImageRenderTargetConstructor = false;

template<typename Image>
constexpr bool ImageRenderTargetConstructor<Image, std::void_t<decltype(RenderTarget(std::declval<Image *>()))>> = true;

// EffectFrame::render() only shows the view and forwards the opacity values; its
// render target and viewport are signature scaffolding that nothing reads. Build
// the placeholder from whichever shape this build provides. The optional types
// are template parameters so each branch's emplace call is only checked in the
// builds that take that branch.
template<bool HasImageConstructor, typename ImageOptional, typename TargetOptional>
void emplaceEffectFrameTarget(ImageOptional &image, TargetOptional &target)
{
    if constexpr (HasImageConstructor) {
        image.emplace(1280, 1024, QImage::Format_ARGB32_Premultiplied);
        target.emplace(&image.value());
    } else {
        // A foreign wrapper owns nothing and performs no GL calls; it is static
        // so it never destructs mid-test, and its context is recorded while the
        // shared context is current, the same way grabOutput() makes it current.
        static GLFramebuffer targetFramebuffer = []() {
            auto openglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend());
            openglBackend->openglContext()->makeCurrent();
            return GLFramebuffer(0, QSize(1280, 1024));
        }();
        target.emplace(&targetFramebuffer);
    }
}

class OffscreenQuickViewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void testOpacity_data();
    void testOpacity();
    void testLayerCandidatesWithParentOpacity_data();
    void testLayerCandidatesWithParentOpacity();
    void testResynchronizeOpacity();
    void testOpacityCapabilityVersion();
    void testOpaqueRegion_data();
    void testOpaqueRegion();
    void testRepaint();
    void testComposition();
    void testEffectFrame();
    void testRendererRefusesToStart();
    void testCursorCaptureRendererRefusesToStart();
};

class FailingItemRenderer : public ItemRendererOpenGL
{
public:
    using ItemRendererOpenGL::ItemRendererOpenGL;

    bool beginFrame(const RenderTarget &target, const RenderViewport &viewport) override
    {
        ++beginCount;
        sawRenderTarget = target.framebuffer() && target.framebuffer()->valid();
        return !failBegin && ItemRendererOpenGL::beginFrame(target, viewport);
    }

    void endFrame() override
    {
        ++endCount;
        ItemRendererOpenGL::endFrame();
    }

    int beginCount = 0;
    int endCount = 0;
    bool failBegin = false;
    bool sawRenderTarget = false;
};

class CursorCaptureRenderer : public FailingItemRenderer
{
public:
    using FailingItemRenderer::FailingItemRenderer;

    bool beginFrame(const RenderTarget &target, const RenderViewport &viewport) override
    {
        const bool started = FailingItemRenderer::beginFrame(target, viewport);
        if (failBegin) {
            // Define every pixel of the private test target even on the refusal path.
            // A baseline build that reads it back can only see this synthetic color.
            if (!sawRenderTarget || !target.texture()) {
                qFatal("Missing synthetic cursor render target");
            }
            QImage sentinel(target.texture()->size(), QImage::Format_RGBA8888_Premultiplied);
            sentinel.fill(Qt::magenta);
            target.texture()->update(sentinel, Region(Rect(sentinel.rect())));
            if (glGetError() != GL_NO_ERROR) {
                qFatal("Could not initialize the synthetic cursor render target");
            }
        }
        return started;
    }
};

void OffscreenQuickViewTest::testCursorCaptureRendererRefusesToStart()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat));
    QVERIFY(Test::waitForWaylandPointer());
    std::unique_ptr<KWayland::Client::Pointer> pointer(Test::waylandSeat()->createPointer());
    QVERIFY(pointer && pointer->isValid());
    QSignalSpy entered(pointer.get(), &KWayland::Client::Pointer::entered);

    input()->pointer()->warp(QPointF(0, 0));
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(window);
    input()->pointer()->warp(window->clientGeometry().center());
    QTRY_VERIFY(!entered.isEmpty());

    auto cursorSurface = Test::createSurface();
    QSignalSpy cursorRendered(cursorSurface.get(), &KWayland::Client::Surface::frameRendered);
    QImage cursor(QSize(10, 10), QImage::Format_RGBA8888_Premultiplied);
    cursor.fill(Qt::red);
    cursorSurface->attachBuffer(Test::waylandShmPool()->createBuffer(cursor));
    cursorSurface->damage(cursor.rect());
    cursorSurface->commit();
    const QPoint hotspot(4, 5);
    pointer->setCursor(cursorSurface.get(), hotspot);
    QTRY_VERIFY(!cursorRendered.isEmpty());

    auto scene = kwinApp()->scene();
    auto device = Compositor::self()->primaryDevice();
    auto captureRenderer = std::make_unique<CursorCaptureRenderer>(device);
    auto renderer = captureRenderer.get();
    scene->attachRenderer(device, std::move(captureRenderer));
    const auto restoreRenderer = qScopeGuard([scene, device]() {
        scene->attachRenderer(device, std::make_unique<ItemRendererOpenGL>(device));
    });

    renderer->failBegin = true;
    const auto refused = kwinApp()->cursorImage();
    QCOMPARE(renderer->beginCount, 1);
    QVERIFY(renderer->sawRenderTarget);
    QCOMPARE(renderer->endCount, 0);
    QVERIFY(refused.isNull());

    renderer->failBegin = false;
    const auto captured = kwinApp()->cursorImage();
    QCOMPARE(renderer->beginCount, 2);
    QCOMPARE(renderer->endCount, 1);
    QCOMPARE(captured.image(), cursor);
    QCOMPARE(captured.hotSpot(), QPointF(hotspot));
}

void OffscreenQuickViewTest::testRendererRefusesToStart()
{
    QVERIFY(Test::setupWaylandConnection());
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(window);

    auto scene = kwinApp()->scene();
    auto device = Compositor::self()->primaryDevice();
    auto output = window->output();
    auto loop = output->backendOutput()->renderLoop();
    auto loopState = RenderLoopPrivate::get(loop);
    auto layer = Compositor::self()->backend()->compatibleOutputLayers(output->backendOutput()).front();
    const auto sample = window->clientGeometry().center().toPoint();
    QTRY_COMPARE(grabOutput(output).pixelColor(sample), QColor(Qt::blue));
    QTRY_COMPARE(loopState->pendingFrameCount, 0);

    auto failingRenderer = std::make_unique<FailingItemRenderer>(device);
    auto renderer = failingRenderer.get();
    scene->attachRenderer(device, std::move(failingRenderer));
    const auto restoreRenderer = qScopeGuard([scene, device]() {
        scene->attachRenderer(device, std::make_unique<ItemRendererOpenGL>(device));
    });
    loop->inhibit();
    auto resumeLoop = qScopeGuard([loop]() {
        loop->uninhibit();
    });

    // Queue ordinary client damage, then drive just one attempt with the virtual output.
    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QTRY_COMPARE(committed.count(), 1);
    QVERIFY(layer->needsRepaint());
    loopState->pendingReschedule.reset();
    const auto previousFramebuffer = renderer->context()->currentFramebuffer();
    renderer->failBegin = true;
    loopState->dispatch();
    QCOMPARE(renderer->beginCount, 1);
    QVERIFY(renderer->sawRenderTarget);
    // The virtual backend would hold a submitted frame until synthetic vblank.
    // A dropped frame decrements this synchronously, without waiting for a timer.
    QCOMPARE(loopState->pendingFrameCount, 0);
    QCOMPARE(renderer->endCount, 0);
    QCOMPARE(renderer->context()->currentFramebuffer(), previousFramebuffer);
    QCOMPARE(grabOutput(output).pixelColor(sample), QColor(Qt::blue));
    QCOMPARE(layer->deviceRepaints(), Region::infinite());
    QVERIFY(loopState->pendingReschedule.has_value());

    renderer->failBegin = false;
    QSignalSpy presented(loop, &RenderLoop::framePresented);
    resumeLoop.dismiss();
    loop->uninhibit();
    QVERIFY(presented.wait());
    QCOMPARE(renderer->beginCount, 2);
    QCOMPARE(renderer->endCount, 1);
    QCOMPARE(grabOutput(output).pixelColor(sample), QColor(Qt::red));
}

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
    QCOMPARE(item->opacity(), opacity);
    // The opacity is the item's, as in upstream's own fix (ff26dc46b3). The view's QWindow is
    // left untouched: it has no platform window, so a value set there is stored and never used.
    QCOMPARE(view.window()->opacity(), 1.0);
    QVERIFY(view.isVisible());
    QVERIFY(item->isVisible());
}

void OffscreenQuickViewTest::testLayerCandidatesWithParentOpacity_data()
{
    QTest::addColumn<qreal>("opacity");
    QTest::newRow("invisible") << qreal(0);
    QTest::newRow("translucent") << qreal(0.5);
    QTest::newRow("opaque") << qreal(1);
}

void OffscreenQuickViewTest::testLayerCandidatesWithParentOpacity()
{
    QFETCH(qreal, opacity);
    auto scene = effects->scene();
    QTRY_VERIFY(!scene->sceneViews().empty());
    auto view = scene->sceneViews().front();
    QVERIFY(!effects->blocksDirectScanout());

    Item *cursor = scene->cursorItem();
    const bool cursorWasVisible = cursor->isVisible();
    cursor->setVisible(false);
    const auto restoreCursor = qScopeGuard([cursor, cursorWasVisible] {
        cursor->setVisible(cursorWasVisible);
    });

    auto buffer = new LayerCandidateBuffer(view->viewport().size().toSize());
    // containerItem's children must be WindowItems. Both synthetic surfaces
    // belong to the overlay tree; z orders the translucent one above scanout.
    LayerCandidateSurface background(scene->overlayItem(), buffer);
    Item foreground(scene->overlayItem());
    foreground.setZ(1);
    foreground.setOpacity(opacity);
    LayerCandidateSurface foregroundSurface(&foreground, buffer);
    buffer->drop(); // The two SurfaceItems now own its references.

    view->prePaint();
    const auto layers = scene->layerCandidates(1);
    view->postPaint();
    Item *expected = scene->containerItem();
    if (opacity == 0) {
        expected = &background;
    } else if (opacity == 1) {
        expected = &foregroundSurface;
    }
    QCOMPARE(layers, QList<Item *>{expected});
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
    // A value written to the QWindow does not become the view's opacity.
    QCOMPARE(view.opacity(), 0.5);
    QCOMPARE(item->opacity(), 0.5);
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
    // EffectFrame::render() only shows the view and forwards the opacity values;
    // its render target and viewport are signature scaffolding that nothing
    // reads (the assertions below go through grab()).
    std::optional<QImage> targetImage;
    std::optional<RenderTarget> target;
    std::optional<RenderViewport> viewport;
    emplaceEffectFrameTarget<ImageRenderTargetConstructor<QImage>>(targetImage, target);
    viewport.emplace(RectF(0, 0, 1280, 1024), 1, target.value(), QPoint());
    frame.render(target.value(), viewport.value(), Region::infinite(), 1, 1);
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::red));
    const QPoint edge(frame.geometry().left() + 2, sample.y());
    QTRY_COMPARE(grab().pixelColor(edge), QColor(Qt::black));

    frame.render(target.value(), viewport.value(), Region::infinite(), 0.5, 1);
    QVERIFY(frameRendered.wait());
    const QColor mixed = grab().pixelColor(sample);
    QVERIFY(mixed.red() > 0 && mixed.red() < 255);
    QVERIFY(std::abs(mixed.red() - mixed.blue()) <= 1);
    const QColor fadedFrame = grab().pixelColor(edge);
    QVERIFY(fadedFrame.blue() > 0 && fadedFrame.blue() < 255);

    frame.render(target.value(), viewport.value(), Region::infinite(), 0, 1);
    QVERIFY(frameRendered.wait());
    QCOMPARE(grab().pixelColor(sample), QColor(Qt::blue));
    QCOMPARE(grab().pixelColor(edge), QColor(Qt::blue));

    frame.render(target.value(), viewport.value(), Region::infinite(), 1, 0);
    QTRY_COMPARE(grab().pixelColor(sample), QColor(Qt::red));
    QTRY_COMPARE(grab().pixelColor(edge), QColor(Qt::blue));
    frame.render(target.value(), viewport.value(), Region::infinite(), 1, 1);
    QTRY_COMPARE(grab().pixelColor(edge), QColor(Qt::black));
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::OffscreenQuickViewTest)
#include "offscreenquickview_test.moc"
