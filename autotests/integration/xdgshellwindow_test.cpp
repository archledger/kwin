/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2019 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "core/backendoutput.h"
#include "core/colorspace.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/outputconfiguration.h"
#include "decorations/decorationbridge.h"
#include "decorations/settings.h"
#include "effect/effecthandler.h"
#include "pointer_input.h"
#include "tiles/tilemanager.h"
#include "tiles/transientplacementwriter.h"
#include "tiles/transienttilecontroller.h"
#include "virtualdesktops.h"
#include "wayland/clientconnection.h"
#include "wayland/display.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KDecoration3/DecoratedWindow>
#include <KDecoration3/Decoration>
#include <KDecoration3/DecorationSettings>

#include <KWayland/Client/appmenu.h>
#include <KWayland/Client/compositor.h>
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/output.h>
#include <KWayland/Client/pointer.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>

#include <QChildEvent>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <KConfigGroup>

#include <KPluginFactory>
#include <KPluginMetaData>

// system
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/input.h>

#include <array>
#include <csignal>
#include <cstdio>
#include <functional>
#include <limits>
#include <type_traits>

using namespace KWin;

struct WorkspaceShutdownObservation
{
    void require(bool condition, const QString &message)
    {
        if (!condition) {
            failures.append(message);
        }
    }

    bool armed = false;
    bool windowClosed = false;
    bool workspaceDestroyed = false;
    int outputsRemoved = 0;
    QStringList failures;
    QPointer<TransientTileController> controller;
    QList<QPointer<TileManager>> managers;
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<TransientPlacementWriter> writer;
    std::unique_ptr<TransientTileController> challenger;
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    // Last declared, so captured state survives all observer disconnections.
    std::unique_ptr<QObject> context;
};

static WorkspaceShutdownObservation *shutdownObservation = nullptr;

class RegistryEventFilter : public QObject
{
public:
    RegistryEventFilter(QEvent::Type type, std::function<void(QObject *, QEvent *)> callback, bool consume = false)
        : m_type(type)
        , m_callback(std::move(callback))
        , m_consume(consume)
    {
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != m_type) {
            return false;
        }
        m_callback(object, event);
        return m_consume;
    }

private:
    QEvent::Type m_type;
    std::function<void(QObject *, QEvent *)> m_callback;
    bool m_consume;
};

class RequestTestTile : public Tile
{
public:
    explicit RequestTestTile(Window *window, TileManager *manager = nullptr)
        : Tile(manager ? manager : workspace()->tileManager(window->moveResizeOutput()), nullptr, Type::Transient)
        , m_window(window)
    {
        m_desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
        m_relativeGeometry = RectF(0.25, 0.25, 0.5, 0.5);
        m_padding = 0;
        m_windows.append(window);
    }

    ~RequestTestTile() override
    {
        m_windows.clear();
        if (m_window && !m_window->isDeleted()) {
            m_window->forgetTile(this);
        }
    }

    void setTargetForTest(const RectF &absolute)
    {
        const auto output = manager()->output()->geometryF();
        m_relativeGeometry = RectF((absolute.x() - output.x()) / output.width(), (absolute.y() - output.y()) / output.height(),
                                   absolute.width() / output.width(), absolute.height() / output.height());
    }

    void abandonWithoutForgetForTest()
    {
        // Model native node destruction before the later structural-release leaf is wired.
        m_windows.clear();
        m_window.clear();
    }

private:
    QPointer<Window> m_window;
};

class TestXdgShellWindow : public QObject
{
    Q_OBJECT

    struct RequestAuthority
    {
        QPointer<QObject> owner;
        bool valid = true;
    };

    class RequestForTest : public Window::TransientTileRequest
    {
    public:
        RequestForTest(Window *window, Tile *tile, std::shared_ptr<RequestAuthority> authority, quint64 generation = 7)
            : TransientTileRequest(window, tile, generation)
            , m_authority(std::move(authority))
        {
        }

    private:
        bool authorityIsCurrent() const override
        {
            return m_authority->owner && m_authority->valid;
        }
        std::shared_ptr<RequestAuthority> m_authority;
    };

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testDecorationSettingsReentry_data();
    void testDecorationSettingsReentry();
    void testDecorationSettingsDestruction();
    void testDecorationRecreationTraversal_data();
    void testDecorationRecreationTraversal();
    void testDecorationBridgeReentry_data();
    void testDecorationBridgeReentry();
    void testDecorationFactoryOwnership_data();
    void testDecorationFactoryOwnership();

    void testMapUnmap();
    void testWindowOutputs();
    void testMinimizeActiveWindow();
    void testFullscreen_data();
    void testFullscreen();
    void testUserCanSetFullscreen();
    void testSendFullScreenWindowToAnotherOutput();
    void testFullscreenPerOutputVirtualDesktops();

    void testMaximizeHorizontal();
    void testMaximizeVertical();
    void testMaximizeFull();
    void testMaximizedToFullscreen_data();
    void testMaximizedToFullscreen();
    void testSendMaximizedWindowToAnotherOutput();
    void testInteractiveMoveUnmaximizeFull();
    void testInteractiveMoveUnmaximizeInitiallyFull();
    void testInteractiveMoveUnmaximizeHorizontal();
    void testInteractiveMoveUnmaximizeVertical();
    void testRestoreWithNewMinSize();
    void testFullscreenMultipleOutputs();
    void testHidden();
    void testDesktopFileName();
    void testCaptionSimplified();
    void testUnresponsiveWindow_data();
    void testUnresponsiveWindow();
    void testAppMenu();
    void testSendClientWithTransientToDesktop();
    void testMinimizeWindowWithTransients();
    void testXdgDecoration_data();
    void testXdgDecoration();
    void testXdgNeverCommitted();
    void testXdgInitialState();
    void testXdgInitiallyMaximised();
    void testXdgInitiallyFullscreen();
    void testXdgInitiallyMinimized();
    void testXdgWindowGeometryIsntSet();
    void testXdgWindowGeometryAttachBuffer();
    void testXdgWindowGeometryAttachSubSurface();
    void testXdgWindowGeometryInteractiveResize();
    void testXdgWindowGeometryFullScreen();
    void testXdgWindowGeometryMaximize();
    void testXdgPopupReactive_data();
    void testXdgPopupReactive();
    void testXdgPopupReposition();
    void testXdgPopupRepositionBeforeInitialCommit();
    void testPointerInputTransform();
    void testReentrantSetFrameGeometry();
    void testDoubleMaximize();
    void testDoubleFullscreenSeparatedByCommit();
    void testMaximizeAndChangeDecorationModeAfterInitialCommit();
    void testFullScreenAndChangeDecorationModeAfterInitialCommit();
    void testChangeDecorationModeAfterInitialCommit();
    void testModal();
    void testNestedModals();
    void testCloseModal();
    void testCloseModalPreSetup();
    void testCloseInactiveModal();
    void testClosePopupOnParentUnmapped();
    void testPopupWithDismissedParent();
    void testPopupDismissedOnFocusChange();
    void testMinimumSize();
    void testNoMinimumSize();
    void testMaximumSize();
    void testNoMaximumSize();
    void testUnconfiguredBufferToplevel();
    void testUnconfiguredBufferPopup();
    void testRemoveActiveDesktopBeforeInitialCommit();
    void testRemoveActiveDesktopBeforeMap();
    void testRemoveActiveOutputBeforeInitialCommit();
    void testRemoveActiveOutputBeforeMap();
    void testRequestedGeometrySettlesOnCommit();
    void testRequestedGeometryAfterMappingDestroysWindow();
    void testRequestedGeometryReentrantMove();
    void testGeometrySupersession_data();
    void testGeometrySupersession();
    void testGeometryNotificationHandoff_data();
    void testGeometryNotificationHandoff();
    void testGeometryCallbackClose_data();
    void testGeometryCallbackClose();
    void testGeometrySameRectangleIntent();
    void testMoveDuringQueuedResize();
    void testNestedOutputPropertySync();
    void testGeometryNotificationNetZero();
    void testOutputScaleCallbackClose();
    void testPrivateTransientRequest();
    void testTransientRequestCancellation_data();
    void testTransientRequestCancellation();
    void testTransientRequestUnmapped();
    void testTransientRequestPendingCallback();
    void testTransientRequestCommitReplacement();
    void testTransientRequestNoOpCancellation_data();
    void testTransientRequestNoOpCancellation();
    void testTransientRequestPrivateReentry();
    void testTransientRequestNewTileWins();
    void testTransientRequestPendingStates_data();
    void testTransientRequestPendingStates();
    void testTransientRequestConstraints_data();
    void testTransientRequestConstraints();
    void testTransientRequestFractional_data();
    void testTransientRequestFractional();
    void testTransientRequestDecorated();
    void testTransientRequestRequestedRevocation_data();
    void testTransientRequestRequestedRevocation();
    void testTransientNoRequestProofReplay();
    void testNoRequestFinalization_data();
    void testNoRequestFinalization();
    void testNoRequestFinalizationAfterRequested_data();
    void testNoRequestFinalizationAfterRequested();
    void testNoRequestFinalizationDuringClose_data();
    void testNoRequestFinalizationDuringClose();
    void testNoRequestFinalizationValidity_data();
    void testNoRequestFinalizationValidity();
    void testTransientOwnershipPreparation_data();
    void testTransientOwnershipPreparation();
    void testTransientOwnershipBeforeRequest_data();
    void testTransientOwnershipBeforeRequest();
    void testTransientOwnershipRevocation_data();
    void testTransientOwnershipRevocation();
    void testTransientOwnershipNativeProgress_data();
    void testTransientOwnershipNativeProgress();
    void testTransientOwnershipClose_data();
    void testTransientOwnershipClose();
    void testTransientOwnershipWriterLoss();
    void testTransientOwnershipExhaustion();
    void testTransientOwnershipApplied();
    void testTransientConfigureProvenance_data();
    void testTransientConfigureProvenance();
    void testTransientAssociationOrigin();
    void testControllerWindowAdmission();
    void testControllerAdmissionCallback_data();
    void testControllerAdmissionCallback();
    void testControllerDetachNotification_data();
    void testControllerDetachNotification();
    void testControllerProviderChange_data();
    void testControllerProviderChange();
    void testControllerPlacementKey_data();
    void testControllerPlacementKey();
    void testControllerOwnedDestruction_data();
    void testControllerOwnedDestruction();
    void testControllerReleasePriority_data();
    void testControllerReleasePriority();
    void testAssociationCounterSaturation();
    void testControllerForeignMembership();
    void testControllerClearAfterWriterLoss();
    void testPublicTransientRelease_data();
    void testPublicTransientRelease();
    void testPublicTransientPreRequestRelease_data();
    void testPublicTransientPreRequestRelease();
    void testPublicTransientReleaseDuringClose_data();
    void testPublicTransientReleaseDuringClose();
    void testPublicTransientReleaseDestruction_data();
    void testPublicTransientReleaseDestruction();
    void testPublicTransientReleaseRefusal();
    void testTransientRegistryLookup();
    void testTransientRegistryRetiredRoot();
    void testTransientRegistryDestructorReentry_data();
    void testTransientRegistryDestructorReentry();
    void testTransientRegistryDrainSnapshots_data();
    void testTransientRegistryDrainSnapshots();
    void testTransientRegistryTransactions_data();
    void testTransientRegistryTransactions();
    void testTransientRegistryClosedRegistration();
    void testTransientRegistryDesktopRemoval();
    void testTransientRegistryForgetReentry_data();
    void testTransientRegistryForgetReentry();
    void testWorkspaceIdleController();
    void testWorkspaceMissingTileManager();
    void testMissingManagerTilingRequests();
    void testMissingManagerOutputTransfer_data();
    void testMissingManagerOutputTransfer();
    void testQuickTileCallbackWins_data();
    void testQuickTileCallbackWins();
    void testWorkspaceShutdown_data();
    void testWorkspaceShutdown();
    void testOutputUpdatePending_data();
    void testOutputUpdatePending();
    void testOutputUpdateFrozenConfiguration();
    void testOutputUpdateFrozenOrder();
    void testOutputUpdateBackendReplacement();
    void testOutputUpdateRetainsClosedWindow();
    void testOutputUpdateCleanupReentry();
    void testOutputUpdatePlaceholderRoundTrip();
    void testOutputUpdateCallerLifetime();
    void testOutputUpdateCallerLifetime_data();
    void testOutputRemovalDrain_data();
    void testOutputRemovalDrain();
    void testWorkspaceWindowContinuation_data();
    void testWorkspaceWindowContinuation();
    void testOutputRemovalBatch_data();
    void testOutputRemovalBatch();
    void testOutputRemovalPreservesNewerTile();
    void testOutputRemovalMultiDesktop_data();
    void testOutputRemovalMultiDesktop();
};

void TestXdgShellWindow::testXdgPopupReactive_data()
{
    QTest::addColumn<bool>("reactive");
    QTest::addColumn<QPointF>("parentPos");
    QTest::addColumn<QPointF>("popupPos");

    QTest::addRow("reactive") << true << QPointF(0, 1024) << QPointF(50, 1024 - 10);
    QTest::addRow("not reactive") << false << QPointF(0, 1024) << QPointF(50, 1024 + 40);
}

void TestXdgShellWindow::testXdgPopupReactive()
{
    // This test verifies the behavior of reactive popups. If a popup is not reactive,
    // it only has to move together with its parent. If a popup is reactive, it moves
    // with its parent and it's reconstrained as needed.

    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);
    positioner->set_anchor_rect(0, 0, 50, 40);
    positioner->set_anchor(Test::XdgPositioner::anchor_bottom_right);
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    positioner->set_constraint_adjustment(Test::XdgPositioner::constraint_adjustment_slide_y);

    QFETCH(bool, reactive);
    if (reactive) {
        positioner->set_reactive();
    }

    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));
    auto rootWindow = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);

    std::unique_ptr<KWayland::Client::Surface> childSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(childSurface.get(), root->xdgSurface(), positioner.get()));
    auto childWindow = Test::renderAndWaitForShown(childSurface.get(), QSize(10, 10), Qt::cyan);
    QVERIFY(childWindow);

    QFETCH(QPointF, parentPos);
    QFETCH(QPointF, popupPos);

    QSignalSpy popupConfigureRequested(popup.get(), &Test::XdgPopup::configureRequested);
    rootWindow->move(parentPos);
    QVERIFY(popupConfigureRequested.wait());
    QCOMPARE(popupConfigureRequested.count(), 1);

    QCOMPARE(childWindow->pos(), popupPos);
}

void TestXdgShellWindow::testXdgPopupReposition()
{
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<Test::XdgPositioner> otherPositioner(Test::createXdgPositioner());
    otherPositioner->set_size(50, 50);
    otherPositioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));
    auto rootWindow = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);

    std::unique_ptr<KWayland::Client::Surface> childSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(childSurface.get(), root->xdgSurface(), positioner.get()));
    auto childWindow = Test::renderAndWaitForShown(childSurface.get(), QSize(10, 10), Qt::cyan);
    QVERIFY(childWindow);

    QSignalSpy reconfigureSpy(popup.get(), &Test::XdgPopup::configureRequested);
    QSignalSpy repositionedSpy(popup.get(), &Test::XdgPopup::repositioned);

    popup->reposition(otherPositioner->object(), 500000);

    QVERIFY(reconfigureSpy.wait());
    QCOMPARE(reconfigureSpy.count(), 1);
    QCOMPARE(repositionedSpy.count(), 1);
    QCOMPARE(repositionedSpy.last().at(0).toUInt(), 500000);
}

void TestXdgShellWindow::testXdgPopupRepositionBeforeInitialCommit()
{
    // This test verifies that reposition requests before the initial commit are handled in a reasonable
    // way. How this case should be handled is left out of the xdg-shell spec. Also, with explicit sync,
    // due to transaction fences, the reposition request may be reordered after the initial commit.

    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<Test::XdgPositioner> otherPositioner(Test::createXdgPositioner());
    otherPositioner->set_size(50, 50);
    otherPositioner->set_anchor_rect(10, 10, 10, 10);

    // Create the parent surface.
    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));
    auto rootWindow = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);

    // Create a popup surface.
    std::unique_ptr<KWayland::Client::Surface> childSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(childSurface.get(), root->xdgSurface(), positioner.get(), Test::CreationSetup::CreateOnly));
    popup->reposition(otherPositioner->object(), 666);

    QSignalSpy popupConfigureRequestedSpy(popup.get(), &Test::XdgPopup::configureRequested);
    QSignalSpy popupRepositionedSpy(popup.get(), &Test::XdgPopup::repositioned);
    QSignalSpy surfaceConfigureRequestedSpy(popup->xdgSurface(), &Test::XdgSurface::configureRequested);
    childSurface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(popupRepositionedSpy.count(), 1);
    QCOMPARE(popupRepositionedSpy.last().at(0).toUInt(), 666);

    popup->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).toUInt());
    auto childWindow = Test::renderAndWaitForShown(childSurface.get(), QSize(10, 10), Qt::cyan);
    QVERIFY(childWindow);
}

void TestXdgShellWindow::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    qRegisterMetaType<KWayland::Client::Output *>();

    QVERIFY(waylandServer()->init(qAppName()));

    kwinApp()->start();
}

void TestXdgShellWindow::init()
{
    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
        Rect(1280, 0, 1280, 1024),
    });
    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.count(), 2);
    QCOMPARE(outputs[0]->geometry(), Rect(0, 0, 1280, 1024));
    QCOMPARE(outputs[1]->geometry(), Rect(1280, 0, 1280, 1024));

    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Seat | Test::AdditionalWaylandInterface::XdgDecorationV1 | Test::AdditionalWaylandInterface::AppMenu | Test::AdditionalWaylandInterface::XdgDialogV1));
    QVERIFY(Test::waitForWaylandPointer());

    workspace()->setActiveOutput(QPoint(640, 512));
    // put mouse in the middle of screen one
    KWin::input()->pointer()->warp(QPoint(640, 512));
    VirtualDesktopManager::self()->setCount(1);
    VirtualDesktopManager::self()->setPerOutputVirtualDesktops(false);
}

void TestXdgShellWindow::cleanup()
{
    if (!shutdownObservation || !shutdownObservation->armed) {
        Test::destroyWaylandConnection();
    }
}

void TestXdgShellWindow::testDecorationSettingsReentry_data()
{
    QTest::addColumn<int>("boundary");
    QTest::newRow("left-buttons") << 0;
    QTest::newRow("right-buttons") << 1;
    QTest::newRow("close-on-double-click") << 2;
    QTest::newRow("exclude-from-capture") << 3;
    QTest::newRow("border-size") << 4;
}

void TestXdgShellWindow::testDecorationSettingsReentry()
{
    QFETCH(int, boundary);
    const auto settings = workspace()->decorationBridge()->settings();
    QVERIFY(settings);
    KConfigGroup config = kwinApp()->config()->group(QStringLiteral("org.kde.kdecoration2"));
    const QStringList keys = {QStringLiteral("ButtonsOnLeft"), QStringLiteral("ButtonsOnRight"),
                              QStringLiteral("CloseOnDoubleClickOnMenu"), QStringLiteral("AlwaysShowExcludeFromCapture"),
                              QStringLiteral("BorderSizeAuto"), QStringLiteral("BorderSize")};
    const auto saved = config.entryMap();
    QObject context;
    const auto restore = qScopeGuard([&]() {
        disconnect(settings.get(), nullptr, &context, nullptr);
        for (const auto &key : keys) {
            if (saved.contains(key)) {
                config.writeEntry(key, saved.value(key));
            } else {
                config.deleteEntry(key);
            }
        }
        Q_EMIT workspace()->configChanged();
    });
    const auto writeSettings = [&](bool outer) {
        config.writeEntry("ButtonsOnLeft", outer ? "N" : "M");
        config.writeEntry("ButtonsOnRight", outer ? "NX" : "X");
        config.writeEntry("CloseOnDoubleClickOnMenu", outer);
        config.writeEntry("AlwaysShowExcludeFromCapture", outer);
        config.writeEntry("BorderSizeAuto", false);
        config.writeEntry("BorderSize", outer ? "Large" : "Normal");
    };
    writeSettings(false);
    Q_EMIT workspace()->configChanged();
    QSignalSpy reconfigured(settings.get(), &KDecoration3::DecorationSettings::reconfigured);
    bool interrupted = false;
    const auto interrupt = [&]() {
        if (interrupted) {
            return;
        }
        interrupted = true;
        writeSettings(false);
        Q_EMIT workspace()->configChanged();
    };
    switch (boundary) {
    case 0:
        connect(settings.get(), &KDecoration3::DecorationSettings::decorationButtonsLeftChanged, &context, interrupt);
        break;
    case 1:
        connect(settings.get(), &KDecoration3::DecorationSettings::decorationButtonsRightChanged, &context, interrupt);
        break;
    case 2:
        connect(settings.get(), &KDecoration3::DecorationSettings::closeOnDoubleClickOnMenuChanged, &context, interrupt);
        break;
    case 3:
        connect(settings.get(), &KDecoration3::DecorationSettings::alwaysShowExcludeFromCaptureChanged, &context, interrupt);
        break;
    case 4:
        connect(settings.get(), &KDecoration3::DecorationSettings::borderSizeChanged, &context, interrupt);
        break;
    }
    QList<QList<KDecoration3::DecorationButtonType>> delivered;
    if (boundary == 0) {
        connect(settings.get(), &KDecoration3::DecorationSettings::decorationButtonsLeftChanged, &context,
                [&](const QList<KDecoration3::DecorationButtonType> &buttons) {
            delivered.append(buttons);
        });
    } else if (boundary == 1) {
        connect(settings.get(), &KDecoration3::DecorationSettings::decorationButtonsRightChanged, &context,
                [&](const QList<KDecoration3::DecorationButtonType> &buttons) {
            delivered.append(buttons);
        });
    }
    writeSettings(true);
    Q_EMIT workspace()->configChanged();
    QVERIFY(interrupted);
    const QList<KDecoration3::DecorationButtonType> expectedLeft = {KDecoration3::DecorationButtonType::Menu};
    const QList<KDecoration3::DecorationButtonType> expectedRight = {KDecoration3::DecorationButtonType::Close};
    QCOMPARE(settings->decorationButtonsLeft(), expectedLeft);
    QCOMPARE(settings->decorationButtonsRight(), expectedRight);
    QVERIFY(!settings->isCloseOnDoubleClickOnMenu());
    QVERIFY(!settings->isAlwaysShowExcludeFromCapture());
    QCOMPARE(settings->borderSize(), KDecoration3::BorderSize::Normal);
    const auto registered = QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.kappmenuview"));
    QVERIFY(registered.isValid());
    QVERIFY(!registered.value());
    QCOMPARE(reconfigured.count(), 1);
    if (boundary == 0 || boundary == 1) {
        const QList<KDecoration3::DecorationButtonType> outerLeft = {KDecoration3::DecorationButtonType::ApplicationMenu};
        const QList<KDecoration3::DecorationButtonType> outerRight = {KDecoration3::DecorationButtonType::ApplicationMenu, KDecoration3::DecorationButtonType::Close};
        QCOMPARE(delivered.size(), 2);
        // The nested emission finishes first. The still-unwinding outer signal must keep
        // its original payload, not alias a member overwritten by the nested read.
        QCOMPARE(delivered.at(0), boundary == 0 ? expectedLeft : expectedRight);
        QCOMPARE(delivered.at(1), boundary == 0 ? outerLeft : outerRight);
    }
}

void TestXdgShellWindow::testDecorationSettingsDestruction()
{
    KConfigGroup config = kwinApp()->config()->group(QStringLiteral("org.kde.kdecoration2"));
    const auto saved = config.entryMap();
    const QStringList keys = {QStringLiteral("ButtonsOnLeft"), QStringLiteral("ButtonsOnRight")};
    const auto restore = qScopeGuard([&]() {
        for (const auto &key : keys) {
            if (saved.contains(key)) {
                config.writeEntry(key, saved.value(key));
            } else {
                config.deleteEntry(key);
            }
        }
        Q_EMIT workspace()->configChanged();
    });
    config.writeEntry("ButtonsOnLeft", "M");
    config.writeEntry("ButtonsOnRight", "X");
    Q_EMIT workspace()->configChanged();
    auto extra = std::make_unique<KDecoration3::DecorationSettings>(workspace()->decorationBridge());
    QPointer<KDecoration3::DecorationSettings> guard(extra.get());
    QSignalSpy reconfigured(extra.get(), &KDecoration3::DecorationSettings::reconfigured);
    QObject context;
    connect(extra.get(), &KDecoration3::DecorationSettings::decorationButtonsLeftChanged, &context, [&]() {
        extra.reset();
    });
    config.writeEntry("ButtonsOnLeft", "N");
    Q_EMIT workspace()->configChanged();
    QVERIFY(!guard);
    QCOMPARE(reconfigured.count(), 0);
    const auto active = workspace()->decorationBridge()->settings();
    QVERIFY(active);
    const QList<KDecoration3::DecorationButtonType> expectedLeft = {KDecoration3::DecorationButtonType::ApplicationMenu};
    const QList<KDecoration3::DecorationButtonType> expectedRight = {KDecoration3::DecorationButtonType::Close};
    QCOMPARE(active->decorationButtonsLeft(), expectedLeft);
    QCOMPARE(active->decorationButtonsRight(), expectedRight);
    const auto registered = QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.kappmenuview"));
    QVERIFY(registered.isValid());
    QVERIFY(registered.value());
}

void TestXdgShellWindow::testDecorationRecreationTraversal_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("unchanged-order") << 0;
    QTest::newRow("remove-later-member") << 1;
    QTest::newRow("remove-earlier-member") << 2;
    QTest::newRow("retained-deleted-member") << 3;
    QTest::newRow("current-member-retained") << 4;
    QTest::newRow("new-arrival-not-in-pass") << 5;
    QTest::newRow("self-close-during-visit") << 6;
}

void TestXdgShellWindow::testDecorationRecreationTraversal()
{
    QFETCH(int, scenario);
    struct Fixture
    {
        std::unique_ptr<KWayland::Client::Surface> surface;
        std::unique_ptr<Test::XdgToplevel> shell;
        std::unique_ptr<Test::XdgToplevelDecorationV1> decoration;
        QPointer<Window> window;
        int initialReferences = 0;
        int visits = 0;
    };
    std::array<Fixture, 4> fixtures;
    const auto create = [&](int i) -> Window * {
        auto &fixture = fixtures[i];
        fixture.surface = Test::createSurface();
        fixture.shell = Test::createXdgToplevelSurface(fixture.surface.get(), Test::CreationSetup::CreateOnly);
        fixture.decoration = Test::createXdgToplevelDecorationV1(fixture.shell.get());
        fixture.decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
        QSignalSpy configure(fixture.shell->xdgSurface(), &Test::XdgSurface::configureRequested);
        fixture.surface->commit(KWayland::Client::Surface::CommitFlag::None);
        if (!configure.wait()) {
            return nullptr;
        }
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        configure.clear();
        fixture.window = Test::renderAndWaitForShown(fixture.surface.get(), QSize(320, 240), Qt::blue);
        if (!fixture.window || (configure.isEmpty() && !configure.wait())) {
            return nullptr;
        }
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(fixture.surface.get(), QSize(320, 240), Qt::blue);
        if (!Test::waylandSync()) {
            return nullptr;
        }
        fixture.initialReferences = fixture.window->m_refCount;
        return fixture.window;
    };
    for (int i = 0; i < 3; ++i) {
        QVERIFY(create(i));
        QVERIFY(fixtures[i].window->isDecorated());
    }
    const auto initialOrder = workspace()->windows();
    QVERIFY(initialOrder.indexOf(fixtures[0].window) < initialOrder.indexOf(fixtures[1].window));
    QVERIFY(initialOrder.indexOf(fixtures[1].window) < initialOrder.indexOf(fixtures[2].window));
    QPointer<Window> retained;
    if (scenario == 3) {
        retained = fixtures[1].window;
        retained->ref();
    }
    KConfigGroup config = kwinApp()->config()->group(QStringLiteral("org.kde.kdecoration2"));
    const bool hadTheme = config.hasKey("theme");
    const QString previousTheme = config.readEntry("theme", QString());
    QList<int> visited;
    bool callback = false;
    // A window is visited when the recreated plugin decoration is parented to it. The trigger
    // row's callback removes another member, removes the visited window itself, or adds one.
    const int triggerIndex = (scenario == 1 || scenario == 3 || scenario == 5) ? 0 : ((scenario == 2 || scenario == 6) ? 1 : -1);
    RegistryEventFilter filter(QEvent::ChildAdded, [&](QObject *object, QEvent *) {
        for (int i = 0; i < 3; ++i) {
            if (object != fixtures[i].window) {
                continue;
            }
            ++fixtures[i].visits;
            visited.append(i);
            if (scenario == 4) {
                QVERIFY(fixtures[i].window->m_refCount > fixtures[i].initialReferences);
            }
            if (callback || i != triggerIndex) {
                return;
            }
            callback = true;
            if (scenario == 5) {
                QVERIFY(create(3));
                // Installed only after the newcomer's own map-time decoration exists, so any
                // visit counted on it can come only from the running recreation pass.
                fixtures[3].window->installEventFilter(&filter);
            } else {
                auto victim = fixtures[scenario == 2 ? 0 : 1].window;
                victim->setSkipCloseAnimation(true);
                victim->destroyWindow();
            }
            return;
        }
        if (object == fixtures[3].window) {
            ++fixtures[3].visits;
        }
    });
    // Runs after a failed assertion too. Filters go first so the restoring recreation cannot
    // reach this row's state; then the fixtures, the extra reference and the theme entry.
    const auto teardown = qScopeGuard([&]() {
        for (auto &fixture : fixtures) {
            if (fixture.window) {
                fixture.window->removeEventFilter(&filter);
            }
        }
        for (auto &fixture : fixtures) {
            if (fixture.window && !fixture.window->isDeleted()) {
                fixture.window->setSkipCloseAnimation(true);
                fixture.window->destroyWindow();
            }
        }
        if (retained) {
            retained->unref();
            retained.clear();
        }
        if (hadTheme) {
            config.writeEntry("theme", previousTheme);
        } else {
            config.deleteEntry("theme");
        }
        workspace()->decorationBridge()->reconfigure();
    });
    for (int i = 0; i < 3; ++i) {
        fixtures[i].window->installEventFilter(&filter);
    }
    config.writeEntry("theme", previousTheme + QStringLiteral("-traversal-test"));
    workspace()->decorationBridge()->reconfigure();
    for (auto &fixture : fixtures) {
        if (fixture.window) {
            fixture.window->removeEventFilter(&filter);
        }
    }
    if (scenario == 1 || scenario == 3) {
        QCOMPARE(visited, QList<int>({0, 2}));
    } else {
        QCOMPARE(visited, QList<int>({0, 1, 2}));
    }
    QCOMPARE(callback, triggerIndex >= 0);
    if (scenario == 3) {
        QVERIFY(retained && retained->isDeleted());
    }
    if (scenario == 5) {
        QVERIFY(fixtures[3].window);
        QVERIFY(fixtures[3].window->isDecorated());
        QCOMPARE(fixtures[3].visits, 0);
    }
    if (scenario == 6) {
        // The visited list above shows the self-closing member completed its visit and the pass
        // went on; the members after it keep their recreated decorations. Its deferred release
        // is proved by the final guarded-pointer checks, not by this deleted-state sanity check.
        QVERIFY(!fixtures[1].window || fixtures[1].window->isDeleted());
        QVERIFY(fixtures[0].window && fixtures[0].window->isDecorated());
        QVERIFY(fixtures[2].window && fixtures[2].window->isDecorated());
    }
    for (auto &fixture : fixtures) {
        if (fixture.window && !fixture.window->isDeleted()) {
            fixture.window->setSkipCloseAnimation(true);
            fixture.window->destroyWindow();
        }
    }
    if (retained) {
        retained->unref();
        retained.clear();
    }
    for (const auto &fixture : fixtures) {
        QTRY_VERIFY(!fixture.window);
    }
}

