/*
    SPDX-FileCopyrightText: 2024 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"

#include "core/colorpipeline.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/outputconfiguration.h"
#include "core/renderbackend.h"
#include "core/renderloop.h"
#include "outputconfigurationstore.h"
#include "pointer_input.h"
#include "scene/item.h"
#include "scene/workspacescene.h"
#include "tiles/tilemanager.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>
#include <deque>
#include <format>

using namespace std::chrono_literals;

namespace KWin
{

class FifoTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testFifo_data();
    void testFifo();
    void testFifoOnMixedRefreshOutputs_data();
    void testFifoOnMixedRefreshOutputs();
    void testFifoIgnoresCaptureViews();
    void testFifoInitiallyHidden();
    void testFifoBarriersOnly();
    void testFifoWaitOnly();
    void testFifoOnSubsurfaces();
    void testBarrierNotClearedByEmptyCommit();
    void testFifoOnUnmappedSurface();
    void testFifoOnSubsurfaceOfUnmappedToplevel_data();
    void testFifoOnSubsurfaceOfUnmappedToplevel();
    void testFifoOnSubsurfaceOfUnmappedSubsurface();
    void testFifoOnSubsurfaceOfUnmappedSubsurfaceInitiallyMapped();
    void testFifoWithExplicitReset_data();
    void testFifoWithExplicitReset();
    void testFifoCommitTiming();
    void testCommitTiming();
};

class FifoV1Surface : public QObject, public QtWayland::wp_fifo_v1
{
    Q_OBJECT

public:
    explicit FifoV1Surface(::wp_fifo_v1 *obj)
        : QtWayland::wp_fifo_v1(obj)
    {
    }

    ~FifoV1Surface() override
    {
        wp_fifo_v1_destroy(object());
    }
};

class CommitTimerV1 : public QtWayland::wp_commit_timer_v1
{
public:
    explicit CommitTimerV1(::wp_commit_timer_v1 *obj)
        : QtWayland::wp_commit_timer_v1(obj)
    {
    }

    ~CommitTimerV1() override
    {
        destroy();
    }

    void setTimestamp(std::chrono::nanoseconds time)
    {
        const uint64_t seconds = time / 1s;
        const uint64_t nanos = (time - std::chrono::seconds(seconds)).count();
        set_timestamp(seconds >> 32, seconds & 0xFFFFFFFF, nanos);
    }
};

// Mimics a screencast capture view: same logical output as a real one, an
// arbitrary (higher) refresh rate, but no backend output and a viewport of
// its own; the compositor's frame clock never drives it.
class CaptureOnlyView : public SceneView
{
public:
    CaptureOnlyView(Scene *scene, LogicalOutput *output)
        : SceneView(scene, output, nullptr, nullptr, nullptr)
    {
    }

    uint refreshRate() const override
    {
        return 240'000;
    }
};

void FifoTest::initTestCase()
{
    qRegisterMetaType<Window *>();

    qputenv("KWIN_WAYLAND_SUPPORT_COMMIT_TIMING_V1", "1");
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
}

void FifoTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::FifoV1
                                         | Test::AdditionalWaylandInterface::PresentationTime
                                         | Test::AdditionalWaylandInterface::CommitTiming));
    Test::setupWaylandConnection();

    workspace()->setActiveOutput(QPoint(640, 512));
    input()->pointer()->warp(QPoint(640, 512));
}

void FifoTest::cleanup()
{
    Test::destroyWaylandConnection();
}

void FifoTest::testFifo_data()
{
    QTest::addColumn<uint32_t>("refreshRate");

    QTest::addRow("50Hz") << 50'000u;
    QTest::addRow("24Hz") << 24'000u;
}

void FifoTest::testFifo()
{
    QFETCH(uint32_t, refreshRate);
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(0, 0, 200, 200),
            .modes = {
                OutputModeline(QSize(200, 200), refreshRate, OutputModeline::Flag::Preferred),
            },
        },
    });

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    // commit 3 frames in quick succession: without fifo, the first two should be discarded, and the last should be presented
    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }

    {
        QSignalSpy discarded1(frames[0].get(), &Test::WpPresentationFeedback::discarded);
        QSignalSpy discarded2(frames[1].get(), &Test::WpPresentationFeedback::discarded);
        QSignalSpy presented(frames[2].get(), &Test::WpPresentationFeedback::presented);

        QVERIFY(presented.wait(100));
        QVERIFY(discarded1.count());
        QVERIFY(discarded2.count());
    }

    // do it again; this time with fifo, all frames should be presented
    fifo->set_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        fifo->wait_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }

    {
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::presented),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::presented),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::presented),
        };
        for (size_t i = 0; i < frames.size(); i++) {
            QVERIFY(spies[i].wait(100));
            if (i > 0) {
                // each frame should be presented in the refresh cycle after the last one
                const auto thisTimestamp = spies[i].last().at(0).value<std::chrono::nanoseconds>();
                const auto lastTimestamp = spies[i - 1].last().at(0).value<std::chrono::nanoseconds>();
                const auto refreshDuration = spies[i].last().at(1).value<std::chrono::nanoseconds>();
                const auto diff = thisTimestamp - lastTimestamp;
                QCOMPARE_GT(diff, refreshDuration / 2);
                // sometimes, the compositor seems to drop a frame -> don't test for this
                // the only requirement for fifo is that the commit is shown for at least one frame
                // so this doesn't technically change the test result
                // QCOMPARE_LT(diff, refreshDuration * 3 / 2);
            }
        }
    }

    // even if the window is hidden, forward progress must be guaranteed
    window->setMinimized(true);

    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        fifo->wait_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    // another commit, so that the last one will be discarded as well
    fifo->set_barrier();
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    {
        const std::chrono::nanoseconds targetRefreshDuration = std::chrono::nanoseconds(1'000'000'000'000 / std::min(30'000u, refreshRate));
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::discarded),
        };
        for (size_t i = 0; i < frames.size(); i++) {
            const auto before = std::chrono::steady_clock::now();
            QVERIFY(spies[i].wait(100));
            const auto diff = std::chrono::steady_clock::now() - before;
            QCOMPARE_GT(diff, targetRefreshDuration * 2 / 3);
            QCOMPARE_LT(diff, targetRefreshDuration * 2);
        }
    }
}

void FifoTest::testFifoOnMixedRefreshOutputs_data()
{
    QTest::addColumn<int>("windowX");
    QTest::addColumn<int>("centerOutput");
    QTest::addRow("center-on-60Hz") << 125 << 0;
    QTest::addRow("center-on-144Hz") << 175 << 1;
}

void FifoTest::testFifoOnMixedRefreshOutputs()
{
    QFETCH(int, windowX);
    QFETCH(int, centerOutput);
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(0, 0, 200, 200),
            .modes = {OutputModeline(QSize(200, 200), 60'000, OutputModeline::Flag::Preferred)},
        },
        Test::OutputInfo{
            .geometry = Rect(200, 0, 200, 200),
            .modes = {OutputModeline(QSize(200, 200), 144'000, OutputModeline::Flag::Preferred)},
        },
    });
    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 2);
    QCOMPARE(outputs[0]->refreshRate(), 60'000u);
    QCOMPARE(outputs[1]->refreshRate(), 144'000u);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    window->move(QPoint(windowX, 75));
    QCOMPARE(workspace()->outputAt(window->frameGeometry().center()), outputs[centerOutput]);
    QVERIFY(window->frameGeometry().intersects(outputs[0]->geometry()));
    QVERIFY(window->frameGeometry().intersects(outputs[1]->geometry()));
    auto scene = kwinApp()->scene();
    const auto views = scene->sceneViews();
    QCOMPARE(views.size(), 2);
    scene->addRepaintFull();
    for (auto view : views) {
        QTRY_COMPARE(view->refreshRate(), view->logicalOutput()->refreshRate());
    }
    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*surface));
    auto firstFrame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
    QSignalSpy firstPresented(firstFrame.get(), &Test::WpPresentationFeedback::presented);
    QSignalSpy firstDiscarded(firstFrame.get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);

    // Keep real-time rendering and the fallback timer from applying either commit.
    // Both are due at the same synthetic presentation time, so only FIFO separates them.
    const auto target = std::chrono::steady_clock::now().time_since_epoch() + 1h;
    fifo->set_barrier();
    fifo->wait_barrier();
    timer->setTimestamp(target);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    auto secondFrame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
    QSignalSpy secondPresented(secondFrame.get(), &Test::WpPresentationFeedback::presented);
    QSignalSpy secondDiscarded(secondFrame.get(), &Test::WpPresentationFeedback::discarded);
    fifo->set_barrier();
    fifo->wait_barrier();
    timer->setTimestamp(target);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(Test::waylandSync());
    QCOMPARE(committed.count(), 0);
    QVERIFY(window->surface()->firstTransaction());
    // The refresh reported with the feedback is the one of the view that drove
    // the presentation: only the center output's traversal reaches the item.
    const auto refreshDuration = std::chrono::nanoseconds(1'000'000'000'000 / outputs[centerOutput]->refreshRate());
    RenderLoop renderLoop(nullptr);
    for (int i = 0; i < 2; ++i) {
        const auto timestamp = target + i * refreshDuration;
        // Use the same item-tree dispatch as WorkspaceScene::prePaint().
        // Calling the surface item directly would bypass the output filter.
        for (auto view : views) {
            scene->containerItem()->prepareFrame(view, view->logicalOutput(), timestamp);
        }
        QCOMPARE(committed.count(), i + 1);
        QVERIFY(window->surface()->hasFifoBarrier());
        for (auto view : views) {
            renderLoop.prepareNewFrame();
            OutputFrame frame(&renderLoop, std::chrono::nanoseconds(1'000'000'000'000 / view->refreshRate()));
            scene->frame(view, &frame);
            renderLoop.newFramePrepared();
            frame.presented(timestamp, PresentationMode::VSync);
        }
        QVERIFY(!window->surface()->hasFifoBarrier());
        QCOMPARE(committed.count(), i + 1);
    }
    QVERIFY(!window->surface()->firstTransaction());
    QVERIFY(Test::waylandSync());
    QCOMPARE(firstPresented.count(), 1);
    QCOMPARE(secondPresented.count(), 1);
    QCOMPARE(firstDiscarded.count(), 0);
    QCOMPARE(secondDiscarded.count(), 0);
    QCOMPARE(firstPresented[0][0].value<std::chrono::nanoseconds>(), target);
    QCOMPARE(secondPresented[0][0].value<std::chrono::nanoseconds>(), target + refreshDuration);
    QCOMPARE(firstPresented[0][1].value<std::chrono::nanoseconds>(), refreshDuration);
    QCOMPARE(secondPresented[0][1].value<std::chrono::nanoseconds>(), refreshDuration);
}

void FifoTest::testFifoIgnoresCaptureViews()
{
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(0, 0, 200, 200),
            .modes = {OutputModeline(QSize(200, 200), 60'000, OutputModeline::Flag::Preferred)},
        },
    });
    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 1);
    auto scene = kwinApp()->scene();
    SceneView *realView = nullptr;
    for (auto view : scene->sceneViews()) {
        if (view->backendOutput()) {
            realView = view;
        }
    }
    QVERIFY(realView);
    QCOMPARE(realView->logicalOutput(), outputs[0]);
    scene->addRepaintFull();
    QTRY_COMPARE(realView->refreshRate(), outputs[0]->refreshRate());

    // The capture region does not intersect the window under test.
    CaptureOnlyView captureView(scene, outputs[0]);
    captureView.setViewport(RectF(150, 150, 50, 50));

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    window->move(QPoint(25, 75));
    QCOMPARE(workspace()->outputAt(window->frameGeometry().center()), outputs[0]);
    QVERIFY(!captureView.viewport().intersects(window->frameGeometry()));

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*surface));
    auto frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
    QSignalSpy presented(frame.get(), &Test::WpPresentationFeedback::presented);
    QSignalSpy discarded(frame.get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy committed(window->surface(), &SurfaceInterface::committed);

    const auto target = std::chrono::steady_clock::now().time_since_epoch() + 1h;
    fifo->set_barrier();
    fifo->wait_barrier();
    timer->setTimestamp(target);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(Test::waylandSync());
    QCOMPARE(committed.count(), 0);
    QVERIFY(window->surface()->firstTransaction());

    // Only the compositor's own view drives frame dispatch; a capture view is
    // never presented by the compositor.
    scene->containerItem()->prepareFrame(realView, realView->logicalOutput(), target);
    QCOMPARE(committed.count(), 1);
    QVERIFY(window->surface()->hasFifoBarrier());
    RenderLoop renderLoop(nullptr);
    renderLoop.prepareNewFrame();
    OutputFrame outputFrame(&renderLoop, std::chrono::nanoseconds(1'000'000'000'000 / realView->refreshRate()));
    scene->frame(realView, &outputFrame);
    renderLoop.newFramePrepared();
    outputFrame.presented(target, PresentationMode::VSync);
    QVERIFY(!window->surface()->hasFifoBarrier());
    QVERIFY(Test::waylandSync());
    QCOMPARE(presented.count(), 1);
    QCOMPARE(discarded.count(), 0);
    QCOMPARE(presented[0][0].value<std::chrono::nanoseconds>(), target);
    QCOMPARE(presented[0][1].value<std::chrono::nanoseconds>(),
             std::chrono::nanoseconds(1'000'000'000'000 / outputs[0]->refreshRate()));
}

void FifoTest::testFifoInitiallyHidden()
{
    // this test verifies that even when the window has never been presented once,
    // we still ensure forward progress

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    window->setMinimized(true);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        fifo->wait_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    // another commit, so that the last one will be discarded as well
    fifo->set_barrier();
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    {
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::discarded),
        };
        for (size_t i = 0; i < frames.size(); i++) {
            const auto before = std::chrono::steady_clock::now();
            QVERIFY(spies[i].wait(100));
            const auto diff = std::chrono::steady_clock::now() - before;
            QCOMPARE_GT(diff, std::chrono::milliseconds(30));
            QCOMPARE_LT(diff, std::chrono::milliseconds(100));
        }
    }
}

void FifoTest::testFifoBarriersOnly()
{
    // this test verifies that if you only set barriers but don't wait on them,
    // commits still get applied immediately

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    // another commit, so that the last one will be discarded as well
    fifo->set_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    {
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::discarded),
        };
        for (auto &spy : spies) {
            QVERIFY(spy.count() || spy.wait(100));
        }
    }
}

void FifoTest::testFifoWaitOnly()
{
    // this test verifies that if you only wait on barriers but don't set any,
    // commits still get applied immediately

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->wait_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    // another commit, so that the last one will be discarded as well
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    {
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::discarded),
        };
        for (auto &spy : spies) {
            QVERIFY(spy.count() || spy.wait(100));
        }
    }
}

void FifoTest::testFifoOnSubsurfaces()
{
    // this test verifies that fifo works correctly with subsurfaces
    Test::setOutputConfig({
        Test::OutputInfo{
            .geometry = Rect(0, 0, 200, 200),
            .modes = {
                OutputModeline(QSize(200, 200), 60'000, OutputModeline::Flag::Preferred),
            },
        },
    });

    std::unique_ptr<KWayland::Client::Surface> parentSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(parentSurface.get()));
    auto window = Test::renderAndWaitForShown(parentSurface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto subsurface = Test::createSubSurface(surface.get(), parentSurface.get());
    Test::render(surface.get(), QSize(100, 50), Qt::red);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    // when the surface is desync, fifo should work like on a toplevel surface
    subsurface->setMode(KWayland::Client::SubSurface::Mode::Desynchronized);
    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        fifo->wait_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    // another commit, so that the last one will be discarded as well
    fifo->set_barrier();
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    {
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::presented),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::presented),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::presented),
        };
        for (auto &spy : spies) {
            QVERIFY(spy.wait(100));
        }
    }

    // when the surface is synchronized though, fifo should be ignored
    subsurface->setMode(KWayland::Client::SubSurface::Mode::Synchronized);
    for (auto &frame : frames) {
        frame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        fifo->wait_barrier();
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    // another commit, so that the last one will be discarded as well
    fifo->set_barrier();
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    {
        std::array<QSignalSpy, frames.size()> spies = {
            QSignalSpy(frames[0].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[1].get(), &Test::WpPresentationFeedback::discarded),
            QSignalSpy(frames[2].get(), &Test::WpPresentationFeedback::discarded),
        };
        for (auto &spy : spies) {
            QVERIFY(spy.count() || spy.wait(100));
        }
    }
}

void FifoTest::testBarrierNotClearedByEmptyCommit()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 2> frames;

    fifo->set_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // this additional commit must not clear the barrier
    auto firstFrame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    auto secondFrame = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    QSignalSpy firstSpy(firstFrame.get(), &Test::WpPresentationFeedback::presented);
    QVERIFY(firstSpy.wait(100));

    QSignalSpy secondSpy(secondFrame.get(), &Test::WpPresentationFeedback::presented);
    QVERIFY(secondSpy.wait(100));

    // the second frame should be presented in the refresh cycle after the first one
    const auto thisTimestamp = secondSpy.last().at(0).value<std::chrono::nanoseconds>();
    const auto lastTimestamp = firstSpy.last().at(0).value<std::chrono::nanoseconds>();
    const auto refreshDuration = secondSpy.last().at(1).value<std::chrono::nanoseconds>();
    const auto diff = thisTimestamp - lastTimestamp;
    QCOMPARE_GT(diff, refreshDuration / 2);
}

void FifoTest::testFifoOnUnmappedSurface()
{
    // This test verifies that transactions on a toplevel that is never mapped
    // don't get stuck because of fifo barriers

    Test::XdgToplevelWindow window;

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*window.m_surface));
    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*window.m_surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;
    for (size_t i = 0; i < frames.size(); i++) {
        fifo->set_barrier();
        fifo->wait_barrier();
        timer->setTimestamp(std::chrono::steady_clock::now().time_since_epoch() + 50ms);
        frames[i] = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*window.m_surface));
        window.m_surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    window.m_surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // all frames should be discarded
    QSignalSpy spy0(frames[0].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy1(frames[1].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy2(frames[2].get(), &Test::WpPresentationFeedback::discarded);
    QVERIFY(spy0.wait());
    QVERIFY(spy1.count() || spy1.wait());
    QVERIFY(spy2.count() || spy2.wait());
}

void FifoTest::testFifoOnSubsurfaceOfUnmappedToplevel_data()
{
    QTest::addColumn<bool>("mapWindow");
    QTest::addColumn<bool>("destroyShell");

    QTest::addRow("never mapped") << false << false;
    QTest::addRow("initially mapped") << true << false;
    QTest::addRow("initially mapped, destroy shell") << true << true;
}

void FifoTest::testFifoOnSubsurfaceOfUnmappedToplevel()
{
    // This test verifies that transactions on subsurfaces of a toplevel
    // that isn't mapped don't get stuck because of fifo barriers
    QFETCH(bool, mapWindow);
    QFETCH(bool, destroyShell);

    Test::XdgToplevelWindow window;
    if (mapWindow) {
        QVERIFY(window.show());
    }

    auto surface = Test::createSurface();
    auto subsurface = Test::createSubSurface(surface.get(), window.m_surface.get());
    subsurface->setMode(KWayland::Client::SubSurface::Mode::Desynchronized);
    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    Test::render(surface.get(), QSize(100, 100), Qt::blue);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*surface));
    for (size_t i = 0; i < frames.size(); i++) {
        fifo->set_barrier();
        fifo->wait_barrier();
        timer->setTimestamp(std::chrono::steady_clock::now().time_since_epoch() + 50ms);
        frames[i] = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    if (destroyShell) {
        window.m_toplevel.reset();
        window.m_window = nullptr;
    } else if (mapWindow) {
        window.unmap();
    }

    QSignalSpy spy0(frames[0].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy1(frames[1].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy2(frames[2].get(), &Test::WpPresentationFeedback::discarded);
    QVERIFY(spy0.wait());
    QVERIFY(spy1.count() || spy1.wait());
    QVERIFY(spy2.count() || spy2.wait());
}

void FifoTest::testFifoOnSubsurfaceOfUnmappedSubsurface()
{
    // This test verifies that transactions on subsurfaces of a subsurface
    // that is never mapped don't get stuck because of fifo barriers

    Test::XdgToplevelWindow window;
    QVERIFY(window.show());

    auto parentSurface = Test::createSurface();
    auto parentSubsurface = Test::createSubSurface(parentSurface.get(), window.m_surface.get());
    parentSubsurface->setMode(KWayland::Client::SubSurface::Mode::Desynchronized);

    auto surface = Test::createSurface();
    auto subsurface = Test::createSubSurface(surface.get(), parentSurface.get());
    subsurface->setMode(KWayland::Client::SubSurface::Mode::Desynchronized);
    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    Test::render(surface.get(), QSize(100, 100), Qt::blue);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    for (size_t i = 0; i < frames.size(); i++) {
        fifo->set_barrier();
        fifo->wait_barrier();
        frames[i] = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    QSignalSpy spy0(frames[0].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy1(frames[1].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy2(frames[2].get(), &Test::WpPresentationFeedback::discarded);
    QVERIFY(spy0.wait());
    QVERIFY(spy1.count() || spy1.wait());
    QVERIFY(spy2.count() || spy2.wait());
}

void FifoTest::testFifoOnSubsurfaceOfUnmappedSubsurfaceInitiallyMapped()
{
    // This test verifies that transactions on subsurfaces of a subsurface
    // that gets unmapped after being mapped don't get stuck because of fifo barriers

    Test::XdgToplevelWindow window;
    QVERIFY(window.show());

    auto parentSurface = Test::createSurface();
    auto parentSubsurface = Test::createSubSurface(parentSurface.get(), window.m_surface.get());
    parentSubsurface->setMode(KWayland::Client::SubSurface::Mode::Desynchronized);
    Test::render(parentSurface.get(), QSize(100, 100), Qt::red);

    auto surface = Test::createSurface();
    auto subsurface = Test::createSubSurface(surface.get(), parentSurface.get());
    subsurface->setMode(KWayland::Client::SubSurface::Mode::Desynchronized);
    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 3> frames;

    Test::render(surface.get(), QSize(100, 100), Qt::blue);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    // queue some commits
    for (size_t i = 0; i < frames.size(); i++) {
        fifo->set_barrier();
        fifo->wait_barrier();
        frames[i] = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    fifo->set_barrier();
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // and unmap the parent surface
    parentSurface->attachBuffer((wl_buffer *)nullptr);
    parentSurface->commit(KWayland::Client::Surface::CommitFlag::None);

    QSignalSpy spy0(frames[0].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy1(frames[1].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy spy2(frames[2].get(), &Test::WpPresentationFeedback::discarded);
    QVERIFY(spy0.wait());
    QVERIFY(spy1.count() || spy1.wait());
    QVERIFY(spy2.count() || spy2.wait());
}

void FifoTest::testFifoWithExplicitReset_data()
{
    QTest::addColumn<bool>("destroyShellFirst");

    QTest::addRow("destroy shell first") << true;
    QTest::addRow("unmap first") << false;
}

void FifoTest::testFifoWithExplicitReset()
{
    // This test verifies that pending transactions with fifo barriers
    // will not be stuck when the shell surface is destroyed
    QFETCH(bool, destroyShellFirst);

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*surface));
    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*surface));

    std::array<std::unique_ptr<Test::WpPresentationFeedback>, 2> frames;
    for (int i = 0; i < 2; i++) {
        frames[i] = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*surface));
        fifo->set_barrier();
        fifo->wait_barrier();
        timer->setTimestamp(std::chrono::steady_clock::now().time_since_epoch() + 50ms);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
    }
    fifo->set_barrier();
    fifo->wait_barrier();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    if (destroyShellFirst) {
        shellSurface.reset();
    }
    surface->attachBuffer(static_cast<wl_buffer *>(nullptr), QPoint());
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    shellSurface.reset();

    // the surface transactions must not be blocked anymore
    QSignalSpy first(frames[0].get(), &Test::WpPresentationFeedback::discarded);
    QSignalSpy second(frames[1].get(), &Test::WpPresentationFeedback::discarded);
    QVERIFY(first.wait());
    QVERIFY(second.count() || second.wait());

    shellSurface = Test::createXdgToplevelSurface(surface.get());
    auto window2 = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window2 && window2->isShown());
}

void FifoTest::testFifoCommitTiming()
{
    // this tests that using fifo + commit timing in combination
    // (like Mesa does) works as expected

    Test::XdgToplevelWindow window;
    QVERIFY(window.show());

    auto fifo = std::make_unique<FifoV1Surface>(Test::fifoManager()->get_fifo(*window.m_surface));
    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*window.m_surface));

    std::deque<std::unique_ptr<Test::WpPresentationFeedback>> frames;

    auto lastTarget = std::chrono::steady_clock::now().time_since_epoch();
    auto refreshDuration = std::chrono::nanoseconds(1'000'000) / workspace()->outputs().front()->refreshRate();
    std::optional<std::chrono::nanoseconds> lastPresentation;

    for (size_t i = 0; i < 10; i++) {
        if (frames.size() >= 3) {
            QSignalSpy presented(frames[0].get(), &Test::WpPresentationFeedback::presented);
            QVERIFY(presented.wait());

            refreshDuration = presented.last()[1].value<std::chrono::nanoseconds>();
            const auto timestamp = presented.last()[0].value<std::chrono::nanoseconds>();
            if (lastPresentation) {
                const auto duration = timestamp - *lastPresentation;
                QCOMPARE_GE(duration, refreshDuration / 2);
                QCOMPARE_LE(duration, refreshDuration * 2);
            }
            lastPresentation = timestamp;

            frames.pop_front();
        }

        frames.push_back(std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*window.m_surface)));
        fifo->set_barrier();
        fifo->wait_barrier();
        timer->setTimestamp(lastTarget + refreshDuration - 500us);
        lastTarget += refreshDuration;
        window.commit();

        fifo->wait_barrier();
        window.commit();
    }
}

void FifoTest::testCommitTiming()
{
    // this test verifies that commit timing delays frames as needed

    Test::XdgToplevelWindow window;
    QVERIFY(window.show());

    auto timer = std::make_unique<CommitTimerV1>(Test::connection()->commitTiming->get_timer(*window.m_surface));

    const auto start = std::chrono::steady_clock::now().time_since_epoch();
    std::array targetTimes{
        start + 50ms,
        start + 150ms,
        start + 170ms,
    };
    std::array<std::unique_ptr<Test::WpPresentationFeedback>, targetTimes.size()> frames;

    // present the frames back to back, with commit timing as the only constraint
    for (size_t i = 0; i < frames.size(); i++) {
        frames[i] = std::make_unique<Test::WpPresentationFeedback>(Test::presentationTime()->feedback(*window.m_surface));
        timer->setTimestamp(targetTimes[i]);
        window.commit();
    }

    std::array spies{
        QSignalSpy{frames[0].get(), &Test::WpPresentationFeedback::presented},
        QSignalSpy{frames[1].get(), &Test::WpPresentationFeedback::presented},
        QSignalSpy{frames[2].get(), &Test::WpPresentationFeedback::presented},
    };
    for (size_t i = 0; i < frames.size(); i++) {
        QVERIFY(spies[i].count() || spies[i].wait());
        QCOMPARE_GE(spies[i].last().first().value<std::chrono::nanoseconds>(), targetTimes[i]);
    }
}

}

WAYLANDTEST_MAIN(KWin::FifoTest)
#include "test_fifo.moc"