void TestXdgShellWindow::testDecorationBridgeReentry_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("idle-switch-control") << 0;
    QTest::newRow("switch-during-recreation") << 1;
    QTest::newRow("switch-during-client-request") << 2;
    QTest::newRow("disable-during-creation") << 3;
    QTest::newRow("coalesced-follow-up") << 4;
}

void TestXdgShellWindow::testDecorationBridgeReentry()
{
    QFETCH(int, scenario);
    // The in-tree fake decoration plugin is built next to the test binary. A loaded C++ plugin's
    // id is its file base name, looked up as <library path>/org.kde.kdecoration3/<id>.
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + QStringLiteral("/fakes"));
    const QString breezePlugin = QStringLiteral("org.kde.breeze");
    const QString breezeClass = QStringLiteral("Breeze::Decoration");
    const QString fakePlugin = QStringLiteral("fakedecoshadows");
    const QString fakeClass = QStringLiteral("FakeDecoWithShadows");
    auto bridge = workspace()->decorationBridge();
    const auto decorationClass = [](Window *window) -> QString {
        const auto decoration = window->nextDecoration();
        return decoration ? QString::fromLatin1(decoration->metaObject()->className()) : QString();
    };
    const auto bridgePlugin = [bridge]() -> QString {
        const QString info = bridge->supportInformation();
        const auto start = info.indexOf(QLatin1String("Plugin: "));
        if (start < 0) {
            return QString();
        }
        const auto end = info.indexOf(QLatin1Char('\n'), start);
        return info.mid(start + 8, end - start - 8);
    };
    struct Fixture
    {
        std::unique_ptr<KWayland::Client::Surface> surface;
        std::unique_ptr<Test::XdgToplevel> shell;
        std::unique_ptr<Test::XdgToplevelDecorationV1> decoration;
        QPointer<Window> window;
        int visits = 0;
    };
    std::array<Fixture, 3> fixtures;
    const auto create = [&](int i, Test::XdgToplevelDecorationV1::mode mode) -> Window * {
        auto &fixture = fixtures[i];
        fixture.surface = Test::createSurface();
        fixture.shell = Test::createXdgToplevelSurface(fixture.surface.get(), Test::CreationSetup::CreateOnly);
        fixture.decoration = Test::createXdgToplevelDecorationV1(fixture.shell.get());
        fixture.decoration->set_mode(mode);
        QSignalSpy configure(fixture.shell->xdgSurface(), &Test::XdgSurface::configureRequested);
        fixture.surface->commit(KWayland::Client::Surface::CommitFlag::None);
        if (!configure.wait()) {
            return nullptr;
        }
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        configure.clear();
        fixture.window = Test::renderAndWaitForShown(fixture.surface.get(), QSize(320, 240), Qt::blue);
        if (!fixture.window || (configure.isEmpty() && !configure.wait())) {
            return nullptr;
        }
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(fixture.surface.get(), QSize(320, 240), Qt::blue);
        if (!Test::waylandSync()) {
            return nullptr;
        }
        return fixture.window;
    };
    QCOMPARE(bridgePlugin(), breezePlugin);
    for (int i = 0; i < 3; ++i) {
        const bool clientSide = scenario == 2 && i == 2;
        QVERIFY(create(i, clientSide ? Test::XdgToplevelDecorationV1::mode_client_side : Test::XdgToplevelDecorationV1::mode_server_side));
        QCOMPARE(decorationClass(fixtures[i].window), clientSide ? QString() : breezeClass);
    }
    KConfigGroup config = kwinApp()->config()->group(QStringLiteral("org.kde.kdecoration2"));
    const QStringList keys = {QStringLiteral("library"), QStringLiteral("NoPlugin"), QStringLiteral("theme")};
    const auto saved = config.entryMap();
    const auto restoreConfiguration = [&]() {
        for (const auto &key : keys) {
            if (saved.contains(key)) {
                config.writeEntry(key, saved.value(key));
            } else {
                config.deleteEntry(key);
            }
        }
    };
    const auto visitCounts = [&]() {
        QList<int> counts;
        for (const auto &fixture : fixtures) {
            counts.append(fixture.visits);
        }
        return counts;
    };
    const auto countsText = [](const QList<int> &counts) {
        QStringList parts;
        for (int count : counts) {
            parts.append(QString::number(count));
        }
        return parts.join(QLatin1Char(','));
    };
    // A deferred follow-up runs from the event loop. Give it a turn, then require quiet: an empty
    // result means no window was recreated during the second turn.
    const auto drain = [&]() -> QString {
        QTest::qWait(50);
        const auto before = visitCounts();
        QTest::qWait(50);
        const auto after = visitCounts();
        if (after == before) {
            return QString();
        }
        return QStringLiteral("recreations still running: %1 then %2").arg(countsText(before), countsText(after));
    };
    bool triggered = false;
    const int triggerIndex = scenario == 0 ? -1 : (scenario == 2 ? 2 : 0);
    RegistryEventFilter filter(QEvent::ChildAdded, [&](QObject *object, QEvent *) {
        for (int i = 0; i < 3; ++i) {
            if (object != fixtures[i].window) {
                continue;
            }
            ++fixtures[i].visits;
            if (triggered || i != triggerIndex) {
                return;
            }
            // Inside the plugin decoration's construction for this window: ask for a different
            // configuration while the bridge is still creating with the current one.
            triggered = true;
            if (scenario == 3) {
                config.writeEntry("NoPlugin", true);
            } else {
                config.writeEntry("library", fakePlugin);
            }
            Q_EMIT workspace()->configChanged();
            if (scenario == 4) {
                config.writeEntry("theme", QStringLiteral("reentry-second-write"));
                Q_EMIT workspace()->configChanged();
            }
            return;
        }
    });
    bool restored = false;
    const auto teardown = qScopeGuard([&]() {
        for (auto &fixture : fixtures) {
            if (fixture.window) {
                fixture.window->removeEventFilter(&filter);
            }
        }
        if (!restored) {
            restoreConfiguration();
            Q_EMIT workspace()->configChanged();
            QTest::qWait(50);
        }
        for (auto &fixture : fixtures) {
            if (fixture.window && !fixture.window->isDeleted()) {
                fixture.window->setSkipCloseAnimation(true);
                fixture.window->destroyWindow();
            }
        }
    });
    for (auto &fixture : fixtures) {
        fixture.window->installEventFilter(&filter);
    }
    if (scenario == 0) {
        config.writeEntry("library", fakePlugin);
        Q_EMIT workspace()->configChanged();
        // An idle reconfiguration stays synchronous: the switch is complete on return.
        QCOMPARE(bridgePlugin(), fakePlugin);
        for (auto &fixture : fixtures) {
            QCOMPARE(decorationClass(fixture.window), fakeClass);
        }
        QCOMPARE(visitCounts(), QList<int>({1, 1, 1}));
        const QString afterSwitch = drain();
        QVERIFY2(afterSwitch.isEmpty(), qPrintable(afterSwitch));
        QCOMPARE(visitCounts(), QList<int>({1, 1, 1}));
    } else {
        if (scenario == 2) {
            fixtures[2].decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
            QVERIFY(Test::waylandSync());
        } else {
            config.writeEntry("theme", QStringLiteral("reentry-first-pass"));
            Q_EMIT workspace()->configChanged();
        }
        QVERIFY(triggered);
        // Per window, so a stale one is named with the decoration it kept.
        const QString expectedClass = scenario == 3 ? QString() : fakeClass;
        const auto describe = [&](int i) {
            const QString current = decorationClass(fixtures[i].window);
            return QStringLiteral("window %1 has %2").arg(i).arg(current.isEmpty() ? QStringLiteral("no decoration") : current);
        };
        for (int i = 0; i < 3; ++i) {
            QTRY_VERIFY2(decorationClass(fixtures[i].window) == expectedClass, qPrintable(describe(i)));
        }
        if (scenario == 3) {
            QVERIFY(!Decoration::DecorationBridge::hasPlugin());
            QCOMPARE(bridgePlugin(), QString());
        } else {
            QCOMPARE(bridgePlugin(), fakePlugin);
        }
        const QString afterReentry = drain();
        QVERIFY2(afterReentry.isEmpty(), qPrintable(afterReentry));
        // One interrupted pass plus one complete follow-up; the disabled follow-up creates nothing,
        // and the client-request row's other windows are reached only by the follow-up.
        const QList<int> expected = scenario == 3 ? QList<int>({1, 1, 1}) : (scenario == 2 ? QList<int>({1, 1, 2}) : QList<int>({2, 2, 2}));
        QCOMPARE(visitCounts(), expected);
    }
    restoreConfiguration();
    Q_EMIT workspace()->configChanged();
    restored = true;
    const QString afterRestore = drain();
    QVERIFY2(afterRestore.isEmpty(), qPrintable(afterRestore));
    QCOMPARE(bridgePlugin(), breezePlugin);
    QVERIFY(Decoration::DecorationBridge::hasPlugin());
    for (auto &fixture : fixtures) {
        QCOMPARE(decorationClass(fixture.window), breezeClass);
    }
}

void TestXdgShellWindow::testDecorationFactoryOwnership_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("switch-keeps-singleton") << 0;
    QTest::newRow("same-library-alias") << 1;
    QTest::newRow("second-bridge-teardown") << 2;
    QTest::newRow("external-destruction") << 3;
}

void TestXdgShellWindow::testDecorationFactoryOwnership()
{
    QFETCH(int, scenario);
    const QString fakesPath = QCoreApplication::applicationDirPath() + QStringLiteral("/fakes");
    QCoreApplication::addLibraryPath(fakesPath);
    const QString pluginNamespace = QStringLiteral("org.kde.kdecoration3");
    const QString breezePlugin = QStringLiteral("org.kde.breeze");
    const QString breezeClass = QStringLiteral("Breeze::Decoration");
    const QString fakePlugin = QStringLiteral("fakedecoshadows");
    const QString aliasPlugin = QStringLiteral("fakedecoshadowsalias");
    const QString fakeClass = QStringLiteral("FakeDecoWithShadows");
    auto bridge = workspace()->decorationBridge();
    // The plugin loader hands every holder of a library the same root object; for the same
    // metadata this is the object the bridge receives.
    const auto sharedFactory = [&](const QString &pluginId) -> QPointer<KPluginFactory> {
        const KPluginMetaData metaData = KPluginMetaData::findPluginById(pluginNamespace, pluginId);
        if (!metaData.isValid()) {
            return nullptr;
        }
        return KPluginFactory::loadFactory(metaData).plugin;
    };
    const auto decorationClass = [](Window *window) -> QString {
        const auto decoration = window->nextDecoration();
        return decoration ? QString::fromLatin1(decoration->metaObject()->className()) : QString();
    };
    const auto bridgePlugin = [bridge]() -> QString {
        const QString info = bridge->supportInformation();
        const auto start = info.indexOf(QLatin1String("Plugin: "));
        if (start < 0) {
            return QString();
        }
        const auto end = info.indexOf(QLatin1Char('\n'), start);
        return info.mid(start + 8, end - start - 8);
    };
    struct Fixture
    {
        std::unique_ptr<KWayland::Client::Surface> surface;
        std::unique_ptr<Test::XdgToplevel> shell;
        std::unique_ptr<Test::XdgToplevelDecorationV1> decoration;
        QPointer<Window> window;
    };
    std::array<Fixture, 2> fixtures;
    const auto create = [&](int i) -> Window * {
        auto &fixture = fixtures[i];
        fixture.surface = Test::createSurface();
        fixture.shell = Test::createXdgToplevelSurface(fixture.surface.get(), Test::CreationSetup::CreateOnly);
        fixture.decoration = Test::createXdgToplevelDecorationV1(fixture.shell.get());
        fixture.decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
        QSignalSpy configure(fixture.shell->xdgSurface(), &Test::XdgSurface::configureRequested);
        fixture.surface->commit(KWayland::Client::Surface::CommitFlag::None);
        if (!configure.wait()) {
            return nullptr;
        }
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        configure.clear();
        fixture.window = Test::renderAndWaitForShown(fixture.surface.get(), QSize(320, 240), Qt::blue);
        if (!fixture.window || (configure.isEmpty() && !configure.wait())) {
            return nullptr;
        }
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(fixture.surface.get(), QSize(320, 240), Qt::blue);
        if (!Test::waylandSync()) {
            return nullptr;
        }
        return fixture.window;
    };
    QCOMPARE(bridgePlugin(), breezePlugin);
    for (int i = 0; i < int(fixtures.size()); ++i) {
        QVERIFY(create(i));
        QCOMPARE(decorationClass(fixtures[i].window), breezeClass);
    }
    KConfigGroup config = kwinApp()->config()->group(QStringLiteral("org.kde.kdecoration2"));
    const QStringList keys = {QStringLiteral("library"), QStringLiteral("NoPlugin"), QStringLiteral("theme")};
    const auto saved = config.entryMap();
    const auto restoreConfiguration = [&]() {
        for (const auto &key : keys) {
            if (saved.contains(key)) {
                config.writeEntry(key, saved.value(key));
            } else {
                config.deleteEntry(key);
            }
        }
        Q_EMIT workspace()->configChanged();
    };
    const auto apply = [&](const char *key, const QString &value) {
        config.writeEntry(key, value);
        Q_EMIT workspace()->configChanged();
    };
    // A recreation pass gives every window a new decoration object.
    const auto currentDecorations = [&]() {
        QList<QPointer<KDecoration3::Decoration>> decorations;
        for (const auto &fixture : fixtures) {
            decorations.append(fixture.window->nextDecoration());
        }
        return decorations;
    };
    const auto replaced = [&](const QList<QPointer<KDecoration3::Decoration>> &before) {
        for (int i = 0; i < int(fixtures.size()); ++i) {
            const auto now = fixtures[i].window->nextDecoration();
            if (!now || (before[i] && before[i].data() == now)) {
                return false;
            }
        }
        return true;
    };
    QTemporaryDir aliasRoot;
    bool restored = false;
    const auto teardown = qScopeGuard([&]() {
        if (!restored) {
            restoreConfiguration();
        }
        QCoreApplication::removeLibraryPath(aliasRoot.path());
        for (auto &fixture : fixtures) {
            if (fixture.window && !fixture.window->isDeleted()) {
                fixture.window->setSkipCloseAnimation(true);
                fixture.window->destroyWindow();
            }
        }
    });

    if (scenario == 0) {
        QPointer<KPluginFactory> breeze = sharedFactory(breezePlugin);
        QVERIFY(breeze);
        apply("library", fakePlugin);
        QCOMPARE(bridgePlugin(), fakePlugin);
        for (auto &fixture : fixtures) {
            QCOMPARE(decorationClass(fixture.window), fakeClass);
        }
        QVERIFY2(breeze, "switching plugins destroyed the Breeze factory");
        restoreConfiguration();
        QCOMPARE(bridgePlugin(), breezePlugin);
        // Switching back reuses the same object.
        QCOMPARE(sharedFactory(breezePlugin).data(), breeze.data());
    } else if (scenario == 1) {
        QVERIFY(aliasRoot.isValid());
        QVERIFY(QDir(aliasRoot.path()).mkpath(pluginNamespace));
        const QString library = fakesPath + QLatin1Char('/') + pluginNamespace + QLatin1Char('/') + fakePlugin + QStringLiteral(".so");
        QVERIFY(QFile::exists(library));
        QVERIFY(QFile::link(library, aliasRoot.path() + QLatin1Char('/') + pluginNamespace + QLatin1Char('/') + aliasPlugin + QStringLiteral(".so")));
        QCoreApplication::addLibraryPath(aliasRoot.path());
        apply("library", fakePlugin);
        QCOMPARE(bridgePlugin(), fakePlugin);
        QPointer<KPluginFactory> fake = sharedFactory(fakePlugin);
        QVERIFY(fake);
        // The alias id reaches the same loaded library and therefore the same object.
        QCOMPARE(sharedFactory(aliasPlugin).data(), fake.data());
        const auto before = currentDecorations();
        apply("library", aliasPlugin);
        QVERIFY2(fake, "loading the same library under another id destroyed its factory");
        QCOMPARE(bridgePlugin(), aliasPlugin);
        QVERIFY(replaced(before));
        for (auto &fixture : fixtures) {
            QCOMPARE(decorationClass(fixture.window), fakeClass);
        }
    } else if (scenario == 2) {
        QPointer<KPluginFactory> breeze = sharedFactory(breezePlugin);
        QVERIFY(breeze);
        {
            // Another holder of the same library, constructed and initialized like the
            // workspace bridge.
            auto second = std::make_unique<Decoration::DecorationBridge>();
            second->init();
            QVERIFY(second->supportInformation().startsWith(QStringLiteral("Plugin: ") + breezePlugin + QLatin1Char('\n')));
        }
        QVERIFY2(breeze, "destroying a second bridge destroyed the shared Breeze factory");
        QVERIFY(Decoration::DecorationBridge::hasPlugin());
        const auto before = currentDecorations();
        apply("theme", QStringLiteral("factory-ownership-pass"));
        QVERIFY(replaced(before));
        for (auto &fixture : fixtures) {
            QCOMPARE(decorationClass(fixture.window), breezeClass);
        }
    } else {
        QPointer<KPluginFactory> breeze = sharedFactory(breezePlugin);
        QVERIFY(breeze);
        const auto before = currentDecorations();
        // As another holder of the library, or an unload, would.
        delete breeze.data();
        QVERIFY(!breeze);
        QVERIFY2(!Decoration::DecorationBridge::hasPlugin(), "the bridge still reports a destroyed factory");
        // The configuration is unchanged; the bridge loads the configured plugin again.
        Q_EMIT workspace()->configChanged();
        QVERIFY2(Decoration::DecorationBridge::hasPlugin(), "the bridge did not reload the destroyed factory");
        QVERIFY(sharedFactory(breezePlugin));
        QVERIFY(replaced(before));
        for (auto &fixture : fixtures) {
            QCOMPARE(decorationClass(fixture.window), breezeClass);
        }
    }
    restoreConfiguration();
    restored = true;
    QCOMPARE(bridgePlugin(), breezePlugin);
    QVERIFY(Decoration::DecorationBridge::hasPlugin());
    QVERIFY(sharedFactory(breezePlugin));
    for (auto &fixture : fixtures) {
        QCOMPARE(decorationClass(fixture.window), breezeClass);
    }
}

void TestXdgShellWindow::testMapUnmap()
{
    // This test verifies that the compositor destroys XdgToplevelWindow when the
    // associated xdg_toplevel surface is unmapped.

    // Create a wl_surface and an xdg_toplevel, but don't commit them yet!
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));

    QSignalSpy windowAddedSpy(workspace(), &Workspace::windowAdded);

    QSignalSpy configureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Tell the compositor that we want to map the surface.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // The compositor will respond with a configure event.
    QVERIFY(configureRequestedSpy.wait());
    QCOMPARE(configureRequestedSpy.count(), 1);

    // Now we can attach a buffer with actual data to the surface.
    Test::render(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(windowAddedSpy.wait());
    QCOMPARE(windowAddedSpy.count(), 1);
    Window *window = windowAddedSpy.last().first().value<Window *>();
    QVERIFY(window);
    QCOMPARE(window->readyForPainting(), true);

    // When the window becomes active, the compositor will send another configure event.
    QVERIFY(configureRequestedSpy.wait());
    QCOMPARE(configureRequestedSpy.count(), 2);

    // Unmap the xdg_toplevel surface by committing a null buffer.
    surface->attachBuffer(KWayland::Client::Buffer::Ptr());
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(Test::waitForWindowClosed(window));

    // Tell the compositor that we want to re-map the xdg_toplevel surface.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // The compositor will respond with a configure event.
    QVERIFY(configureRequestedSpy.wait());
    QCOMPARE(configureRequestedSpy.count(), 3);

    // Now we can attach a buffer with actual data to the surface.
    Test::render(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(windowAddedSpy.wait());
    QCOMPARE(windowAddedSpy.count(), 2);
    window = windowAddedSpy.last().first().value<Window *>();
    QVERIFY(window);
    QCOMPARE(window->readyForPainting(), true);

    // The compositor will respond with a configure event.
    QVERIFY(configureRequestedSpy.wait());
    QCOMPARE(configureRequestedSpy.count(), 4);

    // Destroy the test window.
    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testWindowOutputs()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto size = QSize(200, 200);

    QSignalSpy outputEnteredSpy(surface.get(), &KWayland::Client::Surface::outputEntered);
    QSignalSpy outputLeftSpy(surface.get(), &KWayland::Client::Surface::outputLeft);

    auto window = Test::renderAndWaitForShown(surface.get(), size, Qt::blue);
    // assumption: window is initially placed on first screen
    QVERIFY(outputEnteredSpy.wait());
    QCOMPARE(outputEnteredSpy.count(), 1);
    QCOMPARE(surface->outputs().count(), 1);
    QCOMPARE(surface->outputs().first()->globalPosition(), QPoint(0, 0));

    // move to overlapping both first and second screen
    window->moveResize(RectF(QPoint(1250, 100), size));
    QVERIFY(outputEnteredSpy.wait());
    QCOMPARE(outputEnteredSpy.count(), 2);
    QCOMPARE(outputLeftSpy.count(), 0);
    QCOMPARE(surface->outputs().count(), 2);
    QVERIFY(surface->outputs()[0] != surface->outputs()[1]);

    // move entirely into second screen
    window->moveResize(RectF(QPoint(1400, 100), size));
    QVERIFY(outputLeftSpy.wait());
    QCOMPARE(outputEnteredSpy.count(), 2);
    QCOMPARE(outputLeftSpy.count(), 1);
    QCOMPARE(surface->outputs().count(), 1);
    QCOMPARE(surface->outputs().first()->globalPosition(), QPoint(1280, 0));
}

void TestXdgShellWindow::testMinimizeActiveWindow()
{
    // this test verifies that when minimizing the active window it gets deactivated
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<QObject> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(workspace()->activeWindow(), window);
    QVERIFY(window->wantsInput());
    QVERIFY(window->wantsTabFocus());
    QVERIFY(window->isShown());

    workspace()->slotWindowMinimize();
    QVERIFY(!window->isShown());
    QVERIFY(window->wantsInput());
    QVERIFY(window->wantsTabFocus());
    QVERIFY(!window->isActive());
    QVERIFY(!workspace()->activeWindow());
    QVERIFY(window->isMinimized());

    // unminimize again
    window->setMinimized(false);
    QVERIFY(!window->isMinimized());
    QVERIFY(!window->isActive());
    QVERIFY(window->wantsInput());
    QVERIFY(window->wantsTabFocus());
    QVERIFY(window->isShown());
    QCOMPARE(workspace()->activeWindow(), nullptr);
}

void TestXdgShellWindow::testFullscreen_data()
{
    QTest::addColumn<Test::XdgToplevelDecorationV1::mode>("decoMode");

    QTest::newRow("client-side deco") << Test::XdgToplevelDecorationV1::mode_client_side;
    QTest::newRow("server-side deco") << Test::XdgToplevelDecorationV1::mode_server_side;
}

void TestXdgShellWindow::testFullscreen()
{
    // this test verifies that a window can be properly fullscreened

    Test::XdgToplevel::States states;

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shellSurface.get()));
    QSignalSpy decorationConfigureRequestedSpy(decoration.get(), &Test::XdgToplevelDecorationV1::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Initialize the xdg-toplevel surface.
    QFETCH(Test::XdgToplevelDecorationV1::mode, decoMode);
    decoration->set_mode(decoMode);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(500, 250), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(window->layer(), NormalLayer);
    QVERIFY(!window->isFullScreen());
    QCOMPARE(window->clientSize(), QSize(500, 250));
    QCOMPARE(window->isDecorated(), decoMode == Test::XdgToplevelDecorationV1::mode_server_side);
    QCOMPARE(window->clientSizeToFrameSize(window->clientSize()), window->size());

    QSignalSpy fullScreenChangedSpy(window, &Window::fullScreenChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);

    // Wait for the compositor to send a configure event with the Activated state.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states & Test::XdgToplevel::State::Activated);

    // Ask the compositor to show the window in full screen mode.
    shellSurface->set_fullscreen(nullptr);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states & Test::XdgToplevel::State::Fullscreen);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), window->output()->geometry().size());

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);

    QVERIFY(fullScreenChangedSpy.wait());
    QCOMPARE(fullScreenChangedSpy.count(), 1);
    QVERIFY(window->isFullScreen());
    QVERIFY(!window->isDecorated());
    QCOMPARE(window->layer(), ActiveLayer);
    QCOMPARE(window->frameGeometry(), RectF(QPoint(0, 0), window->output()->geometry().size()));

    // Ask the compositor to show the window in normal mode.
    shellSurface->unset_fullscreen();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!(states & Test::XdgToplevel::State::Fullscreen));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(500, 250));

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::blue);

    QVERIFY(fullScreenChangedSpy.wait());
    QCOMPARE(fullScreenChangedSpy.count(), 2);
    QCOMPARE(window->clientSize(), QSize(500, 250));
    QVERIFY(!window->isFullScreen());
    QCOMPARE(window->isDecorated(), decoMode == Test::XdgToplevelDecorationV1::mode_server_side);
    QCOMPARE(window->layer(), NormalLayer);

    // Destroy the window.
    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testUserCanSetFullscreen()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QVERIFY(!window->isFullScreen());
    QVERIFY(window->isFullScreenable());
}

void TestXdgShellWindow::testSendFullScreenWindowToAnotherOutput()
{
    // This test verifies that the fullscreen window will have correct geometry restore
    // after it's sent to another output.

    const auto outputs = workspace()->outputs();

    // Create the window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    // Wait for the compositor to send a configure event with the activated state.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Move the window to the left monitor.
    window->move(QPointF(10, 20));
    QCOMPARE(window->frameGeometry(), RectF(10, 20, 100, 50));
    QCOMPARE(window->output(), outputs[0]);

    // Make the window fullscreen.
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->set_fullscreen(nullptr);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->isFullScreen(), true);
    QCOMPARE(window->frameGeometry(), RectF(0, 0, 1280, 1024));
    QCOMPARE(window->fullscreenGeometryRestore(), RectF(10, 20, 100, 50));
    QCOMPARE(window->output(), outputs[0]);

    // Send the window to another output.
    window->sendToOutput(outputs[1]);
    QCOMPARE(window->isFullScreen(), true);
    QCOMPARE(window->frameGeometry(), RectF(1280, 0, 1280, 1024));
    QCOMPARE(window->fullscreenGeometryRestore(), RectF(1280 + 10, 20, 100, 50));
    QCOMPARE(window->output(), outputs[1]);
}

void TestXdgShellWindow::testFullscreenPerOutputVirtualDesktops()
{
    // This test verifies that opening a fullscreen window on inactive output doesn't cause it to switch the output's desktop

    const auto outputs = workspace()->outputs();
    VirtualDesktopManager::self()->setCount(2);
    VirtualDesktopManager::self()->setPerOutputVirtualDesktops(true);
    const auto desktops = VirtualDesktopManager::self()->desktops();
    VirtualDesktopManager::self()->setCurrent(desktops[0], outputs[0]);
    VirtualDesktopManager::self()->setCurrent(desktops[1], outputs[1]);

    QCOMPARE(workspace()->activeOutput(), outputs[0]);
    QCOMPARE(VirtualDesktopManager::self()->currentDesktop(outputs[0]), desktops[0]);
    QCOMPARE(VirtualDesktopManager::self()->currentDesktop(outputs[1]), desktops[1]);

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));

    QSignalSpy windowAddedSpy(workspace(), &Workspace::windowAdded);
    QSignalSpy configureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    shellSurface->set_fullscreen(*Test::waylandOutput(outputs[1]->name()));

    // Tell the compositor that we want to map the surface.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // The compositor will respond with a configure event.
    QVERIFY(configureRequestedSpy.wait());
    QCOMPARE(configureRequestedSpy.count(), 1);
    shellSurface->xdgSurface()->ack_configure(configureRequestedSpy.last().at(0).value<quint32>());

    // Now we can attach a buffer with actual data to the surface.
    Test::render(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(windowAddedSpy.wait());
    QCOMPARE(windowAddedSpy.count(), 1);
    Window *window = windowAddedSpy.last().first().value<Window *>();
    QVERIFY(window);
    QCOMPARE(window->readyForPainting(), true);
    QCOMPARE(window->output(), outputs[1]);

    // Make sure that the virtual desktop assignment didn't change
    QCOMPARE(VirtualDesktopManager::self()->currentDesktop(outputs[0]), desktops[0]);
    QCOMPARE(VirtualDesktopManager::self()->currentDesktop(outputs[1]), desktops[1]);

    // Destroy the test window.
    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testMaximizedToFullscreen_data()
{
    QTest::addColumn<Test::XdgToplevelDecorationV1::mode>("decoMode");

    QTest::newRow("client-side deco") << Test::XdgToplevelDecorationV1::mode_client_side;
    QTest::newRow("server-side deco") << Test::XdgToplevelDecorationV1::mode_server_side;
}

void TestXdgShellWindow::testMaximizedToFullscreen()
{
    // this test verifies that a window can be properly fullscreened after maximizing

    Test::XdgToplevel::States states;

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shellSurface.get()));
    QSignalSpy decorationConfigureRequestedSpy(decoration.get(), &Test::XdgToplevelDecorationV1::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Initialize the xdg-toplevel surface.
    QFETCH(Test::XdgToplevelDecorationV1::mode, decoMode);
    decoration->set_mode(decoMode);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(300, 200), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QVERIFY(!window->isFullScreen());
    QCOMPARE(window->clientSize(), QSize(300, 200));
    QCOMPARE(window->isDecorated(), decoMode == Test::XdgToplevelDecorationV1::mode_server_side);

    QSignalSpy fullscreenChangedSpy(window, &Window::fullScreenChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);

    // Wait for the compositor to send a configure event with the Activated state.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states & Test::XdgToplevel::State::Activated);

    // Ask the compositor to maximize the window.
    shellSurface->set_maximized();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states & Test::XdgToplevel::State::Maximized);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeFull);

    // Ask the compositor to show the window in full screen mode.
    shellSurface->set_fullscreen(nullptr);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), window->output()->geometry().size());
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states & Test::XdgToplevel::State::Maximized);
    QVERIFY(states & Test::XdgToplevel::State::Fullscreen);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);

    QVERIFY(fullscreenChangedSpy.wait());
    QCOMPARE(fullscreenChangedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QVERIFY(window->isFullScreen());
    QVERIFY(!window->isDecorated());

    // Switch back to normal mode.
    shellSurface->unset_fullscreen();
    shellSurface->unset_maximized();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 5);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(300, 200));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!(states & Test::XdgToplevel::State::Maximized));
    QVERIFY(!(states & Test::XdgToplevel::State::Fullscreen));

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QVERIFY(!window->isFullScreen());
    QCOMPARE(window->isDecorated(), decoMode == Test::XdgToplevelDecorationV1::mode_server_side);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // Destroy the window.
    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testFullscreenMultipleOutputs()
{
    // this test verifies that kwin will place fullscreen windows in the outputs its instructed to

    const auto outputs = workspace()->outputs();
    for (KWin::LogicalOutput *output : outputs) {
        Test::XdgToplevel::States states;

        std::unique_ptr<KWayland::Client::Surface> surface = Test::createSurface();
        QVERIFY(surface);
        std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
        QVERIFY(shellSurface);

        auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
        QVERIFY(window);
        QVERIFY(window->isActive());
        QVERIFY(!window->isFullScreen());
        QCOMPARE(window->clientSize(), QSize(100, 50));
        QVERIFY(!window->isDecorated());

        QSignalSpy fullscreenChangedSpy(window, &Window::fullScreenChanged);
        QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
        QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
        QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

        // Wait for the compositor to send a configure event with the Activated state.
        QVERIFY(surfaceConfigureRequestedSpy.wait());
        QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
        states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
        QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));

        // Ask the compositor to show the window in full screen mode.
        shellSurface->set_fullscreen(*Test::waylandOutput(output->name()));
        QVERIFY(surfaceConfigureRequestedSpy.wait());
        QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
        QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), output->geometry().size());

        shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);

        QVERIFY(!fullscreenChangedSpy.isEmpty() || fullscreenChangedSpy.wait());
        QCOMPARE(fullscreenChangedSpy.count(), 1);

        QVERIFY(!frameGeometryChangedSpy.isEmpty() || frameGeometryChangedSpy.wait());

        QVERIFY(window->isFullScreen());

        QCOMPARE(window->frameGeometry(), RectF(output->geometry()));
    }
}

void TestXdgShellWindow::testHidden()
{
    // this test verifies that when hiding window it doesn't get shown
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<QObject> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(workspace()->activeWindow(), window);
    QVERIFY(window->wantsInput());
    QVERIFY(window->wantsTabFocus());
    QVERIFY(window->isShown());

    window->setHidden(true);
    QVERIFY(!window->isShown());
    QVERIFY(!window->isActive());
    QVERIFY(window->wantsInput());
    QVERIFY(window->wantsTabFocus());

    // unhide again
    window->setHidden(false);
    QVERIFY(window->isShown());
    QVERIFY(window->wantsInput());
    QVERIFY(window->wantsTabFocus());

    // QCOMPARE(workspace()->activeClient(), c);
}

void TestXdgShellWindow::testDesktopFileName()
{
    QIcon::setThemeName(QStringLiteral("breeze"));
    // this test verifies that desktop file name is passed correctly to the window
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    // only xdg-shell as ShellSurface misses the setter
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    // A client that never call set_app_id still gets the default icon
    QCOMPARE(window->desktopFileName(), QString());
    QVERIFY(window->resourceClass().startsWith("testXdgShellWindow"));
    QVERIFY(window->resourceName().startsWith("testXdgShellWindow"));
    QCOMPARE(window->icon().name(), QStringLiteral("wayland"));

    QSignalSpy desktopFileNameChangedSpy(window, &Window::desktopFileNameChanged);

    shellSurface->set_app_id(QStringLiteral("org.kde.foo"));
    QVERIFY(desktopFileNameChangedSpy.wait());
    QCOMPARE(window->desktopFileName(), QStringLiteral("org.kde.foo"));
    QCOMPARE(window->resourceClass(), QStringLiteral("org.kde.foo"));
    QVERIFY(window->resourceName().startsWith("testXdgShellWindow"));
    // the desktop file does not exist, so icon should be generic Wayland
    QCOMPARE(window->icon().name(), QStringLiteral("wayland"));

    QSignalSpy iconChangedSpy(window, &Window::iconChanged);
    shellSurface->set_app_id(QStringLiteral("org.kde.bar"));
    QVERIFY(desktopFileNameChangedSpy.wait());
    QCOMPARE(window->desktopFileName(), QStringLiteral("org.kde.bar"));
    QCOMPARE(window->resourceClass(), QStringLiteral("org.kde.bar"));
    QVERIFY(window->resourceName().startsWith("testXdgShellWindow"));
    // icon should still be wayland
    QCOMPARE(window->icon().name(), QStringLiteral("wayland"));
    QVERIFY(iconChangedSpy.isEmpty());

    const QString dfPath = QFINDTESTDATA("data/example.desktop");
    shellSurface->set_app_id(dfPath.toUtf8());
    QVERIFY(desktopFileNameChangedSpy.wait());
    QCOMPARE(iconChangedSpy.count(), 1);
    QCOMPARE(window->desktopFileName(), dfPath);
    QCOMPARE(window->icon().name(), QStringLiteral("kwin"));
}

void TestXdgShellWindow::testCaptionSimplified()
{
    // this test verifies that caption is properly trimmed
    // see BUG 323798 comment #12
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    // only done for xdg-shell as ShellSurface misses the setter
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    const QString origTitle = QString::fromUtf8(QByteArrayLiteral("Was tun, wenn Schüler Autismus haben?\342\200\250\342\200\250\342\200\250 – Marlies Hübner - Mozilla Firefox"));
    shellSurface->set_title(origTitle);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->caption() != origTitle);
    QCOMPARE(window->caption(), origTitle.simplified());
}

void TestXdgShellWindow::testUnresponsiveWindow_data()
{
    QTest::addColumn<QString>("shellInterface"); // see env selection in qwaylandintegration.cpp
    QTest::addColumn<bool>("socketMode");

    QTest::newRow("xdg display") << "xdg-shell" << false;
    QTest::newRow("xdg socket") << "xdg-shell" << true;
}

void TestXdgShellWindow::testUnresponsiveWindow()
{
    // this test verifies that killWindow properly terminates a process
    // for this an external binary is launched
    const QString kill = QFINDTESTDATA(QStringLiteral("kill"));
    QVERIFY(!kill.isEmpty());
    QSignalSpy windowAddedSpy(workspace(), &Workspace::windowAdded);

    std::unique_ptr<QProcess> process(new QProcess);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    QFETCH(QString, shellInterface);
    QFETCH(bool, socketMode);
    env.insert("QT_WAYLAND_SHELL_INTEGRATION", shellInterface);
    if (socketMode) {
        int sx[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sx) >= 0);
        waylandServer()->display()->createClient(sx[0]);
        int socket = dup(sx[1]);
        QVERIFY(socket != -1);
        env.insert(QStringLiteral("WAYLAND_SOCKET"), QByteArray::number(socket));
        env.remove("WAYLAND_DISPLAY");
    } else {
        env.insert("WAYLAND_DISPLAY", waylandServer()->socketName());
    }
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setProgram(kill);
    QSignalSpy processStartedSpy{process.get(), &QProcess::started};
    process->start();
    QVERIFY(processStartedSpy.wait());

    Window *killWindow = nullptr;
    if (windowAddedSpy.isEmpty()) {
        QVERIFY(windowAddedSpy.wait());
    }
    ::kill(process->processId(), SIGUSR1); // send a signal to freeze the process

    killWindow = windowAddedSpy.first().first().value<Window *>();
    QVERIFY(killWindow);
    QSignalSpy unresponsiveSpy(killWindow, &Window::unresponsiveChanged);
    QSignalSpy killedSpy(process.get(), static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished));
    QSignalSpy deletedSpy(killWindow, &QObject::destroyed);

    qint64 startTime = QDateTime::currentMSecsSinceEpoch();

    // wait for the process to be frozen
    QTest::qWait(10);

    // pretend the user clicked the close button
    killWindow->closeWindow();

    // window should not yet be marked unresponsive nor killed
    QVERIFY(!killWindow->unresponsive());
    QVERIFY(killedSpy.isEmpty());

    QVERIFY(unresponsiveSpy.wait());
    // window should be marked unresponsive but not killed
    auto elapsed1 = QDateTime::currentMSecsSinceEpoch() - startTime;
    const int timeout = options->killPingTimeout() / 2; // first timeout at half the time is for "unresponsive".
    QVERIFY(elapsed1 > timeout - 200 && elapsed1 < timeout + 200); // coarse timers on a test across two processes means we need a fuzzy compare
    QVERIFY(killWindow->unresponsive());
    QVERIFY(killedSpy.isEmpty());

    // TODO verify that kill prompt works.
    killWindow->killWindow();
    process->kill();

    QVERIFY(killedSpy.wait());

    if (deletedSpy.isEmpty()) {
        QVERIFY(deletedSpy.wait());
    }
}

void TestXdgShellWindow::testAppMenu()
{
    // register a faux appmenu client
    QVERIFY(QDBusConnection::sessionBus().registerService("org.kde.kappmenu"));

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::AppMenu> menu(Test::waylandAppMenuManager()->create(surface.get()));
    QSignalSpy spy(window, &Window::hasApplicationMenuChanged);
    menu->setAddress("service.name", "object/path");
    spy.wait();
    QCOMPARE(window->hasApplicationMenu(), true);
    QCOMPARE(window->applicationMenuServiceName(), QString("service.name"));
    QCOMPARE(window->applicationMenuObjectPath(), QString("object/path"));

    QVERIFY(QDBusConnection::sessionBus().unregisterService("org.kde.kappmenu"));
}

void TestXdgShellWindow::testSendClientWithTransientToDesktop()
{
    // this test verifies that when sending a window to a desktop all transients are also send to that desktop

    VirtualDesktopManager *vds = VirtualDesktopManager::self();
    vds->setCount(2);
    const QList<VirtualDesktop *> desktops = vds->desktops();

    std::unique_ptr<KWayland::Client::Surface> surface{Test::createSurface()};
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));

    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    // let's create a transient window
    std::unique_ptr<KWayland::Client::Surface> transientSurface{Test::createSurface()};
    std::unique_ptr<Test::XdgToplevel> transientShellSurface(Test::createXdgToplevelSurface(transientSurface.get()));
    transientShellSurface->set_parent(shellSurface->object());

    auto transient = Test::renderAndWaitForShown(transientSurface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(transient);
    QCOMPARE(workspace()->activeWindow(), transient);
    QCOMPARE(transient->transientFor(), window);
    QVERIFY(window->transients().contains(transient));

    // initially, the parent and the transient are on the first virtual desktop
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{desktops[0]});
    QVERIFY(!window->isOnAllDesktops());
    QCOMPARE(transient->desktops(), QList<VirtualDesktop *>{desktops[0]});
    QVERIFY(!transient->isOnAllDesktops());

    // send the transient to the second virtual desktop
    workspace()->slotWindowToDesktop(desktops[1]);
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{desktops[0]});
    QCOMPARE(transient->desktops(), QList<VirtualDesktop *>{desktops[1]});

    // activate c
    workspace()->activateWindow(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QVERIFY(window->isActive());

    // and send it to the desktop it's already on
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{desktops[0]});
    QCOMPARE(transient->desktops(), QList<VirtualDesktop *>{desktops[1]});
    workspace()->slotWindowToDesktop(desktops[0]);

    // which should move the transient back to the desktop
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{desktops[0]});
    QCOMPARE(transient->desktops(), QList<VirtualDesktop *>{desktops[0]});
}

void TestXdgShellWindow::testMinimizeWindowWithTransients()
{
    // this test verifies that when minimizing/unminimizing a window all its
    // transients will be minimized/unminimized as well

    // create the main window
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(!window->isMinimized());

    // create a transient window
    std::unique_ptr<KWayland::Client::Surface> transientSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> transientShellSurface(Test::createXdgToplevelSurface(transientSurface.get()));
    transientShellSurface->set_parent(shellSurface->object());
    auto transient = Test::renderAndWaitForShown(transientSurface.get(), QSize(100, 50), Qt::red);
    QVERIFY(transient);
    QVERIFY(!transient->isMinimized());
    QCOMPARE(transient->transientFor(), window);
    QVERIFY(window->hasTransient(transient, false));

    // minimize the main window, the transient should be minimized as well
    window->setMinimized(true);
    QVERIFY(window->isMinimized());
    QVERIFY(transient->isMinimized());

    // unminimize the main window, the transient should be unminimized as well
    window->setMinimized(false);
    QVERIFY(!window->isMinimized());
    QVERIFY(!transient->isMinimized());
}

void TestXdgShellWindow::testXdgDecoration_data()
{
    QTest::addColumn<Test::XdgToplevelDecorationV1::mode>("requestedMode");
    QTest::addColumn<Test::XdgToplevelDecorationV1::mode>("expectedMode");

    QTest::newRow("client side requested") << Test::XdgToplevelDecorationV1::mode_client_side << Test::XdgToplevelDecorationV1::mode_client_side;
    QTest::newRow("server side requested") << Test::XdgToplevelDecorationV1::mode_server_side << Test::XdgToplevelDecorationV1::mode_server_side;
}

void TestXdgShellWindow::testXdgDecoration()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    std::unique_ptr<Test::XdgToplevelDecorationV1> deco(Test::createXdgToplevelDecorationV1(shellSurface.get()));

    QSignalSpy decorationConfigureRequestedSpy(deco.get(), &Test::XdgToplevelDecorationV1::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    QFETCH(Test::XdgToplevelDecorationV1::mode, requestedMode);
    QFETCH(Test::XdgToplevelDecorationV1::mode, expectedMode);

    // request a mode
    deco->set_mode(requestedMode);

    // kwin will send a configure
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    QCOMPARE(decorationConfigureRequestedSpy.count(), 1);
    QCOMPARE(decorationConfigureRequestedSpy.last()[0].value<Test::XdgToplevelDecorationV1::mode>(), expectedMode);
    QVERIFY(decorationConfigureRequestedSpy.count() > 0);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last()[0].toInt());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QCOMPARE(window->isDecorated(), expectedMode == Test::XdgToplevelDecorationV1::mode_server_side);
}

void TestXdgShellWindow::testXdgNeverCommitted()
{
    // check we don't crash if we create a shell object but delete the XdgShellClient before committing it
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
}

void TestXdgShellWindow::testXdgInitialState()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    surfaceConfigureRequestedSpy.wait();
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    const auto size = toplevelConfigureRequestedSpy.first()[0].value<QSize>();

    QCOMPARE(size, QSize(0, 0)); // window should chose it's preferred size

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.first()[0].toUInt());

    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::blue);
    QCOMPARE(window->size(), QSize(200, 100));
}

void TestXdgShellWindow::testXdgInitiallyMaximised()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    shellSurface->set_maximized();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    surfaceConfigureRequestedSpy.wait();

    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    const auto size = toplevelConfigureRequestedSpy.first()[0].value<QSize>();
    const auto state = toplevelConfigureRequestedSpy.first()[1].value<Test::XdgToplevel::States>();

    QCOMPARE(size, QSize(1280, 1024));
    QVERIFY(state & Test::XdgToplevel::State::Maximized);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.first()[0].toUInt());

    auto window = Test::renderAndWaitForShown(surface.get(), size, Qt::blue);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->size(), QSize(1280, 1024));
}

void TestXdgShellWindow::testXdgInitiallyFullscreen()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    shellSurface->set_fullscreen(nullptr);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    surfaceConfigureRequestedSpy.wait();

    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    const auto size = toplevelConfigureRequestedSpy.first()[0].value<QSize>();
    const auto state = toplevelConfigureRequestedSpy.first()[1].value<Test::XdgToplevel::States>();

    QCOMPARE(size, QSize(1280, 1024));
    QVERIFY(state & Test::XdgToplevel::State::Fullscreen);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.first()[0].toUInt());

    auto window = Test::renderAndWaitForShown(surface.get(), size, Qt::blue);
    QCOMPARE(window->isFullScreen(), true);
    QCOMPARE(window->size(), QSize(1280, 1024));
}

void TestXdgShellWindow::testXdgInitiallyMinimized()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    shellSurface->set_minimized();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    surfaceConfigureRequestedSpy.wait();
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    const auto size = toplevelConfigureRequestedSpy.first()[0].value<QSize>();
    const auto state = toplevelConfigureRequestedSpy.first()[1].value<Test::XdgToplevel::States>();

    QCOMPARE(size, QSize(0, 0));
    QCOMPARE(state, 0);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.first()[0].toUInt());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isMinimized());
}

void TestXdgShellWindow::testXdgWindowGeometryIsntSet()
{
    // This test verifies that the effective window geometry corresponds to the
    // bounding rectangle of the main surface and its sub-surfaces if no window
    // geometry is set by the window.
    //
    // Note that when the window geometry changes spontaneously, the window will be centered.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    Test::render(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(50, 25));
    QCOMPARE(window->frameGeometry().size(), QSize(100, 50));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(50, 25));
    QCOMPARE(window->bufferGeometry().size(), QSize(100, 50));

    std::unique_ptr<KWayland::Client::Surface> childSurface(Test::createSurface());
    std::unique_ptr<KWayland::Client::SubSurface> subSurface(Test::createSubSurface(childSurface.get(), surface.get()));
    QVERIFY(subSurface);
    subSurface->setPosition(QPoint(-20, -10));
    Test::render(childSurface.get(), QSize(100, 50), Qt::blue);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(40, 20));
    QCOMPARE(window->frameGeometry().size(), QSize(120, 60));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(40, 20) + QPoint(20, 10));
    QCOMPARE(window->bufferGeometry().size(), QSize(100, 50));
}

void TestXdgShellWindow::testXdgWindowGeometryAttachBuffer()
{
    // This test verifies that the effective window geometry remains the same when
    // a new buffer is attached and xdg_surface.set_window_geometry is not called
    // again. Notice that the window geometry must remain the same even if the new
    // buffer is smaller.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 1);
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(10, 10));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(10, 10) - QPoint(10, 10));
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));

    Test::render(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(frameGeometryChangedSpy.count(), 1);
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(10, 10));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(10, 10) - QPoint(10, 10));
    QCOMPARE(window->bufferGeometry().size(), QSize(100, 50));

    shellSurface->xdgSurface()->set_window_geometry(0, 0, 100, 50);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 2);
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(50, 25));
    QCOMPARE(window->frameGeometry().size(), QSize(100, 50));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(50, 25));
    QCOMPARE(window->bufferGeometry().size(), QSize(100, 50));

    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testXdgWindowGeometryAttachSubSurface()
{
    // This test verifies that the effective window geometry remains the same
    // when a new sub-surface is added and xdg_surface.set_window_geometry is
    // not called again.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(10, 10));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(10, 10) - QPoint(10, 10));
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));

    std::unique_ptr<KWayland::Client::Surface> childSurface(Test::createSurface());
    std::unique_ptr<KWayland::Client::SubSurface> subSurface(Test::createSubSurface(childSurface.get(), surface.get()));
    QVERIFY(subSurface);
    subSurface->setPosition(QPoint(-20, -20));
    Test::render(childSurface.get(), QSize(100, 50), Qt::blue);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(10, 10));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(10, 10) - QPoint(10, 10));
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));

    shellSurface->xdgSurface()->set_window_geometry(-15, -15, 50, 40);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().topLeft(), QPointF(75, 30));
    QCOMPARE(window->frameGeometry().size(), QSize(50, 40));
    QCOMPARE(window->bufferGeometry().topLeft(), QPointF(75, 30) - QPoint(-15, -15));
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
}

void TestXdgShellWindow::testXdgWindowGeometryInteractiveResize()
{
    // This test verifies that correct window geometry is provided along each
    // configure event when an xdg-shell is being interactively resized.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));

    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    QSignalSpy interactiveMoveResizeSteppedSpy(window, &Window::interactiveMoveResizeStepped);
    QSignalSpy interactiveMoveResizeFinishedSpy(window, &Window::interactiveMoveResizeFinished);

    // Start interactively resizing the window.
    QCOMPARE(workspace()->moveResizeWindow(), nullptr);
    workspace()->slotWindowResize();
    QCOMPARE(workspace()->moveResizeWindow(), window);
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 1);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    Test::XdgToplevel::States states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));

    // Go right.
    QPointF cursorPos = KWin::Cursors::self()->mouse()->pos();
    window->keyPressEvent(Qt::Key_Right);
    window->updateInteractiveMoveResize(KWin::Cursors::self()->mouse()->pos(), Qt::KeyboardModifiers());
    QCOMPARE(KWin::Cursors::self()->mouse()->pos(), cursorPos + QPoint(8, 0));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(188, 80));
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 188, 80);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(208, 100), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->bufferGeometry().size(), QSize(208, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(188, 80));

    // Go down.
    cursorPos = KWin::Cursors::self()->mouse()->pos();
    window->keyPressEvent(Qt::Key_Down);
    window->updateInteractiveMoveResize(KWin::Cursors::self()->mouse()->pos(), Qt::KeyboardModifiers());
    QCOMPARE(KWin::Cursors::self()->mouse()->pos(), cursorPos + QPoint(0, 8));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(188, 88));
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 188, 88);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(208, 108), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 2);
    QCOMPARE(window->bufferGeometry().size(), QSize(208, 108));
    QCOMPARE(window->frameGeometry().size(), QSize(188, 88));

    // Finish resizing the window.
    window->keyPressEvent(Qt::Key_Enter);
    QCOMPARE(interactiveMoveResizeFinishedSpy.count(), 1);
    QCOMPARE(workspace()->moveResizeWindow(), nullptr);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 5);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Resizing));

    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testXdgWindowGeometryFullScreen()
{
    // This test verifies that an xdg-shell receives correct window geometry when
    // its fullscreen state gets changed.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));

    workspace()->slotWindowFullScreen();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1280, 1024));
    Test::XdgToplevel::States states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Fullscreen));
    shellSurface->xdgSurface()->set_window_geometry(0, 0, 1280, 1024);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1280, 1024), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(1280, 1024));
    QCOMPARE(window->frameGeometry().size(), QSize(1280, 1024));

    workspace()->slotWindowFullScreen();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(180, 80));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Fullscreen));
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));

    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testXdgWindowGeometryMaximize()
{
    // This test verifies that an xdg-shell receives correct window geometry when
    // its maximized state gets changed.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));

    workspace()->slotWindowMaximize();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1280, 1024));
    Test::XdgToplevel::States states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Maximized));
    shellSurface->xdgSurface()->set_window_geometry(0, 0, 1280, 1024);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1280, 1024), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(1280, 1024));
    QCOMPARE(window->frameGeometry().size(), QSize(1280, 1024));

    workspace()->slotWindowMaximize();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(180, 80));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));
    shellSurface->xdgSurface()->set_window_geometry(10, 10, 180, 80);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(180, 80));

    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testPointerInputTransform()
{
    // This test verifies that XdgToplevelWindow provides correct input transform matrix.
    // The input transform matrix is used by seat to map pointer events from the global
    // screen coordinates to the surface-local coordinates.

    // Get a wl_pointer object on the client side.
    std::unique_ptr<KWayland::Client::Pointer> pointer(Test::waylandSeat()->createPointer());
    QVERIFY(pointer);
    QVERIFY(pointer->isValid());
    QSignalSpy pointerEnteredSpy(pointer.get(), &KWayland::Client::Pointer::entered);
    QSignalSpy pointerMotionSpy(pointer.get(), &KWayland::Client::Pointer::motion);

    // Create an xdg_toplevel surface and wait for the compositor to catch up.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QCOMPARE(window->bufferGeometry().size(), QSize(200, 100));
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));

    // Enter the surface.
    quint32 timestamp = 0;
    Test::pointerMotion(window->pos(), timestamp++);
    QVERIFY(pointerEnteredSpy.wait());

    // Move the pointer to (10, 5) relative to the upper left frame corner, which is located
    // at (0, 0) in the surface-local coordinates.
    Test::pointerMotion(window->pos() + QPointF(10, 5), timestamp++);
    QVERIFY(pointerMotionSpy.wait());
    QCOMPARE(pointerMotionSpy.last().first().toPointF(), QPointF(10, 5));

    // Let's pretend that the window has changed the extents of the client-side drop-shadow
    // but the frame geometry didn't change.
    QSignalSpy bufferGeometryChangedSpy(window, &Window::bufferGeometryChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->set_window_geometry(10, 20, 200, 100);
    Test::render(surface.get(), QSize(220, 140), Qt::blue);
    QVERIFY(bufferGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 0);
    QCOMPARE(window->frameGeometry().size(), QSize(200, 100));
    QCOMPARE(window->bufferGeometry().size(), QSize(220, 140));

    // Move the pointer to (20, 50) relative to the upper left frame corner, which is located
    // at (10, 20) in the surface-local coordinates.
    Test::pointerMotion(window->pos() + QPointF(20, 50), timestamp++);
    QVERIFY(pointerMotionSpy.wait());
    QCOMPARE(pointerMotionSpy.last().first().toPointF(), QPointF(10, 20) + QPointF(20, 50));

    // Destroy the xdg-toplevel surface.
    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testReentrantSetFrameGeometry()
{
    // This test verifies that calling moveResize() from a slot connected directly
    // to the frameGeometryChanged() signal won't cause an infinite recursion.

    // Create an xdg-toplevel surface and wait for the compositor to catch up.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QCOMPARE(window->pos(), QPoint(0, 0));

    // Let's pretend that there is a script that really wants the window to be at (100, 100).
    connect(window, &Window::frameGeometryChanged, this, [window]() {
        window->moveResize(RectF(QPointF(100, 100), window->size()));
    });

    // Trigger the lambda above.
    window->move(QPoint(40, 50));

    // Eventually, the window will end up at (100, 100).
    QCOMPARE(window->pos(), QPoint(100, 100));

    // Destroy the xdg-toplevel surface.
    shellSurface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testDoubleMaximize()
{
    // This test verifies that the case where a window issues two set_maximized() requests
    // separated by the initial commit is handled properly.

    // Create the test surface.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    shellSurface->set_maximized();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // Wait for the compositor to respond with a configure event.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QSize size = toplevelConfigureRequestedSpy.last().at(0).toSize();
    QCOMPARE(size, QSize(1280, 1024));
    Test::XdgToplevel::States states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Maximized));

    // Send another set_maximized() request, but do not attach any buffer yet.
    shellSurface->set_maximized();
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // The compositor must respond with another configure event even if the state hasn't changed.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    size = toplevelConfigureRequestedSpy.last().at(0).toSize();
    QCOMPARE(size, QSize(1280, 1024));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Maximized));
}

void TestXdgShellWindow::testDoubleFullscreenSeparatedByCommit()
{
    // Some applications do weird things at startup and this is one of them. This test verifies
    // that the window will have good frame geometry if the window has issued several
    // xdg_toplevel.set_fullscreen requests and they are separated by a surface commit with
    // no attached buffer.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Tell the compositor that we want the window to be shown in fullscreen mode.
    shellSurface->set_fullscreen(nullptr);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(1280, 1024));
    QVERIFY(toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen);

    // Ask again.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    shellSurface->set_fullscreen(nullptr);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(1280, 1024));
    QVERIFY(toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen);

    // Map the window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(1280, 1024), Qt::blue);
    QVERIFY(window->isFullScreen());
    QCOMPARE(window->frameGeometry(), RectF(0, 0, 1280, 1024));
}

void TestXdgShellWindow::testMaximizeHorizontal()
{
    // Create the test window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // Wait for the initial configure event.
    Test::XdgToplevel::States states;
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(0, 0));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Map the window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QVERIFY(window->isMaximizable());
    QCOMPARE(window->maximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->size(), QSize(800, 600));

    // We should receive a configure event when the window becomes active.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Maximize the test window in horizontal direction.
    workspace()->slotWindowMaximizeHorizontal();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1280, 600));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the maximized window.
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1280, 600), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(1280, 600));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);

    // Restore the window.
    workspace()->slotWindowMaximizeHorizontal();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(800, 600));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the restored window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(800, 600));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // Destroy the window.
    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testMaximizeVertical()
{
    // Create the test window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // Wait for the initial configure event.
    Test::XdgToplevel::States states;
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(0, 0));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Map the window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QVERIFY(window->isMaximizable());
    QCOMPARE(window->maximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->size(), QSize(800, 600));

    // We should receive a configure event when the window becomes active.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Maximize the test window in vertical direction.
    workspace()->slotWindowMaximizeVertical();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeVertical);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(800, 1024));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the maximized window.
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(800, 1024), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(800, 1024));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeVertical);
    QCOMPARE(window->maximizeMode(), MaximizeVertical);

    // Restore the window.
    workspace()->slotWindowMaximizeVertical();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeVertical);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(800, 600));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the restored window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(800, 600));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // Destroy the window.
    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testMaximizeFull()
{
    // Create the test window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // Wait for the initial configure event.
    Test::XdgToplevel::States states;
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(0, 0));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Map the window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QVERIFY(window->isMaximizable());
    QCOMPARE(window->maximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->size(), QSize(800, 600));

    // We should receive a configure event when the window becomes active.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Maximize the test window.
    workspace()->slotWindowMaximize();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1280, 1024));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the maximized window.
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1280, 1024), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(1280, 1024));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    QCOMPARE(window->maximizeMode(), MaximizeFull);

    // Restore the window.
    workspace()->slotWindowMaximize();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(800, 600));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the restored window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(800, 600));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // Destroy the window.
    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testSendMaximizedWindowToAnotherOutput()
{
    // This test verifies that the maximized window will have correct geometry restore
    // after it's sent to another output.

    const auto outputs = workspace()->outputs();

    // Create the window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);

    // Wait for the compositor to send a configure event with the activated state.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Move the window to the left monitor.
    window->move(QPointF(10, 20));
    QCOMPARE(window->frameGeometry(), RectF(10, 20, 100, 50));
    QCOMPARE(window->output(), outputs[0]);

    // Make the window maximized.
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->set_maximized();
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->frameGeometry(), RectF(0, 0, 1280, 1024));
    QCOMPARE(window->geometryRestore(), RectF(10, 20, 100, 50));
    QCOMPARE(window->output(), outputs[0]);

    // Send the window to another output.
    window->sendToOutput(outputs[1]);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->frameGeometry(), RectF(1280, 0, 1280, 1024));
    QCOMPARE(window->geometryRestore(), RectF(1280 + 10, 20, 100, 50));
    QCOMPARE(window->output(), outputs[1]);
}

void TestXdgShellWindow::testInteractiveMoveUnmaximizeFull()
{
    // This test verifies that a maximized xdg-toplevel is going to be properly unmaximized when it's dragged.

    // Create the window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    // Wait for the compositor to send a configure event with the activated state.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Make the window maximized.
    const RectF originalGeometry = window->frameGeometry();
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    window->maximize(MaximizeFull);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);

    // Start interactive move.
    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    QSignalSpy interactiveMoveResizeSteppedSpy(window, &Window::interactiveMoveResizeStepped);
    QSignalSpy interactiveMoveResizeFinishedSpy(window, &Window::interactiveMoveResizeFinished);
    const qreal xOffset = 0.25;
    const qreal yOffset = 0.5;
    quint32 timestamp = 0;
    Test::pointerMotion(QPointF(window->x() + window->width() * xOffset, window->y() + window->height() * yOffset), timestamp++);
    window->performMousePressCommand(Options::MouseMove, input()->pointer()->pos());
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);

    // Move the window to unmaximize it.
    const RectF maximizedGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(0, 100), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 0);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry);

    // Move the window a tiny bit more.
    Test::pointerMotionRelative(QPointF(0, 10), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 0);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry);

    // Render the window at the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), RectF(input()->pointer()->pos() - QPointF(originalGeometry.width() * xOffset, originalGeometry.height() * yOffset), originalGeometry.size()));

    // Move the window again.
    const RectF normalGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(0, 10), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), normalGeometry.translated(0, 10));

    // Finish interactive move.
    window->keyPressEvent(Qt::Key_Enter);
    QCOMPARE(interactiveMoveResizeFinishedSpy.count(), 1);
}

void TestXdgShellWindow::testInteractiveMoveUnmaximizeInitiallyFull()
{
    // This test verifies that an initially maximized xdg-toplevel will be properly unmaximized when it's dragged.

    // Create the window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), [](Test::XdgToplevel *toplevel) {
        toplevel->set_maximized();
    }));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    // Wait for the compositor to send a configure event with the activated state.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Start interactive move.
    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    QSignalSpy interactiveMoveResizeSteppedSpy(window, &Window::interactiveMoveResizeStepped);
    QSignalSpy interactiveMoveResizeFinishedSpy(window, &Window::interactiveMoveResizeFinished);
    const qreal xOffset = 0.25;
    const qreal yOffset = 0.5;
    quint32 timestamp = 0;
    Test::pointerMotion(QPointF(window->x() + window->width() * xOffset, window->y() + window->height() * yOffset), timestamp++);
    window->performMousePressCommand(Options::MouseMove, input()->pointer()->pos());
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);

    // Move the window to unmaximize it.
    const RectF maximizedGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(0, 100), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 0);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry);

    // Move the window a tiny bit more.
    Test::pointerMotionRelative(QPointF(0, 10), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 0);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry);

    // Render the window at the new size.
    const QSize restoredSize(100, 50);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), restoredSize, Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), RectF(input()->pointer()->pos() - QPointF(restoredSize.width() * xOffset, restoredSize.height() * yOffset), restoredSize));

    // Move the window again.
    const RectF normalGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(0, 10), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), normalGeometry.translated(0, 10));

    // Finish interactive move.
    window->keyPressEvent(Qt::Key_Enter);
    QCOMPARE(interactiveMoveResizeFinishedSpy.count(), 1);
}

void TestXdgShellWindow::testInteractiveMoveUnmaximizeHorizontal()
{
    // This test verifies that a maximized horizontally xdg-toplevel is going to be properly unmaximized when it's dragged horizontally.

    // Create the window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    // Wait for the compositor to send a configure event with the activated state.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Make the window maximized.
    const RectF originalGeometry = window->frameGeometry();
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    window->maximize(MaximizeHorizontal);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeHorizontal);

    // Start interactive move.
    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    QSignalSpy interactiveMoveResizeSteppedSpy(window, &Window::interactiveMoveResizeStepped);
    QSignalSpy interactiveMoveResizeFinishedSpy(window, &Window::interactiveMoveResizeFinished);
    const qreal xOffset = 0.25;
    const qreal yOffset = 0.5;
    quint32 timestamp = 0;
    Test::pointerMotion(QPointF(window->x() + window->width() * xOffset, window->y() + window->height() * yOffset), timestamp++);
    window->performMousePressCommand(Options::MouseMove, input()->pointer()->pos());
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeHorizontal);

    // Move the window vertically, it's not going to be unmaximized.
    const RectF maximizedGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(0, 100), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->frameGeometry(), maximizedGeometry.translated(0, 100));

    // Move the window horizontally.
    Test::pointerMotionRelative(QPointF(100, 0), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry.translated(0, 100));

    // Move the window to the right a bit more.
    Test::pointerMotionRelative(QPointF(10, 0), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry.translated(0, 100));

    // Render the window at the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), RectF(input()->pointer()->pos() - QPointF(originalGeometry.width() * xOffset, originalGeometry.height() * yOffset), originalGeometry.size()));

    // Move the window again.
    const RectF normalGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(10, 0), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 2);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), normalGeometry.translated(10, 0));

    // Finish interactive move.
    window->keyPressEvent(Qt::Key_Enter);
    QCOMPARE(interactiveMoveResizeFinishedSpy.count(), 1);
}

void TestXdgShellWindow::testInteractiveMoveUnmaximizeVertical()
{
    // This test verifies that a maximized vertically xdg-toplevel is going to be properly unmaximized when it's dragged vertically.

    // Create the window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    // Wait for the compositor to send a configure event with the activated state.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Make the window maximized.
    const RectF originalGeometry = window->frameGeometry();
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    window->maximize(MaximizeVertical);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeVertical);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeVertical);

    // Start interactive move.
    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    QSignalSpy interactiveMoveResizeSteppedSpy(window, &Window::interactiveMoveResizeStepped);
    QSignalSpy interactiveMoveResizeFinishedSpy(window, &Window::interactiveMoveResizeFinished);
    const qreal xOffset = 0.25;
    const qreal yOffset = 0.5;
    quint32 timestamp = 0;
    Test::pointerMotion(QPointF(window->x() + window->width() * xOffset, window->y() + window->height() * yOffset), timestamp++);
    window->performMousePressCommand(Options::MouseMove, input()->pointer()->pos());
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeVertical);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeVertical);

    // Move the window to the right, it's not going to be unmaximized.
    const RectF maximizedGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(100, 0), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeVertical);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeVertical);
    QCOMPARE(window->frameGeometry(), maximizedGeometry.translated(100, 0));

    // Move the window vertically.
    Test::pointerMotionRelative(QPointF(0, 100), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeVertical);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry.translated(100, 0));

    // Move the window down a bit more.
    Test::pointerMotionRelative(QPointF(0, 10), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeVertical);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), maximizedGeometry.translated(100, 0));

    // Render the window at the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), RectF(input()->pointer()->pos() - QPointF(originalGeometry.width() * xOffset, originalGeometry.height() * yOffset), originalGeometry.size()));

    // Move the window again.
    const RectF normalGeometry = window->frameGeometry();
    Test::pointerMotionRelative(QPointF(0, 10), timestamp++);
    QCOMPARE(interactiveMoveResizeSteppedSpy.count(), 2);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->frameGeometry(), normalGeometry.translated(0, 10));

    // Finish interactive move.
    window->keyPressEvent(Qt::Key_Enter);
    QCOMPARE(interactiveMoveResizeFinishedSpy.count(), 1);
}

void TestXdgShellWindow::testRestoreWithNewMinSize()
{
    // This test verifies that new min size hint will taken into account when restoring a window.

    // Create the test window.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));

    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);

    // Wait for the initial configure event.
    Test::XdgToplevel::States states;
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(0, 0));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Map the window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());
    QVERIFY(window->isMaximizable());
    QCOMPARE(window->maximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeMode::MaximizeRestore);
    QCOMPARE(window->size(), QSize(800, 600));

    // We should receive a configure event when the window becomes active.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Maximize the test window.
    workspace()->slotWindowMaximize();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1280, 1024));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the maximized window.
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1280, 1024), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(1280, 1024));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    QCOMPARE(window->maximizeMode(), MaximizeFull);

    // Set a minimum size hint bigger than the restored size.
    shellSurface->set_min_size(1000, 900);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(Test::waylandSync());

    // Restore the window.
    workspace()->slotWindowMaximize();
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1000, 900));
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(!states.testFlag(Test::XdgToplevel::State::Maximized));

    // Draw contents of the restored window.
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1000, 900), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->size(), QSize(1000, 900));
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // Destroy the window.
    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
}

void TestXdgShellWindow::testMaximizeAndChangeDecorationModeAfterInitialCommit()
{
    // Ideally, the app would initialize the xdg-toplevel surface before the initial commit, but
    // many don't do it. They initialize the surface after the first commit.
    // This test verifies that the window will receive a configure event with correct size
    // if an xdg-toplevel surface is set maximized and decoration mode changes after initial commit.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shellSurface.get()));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Commit the initial state.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));

    // Request maximized mode and set decoration mode, i.e. perform late initialization.
    shellSurface->set_maximized();
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);

    // The compositor will respond with a new configure event, which should contain maximized state.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(1280, 1024));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>(), Test::XdgToplevel::State::Maximized);
}

void TestXdgShellWindow::testFullScreenAndChangeDecorationModeAfterInitialCommit()
{
    // Ideally, the app would initialize the xdg-toplevel surface before the initial commit, but
    // many don't do it. They initialize the surface after the first commit.
    // This test verifies that the window will receive a configure event with correct size
    // if an xdg-toplevel surface is set fullscreen and decoration mode changes after initial commit.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shellSurface.get()));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Commit the initial state.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));

    // Request fullscreen mode and set decoration mode, i.e. perform late initialization.
    shellSurface->set_fullscreen(nullptr);
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);

    // The compositor will respond with a new configure event, which should contain fullscreen state.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(1280, 1024));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>(), Test::XdgToplevel::State::Fullscreen);
}

void TestXdgShellWindow::testChangeDecorationModeAfterInitialCommit()
{
    // This test verifies that the compositor will respond with a good configure event when
    // the decoration mode changes after the first surface commit but before the surface is mapped.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shellSurface.get()));
    QSignalSpy decorationConfigureRequestedSpy(decoration.get(), &Test::XdgToplevelDecorationV1::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Perform the initial commit.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    QCOMPARE(decorationConfigureRequestedSpy.last().at(0).value<Test::XdgToplevelDecorationV1::mode>(), Test::XdgToplevelDecorationV1::mode_server_side);

    // Change decoration mode.
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);

    // The configure event should still have 0x0 size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    QCOMPARE(decorationConfigureRequestedSpy.last().at(0).value<Test::XdgToplevelDecorationV1::mode>(), Test::XdgToplevelDecorationV1::mode_client_side);
}

void TestXdgShellWindow::testModal()
{
    auto parentSurface = Test::createSurface();
    auto parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    auto parentWindow = Test::renderAndWaitForShown(parentSurface.get(), {200, 200}, Qt::cyan);
    QVERIFY(parentWindow);

    auto childSurface = Test::createSurface();
    auto childToplevel = Test::createXdgToplevelSurface(childSurface.get(), [&parentToplevel](Test::XdgToplevel *toplevel) {
        toplevel->set_parent(parentToplevel->object());
    });
    auto childWindow = Test::renderAndWaitForShown(childSurface.get(), {200, 200}, Qt::yellow);
    QVERIFY(childWindow);
    QVERIFY(!childWindow->isModal());
    QCOMPARE(childWindow->transientFor(), parentWindow);

    auto dialog = Test::createXdgDialogV1(childToplevel.get());
    QVERIFY(Test::waylandSync());
    QVERIFY(dialog);
    QVERIFY(!childWindow->isModal());

    QSignalSpy modalChangedSpy(childWindow, &Window::modalChanged);

    dialog->set_modal();
    Test::flushWaylandConnection();
    QVERIFY(modalChangedSpy.wait());
    QVERIFY(childWindow->isModal());

    dialog->unset_modal();
    Test::flushWaylandConnection();
    QVERIFY(modalChangedSpy.wait());
    QVERIFY(!childWindow->isModal());

    dialog->set_modal();
    Test::flushWaylandConnection();
    QVERIFY(modalChangedSpy.wait());
    Workspace::self()->activateWindow(parentWindow);
    QCOMPARE(Workspace::self()->activeWindow(), childWindow);

    dialog.reset();
    Test::flushWaylandConnection();
    QVERIFY(modalChangedSpy.wait());
    QVERIFY(!childWindow->isModal());
}

void TestXdgShellWindow::testNestedModals()
{
    // This test verifies that activating the parent window activates the most-nested modal.
    Test::XdgToplevelWindow parentWindow;
    QVERIFY(parentWindow.show({200, 200}, Qt::cyan));

    // 1st modal
    Test::XdgToplevelWindow childWindow([&parentWindow](Test::XdgToplevel *toplevel) {
        toplevel->set_parent(parentWindow.m_toplevel->object());
    });
    QVERIFY(childWindow.show({200, 200}, Qt::yellow));
    QVERIFY(!childWindow.m_window->isModal());
    QCOMPARE(childWindow.m_window->transientFor(), parentWindow.m_window);

    auto childDialog = Test::createXdgDialogV1(childWindow.m_toplevel.get());
    QVERIFY(Test::waylandSync());
    QVERIFY(childDialog);
    QVERIFY(!childWindow.m_window->isModal());

    QSignalSpy childModalChangedSpy(childWindow.m_window, &Window::modalChanged);

    childDialog->set_modal();
    Test::flushWaylandConnection();
    QVERIFY(childModalChangedSpy.wait());
    QVERIFY(childWindow.m_window->isModal());

    Workspace::self()->activateWindow(parentWindow.m_window);
    QCOMPARE(Workspace::self()->activeWindow(), childWindow.m_window);

    // 2nd modal
    Test::XdgToplevelWindow grandChildWindow([&childWindow](Test::XdgToplevel *toplevel) {
        toplevel->set_parent(childWindow.m_toplevel->object());
    });
    QVERIFY(grandChildWindow.show({200, 200}, Qt::green));
    QVERIFY(!grandChildWindow.m_window->isModal());
    QCOMPARE(grandChildWindow.m_window->transientFor(), childWindow.m_window);

    auto grandChildDialog = Test::createXdgDialogV1(grandChildWindow.m_toplevel.get());
    QVERIFY(Test::waylandSync());
    QVERIFY(grandChildDialog);
    QVERIFY(!grandChildWindow.m_window->isModal());

    QSignalSpy grandChildModalChangedSpy(grandChildWindow.m_window, &Window::modalChanged);

    grandChildDialog->set_modal();
    Test::flushWaylandConnection();
    QVERIFY(grandChildModalChangedSpy.wait());
    QVERIFY(grandChildWindow.m_window->isModal());

    Workspace::self()->activateWindow(parentWindow.m_window);
    QCOMPARE(Workspace::self()->activeWindow(), grandChildWindow.m_window);
}

void TestXdgShellWindow::testCloseModal()
{
    // This test verifies that the parent window will be activated when an active modal dialog is closed.

    // Create a parent and a child windows.
    auto parentSurface = Test::createSurface();
    auto parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    auto parent = Test::renderAndWaitForShown(parentSurface.get(), {200, 200}, Qt::cyan);
    QVERIFY(parent);

    auto childSurface = Test::createSurface();
    auto childToplevel = Test::createXdgToplevelSurface(childSurface.get(), [&parentToplevel](Test::XdgToplevel *toplevel) {
        toplevel->set_parent(parentToplevel->object());
    });
    auto child = Test::renderAndWaitForShown(childSurface.get(), {200, 200}, Qt::yellow);
    QVERIFY(child);
    QVERIFY(!child->isModal());
    QCOMPARE(child->transientFor(), parent);

    // Set modal state.
    auto dialog = Test::createXdgDialogV1(childToplevel.get());
    QSignalSpy modalChangedSpy(child, &Window::modalChanged);
    dialog->set_modal();
    Test::flushWaylandConnection();
    QVERIFY(modalChangedSpy.wait());
    QVERIFY(child->isModal());
    QCOMPARE(workspace()->activeWindow(), child);

    // Close the child.
    QSignalSpy childClosedSpy(child, &Window::closed);
    childToplevel.reset();
    childSurface.reset();
    dialog.reset();
    Test::flushWaylandConnection();
    QVERIFY(childClosedSpy.wait());
    QCOMPARE(workspace()->activeWindow(), parent);
}

void TestXdgShellWindow::testCloseModalPreSetup()
{
    // This test verifies that the parent window will be activated when an active modal dialog is closed
    // even if the modality existed before mapping the parent

    // Create a parent and a child windows.
    auto parentSurface = Test::createSurface();
    auto parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());

    auto childSurface = Test::createSurface();
    auto childToplevel = Test::createXdgToplevelSurface(childSurface.get(), [&parentToplevel](Test::XdgToplevel *toplevel) {
        toplevel->set_parent(parentToplevel->object());
    });
    auto dialog = Test::createXdgDialogV1(childToplevel.get());
    dialog->set_modal();

    auto parent = Test::renderAndWaitForShown(parentSurface.get(), {200, 200}, Qt::cyan);
    QVERIFY(parent);
    auto child = Test::renderAndWaitForShown(childSurface.get(), {200, 200}, Qt::yellow);
    QVERIFY(child);
    QCOMPARE(child->transientFor(), parent);

    Test::flushWaylandConnection();
    QVERIFY(child->isModal());
    QCOMPARE(workspace()->activeWindow(), child);

    // Close the child.
    QSignalSpy childClosedSpy(child, &Window::closed);
    childToplevel.reset();
    childSurface.reset();
    Test::flushWaylandConnection();
    QVERIFY(childClosedSpy.wait());
    QCOMPARE(workspace()->activeWindow(), parent);
}

void TestXdgShellWindow::testCloseInactiveModal()
{
    // This test verifies that the parent window will not be activated when an inactive modal dialog is closed.

    // Create a parent and a child windows.
    auto parentSurface = Test::createSurface();
    auto parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    auto parent = Test::renderAndWaitForShown(parentSurface.get(), {200, 200}, Qt::cyan);
    QVERIFY(parent);

    auto childSurface = Test::createSurface();
    auto childToplevel = Test::createXdgToplevelSurface(childSurface.get(), [&parentToplevel](Test::XdgToplevel *toplevel) {
        toplevel->set_parent(parentToplevel->object());
    });
    auto child = Test::renderAndWaitForShown(childSurface.get(), {200, 200}, Qt::yellow);
    QVERIFY(child);
    QVERIFY(!child->isModal());
    QCOMPARE(child->transientFor(), parent);

    // Set modal state.
    auto dialog = Test::createXdgDialogV1(childToplevel.get());
    QSignalSpy modalChangedSpy(child, &Window::modalChanged);
    dialog->set_modal();
    Test::flushWaylandConnection();
    QVERIFY(modalChangedSpy.wait());
    QVERIFY(child->isModal());
    QCOMPARE(workspace()->activeWindow(), child);

    // Show another window.
    auto otherSurface = Test::createSurface();
    auto otherToplevel = Test::createXdgToplevelSurface(otherSurface.get());
    auto otherWindow = Test::renderAndWaitForShown(otherSurface.get(), {200, 200}, Qt::magenta);
    QVERIFY(otherWindow);
    workspace()->activateWindow(otherWindow);
    QCOMPARE(workspace()->activeWindow(), otherWindow);

    // Close the child.
    QSignalSpy childClosedSpy(child, &Window::closed);
    childToplevel.reset();
    childSurface.reset();
    dialog.reset();
    Test::flushWaylandConnection();
    QVERIFY(childClosedSpy.wait());
    QCOMPARE(workspace()->activeWindow(), otherWindow);
}

void TestXdgShellWindow::testClosePopupOnParentUnmapped()
{
    // This test verifies that a popup window will be closed when the parent window is closed.

    std::unique_ptr<KWayland::Client::Surface> parentSurface = Test::createSurface();
    std::unique_ptr<Test::XdgToplevel> parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    Window *parent = Test::renderAndWaitForShown(parentSurface.get(), QSize(200, 200), Qt::cyan);
    QVERIFY(parent);

    std::unique_ptr<Test::XdgPositioner> positioner = Test::createXdgPositioner();
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<KWayland::Client::Surface> childSurface = Test::createSurface();
    std::unique_ptr<Test::XdgPopup> popup = Test::createXdgPopupSurface(childSurface.get(), parentToplevel->xdgSurface(), positioner.get());
    Window *child = Test::renderAndWaitForShown(childSurface.get(), QSize(10, 10), Qt::cyan);
    QVERIFY(child);

    QSignalSpy childClosedSpy(child, &Window::closed);
    parentToplevel.reset();
    parentSurface.reset();
    QVERIFY(childClosedSpy.wait());
}

void TestXdgShellWindow::testPopupWithDismissedParent()
{
    // This test verifies that a popup window will be closed when the parent window is already dismissed by the compositor.

    std::unique_ptr<KWayland::Client::Surface> parentSurface = Test::createSurface();
    std::unique_ptr<Test::XdgToplevel> parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    Window *parent = Test::renderAndWaitForShown(parentSurface.get(), QSize(200, 200), Qt::cyan);
    QVERIFY(parent);

    std::unique_ptr<Test::XdgPositioner> positioner = Test::createXdgPositioner();
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<KWayland::Client::Surface> childSurface = Test::createSurface();
    std::unique_ptr<Test::XdgPopup> popup = Test::createXdgPopupSurface(childSurface.get(), parentToplevel->xdgSurface(), positioner.get());
    Window *child = Test::renderAndWaitForShown(childSurface.get(), QSize(10, 10), Qt::cyan);
    QVERIFY(child);

    QSignalSpy childDoneSpy(popup.get(), &Test::XdgPopup::doneReceived);
    child->popupDone();
    QVERIFY(childDoneSpy.wait());

    // A nested popup will be dismissed immediately if its parent popup is already dismissed.
    std::unique_ptr<KWayland::Client::Surface> grandChildSurface = Test::createSurface();
    std::unique_ptr<Test::XdgPopup> grandChildPopup = Test::createXdgPopupSurface(grandChildSurface.get(), popup->xdgSurface(), positioner.get(), Test::CreationSetup::CreateOnly);
    QSignalSpy grandChildDoneSpy(grandChildPopup.get(), &Test::XdgPopup::doneReceived);
    grandChildPopup->xdgSurface()->surface()->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(grandChildDoneSpy.wait());
}

void TestXdgShellWindow::testPopupDismissedOnFocusChange()
{
    // This test verifies that a popup window will be dismissed when the focus changes.

    std::unique_ptr<KWayland::Client::Surface> parentSurface = Test::createSurface();
    std::unique_ptr<Test::XdgToplevel> parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    std::unique_ptr<KWayland::Client::Pointer> pointer(Test::waylandSeat()->createPointer());
    Window *parent = Test::renderAndWaitForShown(parentSurface.get(), QSize(200, 200), Qt::cyan);
    QVERIFY(parent);

    QSignalSpy buttonSpy(pointer.get(), &KWayland::Client::Pointer::buttonStateChanged);
    input()->pointer()->warp(parent->frameGeometry().center());
    // simulate press
    quint32 timestamp = 1;
    Test::pointerButtonPressed(BTN_LEFT, timestamp++);
    QVERIFY(buttonSpy.wait());

    std::unique_ptr<Test::XdgPositioner> positioner = Test::createXdgPositioner();
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<KWayland::Client::Surface> childSurface = Test::createSurface();
    std::unique_ptr<Test::XdgPopup> popup = Test::createXdgPopupSurface(childSurface.get(), parentToplevel->xdgSurface(), positioner.get());
    popup->grab(*Test::waylandSeat(), buttonSpy.first().first().value<quint32>());
    QPointer<Window> child = Test::renderAndWaitForShown(childSurface.get(), QSize(10, 10), Qt::cyan);
    QVERIFY(child);

    QSignalSpy popupDismissedSpy(popup.get(), &Test::XdgPopup::doneReceived);

    // create another toplevel that gets focus
    std::unique_ptr<KWayland::Client::Surface> otherSurface = Test::createSurface();
    std::unique_ptr<Test::XdgToplevel> otherToplevel = Test::createXdgToplevelSurface(otherSurface.get());
    Window *other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 200), Qt::cyan);
    QVERIFY(other);

    workspace()->activateWindow(other);

    QVERIFY(popupDismissedSpy.wait());
    QVERIFY(!child); // and the server-side window closed immediately too
}

void TestXdgShellWindow::testMinimumSize()
{
    // NOTE: a minimum size of 20px is forced by the compositor

    auto surface = Test::createSurface();
    auto shellSurface = Test::createXdgToplevelSurface(surface.get(), [](Test::XdgToplevel *toplevel) {
        toplevel->set_min_size(200, 250);
    });
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(300, 300), Qt::cyan);
    QCOMPARE(window->minSize(), QSizeF(200, 250));

    QSignalSpy committedSpy(window->surface(), &SurfaceInterface::committed);

    shellSurface->set_min_size(100, 100);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->minSize(), QSizeF(100, 100));

    shellSurface->set_min_size(0, 100);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->minSize(), QSizeF(20, 100));

    shellSurface->set_min_size(100, 0);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->minSize(), QSizeF(100, 20));

    shellSurface->set_min_size(0, 0);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->minSize(), QSizeF(20, 20));
}

void TestXdgShellWindow::testNoMinimumSize()
{
    // NOTE: a minimum size of 20px is forced by the compositor

    auto surface = Test::createSurface();
    auto shellSurface = Test::createXdgToplevelSurface(surface.get());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(300, 300), Qt::cyan);
    QCOMPARE(window->minSize(), QSizeF(20, 20));
}

void TestXdgShellWindow::testMaximumSize()
{
    auto surface = Test::createSurface();
    auto shellSurface = Test::createXdgToplevelSurface(surface.get(), [](Test::XdgToplevel *toplevel) {
        toplevel->set_max_size(300, 350);
    });
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 200), Qt::cyan);
    QCOMPARE(window->maxSize(), QSizeF(300, 350));

    QSignalSpy committedSpy(window->surface(), &SurfaceInterface::committed);

    shellSurface->set_max_size(400, 400);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->maxSize(), QSizeF(400, 400));

    shellSurface->set_max_size(0, 400);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->maxSize(), QSizeF(INT_MAX, 400));

    shellSurface->set_max_size(400, 0);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->maxSize(), QSizeF(400, INT_MAX));

    shellSurface->set_max_size(0, 0);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(committedSpy.wait());
    QCOMPARE(window->maxSize(), QSizeF(INT_MAX, INT_MAX));
}

void TestXdgShellWindow::testNoMaximumSize()
{
    auto surface = Test::createSurface();
    auto shellSurface = Test::createXdgToplevelSurface(surface.get());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(300, 300), Qt::cyan);
    QCOMPARE(window->maxSize(), QSizeF(INT_MAX, INT_MAX));
}

void TestXdgShellWindow::testUnconfiguredBufferToplevel()
{
    // This test verifies that a protocol error is posted when a client attaches a buffer to
    // the initial xdg-toplevel commit.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    Test::render(surface.get(), QSize(100, 50), Qt::blue);

    QSignalSpy connectionErrorSpy(Test::waylandConnection(), &KWayland::Client::ConnectionThread::errorOccurred);
    QVERIFY(connectionErrorSpy.wait());
}

void TestXdgShellWindow::testUnconfiguredBufferPopup()
{
    // This test verifies that a protocol error is posted when a client attaches a buffer to
    // the initial xdg-popup commit.

    std::unique_ptr<KWayland::Client::Surface> parentSurface = Test::createSurface();
    std::unique_ptr<Test::XdgToplevel> parentToplevel = Test::createXdgToplevelSurface(parentSurface.get());
    Test::renderAndWaitForShown(parentSurface.get(), QSize(200, 200), Qt::cyan);

    std::unique_ptr<Test::XdgPositioner> positioner = Test::createXdgPositioner();
    positioner->set_size(10, 10);
    positioner->set_anchor_rect(10, 10, 10, 10);

    std::unique_ptr<KWayland::Client::Surface> childSurface = Test::createSurface();
    std::unique_ptr<Test::XdgPopup> popup = Test::createXdgPopupSurface(childSurface.get(), parentToplevel->xdgSurface(), positioner.get(), Test::CreationSetup::CreateOnly);
    Test::render(childSurface.get(), QSize(100, 50), Qt::blue);

    QSignalSpy connectionErrorSpy(Test::waylandConnection(), &KWayland::Client::ConnectionThread::errorOccurred);
    QVERIFY(connectionErrorSpy.wait());
}

void TestXdgShellWindow::testRemoveActiveDesktopBeforeInitialCommit()
{
    // This test verifies that a window will be placed on the right desktop if the current desktop
    // is removed before the client has a chance to commit the initial state.

    VirtualDesktopManager *virtualDesktopManager = VirtualDesktopManager::self();
    virtualDesktopManager->setCount(2);

    const auto virtualDesktops = virtualDesktopManager->desktops();
    virtualDesktopManager->setCurrent(virtualDesktops[1]);

    // Create an xdg-toplevel surface.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Wait for the corresponding Window object to be created on the compositor side.
    QSignalSpy windowCreatedSpy(waylandServer(), &WaylandServer::windowCreated);
    QVERIFY(windowCreatedSpy.wait());
    Window *window = windowCreatedSpy.last().at(0).value<Window *>();
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{virtualDesktops[1]});

    // Remove the current desktop.
    virtualDesktopManager->removeVirtualDesktop(virtualDesktops[1]);

    // Commit the initial state.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Map the window.
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *mapped = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QCOMPARE(mapped, window);

    // The window should have been evacuated from the second virtual desktop to the first virtual desktop.
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{virtualDesktops[0]});
}

void TestXdgShellWindow::testRemoveActiveDesktopBeforeMap()
{
    // This test verifies that a window will be placed on the right desktop if the current desktop
    // is removed before the client has a chance to map the surface (after committing the initial state).

    VirtualDesktopManager *virtualDesktopManager = VirtualDesktopManager::self();
    virtualDesktopManager->setCount(2);

    const auto virtualDesktops = virtualDesktopManager->desktops();
    virtualDesktopManager->setCurrent(virtualDesktops[1]);

    // Create an xdg-toplevel surface.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Wait for the corresponding Window object to be created on the compositor side.
    QSignalSpy windowCreatedSpy(waylandServer(), &WaylandServer::windowCreated);
    QVERIFY(windowCreatedSpy.wait());
    Window *window = windowCreatedSpy.last().at(0).value<Window *>();
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{virtualDesktops[1]});

    // Commit the initial state.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Remove the current desktop.
    virtualDesktopManager->removeVirtualDesktop(virtualDesktops[1]);

    // Map the window.
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *mapped = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QCOMPARE(mapped, window);

    // The window should have been evacuated from the second virtual desktop to the first virtual desktop.
    QCOMPARE(window->desktops(), QList<VirtualDesktop *>{virtualDesktops[0]});
}

void TestXdgShellWindow::testRemoveActiveOutputBeforeInitialCommit()
{
    // This test verifies that a window will be placed on the right output if the active output
    // is removed before the client has a chance to commit the initial state.

    const auto outputs = workspace()->outputs();
    workspace()->setActiveOutput(outputs[1]);

    // Create an xdg-toplevel surface.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Wait for the corresponding Window object to be created on the compositor side.
    QSignalSpy windowCreatedSpy(waylandServer(), &WaylandServer::windowCreated);
    QVERIFY(windowCreatedSpy.wait());
    Window *window = windowCreatedSpy.last().at(0).value<Window *>();
    QCOMPARE(window->output(), outputs[1]);
    QCOMPARE(window->moveResizeOutput(), outputs[1]);

    // Disable the active output.
    OutputConfiguration config;
    {
        auto changeSet = config.changeSet(outputs[1]->backendOutput());
        changeSet->enabled = false;
    }
    QVERIFY(workspace()->applyOutputConfiguration(config));

    // Commit the initial state.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Map the window.
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *mapped = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QCOMPARE(mapped, window);

    // The window should have been evacuated from the second output to the first output.
    QCOMPARE(window->output(), outputs[0]);
    QCOMPARE(window->moveResizeOutput(), outputs[0]);
}

void TestXdgShellWindow::testRemoveActiveOutputBeforeMap()
{
    // This test verifies that a window will be placed on the right output if the current output
    // is removed before the client has a chance to map the surface (after committing the initial state).

    const auto outputs = workspace()->outputs();
    workspace()->setActiveOutput(outputs[1]);

    // Create an xdg-toplevel surface.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);

    // Wait for the corresponding Window object to be created on the compositor side.
    QSignalSpy windowCreatedSpy(waylandServer(), &WaylandServer::windowCreated);
    QVERIFY(windowCreatedSpy.wait());
    Window *window = windowCreatedSpy.last().at(0).value<Window *>();
    QCOMPARE(window->output(), outputs[1]);
    QCOMPARE(window->moveResizeOutput(), outputs[1]);

    // Commit the initial state.
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    // Disable the active output.
    OutputConfiguration config;
    {
        auto changeSet = config.changeSet(outputs[1]->backendOutput());
        changeSet->enabled = false;
    }
    QVERIFY(workspace()->applyOutputConfiguration(config));

    // Map the window.
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).value<QSize>(), QSize(0, 0));
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Window *mapped = Test::renderAndWaitForShown(surface.get(), QSize(800, 600), Qt::blue);
    QCOMPARE(mapped, window);

    // The window should have been evacuated from the second output to the first output.
    QCOMPARE(window->output(), outputs[0]);
    QCOMPARE(window->moveResizeOutput(), outputs[0]);
}

void TestXdgShellWindow::testRequestedGeometrySettlesOnCommit()
{
    // This test verifies that the requested geometry stays ahead of the frame geometry while a
    // configure event is outstanding, and that it is reset to the frame geometry on a commit with
    // nothing outstanding, even if the client chose a smaller size than requested.

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());

    const QPointF position = window->pos();
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), window->frameGeometry());
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QSignalSpy requestedGeometryChangedSpy(window, &Window::requestedGeometryChanged);

    // Request two sizes, one configure event each.
    const RectF first(position, QSizeF(300, 200));
    window->moveResize(first);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(300, 200));
    const quint32 firstSerial = surfaceConfigureRequestedSpy.last().at(0).value<quint32>();

    const RectF second(position, QSizeF(400, 300));
    window->moveResize(second);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(400, 300));
    const quint32 secondSerial = surfaceConfigureRequestedSpy.last().at(0).value<quint32>();
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), second);
    QCOMPARE(requestedGeometryChangedSpy.count(), 2);
    QCOMPARE(window->frameGeometry().size(), QSizeF(200, 100));

    // The client answers the first configure event; the second one is still outstanding.
    requestedGeometryChangedSpy.clear();
    shellSurface->xdgSurface()->ack_configure(firstSerial);
    Test::render(surface.get(), QSize(300, 200), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), first);
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), second);
    QVERIFY(requestedGeometryChangedSpy.isEmpty());

    // The client answers the second configure event with a smaller size than requested.
    shellSurface->xdgSurface()->ack_configure(secondSerial);
    Test::render(surface.get(), QSize(350, 250), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), RectF(position, QSizeF(350, 250)));
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), window->frameGeometry());
    QCOMPARE(requestedGeometryChangedSpy.count(), 1);
}

void TestXdgShellWindow::testRequestedGeometryAfterMappingDestroysWindow()
{
    // A native mapping handler may destroy a window synchronously. The commit handler must not
    // access it afterward, including reading the cached requested geometry without emitting.
    // A running open animation keeps a destroyed window alive until it ends, so no effect may be
    // loaded while this case runs; they are restored afterward.
    QVERIFY(effects);
    const QStringList loadedEffects = effects->loadedEffects();
    for (const QString &name : loadedEffects) {
        effects->unloadEffect(name);
    }
    const auto restoreEffects = qScopeGuard([&loadedEffects]() {
        for (const QString &name : loadedEffects) {
            effects->loadEffect(name);
        }
    });
    QVERIFY(effects->loadedEffects().isEmpty());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QPointer<Window> mappedWindow;
    bool destroyedDuringMap = false;
    QObject context;
    connect(workspace(), &Workspace::windowAdded, &context, [&](Window *window) {
        mappedWindow = window;
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        destroyedDuringMap = true;
    });

    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QTRY_VERIFY(destroyedDuringMap);
    QVERIFY(mappedWindow.isNull());
}

void TestXdgShellWindow::testRequestedGeometryReentrantMove()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(configureRequestedSpy.wait());
    shellSurface->xdgSurface()->ack_configure(configureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());

    const QPointF first = window->pos() + QPointF(10, 10);
    const QPointF second = first + QPointF(20, 20);
    int notifications = 0;
    QObject context;
    connect(window, &Window::requestedGeometryChanged, &context, [&] {
        if (++notifications == 1) {
            window->move(second);
        }
    });

    window->move(first);
    QCOMPARE(notifications, 2);
    QCOMPARE(window->frameGeometry().topLeft(), second);
    QCOMPARE(window->property("requestedGeometry").value<RectF>().topLeft(), second);
    window->move(second);
    QCOMPARE(notifications, 2);
}

void TestXdgShellWindow::testGeometrySupersession_data()
{
    QTest::addColumn<bool>("queuedResize");
    QTest::addColumn<bool>("partialMove");
    QTest::newRow("immediate-restore") << false << false;
    QTest::newRow("queued-restore") << true << false;
    QTest::newRow("unsubmitted-resize-move") << true << true;
}

void TestXdgShellWindow::testGeometrySupersession()
{
    QFETCH(bool, queuedResize);
    QFETCH(bool, partialMove);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const auto original = window->frameGeometry();
    const RectF outer(QPointF(100, 100), queuedResize ? QSizeF(400, 300) : original.size());
    const RectF moved(QPointF(150, 150), outer.size());
    QObject context;
    bool interrupted = false;
    connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
        if (!interrupted) {
            interrupted = true;
            if (partialMove) {
                window->move(moved.topLeft());
            } else {
                window->moveResize(original);
            }
        }
    });
    window->moveResize(outer);
    QVERIFY(interrupted);
    if (partialMove) {
        QCOMPARE(window->moveResizeGeometry(), moved);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), moved.size().toSize(), Qt::blue);
        QTRY_COMPARE(window->frameGeometry(), moved);
    } else {
        QCOMPARE(window->moveResizeGeometry(), original);
        QCOMPARE(window->frameGeometry(), original);
        QVERIFY(!configure.wait(50));
        QCOMPARE(configure.count(), 0);
    }
}

void TestXdgShellWindow::testGeometryNotificationHandoff_data()
{
    QTest::addColumn<bool>("queuedResize");
    QTest::newRow("immediate-inner") << false;
    QTest::newRow("queued-inner") << true;
}

void TestXdgShellWindow::testGeometryNotificationHandoff()
{
    QFETCH(bool, queuedResize);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    auto originalOutput = window->output();
    const auto originalFrame = window->frameGeometry();
    const RectF outer(QPointF(1400, 100), window->size());
    const RectF inner(QPointF(1500, 200), queuedResize ? QSizeF(400, 300) : window->size());
    QSignalSpy frames(window, &Window::frameGeometryChanged);
    QSignalSpy outputs(window, &Window::outputChanged);
    QObject context;
    bool interrupted = false;
    connect(window, &Window::bufferGeometryChanged, &context, [&]() {
        if (!interrupted) {
            interrupted = true;
            window->moveResize(inner);
        }
    });
    window->moveResize(outer);
    QVERIFY(interrupted);
    QCOMPARE(window->moveResizeGeometry(), inner);
    QCOMPARE(window->frameGeometry(), queuedResize ? outer : inner);
    QCOMPARE(frames.count(), 1);
    QCOMPARE(frames.first().first().value<RectF>(), originalFrame);
    QCOMPARE(outputs.count(), 1);
    QCOMPARE(outputs.first().first().value<LogicalOutput *>(), originalOutput);
    if (queuedResize) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), inner.size().toSize(), Qt::blue);
        QTRY_COMPARE(window->frameGeometry(), inner);
        QCOMPARE(outputs.count(), 1);
    }
}

void TestXdgShellWindow::testGeometryCallbackClose_data()
{
    QTest::addColumn<bool>("clientCommit");
    QTest::newRow("request-callback") << false;
    QTest::newRow("commit-callback") << true;
}

void TestXdgShellWindow::testGeometryCallbackClose()
{
    QFETCH(bool, clientCommit);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QPointer<Window> guard(window);
    QObject context;
    bool closed = false;
    const auto close = [&]() {
        if (!closed) {
            closed = true;
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }
    };
    if (clientCommit) {
        window->moveResize(RectF(100, 100, 400, 300));
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        connect(window, &Window::bufferGeometryChanged, &context, close);
        Test::render(surface.get(), QSize(400, 300), Qt::blue);
        QVERIFY(Test::waylandSync());
    } else {
        connect(window, &Window::frameGeometryAboutToChange, &context, close);
        window->moveResize(RectF(100, 100, 400, 300));
    }
    QVERIFY(closed);
    QTRY_VERIFY(guard.isNull());
}

void TestXdgShellWindow::testGeometrySameRectangleIntent()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const auto before = window->geometryRequestGeneration();
    QObject context;
    bool nested = false;
    connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
        if (!nested) {
            nested = true;
            window->moveResize(window->moveResizeGeometry());
        }
    });
    const RectF target(100, 100, 200, 100);
    window->moveResize(target);
    QVERIFY(nested);
    QCOMPARE(window->geometryRequestGeneration(), before + 2);
    QCOMPARE(window->frameGeometry(), target);
}

void TestXdgShellWindow::testMoveDuringQueuedResize()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    window->resize(QSizeF(400, 300));
    QVERIFY(!configure.isEmpty() || configure.wait());
    const auto serial = configure.last().at(0).value<quint32>();
    window->move(QPointF(150, 150));
    QCOMPARE(window->frameGeometry(), RectF(150, 150, 200, 100));
    QVERIFY(Test::waylandSync());
    QCOMPARE(configure.count(), 1);
    shell->xdgSurface()->ack_configure(serial);
    Test::render(surface.get(), QSize(400, 300), Qt::blue);
    QTRY_COMPARE(window->frameGeometry(), RectF(150, 150, 400, 300));
}

void TestXdgShellWindow::testNestedOutputPropertySync()
{
    auto destination = workspace()->outputs()[1];
    OutputConfiguration configuration;
    auto changes = configuration.changeSet(destination->backendOutput());
    changes->scale = 2.0;
    changes->scaleSetting = 2.0;
    changes->transform = OutputTransform::Kind::Rotate180;
    QVERIFY(workspace()->applyOutputConfiguration(configuration).has_value());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->moveResizeOutput() != destination);
    QVERIFY(window->preferredBufferTransform() != destination->transform());
    // Virtual outputs share an SDR color space; seed a different prior preference to test refresh.
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    QVERIFY(window->preferredColorDescription() != destination->blendingColor());
    QObject context;
    bool nested = false;
    connect(window, &Window::nextTargetScaleChanged, &context, [&]() {
        if (!nested) {
            nested = true;
            window->moveResize(RectF(1450, 150, 200, 100));
        }
    });
    window->moveResize(RectF(1400, 100, 200, 100));
    QVERIFY(nested);
    QCOMPARE(window->moveResizeOutput(), destination);
    QCOMPARE(window->nextTargetScale(), destination->scale());
    QCOMPARE(window->preferredBufferTransform(), destination->transform());
    QCOMPARE(window->preferredColorDescription(), destination->blendingColor());
}

void TestXdgShellWindow::testGeometryNotificationNetZero()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const auto original = window->frameGeometry();
    const auto originalOutput = window->output();
    QSignalSpy frames(window, &Window::frameGeometryChanged);
    QSignalSpy outputs(window, &Window::outputChanged);
    QObject context;
    bool nested = false;
    connect(window, &Window::bufferGeometryChanged, &context, [&]() {
        if (!nested) {
            nested = true;
            window->moveResize(original);
        }
    });
    window->moveResize(RectF(QPointF(1400, 100), original.size()));
    QVERIFY(nested);
    QCOMPARE(window->frameGeometry(), original);
    QCOMPARE(window->output(), originalOutput);
    QCOMPARE(frames.count(), 0);
    QCOMPARE(outputs.count(), 0);
}

void TestXdgShellWindow::testOutputScaleCallbackClose()
{
    auto destination = workspace()->outputs()[1];
    OutputConfiguration configuration;
    auto changes = configuration.changeSet(destination->backendOutput());
    changes->scale = 2.0;
    changes->scaleSetting = 2.0;
    QVERIFY(workspace()->applyOutputConfiguration(configuration).has_value());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QPointer<Window> guard(window);
    QObject context;
    bool closed = false;
    connect(window, &Window::nextTargetScaleChanged, &context, [&]() {
        if (!closed) {
            closed = true;
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }
    });
    window->moveResize(RectF(1400, 100, 200, 100));
    QVERIFY(closed);
    QTRY_VERIFY(guard.isNull());
}

void TestXdgShellWindow::testPrivateTransientRequest()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QVERIFY(window->canStartTransientTileRequest(tile.windowGeometry()));
    const auto original = window->moveResizeGeometry();
    window->requestTile(&tile);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), original);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    QVERIFY(!receipt.noRequestProof);
    QCOMPARE(receipt.ownershipGeneration, quint64(7));
    QCOMPARE(receipt.windowId, window->internalId());
    QCOMPARE(window->requestedTile(), &tile);
    QCOMPARE(window->moveResizeGeometry(), tile.windowGeometry());
}

void TestXdgShellWindow::testTransientRequestCancellation_data()
{
    QTest::addColumn<bool>("afterRequest");
    QTest::newRow("restore-field") << false;
    QTest::newRow("geometry-request") << true;
}

void TestXdgShellWindow::testTransientRequestCancellation()
{
    QFETCH(bool, afterRequest);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QVERIFY(window->canStartTransientTileRequest(tile.windowGeometry()));
    const auto original = window->moveResizeGeometry();
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool cancelled = false;
    const auto signal = afterRequest ? &Window::frameGeometryAboutToChange : &Window::maximizeGeometryRestoreChanged;
    connect(window, signal, &context, [&]() {
        if (!cancelled) {
            cancelled = true;
            window->requestTile(nullptr);
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(cancelled);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, afterRequest ? Window::TransientRequestProgress::Requested : Window::TransientRequestProgress::NoRequest);
    QCOMPARE(bool(receipt.noRequestProof), !afterRequest);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), original);
}

void TestXdgShellWindow::testTransientRequestUnmapped()
{
    QSignalSpy created(waylandServer(), &WaylandServer::windowCreated);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    QVERIFY(Test::waylandSync());
    QCOMPARE(created.count(), 1);
    auto window = created.first().first().value<Window *>();
    QVERIFY(window);
    QVERIFY(!window->readyForPainting());
    QVERIFY(!window->canStartTransientTileRequest(RectF(200, 200, 400, 300)));
}

void TestXdgShellWindow::testTransientRequestPendingCallback()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QVERIFY(window->canStartTransientTileRequest(tile.windowGeometry()));
    const auto original = window->moveResizeGeometry();
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool changed = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (!changed) {
            changed = true;
            window->setActive(!window->isActive());
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(changed);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), original);
}

void TestXdgShellWindow::testTransientRequestCommitReplacement()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QVERIFY(window->canStartTransientTileRequest(tile.windowGeometry()));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool committed = false;
    RectF replacement;
    connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
        if (!committed) {
            committed = true;
            Test::render(surface.get(), QSize(220, 120), Qt::blue);
            QVERIFY(Test::waylandSync());
            replacement = window->moveResizeGeometry();
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(committed);
    QVERIFY(replacement != tile.windowGeometry());
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), replacement);
}

void TestXdgShellWindow::testTransientRequestNoOpCancellation_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("forget-nonmatching") << 0;
    QTest::newRow("equal-compatibility") << 1;
    QTest::newRow("equal-quick-mode") << 2;
    QTest::newRow("equal-geometry") << 3;
    QTest::newRow("authority-destroyed") << 4;
    QTest::newRow("target-changed") << 5;
    QTest::newRow("tile-destroyed") << 6;
    QTest::newRow("manager-destroyed") << 7;
}

void TestXdgShellWindow::testTransientRequestNoOpCancellation()
{
    QFETCH(int, action);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto manager = std::make_unique<TileManager>(window->moveResizeOutput());
    auto tile = std::make_unique<RequestTestTile>(window, manager.get());
    QVERIFY(window->canStartTransientTileRequest(tile->windowGeometry()));
    const auto original = window->moveResizeGeometry();
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    auto owner = std::make_unique<QObject>();
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = owner.get();
    QObject context;
    bool cancelled = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (cancelled) {
            return;
        }
        cancelled = true;
        switch (action) {
        case 0:
            QVERIFY(window->requestedTile() != tile.get());
            window->forgetTile(tile.get());
            break;
        case 1:
            window->setTileCompatibility(nullptr);
            break;
        case 2:
            window->setQuickTileModeAtCurrentPosition(QuickTileFlag::None);
            break;
        case 3:
            window->moveResize(original);
            break;
        case 4:
            owner.reset();
            break;
        case 5:
            tile->setTargetForTest(RectF(100, 100, 400, 300));
            break;
        case 6:
            tile.reset();
            break;
        case 7:
            manager.reset();
            break;
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, tile.get(), authority));
    QVERIFY(cancelled);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), original);
}

void TestXdgShellWindow::testTransientRequestPrivateReentry()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    Window::TransientRequestReceipt nested;
    bool attempted = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (!attempted) {
            attempted = true;
            nested = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(attempted);
    QCOMPARE(nested.outcome, Window::TransientRequestOutcome::Rejected);
    QCOMPARE(nested.progress, Window::TransientRequestProgress::NoRequest);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    QCOMPARE(window->requestedTile(), &tile);
}

void TestXdgShellWindow::testTransientRequestNewTileWins()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    auto quick = workspace()->tileManager(window->moveResizeOutput())->quickTile(QuickTileFlag::Left);
    QVERIFY(quick);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool changed = false;
    connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
        if (!changed) {
            changed = true;
            quick->manage(window);
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(changed);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    QCOMPARE(window->requestedTile(), quick);
    QCOMPARE(window->moveResizeGeometry(), quick->windowGeometry());
}

void TestXdgShellWindow::testTransientRequestPendingStates_data()
{
    QTest::addColumn<int>("state");
    QTest::newRow("timer") << 0;
    QTest::newRow("sent") << 1;
    QTest::newRow("acked-without-commit") << 2;
}

void TestXdgShellWindow::testTransientRequestPendingStates()
{
    QFETCH(int, state);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QVERIFY(window->canStartTransientTileRequest(tile.windowGeometry()));
    const auto original = window->moveResizeGeometry();
    configure.clear();
    window->setActive(!window->isActive());
    if (state > 0) {
        QVERIFY(!configure.isEmpty() || configure.wait());
    }
    if (state == 2) {
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        QVERIFY(Test::waylandSync());
    }
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Rejected);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), original);
    QVERIFY(!window->canStartTransientTileRequest(tile.windowGeometry()));
}

void TestXdgShellWindow::testTransientRequestConstraints_data()
{
    QTest::addColumn<bool>("fixedWidth");
    QTest::addColumn<bool>("fits");
    QTest::newRow("fixed-width-fits") << true << true;
    QTest::newRow("fixed-width-reject") << true << false;
    QTest::newRow("fixed-height-fits") << false << true;
    QTest::newRow("fixed-height-reject") << false << false;
}

void TestXdgShellWindow::testTransientRequestConstraints()
{
    QFETCH(bool, fixedWidth);
    QFETCH(bool, fits);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto shell = Test::createXdgToplevelSurface(surface.get(), [fixedWidth](Test::XdgToplevel *toplevel) {
        toplevel->set_min_size(fixedWidth ? 200 : 0, fixedWidth ? 0 : 100);
        toplevel->set_max_size(fixedWidth ? 200 : 0, fixedWidth ? 0 : 100);
    });
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QVERIFY(window->isResizable());
    RequestTestTile tile(window);
    const RectF target(100, 100, fits && fixedWidth ? 200 : 400, fits && !fixedWidth ? 100 : 300);
    tile.setTargetForTest(target);
    QCOMPARE(window->isTransientTileTargetFeasible(tile.windowGeometry()), fits);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto original = window->moveResizeGeometry();
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(receipt.outcome, fits ? Window::TransientRequestOutcome::Applied : Window::TransientRequestOutcome::Rejected);
    QCOMPARE(receipt.progress, fits ? Window::TransientRequestProgress::Requested : Window::TransientRequestProgress::NoRequest);
    QCOMPARE(window->moveResizeGeometry(), fits ? target : original);
}

void TestXdgShellWindow::testTransientRequestFractional_data()
{
    QTest::addColumn<bool>("fits");
    QTest::newRow("snapped") << true;
    QTest::newRow("nonrepresentable") << false;
}

void TestXdgShellWindow::testTransientRequestFractional()
{
    QFETCH(bool, fits);
    Test::OutputInfo output;
    output.geometry = Rect(0, 0, 1280, 1024);
    output.scale = 1.25;
    Test::setOutputConfig(QList<Test::OutputInfo>{output});
    workspace()->setActiveOutput(QPoint(100, 100));
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->nextTargetScale(), 1.25);
    RequestTestTile tile(window);
    tile.setTargetForTest(RectF(100, 100, fits ? 200 : 201, fits ? 100 : 101));
    QCOMPARE(window->isTransientTileTargetFeasible(tile.windowGeometry()), fits);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(receipt.outcome, fits ? Window::TransientRequestOutcome::Applied : Window::TransientRequestOutcome::Rejected);
}

void TestXdgShellWindow::testTransientRequestDecorated()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    auto shell = Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly);
    auto decoration = Test::createXdgToplevelDecorationV1(shell.get());
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    configure.clear();
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    tile.setTargetForTest(RectF(100, 100, 400, 300));
    QVERIFY(window->canStartTransientTileRequest(tile.windowGeometry()));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(window->moveResizeGeometry(), tile.windowGeometry());
}

void TestXdgShellWindow::testTransientRequestRequestedRevocation_data()
{
    QTest::addColumn<int>("cause");
    QTest::newRow("owner") << 0;
    QTest::newRow("tile") << 1;
    QTest::newRow("manager") << 2;
    QTest::newRow("window") << 3;
}

void TestXdgShellWindow::testTransientRequestRequestedRevocation()
{
    QFETCH(int, cause);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto manager = std::make_unique<TileManager>(window->moveResizeOutput());
    auto tile = std::make_unique<RequestTestTile>(window, manager.get());
    auto owner = std::make_unique<QObject>();
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = owner.get();
    const auto target = tile->windowGeometry();
    QPointer<Window> guard(window);
    QObject context;
    bool revoked = false;
    connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
        if (revoked) {
            return;
        }
        revoked = true;
        if (cause == 0) {
            owner.reset();
        } else if (cause == 1) {
            tile.reset();
        } else if (cause == 2) {
            manager.reset();
        } else {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, tile.get(), authority));
    QVERIFY(revoked);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    if (guard) {
        QVERIFY(!guard->requestedTile());
        QCOMPARE(guard->moveResizeGeometry(), target);
    }
    if (cause == 3) {
        QTRY_VERIFY(guard.isNull());
    }
}

void TestXdgShellWindow::testTransientNoRequestProofReplay()
{
    static_assert(!std::is_copy_constructible_v<TransientNoRequestProof>);
    static_assert(!std::is_move_constructible_v<TransientNoRequestProof>);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto requested = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority, 7));
    QCOMPARE(requested.progress, Window::TransientRequestProgress::Requested);
    QVERIFY(!requested.noRequestProof);
    const auto repeated = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority, 7));
    QCOMPARE(repeated.outcome, Window::TransientRequestOutcome::Rejected);
    QVERIFY(!repeated.noRequestProof);
    const auto rejected = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority, 8));
    QCOMPARE(rejected.outcome, Window::TransientRequestOutcome::Rejected);
    QCOMPARE(rejected.progress, Window::TransientRequestProgress::NoRequest);
    QVERIFY(rejected.noRequestProof);
}

void TestXdgShellWindow::testNoRequestFinalization_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("owned") << 0;
    QTest::newRow("manual-revocation") << 1;
    QTest::newRow("competing-manual-intent") << 2;
}

void TestXdgShellWindow::testNoRequestFinalization()
{
    QFETCH(int, mode);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const QRectF manual(80, 90, 240, 140);
    const QRectF competing(100, 110, 260, 160);
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    const auto snapshot = writer.snapshot();
    QCOMPARE(writer.reserve(token, {id, QStringLiteral("7"), original, snapshot.providerRevision, snapshot.mapRevision, QStringLiteral("app")}), Result::Accepted);
    QCOMPARE(writer.activateReservation(token, id, QStringLiteral("7")), Result::Accepted);
    RequestTestTile tile(window);
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool cancelled = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (cancelled) {
            return;
        }
        cancelled = true;
        if (mode == 0) {
            window->requestTile(nullptr);
        } else {
            // Revoke before observing the manual frame, or Owned would correctly suppress it.
            QCOMPARE(writer.beginRestore(id, QStringLiteral("7")), Result::Accepted);
            window->moveResize(manual);
            QCOMPARE(writer.record(token, 2, {id, manual, 20, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
        }
    });
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(cancelled);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
    QVERIFY(receipt.noRequestProof);
    if (mode == 2) {
        const auto sibling = QUuid::createUuid();
        QVERIFY(writer.trackWindow(sibling, QStringLiteral("app")));
        QCOMPARE(writer.record(token, 3, {sibling, competing, 30, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
        QCOMPARE(writer.record(token, 4, {id, manual, 25, TransientPlacementWriter::Origin::Manual}), Result::OutdatedEvent);
    }
    const auto alias = receipt.noRequestProof;
    QCOMPARE(writer.finalizeNoRequest(receipt.noRequestProof), Result::Accepted);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.finalizeNoRequest(alias), Result::InvalidRecord);
    const QRectF expected = mode == 0 ? original : mode == 1 ? manual
                                                             : competing;
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), expected);
    if (mode != 2) {
        QCOMPARE(writer.record(token, 5, {id, expected, 40, TransientPlacementWriter::Origin::Close}), Result::Accepted);
    }
    QVERIFY(writer.flush());
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), expected);
}

void TestXdgShellWindow::testNoRequestFinalizationAfterRequested_data()
{
    QTest::addColumn<bool>("sameGeometry");
    QTest::newRow("changed-rectangle") << false;
    QTest::newRow("equal-rectangle") << true;
}

void TestXdgShellWindow::testNoRequestFinalizationAfterRequested()
{
    QFETCH(bool, sameGeometry);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    const auto snapshot = writer.snapshot();
    QCOMPARE(writer.reserve(token, {id, QStringLiteral("7"), original, snapshot.providerRevision, snapshot.mapRevision, QStringLiteral("app")}), Result::Accepted);
    QCOMPARE(writer.activateReservation(token, id, QStringLiteral("7")), Result::Accepted);
    RequestTestTile tile(window);
    if (sameGeometry) {
        tile.setTargetForTest(window->frameGeometry());
    }
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto requested = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(requested.progress, Window::TransientRequestProgress::Requested);
    QVERIFY(!requested.noRequestProof);
    const auto rejected = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(rejected.outcome, Window::TransientRequestOutcome::Rejected);
    QVERIFY(!rejected.noRequestProof);
    QCOMPARE(writer.finalizeNoRequest(rejected.noRequestProof), Result::InvalidRecord);
    QVERIFY(writer.ownsReservation(id, QStringLiteral("7")));
}

void TestXdgShellWindow::testNoRequestFinalizationDuringClose_data()
{
    QTest::addColumn<bool>("connectFirst");
    QTest::newRow("earlier-close-observer") << true;
    QTest::newRow("later-close-observer") << false;
}

void TestXdgShellWindow::testNoRequestFinalizationDuringClose()
{
    QFETCH(bool, connectFirst);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    const auto snapshot = writer.snapshot();
    QCOMPARE(writer.reserve(token, {id, QStringLiteral("7"), original, snapshot.providerRevision, snapshot.mapRevision, QStringLiteral("app")}), Result::Accepted);
    QCOMPARE(writer.activateReservation(token, id, QStringLiteral("7")), Result::Accepted);
    std::shared_ptr<const TransientNoRequestProof> proof;
    Result closeResult = Result::Accepted;
    bool protectedDuringClose = false;
    const auto closeObserver = [&]() {
        closeResult = writer.finalizeNoRequest(proof);
        protectedDuringClose = writer.isReserved(id);
    };
    if (connectFirst) {
        connect(window, &Window::closed, &writer, closeObserver);
    }
    RequestTestTile tile(window);
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool cancelled = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (!cancelled) {
            cancelled = true;
            window->requestTile(nullptr);
        }
    });
    proof = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority)).noRequestProof;
    QVERIFY(proof);
    if (!connectFirst) {
        connect(window, &Window::closed, &writer, closeObserver);
    }
    connect(window, &QObject::destroyed, &writer, [&]() {
        writer.forgetWindow(id);
    });
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QCOMPARE(closeResult, Result::Busy);
    QVERIFY(protectedDuringClose);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
}

void TestXdgShellWindow::testNoRequestFinalizationValidity_data()
{
    QTest::addColumn<int>("condition");
    QTest::newRow("prepared") << 0;
    QTest::newRow("provider-retired") << 1;
    QTest::newRow("wrong-generation") << 2;
    QTest::newRow("wrong-window") << 3;
    QTest::newRow("newer-native-attempt") << 4;
    QTest::newRow("consumed-in-other-writer") << 5;
    QTest::newRow("tracked-wrong-owner") << 6;
}

void TestXdgShellWindow::testNoRequestFinalizationValidity()
{
    QFETCH(int, condition);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = condition == 3 || condition == 6 ? QUuid::createUuid() : window->internalId();
    const auto generation = condition == 2 ? QStringLiteral("8") : QStringLiteral("7");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    const auto snapshot = writer.snapshot();
    QCOMPARE(writer.reserve(token, {id, generation, original, snapshot.providerRevision, snapshot.mapRevision, QStringLiteral("app")}), Result::Accepted);
    if (condition != 0) {
        QCOMPARE(writer.activateReservation(token, id, generation), Result::Accepted);
    }
    if (condition == 6) {
        QVERIFY(writer.trackWindow(window->internalId(), QStringLiteral("app")));
    }
    RequestTestTile tile(window);
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    bool cancelled = false;
    const auto cancellation = connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (!cancelled) {
            cancelled = true;
            window->requestTile(nullptr);
        }
    });
    const auto proof = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority)).noRequestProof;
    QVERIFY(proof);
    disconnect(cancellation);
    if (condition == 1) {
        provider.reset();
    } else if (condition == 4) {
        const auto newer = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority, 8));
        QCOMPARE(newer.progress, Window::TransientRequestProgress::Requested);
    }

    auto useCorrectBinding = [&](const QString &file) {
        TransientPlacementWriter correct(directory.filePath(file));
        QObject correctProvider;
        const auto correctToken = correct.registerProvider(&correctProvider);
        QVERIFY(correct.trackWindow(window->internalId(), QStringLiteral("app")));
        QCOMPARE(correct.record(correctToken, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        const auto state = correct.snapshot();
        QCOMPARE(correct.reserve(correctToken, {window->internalId(), QStringLiteral("7"), original, state.providerRevision, state.mapRevision, QStringLiteral("app")}), Result::Accepted);
        QCOMPARE(correct.activateReservation(correctToken, window->internalId(), QStringLiteral("7")), Result::Accepted);
        QCOMPARE(correct.finalizeNoRequest(proof), Result::Accepted);
    };
    if (condition == 5) {
        useCorrectBinding(QStringLiteral("first-consumer"));
    }
    const auto result = writer.finalizeNoRequest(proof);
    if (condition == 1) {
        QCOMPARE(result, Result::Accepted);
        QVERIFY(!writer.isReserved(id));
    } else {
        const auto expected = condition == 0 ? Result::Busy
            : condition == 3                 ? Result::UnknownWindow
            : condition == 5                 ? Result::InvalidRecord
                                             : Result::StaleGeneration;
        QCOMPARE(result, expected);
        QVERIFY(writer.isReserved(id));
    }
    if (condition == 0) {
        QCOMPARE(writer.activateReservation(token, id, generation), Result::Accepted);
        QCOMPARE(writer.finalizeNoRequest(proof), Result::Accepted);
    } else if (condition == 2 || condition == 3 || condition == 6) {
        // A failed check must not consume the proof before a correctly bound consumer uses it.
        useCorrectBinding(QStringLiteral("correct-consumer"));
    }
}

void TestXdgShellWindow::testTransientOwnershipPreparation_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("unchanged") << 0;
    QTest::newRow("retired-provider") << 1;
    QTest::newRow("manual-cancellation") << 2;
    QTest::newRow("native-snapshot-change") << 3;
}

void TestXdgShellWindow::testTransientOwnershipPreparation()
{
    QFETCH(int, change);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    const auto first = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                         std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(first);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Prepared);
    QVERIFY(writer.isReserved(id));
    QVERIFY(!writer.ownsReservation(id, QString::number(first)));
    QCOMPARE(QRectF(window->frameGeometry()), original);
    QCOMPARE(window->m_transientOwnership->nativeSequence, quint64(0));
    QVERIFY(window->revokeTransientOwnership(first));
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));

    const auto second = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                          std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(second > first);
    if (change == 1) {
        provider.reset(); // The queued providerRetired signal has not run yet.
    } else if (change == 2) {
        window->move(QPointF(90, 80));
    } else if (change == 3) {
        // A native snapshot change, not public manual intent: Prepared must remain present.
        window->setMoveResizeGeometry(RectF(90, 80, original.width(), original.height()));
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Prepared);
    }
    QCOMPARE(window->activateTransientOwnership(second), change == 0);
    if (change == 0) {
        QVERIFY(writer.ownsReservation(id, QString::number(second)));
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
        QVERIFY(window->m_transientOwnership->nativeSequence);
    }
    if (window->m_transientOwnership) {
        QVERIFY(window->revokeTransientOwnership(second));
    }
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
}

void TestXdgShellWindow::testTransientOwnershipBeforeRequest_data()
{
    QTest::addColumn<bool>("retireProvider");
    QTest::newRow("explicit-revocation") << false;
    QTest::newRow("provider-retired-before-call") << true;
}

void TestXdgShellWindow::testTransientOwnershipBeforeRequest()
{
    QFETCH(bool, retireProvider);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    const auto generation = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                              std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(generation);
    QVERIFY(window->activateTransientOwnership(generation));
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    if (retireProvider) {
        provider.reset();
        const auto receipt = window->applyTransientOwnership(generation);
        QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
        QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Rejected);
    } else {
        QVERIFY(window->revokeTransientOwnership(generation));
    }
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(geometry.count(), 0);
    QCOMPARE(QRectF(window->moveResizeGeometry()), original);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
}

void TestXdgShellWindow::testTransientOwnershipRevocation_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("writer-revoked-before-call") << 0;
    QTest::newRow("writer-revoked-in-callback") << 1;
    QTest::newRow("returned-proof-already-consumed") << 2;
}

void TestXdgShellWindow::testTransientOwnershipRevocation()
{
    QFETCH(int, mode);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    const auto generation = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                              std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(generation);
    QVERIFY(window->activateTransientOwnership(generation));
    if (mode == 0) {
        QCOMPARE(writer.beginRestore(id, QString::number(generation)), Result::Accepted);
    } else {
        connect(window, &Window::maximizeGeometryRestoreChanged, &writer, [&, generation]() {
            if (mode == 1) {
                QCOMPARE(writer.beginRestore(id, QString::number(generation)), Result::Accepted);
            } else {
                window->requestTile(nullptr);
            }
        });
    }
    const auto receipt = window->applyTransientOwnership(generation);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(QRectF(window->moveResizeGeometry()), original);
    if (mode == 2) {
        QVERIFY(receipt.noRequestProof);
        TransientPlacementWriter another(directory.filePath(QStringLiteral("another")));
        QObject anotherProvider;
        const auto anotherToken = another.registerProvider(&anotherProvider);
        QVERIFY(another.trackWindow(id, QStringLiteral("app")));
        QCOMPARE(another.record(anotherToken, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        const auto snapshot = another.snapshot();
        QCOMPARE(another.reserve(anotherToken, {id, QString::number(generation), original, snapshot.providerRevision, snapshot.mapRevision, QStringLiteral("app")}), Result::Accepted);
        QCOMPARE(another.activateReservation(anotherToken, id, QString::number(generation)), Result::Accepted);
        QCOMPARE(another.finalizeNoRequest(receipt.noRequestProof), Result::InvalidRecord);
        QVERIFY(another.isReserved(id));
    }
}

void TestXdgShellWindow::testTransientOwnershipNativeProgress_data()
{
    QTest::addColumn<bool>("afterRequest");
    QTest::addColumn<bool>("retireProvider");
    QTest::newRow("manual-before-request") << false << false;
    QTest::newRow("provider-before-request") << false << true;
    QTest::newRow("manual-after-request") << true << false;
    QTest::newRow("provider-after-request") << true << true;
}

void TestXdgShellWindow::testTransientOwnershipNativeProgress()
{
    QFETCH(bool, afterRequest);
    QFETCH(bool, retireProvider);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const QRectF manual(70, 80, 240, 140);
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    const auto generation = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                              std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(generation);
    QVERIFY(window->activateTransientOwnership(generation));
    bool observed = false;
    const auto cancel = [&]() {
        if (observed) {
            return;
        }
        observed = true;
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->progress, afterRequest ? Window::TransientRequestProgress::Requested : Window::TransientRequestProgress::NoRequest);
        QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
        QCOMPARE(window->m_transientOwnership->target, tile.windowGeometry());
        QCOMPARE(window->m_transientOwnership->tile.data(), &tile);
        QCOMPARE(window->m_transientOwnership->manager.data(), tile.manager());
        QCOMPARE(window->m_transientOwnership->output.data(), tile.manager()->output());
        QCOMPARE(window->m_transientOwnership->desktop.data(), tile.desktop());
        if (retireProvider) {
            provider.reset();
        } else {
            window->moveResize(manual);
            QVERIFY(window->m_transientOwnership);
            QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
            QCOMPARE(writer.record(token, 2, {id, manual, 20, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
        }
    };
    if (afterRequest) {
        connect(window, &Window::requestedGeometryChanged, &writer, cancel);
    } else {
        connect(window, &Window::maximizeGeometryRestoreChanged, &writer, cancel);
    }
    const auto receipt = window->applyTransientOwnership(generation);
    QVERIFY(observed);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, afterRequest ? Window::TransientRequestProgress::Requested : Window::TransientRequestProgress::NoRequest);
    if (afterRequest) {
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
        QCOMPARE(window->m_transientOwnership->target, tile.windowGeometry());
        QCOMPARE(window->m_transientOwnership->tile.data(), &tile);
        QVERIFY(writer.isReserved(id));
        QVERIFY(!window->revokeTransientOwnership(generation));
        QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
        QPointer<Window> guard(window);
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QTRY_VERIFY(!guard);
    } else {
        QVERIFY(!window->m_transientOwnership);
    }
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), retireProvider ? original : manual);
}

void TestXdgShellWindow::testTransientOwnershipClose_data()
{
    QTest::addColumn<bool>("activate");
    QTest::newRow("prepared") << false;
    QTest::newRow("activated-no-request") << true;
}

void TestXdgShellWindow::testTransientOwnershipClose()
{
    QFETCH(bool, activate);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    const auto generation = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                              std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(generation);
    if (activate) {
        QVERIFY(window->activateTransientOwnership(generation));
    }
    bool protectedDuringClose = false;
    connect(window, &Window::closed, &writer, [&]() {
        QVERIFY(!window->revokeTransientOwnership(generation));
        protectedDuringClose = writer.isReserved(id);
        QVERIFY(window->m_transientOwnership);
    });
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(protectedDuringClose);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
}

void TestXdgShellWindow::testTransientOwnershipWriterLoss()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto writer = std::make_unique<TransientPlacementWriter>(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer->registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer->trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer->record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer->snapshot();
    const auto generation = window->prepareTransientOwnership(writer.get(), token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                              std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(generation);
    QVERIFY(window->activateTransientOwnership(generation));
    writer.reset();
    QCOMPARE(window->applyTransientOwnership(generation).outcome, Window::TransientRequestOutcome::Rejected);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QVERIFY(!window->revokeTransientOwnership(generation));
    TransientPlacementWriter replacement(directory.filePath(QStringLiteral("replacement")));
    const auto replacementToken = replacement.registerProvider(&provider);
    QVERIFY(replacement.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(replacement.record(replacementToken, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    const auto replacementState = replacement.snapshot();
    QCOMPARE(window->prepareTransientOwnership(&replacement, replacementToken, QStringLiteral("app"), replacementState.providerRevision, replacementState.mapRevision,
                                               std::make_unique<RequestForTest>(window, &tile, authority, 0)),
             quint64(0));
    QVERIFY(window->m_transientOwnership);
}

void TestXdgShellWindow::testTransientOwnershipExhaustion()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    window->m_transientOwnershipGeneration = std::numeric_limits<quint64>::max();
    QCOMPARE(window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                               std::make_unique<RequestForTest>(window, &tile, authority, 0)),
             quint64(0));
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(QRectF(window->frameGeometry()), original);
}

void TestXdgShellWindow::testTransientOwnershipApplied()
{
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const RectF target(original.x() + 40, original.y() + 30, original.width(), original.height());
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    tile.setTargetForTest(target);
    QCOMPARE(tile.windowGeometry(), target);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto state = writer.snapshot();
    const auto generation = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), state.providerRevision, state.mapRevision,
                                                              std::make_unique<RequestForTest>(window, &tile, authority, 0));
    QVERIFY(generation);
    QVERIFY(window->activateTransientOwnership(generation));
    bool observed = false;
    connect(window, &Window::frameGeometryChanged, &writer, [&]() {
        observed = true;
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->progress, Window::TransientRequestProgress::Requested);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    });
    const auto receipt = window->applyTransientOwnership(generation);
    QVERIFY(observed);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    QVERIFY(!receipt.noRequestProof);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QCOMPARE(window->m_transientOwnership->target, target);
    QCOMPARE(window->m_transientOwnership->tile.data(), &tile);
    QCOMPARE(window->m_transientOwnership->manager.data(), tile.manager());
    QVERIFY(!window->m_transientOwnership->request);
    QVERIFY(writer.ownsReservation(id, QString::number(generation)));
    QCOMPARE(window->frameGeometry(), target);
    const auto sequence = window->m_transientRequestSequence;
    QCOMPARE(window->applyTransientOwnership(generation).outcome, Window::TransientRequestOutcome::Rejected);
    QCOMPARE(window->m_transientRequestSequence, sequence);
    QVERIFY(writer.ownsReservation(id, QString::number(generation)));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testTransientConfigureProvenance_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("owned") << 0;
    QTest::newRow("revoked-live-tile") << 1;
    QTest::newRow("deleted-after-configure") << 2;
    QTest::newRow("deleted-before-configure") << 3;
}

void TestXdgShellWindow::testTransientConfigureProvenance()
{
    QFETCH(int, mode);
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    auto tile = std::make_unique<RequestTestTile>(window);
    const auto output = tile->manager()->output()->geometryF();
    tile->setTargetForTest(RectF(output.x(), output.y(), output.width() / 2, output.height()));
    QVERIFY(tile->anchors() != Qt::Edges());
    const auto target = tile->windowGeometry();
    QPointer<Tile> tileGuard(tile.get());
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto snapshot = writer.snapshot();
    const auto generation = window->prepareTransientOwnership(&writer, token, QStringLiteral("app"), snapshot.providerRevision, snapshot.mapRevision,
                                                              std::make_unique<RequestForTest>(window, tile.get(), authority, 0));
    QVERIFY(generation);
    QVERIFY(window->activateTransientOwnership(generation));
    const auto receipt = window->applyTransientOwnership(generation);
    QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::Requested);
    Tile ordinary(workspace()->tileManager(window->moveResizeOutput()));
    if (mode == 3) {
        tile->abandonWithoutForgetForTest();
        tile.reset();
        QVERIFY(!tileGuard);
        QVERIFY(!window->requestedTile());
    }
    QVERIFY(!configure.isEmpty() || configure.wait());
    const auto oldSerial = configure.last().at(0).value<quint32>();
    if (mode == 0) {
        QCOMPARE(window->requestedTile(), tile.get());
        QVERIFY(writer.ownsReservation(id, QString::number(generation)));
        window->commitTileFromConfigure(tile.get(), generation + 1);
        QVERIFY(!window->tile());
    }
    if (mode != 0) {
        QVERIFY(!window->revokeTransientOwnership(generation));
        if (mode == 2) {
            tile.reset();
            QVERIFY(!tileGuard);
        }
        // A native association seam, not a claim that a newer client configure committed first.
        window->commitTile(&ordinary);
        QCOMPARE(window->tile(), &ordinary);
    }
    shell->xdgSurface()->ack_configure(oldSerial);
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    if (mode == 0) {
        QCOMPARE(window->tile(), tile.get());
        QVERIFY(writer.ownsReservation(id, QString::number(generation)));
        QVERIFY(!window->revokeTransientOwnership(generation));
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
    } else {
        QCOMPARE(window->tile(), &ordinary);
        QVERIFY(writer.isReserved(id));
    }
    // A later genuine ordinary untile configure must still commit its null association.
    if (configure.last().at(0).value<quint32>() == oldSerial) {
        QVERIFY(configure.wait());
    }
    QVERIFY(configure.last().at(0).value<quint32>() != oldSerial);
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::green);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->tile());
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testTransientAssociationOrigin()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    auto tile = std::make_unique<RequestTestTile>(window);
    const auto target = tile->windowGeometry();
    window->commitTile(tile.get());
    QVERIFY(!window->tile()); // Untagged callers cannot commit a transient association.
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QCOMPARE(window->requestTransientTile(std::make_unique<RequestForTest>(window, tile.get(), authority, 7)).outcome,
             Window::TransientRequestOutcome::Applied);
    QCOMPARE(window->requestedTransientTileGeneration(), quint64(7));
    QCOMPARE(window->requestTransientTile(std::make_unique<RequestForTest>(window, tile.get(), authority, 8)).outcome,
             Window::TransientRequestOutcome::Rejected);
    QCOMPARE(window->m_lastTransientOwnershipGeneration, quint64(8));
    QCOMPARE(window->requestedTransientTileGeneration(), quint64(7));
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QVERIFY(tile);
    QVERIFY(!window->tile()); // A raw receipt with a live tile is not committed ownership.
    tile->abandonWithoutForgetForTest();
    tile.reset();
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->requestedTransientTileGeneration(), quint64(7));
    QVERIFY(!window->tile());
    window->requestTile(nullptr);
    QCOMPARE(window->requestedTransientTileGeneration(), quint64(0));
}

void TestXdgShellWindow::testControllerWindowAdmission()
{
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const auto key = QStringLiteral("org.example.admission");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto treeGeneration = controller.generation();
    QPointer<Tile> leaf(controller.tile());
    const auto target = leaf->windowGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(treeGeneration, window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(leaf->windows().contains(window));
    QCOMPARE(window->requestedTile(), leaf.data());
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    const auto ownershipGeneration = window->m_transientOwnership->generation;
    QVERIFY(writer.ownsReservation(id, QString::number(ownershipGeneration)));
    QCOMPARE(controller.admit(treeGeneration, window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Busy);
    QCOMPARE(controller.setGeometry(treeGeneration, RectF(0.5, 0, 0.5, 1)), Result::Busy);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), leaf.data());
    QCOMPARE(controller.clearTree(treeGeneration), Result::Accepted);
    QVERIFY(!leaf);
    QVERIFY(!window->requestedTile());
    QVERIFY(!window->tile());
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QVERIFY(writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), original);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testControllerAdmissionCallback_data()
{
    QTest::addColumn<bool>("destroyController");
    QTest::newRow("clear-before-request") << false;
    QTest::newRow("destroy-before-request") << true;
}

void TestXdgShellWindow::testControllerAdmissionCallback()
{
    QFETCH(bool, destroyController);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const auto key = QStringLiteral("org.example.admission");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto controller = std::make_unique<TransientTileController>();
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto generation = controller->generation();
    QPointer<Tile> leaf(controller->tile());
    int laterNotifications = 0;
    connect(leaf, &Tile::windowsChanged, &writer, [&]() {
        ++laterNotifications;
    });
    connect(leaf, &Tile::windowAdded, &writer, [&]() {
        if (destroyController) {
            controller.reset();
        } else {
            QCOMPARE(controller->clearTree(generation), Result::Accepted);
        }
    });
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    const auto state = writer.snapshot();
    const auto result = controller->admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision);
    QCOMPARE(result, Result::Superseded);
    QVERIFY(!leaf);
    QCOMPARE(laterNotifications, 0);
    QCOMPARE(geometry.count(), 0);
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QVERIFY(!window->requestedTile());
    QCOMPARE(QRectF(window->frameGeometry()), original);
}

void TestXdgShellWindow::testControllerDetachNotification_data()
{
    QTest::addColumn<bool>("replaceAssociation");
    QTest::newRow("newer-association") << true;
    QTest::newRow("geometry-only") << false;
}

void TestXdgShellWindow::testControllerDetachNotification()
{
    QFETCH(bool, replaceAssociation);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.admission");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto generation = controller.generation();
    const auto target = controller.tile()->windowGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), controller.tile());
    Tile ordinary(manager);
    QSignalSpy changed(window, &Window::tileChanged);
    connect(controller.rootTile(), &QObject::destroyed, &writer, [&]() {
        if (replaceAssociation) {
            window->commitTile(&ordinary);
        } else {
            window->move(window->pos() + QPointF(25, 30));
        }
    });
    QCOMPARE(controller.clearTree(generation), Result::Accepted);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.first().first().value<Tile *>(), replaceAssociation ? &ordinary : nullptr);
    QCOMPARE(window->tile(), replaceAssociation ? &ordinary : nullptr);
    QVERIFY(window->m_transientOwnership);
    QVERIFY(!window->m_transientOwnership->controller);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QVERIFY(writer.isReserved(id));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testControllerProviderChange_data()
{
    QTest::addColumn<bool>("retire");
    QTest::newRow("retired") << true;
    QTest::newRow("revision-changed") << false;
}

void TestXdgShellWindow::testControllerProviderChange()
{
    QFETCH(bool, retire);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const auto other = QUuid::createUuid();
    const auto key = QStringLiteral("org.example.admission");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QVERIFY(writer.trackWindow(other, QStringLiteral("other")));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    QStringList notifications;
    connect(controller.tile(), &Tile::windowAdded, &writer, [&]() {
        notifications.append(QStringLiteral("added"));
        if (retire) {
            provider.reset();
        } else {
            QCOMPARE(writer.record(token, 2, {other, QRectF(10, 10, 180, 90), 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
        }
    });
    connect(controller.tile(), &Tile::windowRemoved, &writer, [&]() {
        notifications.append(QStringLiteral("removed"));
    });
    connect(controller.tile(), &Tile::windowsChanged, &writer, [&]() {
        notifications.append(QStringLiteral("changed"));
    });
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Superseded);
    const QStringList expected{QStringLiteral("added"), QStringLiteral("removed"), QStringLiteral("changed")};
    QCOMPARE(notifications, expected);
    QVERIFY(controller.tile()->windows().isEmpty());
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(QRectF(window->moveResizeGeometry()), original);
}

void TestXdgShellWindow::testControllerPlacementKey_data()
{
    QTest::addColumn<QString>("desktopFile");
    QTest::addColumn<QString>("key");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("resource-fallback") << QString() << QStringLiteral("org.example.admission") << true;
    QTest::newRow("empty-desktop-basename") << QStringLiteral(".desktop") << QStringLiteral("org.example.admission") << false;
    QTest::newRow("empty-path-basename") << QStringLiteral("apps/.desktop") << QStringLiteral("org.example.admission") << false;
    QTest::newRow("basename-and-case") << QStringLiteral("apps/Org.Example.Test.desktop") << QStringLiteral("org.example.test") << true;
    QTest::newRow("case-sensitive-suffix") << QStringLiteral("apps/Org.Example.Test.DESKTOP") << QStringLiteral("org.example.test.desktop") << true;
    QTest::newRow("trailing-slash") << QStringLiteral("apps/") << QStringLiteral("org.example.admission") << false;
}

void TestXdgShellWindow::testControllerPlacementKey()
{
    QFETCH(QString, desktopFile);
    QFETCH(QString, key);
    QFETCH(bool, accepted);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    window->setDesktopFileName(desktopFile);
    QCOMPARE(window->desktopFileName(), desktopFile);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision),
             accepted ? Result::Accepted : Result::Ineligible);
    if (accepted) {
        QCOMPARE(controller.clearTree(controller.generation()), Result::Accepted);
    } else {
        QVERIFY(!window->m_transientOwnership);
        QVERIFY(!writer.isReserved(id));
        QCOMPARE(QRectF(window->moveResizeGeometry()), original);
    }
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testControllerOwnedDestruction_data()
{
    QTest::addColumn<int>("stage");
    QTest::addColumn<int>("cause");
    const QStringList stages{QStringLiteral("before-request"), QStringLiteral("during-request"), QStringLiteral("committed")};
    const QStringList causes{QStringLiteral("leaf"), QStringLiteral("root"), QStringLiteral("controller"), QStringLiteral("window")};
    for (int stage = 0; stage < stages.size(); ++stage) {
        for (int cause = 0; cause < causes.size(); ++cause) {
            const auto name = (stages.at(stage) + QLatin1Char('-') + causes.at(cause)).toLatin1();
            QTest::newRow(name.constData()) << stage << cause;
        }
    }
}

void TestXdgShellWindow::testControllerOwnedDestruction()
{
    QFETCH(int, stage);
    QFETCH(int, cause);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.admission");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto controller = std::make_unique<TransientTileController>();
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto generation = controller->generation();
    QPointer<Tile> root(controller->rootTile());
    QPointer<Tile> leaf(controller->tile());
    QPointer<Window> guard(window);
    const auto target = leaf->windowGeometry();
    int laterNotifications = 0;
    int removals = 0;
    QStringList membershipEvents;
    connect(leaf, &Tile::windowsChanged, &writer, [&]() {
        ++laterNotifications;
        membershipEvents.append(QStringLiteral("changed"));
    });
    connect(leaf, &Tile::windowRemoved, &writer, [&]() {
        ++removals;
        membershipEvents.append(QStringLiteral("removed"));
    });
    bool destroyed = false;
    const auto destroy = [&]() {
        if (destroyed) {
            return;
        }
        destroyed = true;
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->progress, stage == 0 ? Window::TransientRequestProgress::NoRequest : Window::TransientRequestProgress::Requested);
        if (cause == 0) {
            delete leaf.data();
        } else if (cause == 1) {
            delete root.data();
        } else if (cause == 2) {
            controller.reset();
        } else {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }
    };
    if (stage == 0) {
        connect(leaf, &Tile::windowAdded, &writer, destroy);
    } else if (stage == 1) {
        connect(window, &Window::frameGeometryAboutToChange, &writer, destroy);
    }
    const auto state = writer.snapshot();
    const auto result = controller->admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision);
    if (stage == 2) {
        QCOMPARE(result, Result::Accepted);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), target.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf.data());
        destroy();
    } else {
        QCOMPARE(result, Result::Superseded);
    }
    QVERIFY(destroyed);
    QCOMPARE(laterNotifications, (stage == 0 ? 0 : 1) + (cause == 3 ? 1 : 0));
    QCOMPARE(removals, cause == 3 ? 1 : 0);
    if (cause == 3) {
        const QStringList expected{QStringLiteral("removed"), QStringLiteral("changed")};
        QCOMPARE(membershipEvents.mid(membershipEvents.size() - 2), expected);
        QTRY_VERIFY(!guard);
        QVERIFY(leaf->windows().isEmpty());
        QVERIFY(!writer.isReserved(id));
    } else {
        QTRY_VERIFY(!root);
        QVERIFY(!leaf);
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
        if (stage == 0) {
            QVERIFY(!window->m_transientOwnership);
            QVERIFY(!writer.isReserved(id));
        } else {
            QVERIFY(window->m_transientOwnership);
            QVERIFY(!window->m_transientOwnership->controller);
            QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
            QVERIFY(writer.isReserved(id));
        }
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QTRY_VERIFY(!guard);
    }
    QCOMPARE(writer.snapshot().entries.value(key), original);
}

void TestXdgShellWindow::testControllerReleasePriority_data()
{
    QTest::addColumn<bool>("keyConflict");
    QTest::newRow("manual-move") << false;
    QTest::newRow("same-key-window") << true;
}

void TestXdgShellWindow::testControllerReleasePriority()
{
    QFETCH(bool, keyConflict);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.admission");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto target = controller.tile()->windowGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QRectF manual(40, 50, 260, 160);
    QUuid source = id;
    if (keyConflict) {
        source = QUuid::createUuid();
        QVERIFY(writer.trackWindow(source, key));
    } else {
        window->move(QPointF(40, 50));
        manual = window->moveResizeGeometry();
    }
    QVERIFY(controller.tile()->windows().isEmpty());
    QVERIFY(!window->requestedTile());
    QVERIFY(!window->tile());
    QVERIFY(window->m_transientOwnership);
    QVERIFY(!window->m_transientOwnership->controller);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QCOMPARE(writer.record(token, 2, {source, manual, 20, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Deferred);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), manual);
}

void TestXdgShellWindow::testAssociationCounterSaturation()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    Tile ordinary(workspace()->tileManager(window->moveResizeOutput()));
    ordinary.setQuickTileMode(QuickTileFlag::Custom);
    QSignalSpy tileChanged(window, &Window::tileChanged);
    QSignalSpy modeChanged(window, &Window::quickTileModeChanged);
    window->m_committedTileChange = std::numeric_limits<quint64>::max() - 1;
    window->commitTile(&ordinary);
    QCOMPARE(window->tile(), &ordinary);
    QCOMPARE(window->m_committedTileChange, std::numeric_limits<quint64>::max());
    QCOMPARE(tileChanged.count(), 1);
    QCOMPARE(modeChanged.count(), 0);
}

void TestXdgShellWindow::testControllerForeignMembership()
{
    using Result = TransientTileController::Result;
    auto desktops = VirtualDesktopManager::self();
    const auto oldCount = desktops->count();
    const auto restore = qScopeGuard([desktops, oldCount]() {
        desktops->setCount(oldCount);
    });
    desktops->setCount(std::max(oldCount, uint(2)));
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto current = desktops->currentDesktop(window->moveResizeOutput());
    auto other = desktops->desktops().first() == current ? desktops->desktops().last() : desktops->desktops().first();
    window->setOnAllDesktops(true);
    auto foreign = manager->rootTile(other)->pick(window->moveResizeGeometry().center());
    QVERIFY(foreign);
    QVERIFY(!foreign->isActive());
    QVERIFY(foreign->manage(window));
    window->setDesktops({current});
    QVERIFY(foreign->windows().contains(window));
    QVERIFY(!window->requestedTile());
    QVERIFY(!window->tile());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.admission");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, current, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Ineligible);
    QVERIFY(foreign->windows().contains(window));
    QVERIFY(!writer.isReserved(id));
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(foreign->unmanage(window));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testControllerClearAfterWriterLoss()
{
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.admission"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto writer = std::make_unique<TransientPlacementWriter>(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer->registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.admission");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer->trackWindow(id, key));
    QCOMPARE(writer->record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer->snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, writer.get(), token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    writer.reset();
    QCOMPARE(controller.clearTree(controller.generation()), Result::Accepted);
    QVERIFY(!window->requestedTile());
    QVERIFY(!window->tile());
    QVERIFY(window->m_transientOwnership);
    QVERIFY(!window->m_transientOwnership->controller);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    TransientPlacementWriter replacement(directory.filePath(QStringLiteral("placementrc")));
    const auto replacementToken = replacement.registerProvider(&provider);
    QVERIFY(replacement.trackWindow(id, key));
    QCOMPARE(replacement.record(replacementToken, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(replacement.flush());
    const auto replacementState = replacement.snapshot();
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    QCOMPARE(controller.admit(controller.generation(), window, &replacement, replacementToken, key, replacementState.providerRevision, replacementState.mapRevision), Result::Busy);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testPublicTransientRelease_data()
{
    QTest::addColumn<bool>("forget");
    QTest::newRow("unmanage") << false;
    QTest::newRow("forget") << true;
}

void TestXdgShellWindow::testPublicTransientRelease()
{
    QFETCH(bool, forget);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.release"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.release");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    const auto target = leaf->windowGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), leaf);
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    if (forget) {
        leaf->forget(window);
    } else {
        QVERIFY(leaf->unmanage(window));
    }
    QVERIFY(leaf->windows().isEmpty());
    QVERIFY(!window->requestedTile());
    QVERIFY(!window->tile());
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QVERIFY(writer.isReserved(id));
    QCOMPARE(geometry.count(), 0);
    QVERIFY(!leaf->unmanage(window));
    leaf->forget(window);
    QCOMPARE(geometry.count(), 0);
    QVERIFY(writer.isReserved(id));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testPublicTransientPreRequestRelease_data()
{
    QTest::addColumn<bool>("forget");
    QTest::newRow("unmanage") << false;
    QTest::newRow("forget") << true;
}

void TestXdgShellWindow::testPublicTransientPreRequestRelease()
{
    QFETCH(bool, forget);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.release"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.release");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    quint64 oldGeneration = 0;
    const auto callback = connect(leaf, &Tile::windowAdded, &writer, [&]() {
        oldGeneration = window->m_transientOwnership->generation;
        if (forget) {
            leaf->forget(window);
        } else {
            QVERIFY(leaf->unmanage(window));
        }
    });
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Superseded);
    disconnect(callback);
    QVERIFY(oldGeneration);
    QVERIFY(leaf->windows().isEmpty());
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(geometry.count(), 0);
    state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership->generation > oldGeneration);
    QVERIFY(!window->revokeTransientOwnership(oldGeneration));
    QVERIFY(leaf->windows().contains(window));
    QVERIFY(writer.ownsReservation(id, QString::number(window->m_transientOwnership->generation)));
    const auto requestedCount = geometry.count();
    QVERIFY(leaf->unmanage(window));
    QCOMPARE(geometry.count(), requestedCount);
    QVERIFY(writer.isReserved(id));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testPublicTransientReleaseDuringClose_data()
{
    QTest::addColumn<bool>("forget");
    QTest::newRow("unmanage") << false;
    QTest::newRow("forget") << true;
}

void TestXdgShellWindow::testPublicTransientReleaseDuringClose()
{
    QFETCH(bool, forget);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.release"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.release");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    bool protectedDuringClose = false;
    connect(window, &Window::closed, &writer, [&]() {
        if (forget) {
            leaf->forget(window);
        } else {
            QVERIFY(leaf->unmanage(window));
        }
        protectedDuringClose = writer.isReserved(id);
        QVERIFY(window->m_transientOwnership);
        QVERIFY(leaf->windows().isEmpty());
    });
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(protectedDuringClose);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(geometry.count(), 0);
    QCOMPARE(writer.snapshot().entries.value(key), original);
}

void TestXdgShellWindow::testPublicTransientReleaseDestruction_data()
{
    QTest::addColumn<bool>("forget");
    QTest::addColumn<bool>("destroyController");
    QTest::newRow("unmanage-leaf") << false << false;
    QTest::newRow("unmanage-controller") << false << true;
    QTest::newRow("forget-leaf") << true << false;
    QTest::newRow("forget-controller") << true << true;
}

void TestXdgShellWindow::testPublicTransientReleaseDestruction()
{
    QFETCH(bool, forget);
    QFETCH(bool, destroyController);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.release"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.release");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto controller = std::make_unique<TransientTileController>();
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    QPointer<Tile> root(controller->rootTile());
    QPointer<Tile> leaf(controller->tile());
    const auto state = writer.snapshot();
    QCOMPARE(controller->admit(controller->generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    connect(leaf, &Tile::windowRemoved, &writer, [&]() {
        if (destroyController) {
            controller.reset();
        } else {
            delete leaf.data();
        }
    });
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    if (forget) {
        leaf->forget(window);
    } else {
        QVERIFY(leaf->unmanage(window));
    }
    QVERIFY(!leaf);
    QTRY_VERIFY(!root);
    QCOMPARE(geometry.count(), 0);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QVERIFY(writer.isReserved(id));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testPublicTransientReleaseRefusal()
{
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> otherSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> otherShell(Test::createXdgToplevelSurface(otherSurface.get()));
    otherShell->set_app_id(QStringLiteral("org.example.other"));
    Window *other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(other);
    {
        RequestTestTile unowned(other);
        QVERIFY(!unowned.unmanage(other));
        unowned.forget(other);
        QVERIFY(unowned.windows().contains(other));
        unowned.abandonWithoutForgetForTest();
    }
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.release"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.release");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    auto leaf = controller.tile();
    QVERIFY(!leaf->unmanage(nullptr));
    leaf->forget(nullptr);
    QVERIFY(!leaf->unmanage(other));
    leaf->forget(other);
    QVERIFY(!controller.rootTile()->unmanage(window));
    controller.rootTile()->forget(window);
    QVERIFY(leaf->windows().contains(window));
    QVERIFY(writer.ownsReservation(id, QString::number(window->m_transientOwnership->generation)));
    QVERIFY(leaf->unmanage(window));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    other->setSkipCloseAnimation(true);
    other->destroyWindow();
}

void TestXdgShellWindow::testTransientRegistryLookup()
{
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.registry"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    const auto manualRoot = manager->rootTile(desktop);
    const auto manualModel = manager->model();
    const auto timers = manager->findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
    QCOMPARE(timers.size(), 1);
    QSignalSpy saved(timers.first(), &QTimer::timeout);
    const auto padding = manualRoot->padding();
    manualRoot->setPadding(padding + 1);
    manualRoot->setPadding(padding);
    QVERIFY(saved.wait());
    KConfigGroup tiling(kwinApp()->config(), QStringLiteral("Tiling"));
    KConfigGroup deskConfig(&tiling, desktop->id());
    KConfigGroup outputConfig(&deskConfig, manager->output()->uuid());
    const auto originalLayout = outputConfig.readEntry("tiles", QByteArray());
    QVERIFY(!originalLayout.isEmpty());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.registry");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QCOMPARE(manager->tileForWindow(window, desktop), controller.tile());
    QCOMPARE(manager->rootTile(desktop), manualRoot);
    QCOMPARE(manager->model(), manualModel);
    QVERIFY(!manager->bestTileForPosition(100, 100)->isTransient());
    manualRoot->setPadding(padding + 1);
    manualRoot->setPadding(padding);
    QVERIFY(saved.wait());
    QCOMPARE(outputConfig.readEntry("tiles", QByteArray()), originalLayout);
    manager->forgetWindow(window, desktop);
    QVERIFY(controller.tile()->windows().isEmpty());
    QVERIFY(!manager->tileForWindow(window, desktop));
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QVERIFY(writer.isReserved(id));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testTransientRegistryRetiredRoot()
{
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.registry"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto manager = std::make_unique<TileManager>(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.registry");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager.get(), desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QPointer<Tile> root(controller.rootTile());
    delete controller.tile();
    QVERIFY(root); // Its deferred deletion has not run yet.
    QVERIFY(!controller.rootTile());
    manager.reset();
    QVERIFY(!root); // Manager teardown must also drain retired roots synchronously.
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QVERIFY(writer.isReserved(id));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testTransientRegistryDestructorReentry_data()
{
    QTest::addColumn<bool>("leaf");
    QTest::newRow("root-already-destroying") << false;
    QTest::newRow("retired-ancestor-during-leaf-detach") << true;
}

void TestXdgShellWindow::testTransientRegistryDestructorReentry()
{
    QFETCH(bool, leaf);
    using Result = TransientTileController::Result;
    auto output = workspace()->outputs().first();
    auto desktop = VirtualDesktopManager::self()->currentDesktop(output);
    auto manager = std::make_unique<TileManager>(output);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager.get(), desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    QPointer<Tile> root(controller.rootTile());
    QPointer<Tile> child(controller.tile());
    Tile *removed = leaf ? child.data() : root.data();
    bool called = false;
    RegistryEventFilter filter(QEvent::ChildRemoved, [&](QObject *, QEvent *event) {
        if (called || static_cast<QChildEvent *>(event)->child() != removed) {
            return;
        }
        called = true;
        QVERIFY(root);
        QVERIFY(child);
        manager->prepareForRemoval();
        QCOMPARE(bool(root), !leaf);
        QVERIFY(child);
    }, leaf); // The leaf case deletes the watched root; do not dispatch to it afterward.
    (leaf ? static_cast<QObject *>(root.data()) : static_cast<QObject *>(&controller))->installEventFilter(&filter);
    delete removed;
    QVERIFY(called);
    QVERIFY(!root);
    QVERIFY(!child);
    QVERIFY(!controller.rootTile());
}

void TestXdgShellWindow::testTransientRegistryDrainSnapshots_data()
{
    QTest::addColumn<bool>("deleteManager");
    QTest::newRow("retired-during-drain") << false;
    QTest::newRow("manager-deleted-during-drain") << true;
}

void TestXdgShellWindow::testTransientRegistryDrainSnapshots()
{
    QFETCH(bool, deleteManager);
    using Result = TransientTileController::Result;
    auto desktops = VirtualDesktopManager::self();
    const auto oldCount = desktops->count();
    const auto restore = qScopeGuard([desktops, oldCount]() {
        desktops->setCount(oldCount);
    });
    desktops->setCount(std::max(oldCount, uint(2)));
    auto output = workspace()->outputs().first();
    auto manager = std::make_unique<TileManager>(output);
    TransientTileController first;
    TransientTileController second;
    QCOMPARE(first.createTree(manager.get(), desktops->desktops().first(), RectF(0, 0, 0.5, 1)), Result::Accepted);
    QCOMPARE(second.createTree(manager.get(), desktops->desktops().last(), RectF(0, 0, 0.5, 1)), Result::Accepted);
    QPointer<Tile> firstRoot(first.rootTile());
    QPointer<Tile> secondRoot(second.rootTile());
    bool called = false;
    const auto retireOther = [&]() {
        if (called) {
            return;
        }
        called = true;
        if (firstRoot) {
            delete first.tile();
            QVERIFY(firstRoot);
        } else {
            delete second.tile();
            QVERIFY(secondRoot);
        }
        if (deleteManager) {
            manager.reset();
        }
    };
    QObject callbackContext;
    connect(firstRoot, &QObject::destroyed, &callbackContext, retireOther);
    connect(secondRoot, &QObject::destroyed, &callbackContext, retireOther);
    manager->prepareForRemoval();
    QVERIFY(called);
    QVERIFY(!firstRoot);
    QVERIFY(!secondRoot);
    QVERIFY(!first.rootTile());
    QVERIFY(!second.rootTile());
    QCOMPARE(bool(manager), !deleteManager);
}

void TestXdgShellWindow::testTransientRegistryTransactions_data()
{
    QTest::addColumn<bool>("replaceDuringConstruction");
    QTest::newRow("final-registration-race") << true;
    QTest::newRow("old-destroyed-callback-after-replacement") << false;
}

void TestXdgShellWindow::testTransientRegistryTransactions()
{
    QFETCH(bool, replaceDuringConstruction);
    using Result = TransientTileController::Result;
    auto output = workspace()->outputs().first();
    auto desktop = VirtualDesktopManager::self()->currentDesktop(output);
    TileManager manager(output);
    TransientTileController first;
    TransientTileController replacement;
    TransientTileController probe;
    bool registered = false;
    RegistryEventFilter filter(QEvent::ChildAdded, [&](QObject *, QEvent *event) {
        auto root = qobject_cast<Tile *>(static_cast<QChildEvent *>(event)->child());
        QVERIFY(root);
        const auto createReplacement = [&]() {
            QCOMPARE(replacement.createTree(&manager, desktop, RectF(0.5, 0, 0.5, 1)), Result::Accepted);
            registered = true;
        };
        if (replaceDuringConstruction) {
            createReplacement();
        } else {
            // Register this observer before the manager attaches its own destroyed callback.
            connect(root, &QObject::destroyed, &replacement, createReplacement);
        }
    });
    first.installEventFilter(&filter);
    const auto result = first.createTree(&manager, desktop, RectF(0, 0, 0.5, 1));
    if (replaceDuringConstruction) {
        QCOMPARE(result, Result::Refused);
        QVERIFY(!first.rootTile());
    } else {
        QCOMPARE(result, Result::Accepted);
        QCOMPARE(first.clearTree(first.generation()), Result::Accepted);
    }
    QVERIFY(registered);
    QVERIFY(replacement.rootTile());
    QCOMPARE(probe.createTree(&manager, desktop, RectF(0, 0, 1, 1)), Result::Busy);
    QCOMPARE(replacement.clearTree(replacement.generation()), Result::Accepted);
    QCOMPARE(probe.createTree(&manager, desktop, RectF(0, 0, 1, 1)), Result::Accepted);
}

void TestXdgShellWindow::testTransientRegistryClosedRegistration()
{
    using Result = TransientTileController::Result;
    auto output = workspace()->outputs().first();
    auto desktop = VirtualDesktopManager::self()->currentDesktop(output);
    TileManager manager(output);
    TransientTileController first;
    TransientTileController replacement;
    QCOMPARE(first.createTree(&manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    bool called = false;
    connect(first.rootTile(), &QObject::destroyed, &replacement, [&]() {
        called = true;
        QVERIFY(!manager.tearingDown());
        QCOMPARE(replacement.createTree(&manager, desktop, RectF(0.5, 0, 0.5, 1)), Result::Busy);
    });
    manager.prepareForRemoval();
    QVERIFY(called);
    QVERIFY(!first.rootTile());
    QVERIFY(!replacement.rootTile());
    QCOMPARE(replacement.createTree(&manager, desktop, RectF(0.5, 0, 0.5, 1)), Result::Busy);
}

void TestXdgShellWindow::testTransientRegistryDesktopRemoval()
{
    using Result = TransientTileController::Result;
    auto desktops = VirtualDesktopManager::self();
    const auto oldCount = desktops->count();
    const auto restore = qScopeGuard([desktops, oldCount]() {
        desktops->setCount(oldCount);
    });
    desktops->setCount(oldCount + 1);
    auto output = workspace()->outputs().first();
    auto current = desktops->currentDesktop(output);
    auto removed = desktops->desktops().last();
    QVERIFY(removed != current);
    TileManager manager(output);
    TransientTileController first;
    TransientTileController replacement;
    QCOMPARE(first.createTree(&manager, removed, RectF(0, 0, 0.5, 1)), Result::Accepted);
    QPointer<Tile> root(first.rootTile());
    connect(root, &QObject::destroyed, &replacement, [&]() {
        QVERIFY(replacement.createTree(&manager, removed, RectF(0, 0, 0.5, 1)) != Result::Accepted);
    });
    desktops->setCount(oldCount);
    QVERIFY(!root);
    QVERIFY(!first.rootTile());
    QCOMPARE(replacement.createTree(&manager, current, RectF(0, 0, 0.5, 1)), Result::Accepted);
}

void TestXdgShellWindow::testTransientRegistryForgetReentry_data()
{
    QTest::addColumn<int>("cause");
    QTest::newRow("manager-deleted") << 0;
    QTest::newRow("window-closed") << 1;
    QTest::newRow("later-desktop-deleted") << 2;
}

void TestXdgShellWindow::testTransientRegistryForgetReentry()
{
    QFETCH(int, cause);
    using Result = TransientTileController::Result;
    auto desktops = VirtualDesktopManager::self();
    auto output = workspace()->outputs().first();
    const auto oldCount = desktops->count();
    const QPointer<VirtualDesktop> oldCurrent(desktops->currentDesktop(output));
    const auto restore = qScopeGuard([desktops, output, oldCount, oldCurrent]() {
        desktops->setCount(oldCount);
        if (oldCurrent) {
            desktops->setCurrent(oldCurrent, output);
        }
    });
    desktops->setCount(oldCount + 1);
    desktops->setCurrent(desktops->desktops().first(), output);
    QPointer<VirtualDesktop> lastDesktop(desktops->desktops().last());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.registry"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto manager = std::make_unique<TileManager>(window->moveResizeOutput());
    auto desktop = desktops->currentDesktop(window->moveResizeOutput());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.registry");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager.get(), desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QPointer<Window> guard(window);
    bool called = false;
    QObject callbackContext;
    connect(controller.tile(), &Tile::windowRemoved, &callbackContext, [&]() {
        called = true;
        if (cause == 0) {
            manager.reset();
        } else if (cause == 1) {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        } else {
            desktops->setCount(oldCount);
            QVERIFY(!desktops->desktops().contains(lastDesktop));
            QCoreApplication::sendPostedEvents(lastDesktop, QEvent::DeferredDelete);
            QVERIFY(!lastDesktop);
        }
        QVERIFY(guard);
    });
    manager->forgetWindow(window, nullptr);
    QVERIFY(called);
    if (cause == 0) {
        QVERIFY(!manager);
        QVERIFY(!controller.rootTile());
    }
    if (cause == 1) {
        QVERIFY(!guard);
        QVERIFY(!writer.isReserved(id));
    } else {
        if (cause == 2) {
            QVERIFY(!lastDesktop);
        }
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QVERIFY(writer.isReserved(id));
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QTRY_VERIFY(!guard);
        QVERIFY(!writer.isReserved(id));
    }
}

void TestXdgShellWindow::testWorkspaceIdleController()
{
    const auto controller = workspace()->m_transientTileController.get();
    QVERIFY(controller);
    QVERIFY(!controller->rootTile());
    QVERIFY(!controller->tile());
    QCOMPARE(controller->generation(), quint64(0));
}

void TestXdgShellWindow::testWorkspaceMissingTileManager()
{
    const auto output = workspace()->outputs().first();
    QPointer<TileManager> manager(workspace()->tileManager(output));
    QVERIFY(manager);
    QCOMPARE(manager->parent(), output);
    delete manager.data();
    QVERIFY(!manager);
    QVERIFY(!workspace()->tileManager(output));
    QVERIFY(!workspace()->rootTile(output));

    // Lookup is read-only. Explicit output reconciliation replaces a missing manager.
    QVERIFY(output->findChildren<TileManager *>(QString(), Qt::FindDirectChildrenOnly).isEmpty());
    OutputConfiguration config;
    QVERIFY(workspace()->applyOutputConfiguration(config).has_value());
    manager = workspace()->tileManager(output);
    QVERIFY(manager);
    QCOMPARE(manager->parent(), output);
    QCOMPARE(output->findChildren<TileManager *>(QString(), Qt::FindDirectChildrenOnly).size(), 1);
    QCOMPARE(workspace()->tileManager(output), manager.data());
}

void TestXdgShellWindow::testMissingManagerTilingRequests()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const auto outputs = workspace()->outputs();
    const auto unavailable = outputs.last();
    auto source = workspace()->tileManager(outputs.first())->quickTile(QuickTileFlag::Left);
    QVERIFY(source->manage(window));
    QCOMPARE(window->requestedTile(), source);
    delete workspace()->tileManager(unavailable);
    QVERIFY(!workspace()->tileManager(unavailable));
    const auto pos = unavailable->geometry().center();
    window->setQuickTileMode(QuickTileFlag::Right, pos);
    QCOMPARE(window->requestedTile(), source);
    window->setQuickTileMode(QuickTileFlag::Custom, pos);
    QCOMPARE(window->requestedTile(), source);
    QVERIFY(window->quickTileGeometry(QuickTileFlag::Custom, pos).isEmpty());
    QCOMPARE(window->quickTileGeometry(QuickTileFlag::Left, pos), workspace()->clientArea(MaximizeArea, window, pos));
    window->setQuickTileMode(QuickTileFlag::None, pos);
    QVERIFY(!window->requestedTile());
    window->moveResize(RectF(pos.x(), pos.y(), 200, 100));
    const auto geometry = window->moveResizeGeometry();
    window->handleCustomQuickTileShortcut(QuickTileFlag::Right);
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), geometry);
    window->exitQuickTileMode();
}

void TestXdgShellWindow::testMissingManagerOutputTransfer_data()
{
    QTest::addColumn<int>("callback");
    QTest::newRow("ordinary-transfer") << 0;
    QTest::newRow("newer-placement") << 1;
    QTest::newRow("closed-window") << 2;
    QTest::newRow("newer-floating-geometry") << 3;
}

void TestXdgShellWindow::testMissingManagerOutputTransfer()
{
    QFETCH(int, callback);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const auto outputs = workspace()->outputs();
    const auto unavailable = outputs.last();
    auto manager = workspace()->tileManager(outputs.first());
    auto source = manager->quickTile(QuickTileFlag::Left);
    auto replacement = manager->quickTile(QuickTileFlag::Right);
    QVERIFY(source->manage(window));
    QCOMPARE(window->requestedTile(), source);
    delete workspace()->tileManager(unavailable);
    QVERIFY(!workspace()->tileManager(unavailable));
    QPointer<Window> guard(window);
    bool called = false;
    RectF callbackGeometry;
    RectF callbackRestore;
    QObject callbackContext;
    connect(source, &Tile::windowRemoved, &callbackContext, [&]() {
        called = true;
        if (callback == 1) {
            QVERIFY(replacement->manage(window));
            callbackRestore = window->geometryRestore();
        } else if (callback == 2) {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        } else if (callback == 3) {
            window->requestTile(nullptr);
            window->moveResize(RectF(120, 170, 320, 210));
            callbackGeometry = window->moveResizeGeometry();
        }
    });
    window->sendToOutput(unavailable);
    QVERIFY(called);
    if (callback == 2) {
        QVERIFY(!guard);
    } else if (callback == 1) {
        QCOMPARE(window->requestedTile(), replacement);
        QCOMPARE(window->moveResizeGeometry(), replacement->windowGeometry());
        QCOMPARE(window->geometryRestore(), callbackRestore);
    } else if (callback == 3) {
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->moveResizeGeometry(), callbackGeometry);
    } else {
        QVERIFY(!window->requestedTile());
        QVERIFY(!source->windows().contains(window));
        QCOMPARE(window->moveResizeOutput(), unavailable);
    }
}

void TestXdgShellWindow::testQuickTileCallbackWins_data()
{
    QTest::addColumn<bool>("floating");
    QTest::newRow("newer-tile") << false;
    QTest::newRow("newer-floating-geometry") << true;
}

void TestXdgShellWindow::testQuickTileCallbackWins()
{
    QFETCH(bool, floating);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.quicktile-reentry"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const auto manager = workspace()->tileManager(window->moveResizeOutput());
    const auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placements")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.quicktile-reentry");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, window->frameGeometry(), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto snapshot = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, snapshot.providerRevision, snapshot.mapRevision), TransientTileController::Result::Accepted);
    bool called = false;
    RectF newerGeometry;
    QObject callbackContext;
    connect(controller.tile(), &Tile::windowRemoved, &callbackContext, [&]() {
        called = true;
        if (floating) {
            window->moveResize(RectF(120, 170, 320, 210));
        } else {
            window->setQuickTileModeAtCurrentPosition(QuickTileFlag::Left);
        }
        newerGeometry = window->moveResizeGeometry();
    });
    window->setQuickTileModeAtCurrentPosition(QuickTileFlag::Right);
    QVERIFY(called);
    QCOMPARE(window->moveResizeGeometry(), newerGeometry);
    if (floating) {
        QVERIFY(!window->requestedTile());
    } else {
        QCOMPARE(window->requestedTile(), manager->quickTile(QuickTileFlag::Left));
    }
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testWorkspaceShutdown_data()
{
    QTest::addColumn<bool>("deleteManager");
    QTest::newRow("intact-managers") << false;
    QTest::newRow("manager-deleted-during-close") << true;
}

void TestXdgShellWindow::testWorkspaceShutdown()
{
    QFETCH(bool, deleteManager);
    if (!qEnvironmentVariableIsSet("KWIN_TEST_WORKSPACE_SHUTDOWN")) {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString runtime = directory.filePath(QStringLiteral("runtime"));
        QVERIFY(QDir().mkdir(runtime));
        QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("KWIN_TEST_WORKSPACE_SHUTDOWN"), QStringLiteral("1"));
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), directory.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), directory.filePath(QStringLiteral("cache")));
        QProcess child;
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QStringLiteral("dbus-run-session"), {QStringLiteral("--"), QCoreApplication::applicationFilePath(), QStringLiteral("testWorkspaceShutdown:%1").arg(QString::fromLatin1(QTest::currentDataTag()))});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(30000));
        const auto report = child.readAll();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, report.constData());
        QVERIFY2(report.contains("Workspace shutdown observations: PASS"), report.constData());
        return;
    }

    auto &observation = *shutdownObservation;
    const auto cleanupUnarmed = qScopeGuard([&observation]() {
        if (!observation.armed) {
            observation.context.reset();
            observation.challenger.reset();
            observation.shell.reset();
            observation.surface.reset();
            observation.writer.reset();
            observation.directory.reset();
        }
    });
    observation.directory = std::make_unique<QTemporaryDir>();
    QVERIFY(observation.directory->isValid());
    observation.writer = std::make_unique<TransientPlacementWriter>(observation.directory->filePath(QStringLiteral("placements")));
    observation.challenger = std::make_unique<TransientTileController>();
    observation.context = std::make_unique<QObject>();
    observation.surface = Test::createSurface();
    observation.shell = Test::createXdgToplevelSurface(observation.surface.get());
    observation.shell->set_app_id(QStringLiteral("org.example.workspace-shutdown"));
    QSignalSpy configure(observation.shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(observation.surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    observation.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(observation.surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const auto outputs = workspace()->outputs();
    for (auto output : outputs) {
        observation.managers.append(workspace()->tileManager(output));
    }
    observation.controller = workspace()->m_transientTileController.get();
    QVERIFY(observation.controller);
    const auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    const auto manager = workspace()->tileManager(window->moveResizeOutput());
    const QPointer<TileManager> challengeManager = observation.managers.last();
    QVERIFY(challengeManager && challengeManager != manager);
    QCOMPARE(observation.challenger->createTree(challengeManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    QCOMPARE(observation.challenger->clearTree(observation.challenger->generation()), TransientTileController::Result::Accepted);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.workspace-shutdown");
    const auto provider = observation.writer->registerProvider(observation.context.get());
    QVERIFY(observation.writer->trackWindow(id, key));
    QCOMPARE(observation.writer->record(provider, 1, {id, window->frameGeometry(), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(observation.controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto snapshot = observation.writer->snapshot();
    QCOMPARE(observation.controller->admit(observation.controller->generation(), window, observation.writer.get(), provider, key,
                                           snapshot.providerRevision, snapshot.mapRevision),
             TransientTileController::Result::Accepted);

    connect(window, &Window::closed, observation.context.get(), [&, window, manager, challengeManager, desktop, deleteManager, id]() {
        observation.windowClosed = true;
        observation.require(bool(window->m_transientOwnership), QStringLiteral("Window record lost during close"));
        observation.require(observation.writer->isReserved(id), QStringLiteral("Close-time writer barrier lost"));
        observation.require(observation.challenger->createTree(challengeManager, desktop, RectF(0, 0, 0.5, 1)) == TransientTileController::Result::Busy,
                            QStringLiteral("Registration remained open during shutdown"));
        QSignalSpy changed(workspace(), &Workspace::outputsChanged);
        workspace()->updateOutputs();
        observation.require(changed.isEmpty(), QStringLiteral("Output reconciliation ran during shutdown"));
        if (deleteManager) {
            const auto output = manager->output();
            delete manager;
            observation.require(!workspace()->tileManager(output), QStringLiteral("Deleted manager remained in lookup"));
        }
    });
    for (auto guarded : observation.managers) {
        const bool deletedDuringClose = deleteManager && guarded == manager;
        connect(guarded, &QObject::destroyed, observation.context.get(), [&, deletedDuringClose]() {
            if (!deletedDuringClose) {
                observation.require(!observation.controller, QStringLiteral("Surviving manager destroyed before controller"));
            }
        });
    }
    connect(workspace(), &Workspace::outputRemoved, observation.context.get(), [&](LogicalOutput *) {
        ++observation.outputsRemoved;
        observation.require(!observation.controller, QStringLiteral("Controller survived output removal"));
        for (const auto &manager : observation.managers) {
            observation.require(!manager, QStringLiteral("Manager survived output removal"));
        }
    });
    connect(workspace(), &QObject::destroyed, observation.context.get(), [&, id]() {
        observation.workspaceDestroyed = true;
        observation.require(!observation.writer->isReserved(id), QStringLiteral("Writer barrier survived Window destruction"));
        observation.shell.reset();
        observation.surface.reset();
        Test::destroyWaylandConnection();
        observation.writer.reset();
    });
    observation.armed = true;
}

void TestXdgShellWindow::testOutputUpdatePending_data()
{
    QTest::addColumn<bool>("nestedEvents");
    QTest::addColumn<bool>("consumeQueued");
    QTest::newRow("deferred") << false << false;
    QTest::newRow("nested-event-loop") << true << false;
    QTest::newRow("direct-call-consumes-queued") << false << true;
}

void TestXdgShellWindow::testOutputUpdatePending()
{
    QFETCH(bool, nestedEvents);
    QFETCH(bool, consumeQueued);
    int depth = 0;
    int maxDepth = 0;
    int passes = 0;
    QObject callbackContext;
    connect(workspace(), &Workspace::outputsChanged, &callbackContext, [&]() {
        ++depth;
        maxDepth = std::max(maxDepth, depth);
        ++passes;
        if (passes == 1) {
            workspace()->updateOutputs();
            if (nestedEvents) {
                QCoreApplication::processEvents();
            }
        }
        --depth;
    });
    workspace()->updateOutputs();
    QCOMPARE(maxDepth, 1);
    QCOMPARE(passes, 1);
    if (consumeQueued) {
        workspace()->updateOutputs();
    }
    QTRY_COMPARE(passes, 2);
    QCoreApplication::processEvents();
    QCOMPARE(passes, 2);
    QCOMPARE(maxDepth, 1);
}

void TestXdgShellWindow::testOutputUpdateFrozenConfiguration()
{
    const auto outputs = workspace()->outputs();
    QPointer<LogicalOutput> second(outputs.last());
    auto backend = second->backendOutput();
    bool nested = false;
    QObject callbackContext;
    connect(outputs.first(), &LogicalOutput::changed, &callbackContext, [&]() {
        if (nested) {
            return;
        }
        nested = true;
        OutputConfiguration latest;
        latest.changeSet(backend)->pos = QPoint(3000, 0);
        QVERIFY(workspace()->applyOutputConfiguration(latest).has_value());
    });
    QSignalSpy changed(workspace(), &Workspace::outputsChanged);
    OutputConfiguration initial;
    initial.changeSet(backend)->pos = QPoint(1600, 0);
    QVERIFY(workspace()->applyOutputConfiguration(initial).has_value());
    QVERIFY(nested);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(second->geometry().x(), 1600);
    QTRY_COMPARE(second->geometry().x(), 3000);
    QCOMPARE(changed.size(), 2);
}

void TestXdgShellWindow::testOutputUpdateFrozenOrder()
{
    const auto outputs = workspace()->outputs();
    bool nested = false;
    QObject callbackContext;
    connect(outputs.first(), &LogicalOutput::changed, &callbackContext, [&]() {
        if (nested) {
            return;
        }
        nested = true;
        OutputConfiguration latest;
        latest.changeSet(outputs.first()->backendOutput())->priority = 1;
        latest.changeSet(outputs.last()->backendOutput())->priority = 0;
        QVERIFY(workspace()->applyOutputConfiguration(latest).has_value());
    });
    OutputConfiguration initial;
    initial.changeSet(outputs.first()->backendOutput())->priority = 0;
    initial.changeSet(outputs.last()->backendOutput())->priority = 1;
    QVERIFY(workspace()->applyOutputConfiguration(initial).has_value());
    QVERIFY(nested);
    QCOMPARE(workspace()->outputOrder().first(), outputs.first());
    QTRY_COMPARE(workspace()->outputOrder().first(), outputs.last());
}

void TestXdgShellWindow::testOutputUpdateBackendReplacement()
{
    const auto outputs = workspace()->outputs();
    QPointer<LogicalOutput> oldLogical(outputs.last());
    QPointer<BackendOutput> oldBackend(oldLogical->backendOutput());
    bool nested = false;
    QObject callbackContext;
    connect(outputs.first(), &LogicalOutput::changed, &callbackContext, [&]() {
        if (nested) {
            return;
        }
        nested = true;
        Test::setOutputConfig({Rect(0, 0, 1600, 900)});
        QVERIFY(oldLogical);
        QVERIFY(oldBackend);
    });
    workspace()->updateOutputs();
    QVERIFY(nested);
    QCOMPARE(workspace()->outputs(), outputs);
    QVERIFY(oldLogical);
    QVERIFY(oldBackend);
    QTRY_COMPARE(workspace()->outputs().size(), 1);
    QTRY_VERIFY(!oldLogical);
    QTRY_VERIFY(!oldBackend);
    QCOMPARE(workspace()->outputs().first()->geometry(), Rect(0, 0, 1600, 900));
}

void TestXdgShellWindow::testOutputUpdateRetainsClosedWindow()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    window->setSkipCloseAnimation(true);
    QPointer<Window> guard(window);
    bool called = false;
    QObject callbackContext;
    connect(workspace()->outputs().first(), &LogicalOutput::changed, &callbackContext, [&]() {
        if (called) {
            return;
        }
        called = true;
        window->destroyWindow();
        QVERIFY(guard); // The old-output mapping must retain this identity through the pass.
        QVERIFY(guard->isDeleted());
    });
    workspace()->updateOutputs();
    QVERIFY(called);
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testOutputUpdateCleanupReentry()
{
    QPointer<BackendOutput> removed(workspace()->outputs().first()->backendOutput());
    int notifications = 0;
    bool reentered = false;
    QObject callbackContext;
    connect(workspace(), &Workspace::outputsChanged, &callbackContext, [&]() {
        ++notifications;
    });
    connect(removed, &QObject::destroyed, &callbackContext, [&]() {
        reentered = true;
        const auto before = notifications;
        workspace()->updateOutputs();
        QCOMPARE(notifications, before);
    });
    Test::setOutputConfig({Rect(0, 0, 1600, 900)});
    QVERIFY(reentered);
    QVERIFY(!removed);
}

void TestXdgShellWindow::testOutputUpdatePlaceholderRoundTrip()
{
    Test::setOutputConfig(QList<Rect>{});
    QCOMPARE(workspace()->outputs().size(), 1);
    QPointer<LogicalOutput> placeholder(workspace()->outputs().first());
    QPointer<BackendOutput> placeholderBackend(placeholder->backendOutput());
    QVERIFY(placeholder->isPlaceholder());
    QVERIFY(workspace()->tileManager(placeholder));
    Test::setOutputConfig({Rect(0, 0, 1600, 900)});
    QCOMPARE(workspace()->outputs().size(), 1);
    QVERIFY(!workspace()->outputs().first()->isPlaceholder());
    QVERIFY(workspace()->tileManager(workspace()->outputs().first()));
    QTRY_VERIFY(!placeholder);
    QTRY_VERIFY(!placeholderBackend);
}

void TestXdgShellWindow::testOutputUpdateCallerLifetime_data()
{
    QTest::addColumn<bool>("queryConfiguration");
    QTest::newRow("direct-application") << false;
    QTest::newRow("queried-configuration") << true;
}

void TestXdgShellWindow::testOutputUpdateCallerLifetime()
{
    QFETCH(bool, queryConfiguration);
    const auto backends = kwinApp()->outputBackend()->outputs();
    QPointer<BackendOutput> disabled(backends.last());
    OutputConfiguration disable;
    disable.changeSet(disabled)->enabled = false;
    QVERIFY(workspace()->applyOutputConfiguration(disable).has_value());
    QCOMPARE(workspace()->outputs().size(), 1);
    QVERIFY(!workspace()->findOutput(disabled));

    bool removed = false;
    bool destroyed = false;
    QObject callbackContext;
    connect(disabled, &QObject::destroyed, &callbackContext, [&]() {
        destroyed = true;
        // Caller bookkeeping still uses its backend snapshot after the pass releases its holds.
        QVERIFY(!workspace()->m_updatingOutputs);
    });
    connect(workspace()->outputs().first(), &LogicalOutput::changed, &callbackContext, [&]() {
        if (!removed) {
            removed = true;
            kwinApp()->outputBackend()->removeVirtualOutput(disabled);
        }
    });
    if (queryConfiguration) {
        workspace()->updateOutputConfiguration();
    } else {
        OutputConfiguration apply;
        QVERIFY(workspace()->applyOutputConfiguration(apply).has_value());
    }
    QVERIFY(removed);
    QTRY_VERIFY(destroyed);
    QVERIFY(!disabled);
}

void TestXdgShellWindow::testOutputRemovalDrain_data()
{
    QTest::addColumn<int>("callback");
    QTest::newRow("drain-before-notification") << 0;
    QTest::newRow("manager-deleted-in-drain") << 1;
    QTest::newRow("window-closed-in-drain") << 2;
}

void TestXdgShellWindow::testOutputRemovalDrain()
{
    QFETCH(int, callback);
    QPointer<LogicalOutput> removed(workspace()->outputs().last());
    workspace()->setActiveOutput(removed);
    input()->pointer()->warp(removed->geometry().center());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.output-drain"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->moveResizeOutput(), removed.data());
    QPointer<TileManager> manager(workspace()->tileManager(removed));
    const auto desktop = VirtualDesktopManager::self()->currentDesktop(removed);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placements")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.output-drain");
    const QRectF floating = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, floating, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto snapshot = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, snapshot.providerRevision, snapshot.mapRevision), TransientTileController::Result::Accepted);
    QPointer<Tile> root(controller.rootTile());
    QPointer<Window> guard(window);
    bool notified = false;
    bool released = false;
    QObject callbackContext;
    connect(window, &Window::requestedTileChanged, &callbackContext, [&]() {
        if (released || !window->m_transientOwnership || window->m_transientOwnership->phase != Window::TransientOwnershipPhase::Restoring) {
            return;
        }
        released = true;
        if (callback == 1) {
            delete manager.data();
        } else if (callback == 2) {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }
    });
    connect(workspace(), &Workspace::outputRemoved, &callbackContext, [&](LogicalOutput *output) {
        if (output != removed) {
            return;
        }
        notified = true;
        QVERIFY(released);
        QVERIFY(!root);
        QVERIFY(!controller.rootTile());
        QVERIFY(guard);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QCOMPARE(window->m_transientOwnership->floating, floating);
        QVERIFY(writer.isReserved(id));
    });
    OutputConfiguration config;
    config.changeSet(removed->backendOutput())->enabled = false;
    QVERIFY(workspace()->applyOutputConfiguration(config).has_value());
    QVERIFY(notified);
    QVERIFY(released);
    QVERIFY(!root);
    QVERIFY(!manager);
    if (callback == 2) {
        QTRY_VERIFY(!guard);
    } else {
        QVERIFY(guard);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QVERIFY(writer.isReserved(id));
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QTRY_VERIFY(!guard);
    }
    QVERIFY(!writer.isReserved(id));
}

void TestXdgShellWindow::testWorkspaceWindowContinuation_data()
{
    QTest::addColumn<bool>("rearrange");
    QTest::addColumn<bool>("closeCurrent");
    QTest::newRow("output-setter-current") << false << true;
    QTest::newRow("output-setter-later") << false << false;
    QTest::newRow("rearrange-current") << true << true;
    QTest::newRow("rearrange-later") << true << false;
}

void TestXdgShellWindow::testWorkspaceWindowContinuation()
{
    QFETCH(bool, rearrange);
    QFETCH(bool, closeCurrent);
    Test::setOutputConfig(QList<Test::OutputInfo>{
        {.geometry = Rect(0, 0, 1280, 1024), .scale = 1},
        {.geometry = Rect(1280, 0, 1280, 1024), .scale = 2},
    });
    const auto outputs = workspace()->outputs();
    workspace()->setActiveOutput(outputs.last());
    input()->pointer()->warp(outputs.last()->geometry().center());
    std::array<std::unique_ptr<KWayland::Client::Surface>, 3> surfaces;
    std::array<std::unique_ptr<Test::XdgToplevel>, 3> shells;
    for (int i = 0; i < 3; ++i) {
        surfaces[i] = Test::createSurface();
        shells[i] = Test::createXdgToplevelSurface(surfaces[i].get());
        auto window = Test::renderAndWaitForShown(surfaces[i].get(), QSize(200, 100), Qt::red);
        QVERIFY(window);
        QCOMPARE(window->moveResizeOutput(), outputs.last());
        window->move(QPointF(2200, 500));
    }
    QPointer<Window> current;
    QPointer<Window> target;
    QList<QPointer<Window>> survivors;
    {
        const auto windows = rearrange ? workspace()->windows() : workspace()->stackingOrder();
        QCOMPARE(windows.size(), 3);
        current = windows[0];
        target = closeCurrent ? windows[0] : windows[1];
        for (Window *window : windows) {
            if (window != target) {
                survivors.append(window);
            }
            if (!rearrange) {
                window->setOutput(outputs.first());
                window->setMoveResizeOutput(outputs.first());
            }
        }
    }
    bool called = false;
    bool closed = false;
    bool updatedAfterClose = false;
    QObject callbackContext;
    connect(target, &Window::closed, &callbackContext, [&]() {
        closed = true;
    });
    connect(target, &Window::outputChanged, &callbackContext, [&]() {
        updatedAfterClose |= closed;
    });
    const auto closeTarget = [&]() {
        if (called) {
            return;
        }
        called = true;
        if (rearrange) {
            QVERIFY(workspace()->m_inRearrange);
        }
        target->setSkipCloseAnimation(true);
        target->destroyWindow();
        if (!rearrange && closeCurrent) {
            QVERIFY(target); // The caller must retain its current Window until the setter returns.
        }
    };
    if (rearrange) {
        connect(current, &Window::requestedGeometryChanged, &callbackContext, closeTarget);
        OutputConfiguration config;
        config.changeSet(outputs.last()->backendOutput())->enabled = false;
        QVERIFY(workspace()->applyOutputConfiguration(config).has_value());
    } else {
        connect(current, &Window::nextTargetScaleChanged, &callbackContext, closeTarget);
        workspace()->desktopResized({});
    }
    QVERIFY(called);
    QVERIFY(closed);
    QVERIFY(!updatedAfterClose);
    QTRY_VERIFY(!target);
    const auto expectedOutput = rearrange ? outputs.first() : outputs.last();
    for (const auto &survivor : survivors) {
        QVERIFY(survivor);
        QCOMPARE(survivor->moveResizeOutput(), expectedOutput);
        QCOMPARE(survivor->output(), expectedOutput);
        if (rearrange) {
            QVERIFY(workspace()->clientArea(PlacementArea, survivor).contains(survivor->moveResizeGeometry()));
        }
    }
}

void TestXdgShellWindow::testOutputRemovalBatch_data()
{
    QTest::addColumn<bool>("deleteOther");
    QTest::newRow("two-removed-contexts") << false;
    QTest::newRow("other-manager-deleted-during-drain") << true;
}

void TestXdgShellWindow::testOutputRemovalBatch()
{
    QFETCH(bool, deleteOther);
    Test::setOutputConfig({Rect(0, 0, 1280, 1024), Rect(1280, 0, 1280, 1024), Rect(2560, 0, 1280, 1024)});
    const auto outputs = workspace()->outputs();
    const auto desktop = VirtualDesktopManager::self()->currentDesktop(outputs.first());
    std::array<TransientTileController, 2> controllers;
    std::array<QPointer<TileManager>, 2> managers;
    std::array<QPointer<Tile>, 2> roots;
    for (int i = 0; i < 2; ++i) {
        managers[i] = workspace()->tileManager(outputs[i]);
        QCOMPARE(controllers[i].createTree(managers[i], desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        roots[i] = controllers[i].rootTile();
    }
    TransientTileController probe;
    int drained = 0;
    int notified = 0;
    bool deletedOther = false;
    QObject callbackContext;
    for (int i = 0; i < 2; ++i) {
        connect(&controllers[i], &TransientTileController::treeChanged, &callbackContext, [&, i]() {
            if (controllers[i].rootTile()) {
                return;
            }
            ++drained;
            const int other = 1 - i;
            QVERIFY(probe.createTree(managers[other], desktop, RectF(0, 0, 0.5, 1)) != TransientTileController::Result::Accepted);
            if (deleteOther && !deletedOther) {
                deletedOther = true;
                delete managers[other].data();
            }
        });
    }
    connect(workspace(), &Workspace::outputRemoved, &callbackContext, [&](LogicalOutput *output) {
        const int index = outputs.indexOf(output);
        if (index >= 0 && index < 2) {
            ++notified;
            QVERIFY(!roots[index]);
            QVERIFY(!controllers[index].rootTile());
        }
    });
    OutputConfiguration config;
    config.changeSet(outputs[0]->backendOutput())->enabled = false;
    config.changeSet(outputs[1]->backendOutput())->enabled = false;
    QVERIFY(workspace()->applyOutputConfiguration(config).has_value());
    QCOMPARE(notified, 2);
    QCOMPARE(drained, 2);
    QVERIFY(!managers[0] && !managers[1]);
    QVERIFY(!probe.rootTile());
    QCOMPARE(workspace()->outputs(), QList<LogicalOutput *>{outputs.last()});
}

void TestXdgShellWindow::testOutputRemovalPreservesNewerTile()
{
    const auto outputs = workspace()->outputs();
    const auto removed = outputs.last();
    workspace()->setActiveOutput(removed);
    input()->pointer()->warp(removed->geometry().center());
    std::array<std::unique_ptr<KWayland::Client::Surface>, 2> surfaces;
    std::array<std::unique_ptr<Test::XdgToplevel>, 2> shells;
    std::array<QPointer<Window>, 2> windows;
    const auto source = workspace()->tileManager(removed)->quickTile(QuickTileFlag::Left);
    for (int i = 0; i < 2; ++i) {
        surfaces[i] = Test::createSurface();
        shells[i] = Test::createXdgToplevelSurface(surfaces[i].get());
        windows[i] = Test::renderAndWaitForShown(surfaces[i].get(), QSize(200, 100), Qt::red);
        QVERIFY(windows[i]);
        QVERIFY(source->manage(windows[i]));
    }
    const QPointer<Tile> destinationLeft = workspace()->tileManager(outputs.first())->quickTile(QuickTileFlag::Left);
    const QPointer<Tile> destinationRight = workspace()->tileManager(outputs.first())->quickTile(QuickTileFlag::Right);
    bool selectedNewerTile = false;
    QObject callbackContext;
    connect(destinationLeft, &Tile::windowAdded, &callbackContext, [&](Window *window) {
        if (window == windows[0] && !selectedNewerTile) {
            selectedNewerTile = true;
            QVERIFY(destinationRight->manage(windows[1]));
        }
    });
    OutputConfiguration config;
    config.changeSet(removed->backendOutput())->enabled = false;
    QVERIFY(workspace()->applyOutputConfiguration(config).has_value());
    QVERIFY(selectedNewerTile);
    QVERIFY(windows[0] && windows[1]);
    QCOMPARE(windows[0]->requestedTile(), destinationLeft.data());
    QCOMPARE(windows[1]->requestedTile(), destinationRight.data());
    QVERIFY(destinationRight->windows().contains(windows[1]));
}

void TestXdgShellWindow::testOutputRemovalMultiDesktop_data()
{
    QTest::addColumn<uint>("currentDesktop");
    QTest::newRow("active-before-inactive") << uint(1);
    QTest::newRow("inactive-before-active") << uint(2);
}

void TestXdgShellWindow::testOutputRemovalMultiDesktop()
{
    QFETCH(uint, currentDesktop);
    auto desktops = VirtualDesktopManager::self();
    const auto oldCount = desktops->count();
    const auto restore = qScopeGuard([desktops, oldCount]() {
        desktops->setCurrent(1);
        desktops->setCount(oldCount);
    });
    desktops->setCount(2);
    QVERIFY(desktops->setCurrent(currentDesktop) || desktops->current() == currentDesktop);
    const auto outputs = workspace()->outputs();
    const auto removed = outputs.last();
    workspace()->setActiveOutput(removed);
    input()->pointer()->warp(removed->geometry().center());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    window->setDesktops(desktops->desktops());
    const auto firstDesktop = desktops->desktops().first();
    const auto secondDesktop = desktops->desktops().last();
    const auto source = workspace()->tileManager(removed);
    QVERIFY(source->quickRootTile(firstDesktop)->tileForMode(QuickTileFlag::Left)->manage(window));
    QVERIFY(source->quickRootTile(secondDesktop)->tileForMode(QuickTileFlag::Right)->manage(window));
    const auto destination = workspace()->tileManager(outputs.first());
    const QPointer<Tile> firstTile = destination->quickRootTile(firstDesktop)->tileForMode(QuickTileFlag::Left);
    const QPointer<Tile> secondTile = destination->quickRootTile(secondDesktop)->tileForMode(QuickTileFlag::Right);
    OutputConfiguration config;
    config.changeSet(removed->backendOutput())->enabled = false;
    QVERIFY(workspace()->applyOutputConfiguration(config).has_value());
    QVERIFY(firstTile->windows().contains(window));
    QVERIFY(secondTile->windows().contains(window));
    QCOMPARE(window->requestedTile(), currentDesktop == 1 ? firstTile.data() : secondTile.data());
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

int main(int argc, char *argv[])
{
    setenv("QT_QPA_PLATFORM", "wayland-org.kde.kwin.qpa", true);
    setenv("QT_QPA_PLATFORM_PLUGIN_PATH", QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath().toLocal8Bit().constData(), true);
    setenv("KWIN_FORCE_OWN_QPA", "1", true);
    qunsetenv("KDE_FULL_SESSION");
    qunsetenv("KDE_SESSION_VERSION");
    qunsetenv("XDG_SESSION_DESKTOP");
    qunsetenv("XDG_CURRENT_DESKTOP");
    WorkspaceShutdownObservation observation;
    shutdownObservation = &observation;
    int result;
    {
        KWin::WaylandTestApplication app(argc, argv, false);
        qunsetenv("QT_QPA_PLATFORM");
        qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
        qunsetenv("KWIN_FORCE_OWN_QPA");
        app.setAttribute(Qt::AA_Use96Dpi, true);
        TestXdgShellWindow tc;
        result = QTest::qExec(&tc, argc, argv);
    }
    if (qEnvironmentVariableIsSet("KWIN_TEST_WORKSPACE_SHUTDOWN")) {
        observation.require(observation.armed, QStringLiteral("Shutdown fixture was not armed"));
        observation.require(observation.windowClosed, QStringLiteral("Window close not observed"));
        observation.require(observation.workspaceDestroyed, QStringLiteral("Workspace destruction not observed"));
        observation.require(observation.outputsRemoved == observation.managers.size(), QStringLiteral("Output removal incomplete"));
        if (observation.failures.isEmpty()) {
            // Keep the post-destructor verdict independent of Qt's message handler.
            std::puts("Workspace shutdown observations: PASS");
        } else {
            for (const auto &failure : observation.failures) {
                std::fprintf(stderr, "Workspace shutdown observation failed: %s\n", qPrintable(failure));
            }
            result = 1;
        }
    }
    return result;
}
#include "xdgshellwindow_test.moc"
