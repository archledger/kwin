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
#include "rules.h"
#include "tiles/customtile.h"
#include "tiles/tilemanager.h"
#include "tiles/transientplacementwriter.h"
#include "tiles/transienttilecontroller.h"
#include "virtualdesktops.h"
#include "wayland-xdg-shell-server-protocol.h"
#include "wayland/clientconnection.h"
#include "wayland/display.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "waylandwindow.h"
#include "window.h"
#include "workspace.h"
#include "xdgshellwindow.h"

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
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KPluginFactory>
#include <KPluginMetaData>

// system
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/input.h>

#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

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

    // The lifetime rows' request: its destructor records that it ran and may reenter the window
    // through a callback the test state owns, so a result that still borrowed the request would
    // read destroyed memory and a result that validates before the destruction would miss the
    // reentry. The authority is the state's, never the stack's.
    struct LifetimeState
    {
        QPointer<QObject> owner;
        bool valid = true;
        bool destroyed = false;
        std::function<void()> onDestroy;
    };

    class LifetimeRequestForTest : public Window::TransientTileRequest
    {
    public:
        LifetimeRequestForTest(Window *window, Tile *tile, std::shared_ptr<LifetimeState> state)
            : TransientTileRequest(window, tile, 0)
            , m_state(std::move(state))
        {
        }

        ~LifetimeRequestForTest() override
        {
            m_state->destroyed = true;
            if (m_state->onDestroy) {
                m_state->onDestroy();
            }
        }

    private:
        bool authorityIsCurrent() const override
        {
            return m_state->owner && m_state->valid;
        }
        std::shared_ptr<LifetimeState> m_state;
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
    void testDecorationBorderClientSize_data();
    void testDecorationBorderClientSize();
    void testDecorationBorderPendingConfigure_data();
    void testDecorationBorderPendingConfigure();
    void testDecorationBorderManagedFrame_data();
    void testDecorationBorderManagedFrame();
    void testDecorationBorderReentry_data();
    void testDecorationBorderReentry();
    void testDecorationBorderOutputBoundary();
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
    void testGuardedGeometryPublication();
    void testGuardedRequestedGeometry();
    void testGuardedRequestedGeometryReentry_data();
    void testGuardedRequestedGeometryReentry();
    void testGuardedRequestedGeometryBoundaries_data();
    void testGuardedRequestedGeometryBoundaries();
    void testGuardedGeometryRequest_data();
    void testGuardedGeometryRequest();
    void testGuardedGeometryRequestQueuedCompletion();
    void testGuardedGeometryRequestPendingResize();
    void testGuardedGeometryRequestReentry_data();
    void testGuardedGeometryRequestReentry();
    void testGuardedGeometryRequestBoundaries_data();
    void testGuardedGeometryRequestBoundaries();
    void testGuardedGeometryRequestRefusesOwnership();
    void testRequestedGeometryPropertyDrift();
    void testRequestedGeometryNullOutput();
    void testRequestedGeometryRefusesOwnership();
    void testRequestedGeometryRefusesActiveRequest();
    void testGuardedGeometryPublicationReentry_data();
    void testGuardedGeometryPublicationReentry();
    void testGuardedGeometryPublicationBoundaries_data();
    void testGuardedGeometryPublicationBoundaries();
    void testGuardedGeometryPublicationOutput();
    void testGuardedGeometryPublicationOutputRemoval();
    void testGeometryPublicationLegacy();
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
    void testOwnedRequestAuthority_data();
    void testOwnedRequestAuthority();
    void testOwnedRequestExhaustion_data();
    void testOwnedRequestExhaustion();
    void testOwnedMaximize_data();
    void testOwnedMaximize();
    void testOwnedMaximizeLegacy_data();
    void testOwnedMaximizeLegacy();
    void testOwnedMaximizeSupersession_data();
    void testOwnedMaximizeSupersession();
    void testOwnedMaximizeDecorated_data();
    void testOwnedMaximizeDecorated();
    void testOwnedFullScreen_data();
    void testOwnedFullScreen();
    void testOwnedFullScreenLegacy_data();
    void testOwnedFullScreenLegacy();
    void testOwnedFullScreenSupersession_data();
    void testOwnedFullScreenSupersession();
    void testOwnedFullScreenDecorated_data();
    void testOwnedFullScreenDecorated();
    void testNormalizeMaximize_data();
    void testNormalizeMaximize();
    void testNormalizeMaximizeSupersession_data();
    void testNormalizeMaximizeSupersession();
    void testCancellationPreparation_data();
    void testCancellationPreparation();
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
    void testTransientReceiptLifetime_data();
    void testTransientReceiptLifetime();
    void testTransientRestoreClaim_data();
    void testTransientRestoreClaim();
    void testTransientRestoreRequest_data();
    void testTransientRestoreRequest();
    void testMaximizeRestoreNormalization_data();
    void testMaximizeRestoreNormalization();
    void testMaximizeRestoreRecursion();
    void testConfigureSerialMatching_data();
    void testConfigureSerialMatching();
    void testConfigureDiscardReentry_data();
    void testConfigureDiscardReentry();
    void testConfigureCommitExhaustion_data();
    void testConfigureCommitExhaustion();
    void testTransientConfigurePlacement_data();
    void testTransientConfigurePlacement();
    void testConfigureContentHandoff_data();
    void testConfigureContentHandoff();
    void testTransientConfigurePublication_data();
    void testTransientConfigurePublication();
    void testTransientConfigureProvenance_data();
    void testTransientConfigureProvenance();
    void testTransientAssociationOrigin();
    void testControllerWindowAdmission();
    void testTransientAdmissionReceipt_data();
    void testTransientAdmissionReceipt();
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
    void testControllerForeignMembership_data();
    void testControllerForeignMembership();
    void testControllerClearAfterWriterLoss_data();
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
    void testControllerReleaseOrdering_data();
    void testControllerReleaseOrdering();
    void testCompatibilityCancellation_data();
    void testCompatibilityCancellation();
    void testTileAssignmentFenceNeutral();
    void testTileAssignmentFenceInvalidation_data();
    void testTileAssignmentFenceInvalidation();
    void testTileAssignmentFenceAssociations_data();
    void testTileAssignmentFenceAssociations();
    void testTileAssignmentFenceClose();
    void testTileAssignmentFenceCounters_data();
    void testTileAssignmentFenceCounters();
    void testGuardedTileCommit();
    void testGuardedTileCommitRefusal_data();
    void testGuardedTileCommitRefusal();
    void testGuardedTileCommitCapacity_data();
    void testGuardedTileCommitCapacity();
    void testGuardedTileCommitTransientTarget();
    void testGuardedTileCommitReentry_data();
    void testGuardedTileCommitReentry();
    void testGuardedGeometryRestore();
    void testGuardedGeometryRestoreRefusal_data();
    void testGuardedGeometryRestoreRefusal();
    void testGuardedGeometryRestoreReentry_data();
    void testGuardedGeometryRestoreReentry();
    void testGuardedGeometryRestoreSentinels();
    void testGeometryRestorePreservesOwnership();
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

    if (qEnvironmentVariableIsSet("KWIN_TEST_RESTORE_ACTIVITIES")) {
        kwinApp()->setUseKActivities(true);
    }
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

void TestXdgShellWindow::testDecorationBorderClientSize_data()
{
    QTest::addColumn<qreal>("scale");
    QTest::addColumn<QMarginsF>("borderDelta");
    QTest::addColumn<bool>("fixedSize");
    QTest::newRow("grow") << 1.0 << QMarginsF(0, 3, 0, 0) << true;
    QTest::newRow("shrink") << 1.0 << QMarginsF(0, -3, 0, 0) << true;
    QTest::newRow("grow-fractional") << (4.0 / 3.0) << QMarginsF(0, 2.25, 0, 0) << true;
    QTest::newRow("shrink-fractional") << (4.0 / 3.0) << QMarginsF(0, -2.25, 0, 0) << true;
    QTest::newRow("resizable") << 1.0 << QMarginsF(0, 3, 0, 0) << false;
    QTest::newRow("resizable-horizontal") << 1.0 << QMarginsF(3, 0, 2, 0) << false;
    QTest::newRow("resizable-fractional") << (4.0 / 3.0) << QMarginsF(1.5, 2.25, 0.75, 0) << false;
    QTest::newRow("unchanged") << 1.0 << QMarginsF() << false;
}

void TestXdgShellWindow::testDecorationBorderClientSize()
{
    QFETCH(qreal, scale);
    QFETCH(QMarginsF, borderDelta);
    QFETCH(bool, fixedSize);
    Test::setOutputConfig({Test::OutputInfo{.geometry = Rect(0, 0, 1280, 1024), .scale = scale}});

    const QSize clientSize(360, 240);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    if (fixedSize) {
        shell->set_min_size(clientSize.width(), clientSize.height());
        shell->set_max_size(clientSize.width(), clientSize.height());
    }
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    configure.clear();
    auto window = Test::renderAndWaitForShown(surface.get(), clientSize, Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QCOMPARE(window->targetScale(), scale);
    QCOMPARE(window->clientSize(), QSizeF(clientSize));

    // Consume the activation configure before changing only the decoration's borders.
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), clientSize, Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    toplevelConfigure.clear();
    const auto originalGeometry = window->frameGeometry();
    const auto originalBorders = window->decoration()->currentState()->borders();
    auto state = window->nextDecoration()->nextState();
    state->setBorders(state->borders() + borderDelta);
    Q_EMIT window->nextDecoration()->nextStateChanged(state);
    QVERIFY(configure.wait());
    QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), clientSize);
    const QSizeF frameDelta(borderDelta.left() + borderDelta.right(), borderDelta.top() + borderDelta.bottom());
    QCOMPARE(window->moveResizeGeometry().size(), originalGeometry.size() + frameDelta);
    QCOMPARE(window->frameGeometry(), originalGeometry);
    QCOMPARE(window->clientSize(), QSizeF(clientSize));
    QCOMPARE(window->decoration()->currentState()->borders(), originalBorders);
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), clientSize, Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->clientSize(), QSizeF(clientSize));
    QCOMPARE(window->frameGeometry().topLeft(), originalGeometry.topLeft());
    QCOMPARE(window->frameGeometry().size(), originalGeometry.size() + frameDelta);

    // Repeated enter/leave cycles must not accumulate frame or client size drift.
    configure.clear();
    state = window->nextDecoration()->nextState();
    state->setBorders(originalBorders);
    Q_EMIT window->nextDecoration()->nextStateChanged(state);
    QVERIFY(configure.wait());
    QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), clientSize);
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), clientSize, Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->frameGeometry(), originalGeometry);
    QCOMPARE(window->clientSize(), QSizeF(clientSize));
}

void TestXdgShellWindow::testDecorationBorderPendingConfigure_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("queued-resize") << 0;
    QTest::newRow("coalesced-borders") << 1;
    QTest::newRow("unacknowledged-borders") << 2;
}

void TestXdgShellWindow::testDecorationBorderPendingConfigure()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    configure.clear();
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const auto originalGeometry = window->frameGeometry();
    const auto originalBorders = window->decoration()->currentState()->borders();
    const QSize targetClient = scenario == 0 ? QSize(520, 360) : QSize(360, 240);
    const QSizeF targetFrame = window->nextClientSizeToFrameSize(targetClient);

    if (scenario == 0) {
        window->resize(targetFrame);
    } else {
        auto state = window->nextDecoration()->nextState();
        state->setBorders(originalBorders + QMarginsF(0, 3, 0, 0));
        Q_EMIT window->nextDecoration()->nextStateChanged(state);
    }
    quint32 olderSerial = 0;
    if (scenario != 1) {
        QVERIFY(configure.wait());
        olderSerial = configure.last().at(0).toUInt();
        QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), targetClient);
        configure.clear();
    }
    auto state = window->nextDecoration()->nextState();
    state->setBorders(originalBorders + QMarginsF(0, 5, 0, 0));
    Q_EMIT window->nextDecoration()->nextStateChanged(state);
    QVERIFY(configure.wait());
    const quint32 latestSerial = configure.last().at(0).toUInt();
    QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), targetClient);
    QCOMPARE(window->frameGeometry(), originalGeometry);
    QCOMPARE(window->moveResizeGeometry().size(), targetFrame + QSizeF(0, 5));
    QCOMPARE(window->decoration()->currentState()->borders(), originalBorders);

    if (olderSerial) {
        shell->xdgSurface()->ack_configure(olderSerial);
        Test::render(surface.get(), targetClient, Qt::yellow);
        QVERIFY(Test::waylandSync());
        // An older configure may update content, but cannot roll back the latest request.
        QCOMPARE(window->moveResizeGeometry().size(), targetFrame + QSizeF(0, 5));
    }
    shell->xdgSurface()->ack_configure(latestSerial);
    Test::render(surface.get(), targetClient, Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->clientSize(), QSizeF(targetClient));
    QCOMPARE(window->frameGeometry().size(), targetFrame + QSizeF(0, 5));
    QCOMPARE(window->frameGeometry().topLeft(), originalGeometry.topLeft());
    QCOMPARE(window->moveResizeGeometry(), window->frameGeometry());
}

void TestXdgShellWindow::testDecorationBorderManagedFrame_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("phase");
    for (const auto &[mode, state] : {std::pair{MaximizeRestore, "tiled"}, std::pair{MaximizeHorizontal, "maximized-horizontal"},
                                      std::pair{MaximizeVertical, "maximized-vertical"}, std::pair{MaximizeFull, "maximized-full"}}) {
        const std::array<QByteArray, 3> phases = {"requested", "committed", "restore-pending"};
        for (int phase = 0; phase < 3; ++phase) {
            const QByteArray name = QByteArray(state) + '-' + phases[phase];
            QTest::newRow(name.constData()) << int(mode) << phase;
        }
    }
}

void TestXdgShellWindow::testDecorationBorderManagedFrame()
{
    QFETCH(int, mode);
    QFETCH(int, phase);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    configure.clear();
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();

    // mode 0 exercises tiling; the other modes cover partial and full maximization.
    if (mode == int(MaximizeRestore)) {
        window->setQuickTileModeAtCurrentPosition(QuickTileFlag::Left);
    } else {
        window->maximize(MaximizeMode(mode));
    }
    QVERIFY(configure.wait());
    if (phase != 0) {
        shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::blue);
        QVERIFY(Test::waylandSync());
    }
    configure.clear();
    if (phase == 2) {
        if (mode == int(MaximizeRestore)) {
            window->setQuickTileModeAtCurrentPosition(QuickTileFlag::None);
        } else {
            window->maximize(MaximizeRestore);
        }
        QVERIFY(configure.wait());
        configure.clear();
    }
    const auto requested = window->moveResizeGeometry();
    const auto committed = window->frameGeometry();
    const QSizeF requestedClient = window->nextFrameSizeToClientSize(requested.size());
    auto state = window->nextDecoration()->nextState();
    state->setBorders(state->borders() + QMarginsF(0, 3, 0, 0));
    Q_EMIT window->nextDecoration()->nextStateChanged(state);
    QVERIFY(configure.wait());
    // Frame-controlled states retain their target, even while entering or leaving them.
    QCOMPARE(window->moveResizeGeometry(), requested);
    QCOMPARE(window->frameGeometry(), committed);
    QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), (requestedClient - QSizeF(0, 3)).toSize());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->frameGeometry(), requested);
}

void TestXdgShellWindow::testDecorationBorderReentry_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("notify-move") << 0;
    QTest::newRow("notify-resize") << 1;
    QTest::newRow("notify-close") << 2;
    QTest::newRow("resize-in-progress") << 3;
    QTest::newRow("interactive-resize") << 4;
}

void TestXdgShellWindow::testDecorationBorderReentry()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    configure.clear();
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const auto original = window->frameGeometry();
    QPointer<Window> guard(window);
    QObject context;
    bool called = false;
    RectF expected = original;
    auto changeBorders = [&]() {
        auto state = window->nextDecoration()->nextState();
        state->setBorders(state->borders() + QMarginsF(0, 3, 0, 0));
        Q_EMIT window->nextDecoration()->nextStateChanged(state);
    };
    if (scenario <= 2) {
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (called) {
                return;
            }
            called = true;
            if (scenario == 2) {
                window->setSkipCloseAnimation(true);
                window->destroyWindow();
            } else {
                expected = RectF(QPointF(45, 60), scenario == 0 ? window->moveResizeGeometry().size() : window->nextClientSizeToFrameSize(QSize(520, 360)));
                window->moveResize(expected);
            }
        });
        changeBorders();
        QVERIFY(called);
        if (scenario == 2) {
            QVERIFY(guard.isNull());
            return;
        }
    } else if (scenario == 3) {
        connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
            if (!called) {
                called = true;
                changeBorders();
            }
        });
        expected.setSize(window->nextClientSizeToFrameSize(QSize(520, 360)));
        window->resize(expected.size());
        QVERIFY(called);
    } else {
        window->setInteractiveMoveResizeGravity(Gravity::BottomRight);
        QVERIFY(window->startInteractiveMoveResize());
        changeBorders();
    }
    QVERIFY(!configure.isEmpty() || configure.wait());
    QCOMPARE(window->moveResizeGeometry(), expected);
    const QSize expectedClient = window->nextFrameSizeToClientSize(expected.size()).toSize();
    QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), expectedClient);
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), expectedClient, Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->frameGeometry(), expected);
    if (scenario == 4) {
        window->finishInteractiveMoveResize(false);
    }
}

void TestXdgShellWindow::testDecorationBorderOutputBoundary()
{
    Test::setOutputConfig({
        Test::OutputInfo{.geometry = Rect(0, 0, 1280, 1200), .scale = 4.0 / 3.0},
        Test::OutputInfo{.geometry = Rect(0, 1200, 1280, 1200), .scale = 1.0},
    });
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    configure.clear();
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    auto state = window->nextDecoration()->nextState();
    state->setBorders(QMarginsF(0, 50.25, 0, 0));
    Q_EMIT window->nextDecoration()->nextStateChanged(state);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), QSize(360, 240), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    window->move(QPointF(100, 1053.375));
    const auto original = window->moveResizeGeometry();
    QCOMPARE(original.height(), 290.25);
    QCOMPARE(window->nextTargetScale(), 4.0 / 3.0);
    auto originalOutput = window->moveResizeOutput();
    QCOMPARE(originalOutput, workspace()->outputs().first());

    // Pixel-rounded tablet borders feed back into output selection if compensation moves
    // the frame center across the boundary. Bound the callback so a regression cannot hang.
    QObject context;
    int scaleChanges = 0;
    connect(window, &Window::nextTargetScaleChanged, &context, [&]() {
        if (++scaleChanges < 6) {
            const qreal scale = window->nextTargetScale();
            auto next = window->nextDecoration()->nextState();
            next->setBorders(QMarginsF(0, std::ceil(52 * scale) / scale, 0, 0));
            Q_EMIT window->nextDecoration()->nextStateChanged(next);
        }
    });
    state = window->nextDecoration()->nextState();
    state->setBorders(QMarginsF(0, 52.5, 0, 0));
    Q_EMIT window->nextDecoration()->nextStateChanged(state);
    QVERIFY(!configure.isEmpty() || configure.wait());
    QCOMPARE(scaleChanges, 0);
    QCOMPARE(window->moveResizeOutput(), originalOutput);
    QCOMPARE(window->moveResizeGeometry(), original);
    QCOMPARE(window->frameGeometry(), original);
    shell->xdgSurface()->ack_configure(configure.last().at(0).toUInt());
    Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(Test::waylandSync());
    QCOMPARE(scaleChanges, 0);
    QCOMPARE(window->moveResizeOutput(), originalOutput);
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

void TestXdgShellWindow::testGuardedRequestedGeometry()
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
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    const auto frame = window->frameGeometry();
    const auto buffer = window->bufferGeometry();
    const auto client = window->clientGeometry();
    const auto generation = window->m_geometryRequestGeneration;
    const auto revision = window->m_placementContinuationRevision;
    const auto oldTransform = window->preferredBufferTransform();
    const auto oldColor = window->preferredColorDescription();
    QVERIFY(!window->m_outputPropertiesPending);
    const RectF desired(1400, 100, 200, 100);
    const quint64 writes = (window->moveResizeGeometry() != desired) + (window->moveResizeOutput() != destination)
        + (window->nextTargetScale() != destination->scale()) + (oldTransform != destination->transform())
        + (oldColor != destination->blendingColor()) + 2;
    QSignalSpy requestedChanged(window, &Window::requestedGeometryChanged);
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    QSignalSpy scaleChanged(window, &Window::nextTargetScaleChanged);
    QObject context;
    bool scaleSawOldProperties = false;
    connect(window, &Window::nextTargetScaleChanged, &context, [&]() {
        scaleSawOldProperties = window->preferredBufferTransform() == oldTransform && window->preferredColorDescription() == oldColor;
    });
    auto before = window->makeTileAssignmentFence();
    auto result = window->setMoveResizeGeometryForAssignment(desired, before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QVERIFY(!before.isCurrentFor(*window));
    QCOMPARE(window->moveResizeGeometry(), desired);
    QCOMPARE(window->moveResizeOutput(), destination);
    QCOMPARE(window->nextTargetScale(), destination->scale());
    QCOMPARE(window->preferredBufferTransform(), destination->transform());
    QCOMPARE(window->preferredColorDescription(), destination->blendingColor());
    QCOMPARE(window->m_placementContinuationRevision, revision + writes);
    QCOMPARE(window->m_geometryRequestGeneration, generation);
    QCOMPARE(window->frameGeometry(), frame);
    QCOMPARE(window->bufferGeometry(), buffer);
    QCOMPARE(window->clientGeometry(), client);
    QVERIFY(!window->m_outputPropertiesPending);
    QVERIFY(requestedChanged.isEmpty() && frameChanged.isEmpty());
    QCOMPARE(scaleChanged.count(), 1);
    QVERIFY(scaleSawOldProperties);
    auto noop = window->setMoveResizeGeometryForAssignment(desired, *result.checkpoint);
    QCOMPARE(noop.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(noop.checkpoint && noop.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->m_placementContinuationRevision, revision + writes);
    QCOMPARE(scaleChanged.count(), 1);
}

void TestXdgShellWindow::testGuardedRequestedGeometryReentry_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("restore") << 0;
    QTest::newRow("committed-tile") << 1;
    QTest::newRow("equal-request") << 2;
    QTest::newRow("output-roundtrip") << 3;
    QTest::newRow("output-destroyed") << 4;
    QTest::newRow("input-owner") << 5;
    QTest::newRow("input-owner-close") << 6;
    QTest::newRow("copied-input") << 7;
    QTest::newRow("nested-completion") << 8;
    QTest::newRow("foreign-scale") << 9;
}

void TestXdgShellWindow::testGuardedRequestedGeometryReentry()
{
    QFETCH(int, action);
    auto destination = workspace()->outputs()[1];
    QPointer<LogicalOutput> destinationGuard(destination);
    OutputConfiguration configuration;
    auto changes = configuration.changeSet(destination->backendOutput());
    changes->scale = 2.0;
    changes->scaleSetting = 2.0;
    changes->transform = OutputTransform::Kind::Rotate180;
    QVERIFY(workspace()->applyOutputConfiguration(configuration).has_value());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    if (action == 8) {
        window->setPreferredBufferTransform(destination->transform());
        window->setPreferredColorDescription(destination->blendingColor());
    }
    const auto originalOutput = window->moveResizeOutput();
    const auto originalTransform = window->preferredBufferTransform();
    const auto originalColor = window->preferredColorDescription();
    const RectF desired(1400, 100, 200, 100);
    RectF callerValue = desired;
    auto tile = std::make_unique<Tile>(workspace()->tileManager(originalOutput));
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence(window->makeTileAssignmentFence()));
    QPointer<Window> guard(window);
    QObject context;
    bool called = false;
    bool nestedCurrent = false;
    connect(window, &Window::nextTargetScaleChanged, &context, [&]() {
        if (called) {
            return;
        }
        called = true;
        switch (action) {
        case 0:
            window->setGeometryRestore(RectF(30, 40, 220, 120));
            break;
        case 1:
            window->commitTile(tile.get());
            break;
        case 2:
            window->moveResize(window->moveResizeGeometry());
            break;
        case 3:
            window->setMoveResizeOutput(originalOutput);
            window->setMoveResizeOutput(destination);
            break;
        case 4:
            Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
            break;
        case 5:
            before.reset();
            break;
        case 6:
            before.reset();
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            QVERIFY(guard);
            break;
        case 7:
            callerValue = RectF(50, 60, 200, 100);
            break;
        case 8: {
            QVERIFY(window->m_outputPropertiesPending);
            QCOMPARE(window->preferredBufferTransform(), destination->transform());
            QCOMPARE(window->preferredColorDescription(), destination->blendingColor());
            const auto nestedRevision = window->m_placementContinuationRevision;
            auto nestedBefore = window->makeTileAssignmentFence();
            auto nested = window->setMoveResizeGeometryForAssignment(desired, nestedBefore);
            nestedCurrent = nested.outcome == Window::AssignmentStepOutcome::AppliedCurrent && nested.checkpoint && nested.checkpoint->isCurrentFor(*window);
            QCOMPARE(window->m_placementContinuationRevision, nestedRevision + 1);
            break;
        }
        case 9:
            window->setNextTargetScale(3.0);
            break;
        }
    });
    auto result = window->setMoveResizeGeometryForAssignment(callerValue, *before);
    QVERIFY(called);
    const bool current = action == 5 || action == 7;
    QCOMPARE(result.outcome, current ? Window::AssignmentStepOutcome::AppliedCurrent : Window::AssignmentStepOutcome::Superseded);
    QCOMPARE(bool(result.checkpoint), current);
    if (action == 6) {
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    if (action == 4) {
        QVERIFY(!destinationGuard);
        return;
    }
    QCOMPARE(window->moveResizeGeometry(), desired);
    QCOMPARE(window->moveResizeOutput(), destination);
    if (action == 0 || action == 1 || action == 9) {
        QVERIFY(window->m_outputPropertiesPending);
        QCOMPARE(window->preferredBufferTransform(), originalTransform);
        QCOMPARE(window->preferredColorDescription(), originalColor);
    }
    if (action == 8) {
        QVERIFY(nestedCurrent);
    }
    if (action == 9) {
        QCOMPARE(window->nextTargetScale(), 3.0);
    }
    auto resumeBefore = window->makeTileAssignmentFence();
    auto resumed = window->setMoveResizeGeometryForAssignment(desired, resumeBefore);
    QCOMPARE(resumed.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(resumed.checkpoint && resumed.checkpoint->isCurrentFor(*window));
    QVERIFY(!window->m_outputPropertiesPending);
    QCOMPARE(window->nextTargetScale(), destination->scale());
    QCOMPARE(window->preferredBufferTransform(), destination->transform());
    QCOMPARE(window->preferredColorDescription(), destination->blendingColor());
}

void TestXdgShellWindow::testGuardedRequestedGeometryBoundaries_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("stale") << 0;
    QTest::newRow("wrong-window") << 1;
    QTest::newRow("budget-refused") << 2;
    QTest::newRow("exact-budget") << 3;
    QTest::newRow("last-noop") << 4;
    QTest::newRow("saturated") << 5;
    QTest::newRow("pending-budget") << 6;
    QTest::newRow("pending-refused") << 7;
}

void TestXdgShellWindow::testGuardedRequestedGeometryBoundaries()
{
    QFETCH(int, scenario);
    auto destination = workspace()->outputs()[1];
    OutputConfiguration configuration;
    auto changes = configuration.changeSet(destination->backendOutput());
    changes->scale = 2.0;
    changes->scaleSetting = 2.0;
    changes->transform = OutputTransform::Kind::Rotate180;
    QVERIFY(workspace()->applyOutputConfiguration(configuration).has_value());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::Surface> otherSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> otherShell(Test::createXdgToplevelSurface(otherSurface.get()));
    auto other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(other);
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    const RectF desired(1400, 100, 200, 100);
    if (scenario >= 4) {
        auto setup = window->makeTileAssignmentFence();
        auto result = window->setMoveResizeGeometryForAssignment(desired, setup);
        QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    }
    if (scenario >= 6) {
        window->setOutputPropertiesPending(true);
    }
    const auto exhausted = std::numeric_limits<quint64>::max();
    if (scenario == 2) {
        window->m_placementContinuationRevision = exhausted - 7;
    } else if (scenario == 3) {
        window->m_placementContinuationRevision = exhausted - 8;
    } else if (scenario == 4 || scenario == 7) {
        window->m_placementContinuationRevision = exhausted - 1;
    } else if (scenario == 5) {
        window->m_placementContinuationRevision = exhausted;
    } else if (scenario == 6) {
        window->m_placementContinuationRevision = exhausted - 2;
    }
    auto before = (scenario == 1 ? other : window)->makeTileAssignmentFence();
    if (scenario == 0) {
        window->setGeometryRestore(RectF(10, 20, 200, 100));
    }
    const auto originalGeometry = window->moveResizeGeometry();
    const auto originalOutput = window->moveResizeOutput();
    const auto originalScale = window->nextTargetScale();
    const auto originalTransform = window->preferredBufferTransform();
    const auto originalColor = window->preferredColorDescription();
    const auto originalPending = window->m_outputPropertiesPending;
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryRequestGeneration;
    QSignalSpy scaleChanged(window, &Window::nextTargetScaleChanged);
    auto result = window->setMoveResizeGeometryForAssignment(desired, before);
    auto expected = Window::AssignmentStepOutcome::AppliedCurrent;
    if (scenario == 0 || scenario == 1 || scenario == 5) {
        expected = Window::AssignmentStepOutcome::Superseded;
    } else if (scenario == 2 || scenario == 7) {
        expected = Window::AssignmentStepOutcome::Refused;
    }
    QCOMPARE(result.outcome, expected);
    QCOMPARE(bool(result.checkpoint), expected == Window::AssignmentStepOutcome::AppliedCurrent);
    QCOMPARE(window->m_geometryRequestGeneration, generation);
    if (expected != Window::AssignmentStepOutcome::AppliedCurrent) {
        QCOMPARE(window->moveResizeGeometry(), originalGeometry);
        QCOMPARE(window->moveResizeOutput(), originalOutput);
        QCOMPARE(window->nextTargetScale(), originalScale);
        QCOMPARE(window->preferredBufferTransform(), originalTransform);
        QCOMPARE(window->preferredColorDescription(), originalColor);
        QCOMPARE(window->m_outputPropertiesPending, originalPending);
        QCOMPARE(window->m_placementContinuationRevision, revision);
        QVERIFY(scaleChanged.isEmpty());
    } else {
        QVERIFY(result.checkpoint->isCurrentFor(*window));
        QCOMPARE(window->moveResizeGeometry(), desired);
        QCOMPARE(window->moveResizeOutput(), destination);
        QCOMPARE(window->nextTargetScale(), destination->scale());
        QCOMPARE(window->preferredBufferTransform(), destination->transform());
        QCOMPARE(window->preferredColorDescription(), destination->blendingColor());
        QVERIFY(!window->m_outputPropertiesPending);
        quint64 writes = 0;
        if (scenario == 3) {
            writes = 7;
        } else if (scenario == 6) {
            writes = 1;
        }
        QCOMPARE(window->m_placementContinuationRevision, revision + writes);
    }
}

void TestXdgShellWindow::testGuardedGeometryRequest_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("move") << 0;
    QTest::newRow("noop") << 1;
    QTest::newRow("queued") << 2;
    QTest::newRow("cross-output") << 3;
}

void TestXdgShellWindow::testGuardedGeometryRequest()
{
    QFETCH(int, scenario);
    auto destination = workspace()->outputs()[1];
    OutputConfiguration configuration;
    auto changes = configuration.changeSet(destination->backendOutput());
    changes->scale = 2.0;
    changes->scaleSetting = 2.0;
    changes->transform = OutputTransform::Kind::Rotate180;
    QVERIFY(workspace()->applyOutputConfiguration(configuration).has_value());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    // The helper already consumed the initial configure; settle the activation one too.
    QVERIFY(!configure.isEmpty() || configure.wait());
    const int settledConfigures = configure.count();
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->hasPendingNativeGeometry());
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    const RectF frame = window->frameGeometry();
    QCOMPARE(window->moveResizeGeometry(), frame);
    RectF desired = frame;
    auto mode = Window::MoveResizeMode::MoveResize;
    bool published = true;
    switch (scenario) {
    case 0:
        desired = frame.translated(50, 60);
        mode = Window::MoveResizeMode::Move;
        break;
    case 1:
        break;
    case 2:
        desired = RectF(frame.topLeft(), QSizeF(300, 150));
        mode = Window::MoveResizeMode::Resize;
        published = false;
        break;
    case 3:
        desired = RectF(1400, 100, 300, 150);
        published = false;
        break;
    }
    const bool changed = desired != frame;
    // The synchronizer's owned writes, counted as prepareAssignmentGeometrySync() does. The
    // preferred colour set above differs from every output, so same-output requests resync too.
    const auto target = workspace()->outputAt(desired.center());
    QVERIFY(target);
    QVERIFY(!window->m_outputPropertiesPending);
    const bool outputChanged = window->moveResizeOutput() != target;
    const bool scaleChanged = window->nextTargetScale() != target->scale();
    const bool transformChanged = window->preferredBufferTransform() != target->transform();
    const bool colorChanged = window->preferredColorDescription() != target->blendingColor();
    quint64 writes = changed + outputChanged;
    if (outputChanged || scaleChanged || transformChanged || colorChanged) {
        writes += 2 + scaleChanged + transformChanged + colorChanged;
    }
    if (scenario == 3) {
        QCOMPARE(target, destination);
    }
    const auto generation = window->m_geometryRequestGeneration;
    const auto publication = window->m_geometryUpdateGeneration;
    const auto revision = window->m_placementContinuationRevision;
    QSignalSpy requestedChanged(window, &Window::requestedGeometryChanged);
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    QSignalSpy aboutToChange(window, &Window::frameGeometryAboutToChange);
    auto before = window->makeTileAssignmentFence();
    auto request = window->requestMoveResizeForAssignment(desired, mode, before);
    QCOMPARE(request.result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(request.result.checkpoint && request.result.checkpoint->isCurrentFor(*window));
    QVERIFY(!before.isCurrentFor(*window));
    QCOMPARE(request.dispatch, published ? Window::AssignmentRequestDispatch::Published : Window::AssignmentRequestDispatch::Queued);
    QCOMPARE(request.requestGeneration, generation + 1);
    QCOMPARE(window->m_geometryRequestGeneration, generation + 1);
    QVERIFY(!window->m_geometryResizePending);
    QCOMPARE(aboutToChange.count(), 1);
    QCOMPARE(window->moveResizeGeometry(), desired);
    QCOMPARE(requestedChanged.count(), changed ? 1 : 0);
    if (published) {
        QCOMPARE(window->frameGeometry(), desired);
        QCOMPARE(frameChanged.count(), changed ? 1 : 0);
        QCOMPARE(window->m_geometryUpdateGeneration, publication + (changed ? 1 : 0));
        QCOMPARE(window->m_placementContinuationRevision, revision + writes + (changed ? 1 : 0));
        // Published stages no configure of its own. A property hook may still have scheduled one; since 6.8 every
        // configure carries moveResizeGeometry() as its bounds, the rectangle asserted above.
    } else {
        // Queued is staged for the coalesced configure: nothing committed and nothing sent yet.
        QCOMPARE(window->frameGeometry(), frame);
        QVERIFY(frameChanged.isEmpty());
        QCOMPARE(window->m_geometryUpdateGeneration, publication);
        QCOMPARE(window->m_placementContinuationRevision, revision + writes);
        QVERIFY(window->hasPendingNativeGeometry());
        QVERIFY(window->m_configureTimer->isActive());
        // The running timer is the staging: since 6.8 the configure carries moveResizeGeometry() as its bounds.
        QCOMPARE(configure.count(), settledConfigures);
    }
    if (scenario == 3) {
        QCOMPARE(window->moveResizeOutput(), destination);
        QCOMPARE(window->nextTargetScale(), destination->scale());
        QCOMPARE(window->preferredBufferTransform(), destination->transform());
        QCOMPARE(window->preferredColorDescription(), destination->blendingColor());
        QVERIFY(!window->m_outputPropertiesPending);
    }
}

void TestXdgShellWindow::testGuardedGeometryRequestQueuedCompletion()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    // The helper already consumed the initial configure; settle the activation one too.
    QVERIFY(!configure.isEmpty() || configure.wait());
    const int settledConfigures = configure.count();
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->hasPendingNativeGeometry());
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    const RectF frame = window->frameGeometry();
    const RectF desired(frame.topLeft() + QPointF(40, 30), QSizeF(300, 150));
    auto before = window->makeTileAssignmentFence();
    auto request = window->requestMoveResizeForAssignment(desired, Window::MoveResizeMode::MoveResize, before);
    QCOMPARE(request.result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QCOMPARE(request.dispatch, Window::AssignmentRequestDispatch::Queued);
    QVERIFY(request.result.checkpoint && request.result.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->frameGeometry(), frame);
    // The coalesced configure carries the requested size. Acknowledging and rendering it completes
    // the request through the ordinary commit path, not through the step.
    QVERIFY(toplevelConfigure.wait());
    QCOMPARE(toplevelConfigure.last().at(0).toSize(), QSize(300, 150));
    QVERIFY(configure.count() == settledConfigures + 1 || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(300, 150), Qt::red);
    QVERIFY(frameChanged.wait());
    QCOMPARE(window->frameGeometry(), desired);
    QCOMPARE(window->moveResizeGeometry(), desired);
    QCOMPARE(window->m_geometryRequestGeneration, request.requestGeneration);
    QVERIFY(!window->hasPendingNativeGeometry());
    // Completion changed the committed geometry, which is what lapses the queued checkpoint; an
    // acknowledgement that changed nothing would not.
    QVERIFY(!request.result.checkpoint->isCurrentFor(*window));
}

void TestXdgShellWindow::testGuardedGeometryRequestPendingResize()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    // The helper already consumed the initial configure; settle the activation one too.
    QVERIFY(!configure.isEmpty() || configure.wait());
    const int settledConfigures = configure.count();
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->hasPendingNativeGeometry());
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    const RectF frame = window->frameGeometry();
    const QSizeF pendingSize(300, 150);
    const QPointF position = frame.topLeft() + QPointF(40, 30);
    const auto generation = window->m_geometryRequestGeneration;
    Window::AssignmentGeometryRequest nested;
    bool pendingSeen = false;
    bool called = false;
    QObject context;
    connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
        if (called) {
            return;
        }
        called = true;
        // The legacy resize has stored its size but not submitted it yet.
        pendingSeen = window->m_geometryResizePending;
        auto before = window->makeTileAssignmentFence();
        nested = window->requestMoveResizeForAssignment(RectF(position, pendingSize), Window::MoveResizeMode::Move, before);
    });
    window->resize(pendingSize);
    QVERIFY(called);
    QVERIFY(pendingSeen);
    // The nested Move inherited the unsubmitted resize: promoted, it queued the size instead of
    // publishing the position immediately at the old size.
    QCOMPARE(nested.result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QCOMPARE(nested.dispatch, Window::AssignmentRequestDispatch::Queued);
    QCOMPARE(nested.requestGeneration, generation + 2);
    QCOMPARE(window->m_geometryRequestGeneration, generation + 2);
    QVERIFY(!window->m_geometryResizePending);
    QCOMPARE(window->frameGeometry(), frame);
    QVERIFY(frameChanged.isEmpty());
    QCOMPARE(window->moveResizeGeometry(), RectF(position, pendingSize));
    QVERIFY(toplevelConfigure.wait());
    QCOMPARE(toplevelConfigure.last().at(0).toSize(), pendingSize.toSize());
    QVERIFY(configure.count() == settledConfigures + 1 || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), pendingSize.toSize(), Qt::red);
    QVERIFY(frameChanged.wait());
    QCOMPARE(window->frameGeometry(), RectF(position, pendingSize));
}

void TestXdgShellWindow::testGuardedGeometryRequestReentry_data()
{
    QTest::addColumn<int>("hook");
    QTest::addColumn<int>("action");
    QTest::addColumn<bool>("current");
    QTest::addColumn<int>("dispatch");
    QTest::addColumn<bool>("nestedRequest");
    QTest::newRow("nested-legacy-about") << 0 << 0 << false << 0 << true;
    QTest::newRow("nested-drain-exhausting") << 0 << 1 << false << 0 << false;
    QTest::newRow("nested-drain-headroom") << 0 << 2 << true << 1 << false;
    QTest::newRow("close-about") << 0 << 3 << false << 0 << false;
    QTest::newRow("equal-nested-about") << 0 << 4 << false << 0 << true;
    QTest::newRow("copied-input") << 0 << 5 << true << 1 << false;
    QTest::newRow("output-destroyed") << 1 << 6 << false << 0 << false;
    QTest::newRow("input-fence") << 1 << 7 << true << 2 << false;
    QTest::newRow("restore") << 1 << 8 << false << 0 << false;
    QTest::newRow("restore-about") << 0 << 8 << false << 0 << false;
    QTest::newRow("nested-legacy-frame") << 2 << 0 << false << 1 << true;
    QTest::newRow("nested-legacy-requested") << 3 << 0 << false << 1 << true;
}

void TestXdgShellWindow::testGuardedGeometryRequestReentry()
{
    QFETCH(int, hook);
    QFETCH(int, action);
    QFETCH(bool, current);
    QFETCH(int, dispatch);
    QFETCH(bool, nestedRequest);
    auto destination = workspace()->outputs()[1];
    QPointer<LogicalOutput> destinationGuard(destination);
    OutputConfiguration configuration;
    auto changes = configuration.changeSet(destination->backendOutput());
    changes->scale = 2.0;
    changes->scaleSetting = 2.0;
    changes->transform = OutputTransform::Kind::Rotate180;
    QVERIFY(workspace()->applyOutputConfiguration(configuration).has_value());
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    // The helper already consumed the initial configure; settle the activation one too.
    QVERIFY(!configure.isEmpty() || configure.wait());
    const int settledConfigures = configure.count();
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->hasPendingNativeGeometry());
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    const auto exhausted = std::numeric_limits<quint64>::max();
    const RectF frame = window->frameGeometry();
    const RectF desired = hook == 1 ? RectF(1400, 100, 300, 150) : frame.translated(50, 60);
    const auto mode = hook == 1 ? Window::MoveResizeMode::MoveResize : Window::MoveResizeMode::Move;
    const RectF replacement = frame.translated(120, 140);
    RectF callerValue = desired;
    if (action == 1 || action == 2) {
        window->m_pendingGeometryNotifications.frame = frame.translated(-10, 0);
    }
    if (action == 1) {
        window->m_geometryUpdateGeneration = exhausted - 2;
    }
    const auto generation = window->m_geometryRequestGeneration;
    const auto publication = window->m_geometryUpdateGeneration;
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence(window->makeTileAssignmentFence()));
    QPointer<Window> guard(window);
    QSignalSpy requestedChanged(window, &Window::requestedGeometryChanged);
    QSignalSpy aboutToChange(window, &Window::frameGeometryAboutToChange);
    QObject context;
    bool called = false;
    bool drained = false;
    Window::AssignmentGeometryRequest nested;
    const auto interrupt = [&]() {
        if (called) {
            return;
        }
        called = true;
        switch (action) {
        case 0:
            window->moveResize(replacement);
            break;
        case 1:
        case 2: {
            auto drainBefore = window->makeTileAssignmentFence();
            auto drain = window->updateGeometryForAssignment(window->frameGeometry(), drainBefore);
            drained = drain.outcome == Window::AssignmentStepOutcome::AppliedCurrent;
            break;
        }
        case 3:
            before.reset();
            window->setSkipCloseAnimation(true);
            // Through the exported base pointer: the Xdg override itself is not exported.
            guard->destroyWindow();
            QVERIFY(guard);
            break;
        case 4: {
            auto nestedBefore = window->makeTileAssignmentFence();
            nested = window->requestMoveResizeForAssignment(desired, mode, nestedBefore);
            break;
        }
        case 5:
            callerValue = replacement;
            break;
        case 6:
            Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
            break;
        case 7:
            before.reset();
            break;
        case 8:
            window->setGeometryRestore(RectF(30, 40, 220, 120));
            break;
        }
    };
    switch (hook) {
    case 0:
        connect(window, &Window::frameGeometryAboutToChange, &context, interrupt);
        break;
    case 1:
        connect(window, &Window::nextTargetScaleChanged, &context, interrupt);
        break;
    case 2:
        connect(window, &Window::frameGeometryChanged, &context, interrupt);
        break;
    case 3:
        connect(window, &Window::requestedGeometryChanged, &context, interrupt);
        break;
    }
    auto request = window->requestMoveResizeForAssignment(callerValue, mode, *before);
    QVERIFY(called);
    QCOMPARE(request.result.outcome, current ? Window::AssignmentStepOutcome::AppliedCurrent : Window::AssignmentStepOutcome::Superseded);
    QCOMPARE(bool(request.result.checkpoint), current);
    QCOMPARE(int(request.dispatch), dispatch);
    QCOMPARE(request.requestGeneration, generation + 1);
    if (action == 3) {
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    if (action == 6) {
        // Output removal relocates windows through legacy requests, which emit the signal too.
        QVERIFY(!destinationGuard);
        return;
    }
    QCOMPARE(window->m_geometryRequestGeneration, generation + (nestedRequest ? 2 : 1));
    QVERIFY(!window->m_geometryResizePending);
    if (current) {
        QVERIFY(request.result.checkpoint->isCurrentFor(*window));
    }
    // The synchronized request survives supersession; nothing is rolled back.
    QCOMPARE(window->moveResizeGeometry(), action == 0 ? replacement : desired);
    switch (action) {
    case 0:
        QCOMPARE(window->frameGeometry(), replacement);
        QCOMPARE(requestedChanged.count(), hook == 3 ? 2 : 1);
        break;
    case 1:
        QVERIFY(drained);
        QCOMPARE(window->frameGeometry(), frame);
        QCOMPARE(window->m_geometryUpdateGeneration, exhausted - 1);
        QVERIFY(requestedChanged.isEmpty());
        break;
    case 2:
        QVERIFY(drained);
        QCOMPARE(window->frameGeometry(), desired);
        QCOMPARE(window->m_geometryUpdateGeneration, publication + 2);
        QCOMPARE(requestedChanged.count(), 1);
        break;
    case 4:
        QCOMPARE(nested.result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
        QVERIFY(nested.result.checkpoint && nested.result.checkpoint->isCurrentFor(*window));
        QCOMPARE(nested.dispatch, Window::AssignmentRequestDispatch::Published);
        QCOMPARE(window->frameGeometry(), desired);
        QCOMPARE(requestedChanged.count(), 1);
        break;
    case 5:
        QCOMPARE(window->frameGeometry(), desired);
        QCOMPARE(requestedChanged.count(), 1);
        break;
    case 7:
        QCOMPARE(window->frameGeometry(), frame);
        QCOMPARE(window->moveResizeOutput(), destination);
        QVERIFY(window->hasPendingNativeGeometry());
        QCOMPARE(requestedChanged.count(), 1);
        break;
    case 8:
        QCOMPARE(window->frameGeometry(), frame);
        if (hook == 1) {
            QVERIFY(aboutToChange.isEmpty());
            QVERIFY(window->m_outputPropertiesPending);
        } else {
            // The dispatcher saw the stale checkpoint right after the signal and staged nothing.
            QCOMPARE(aboutToChange.count(), 1);
        }
        QVERIFY(requestedChanged.isEmpty());
        break;
    }
}

void TestXdgShellWindow::testGuardedGeometryRequestBoundaries_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("stale") << 0;
    QTest::newRow("wrong-window") << 1;
    QTest::newRow("transient-request") << 2;
    QTest::newRow("request-generation-refused") << 3;
    QTest::newRow("request-generation-exact") << 4;
    QTest::newRow("publication-refused") << 5;
    QTest::newRow("publication-exact") << 6;
    QTest::newRow("placement-refused") << 7;
    QTest::newRow("placement-exact") << 8;
    QTest::newRow("queued-conservative") << 9;
    QTest::newRow("popup") << 10;
}

void TestXdgShellWindow::testGuardedGeometryRequestBoundaries()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    // The helper already consumed the initial configure; settle the activation one too.
    QVERIFY(!configure.isEmpty() || configure.wait());
    const int settledConfigures = configure.count();
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->hasPendingNativeGeometry());
    // A second toplevel takes activation and leaves this one a pending configure, so it exists
    // only for the wrong-window row.
    std::unique_ptr<KWayland::Client::Surface> otherSurface;
    std::unique_ptr<Test::XdgToplevel> otherShell;
    Window *other = nullptr;
    if (scenario == 1) {
        otherSurface = Test::createSurface();
        otherShell = Test::createXdgToplevelSurface(otherSurface.get());
        other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
        QVERIFY(other);
    }
    std::unique_ptr<Test::XdgPositioner> positioner;
    std::unique_ptr<KWayland::Client::Surface> popupSurface;
    std::unique_ptr<Test::XdgPopup> popup;
    Window *subject = window;
    if (scenario == 10) {
        positioner = Test::createXdgPositioner();
        positioner->set_size(10, 10);
        positioner->set_anchor_rect(0, 0, 50, 40);
        popupSurface = Test::createSurface();
        popup = Test::createXdgPopupSurface(popupSurface.get(), shell->xdgSurface(), positioner.get());
        subject = Test::renderAndWaitForShown(popupSurface.get(), QSize(10, 10), Qt::cyan);
        QVERIFY(subject);
    }
    const auto exhausted = std::numeric_limits<quint64>::max();
    const RectF frame = subject->frameGeometry();
    const RectF desired = scenario == 9 ? RectF(frame.topLeft(), QSizeF(300, 150)) : frame.translated(50, 60);
    const auto mode = scenario == 9 ? Window::MoveResizeMode::Resize : Window::MoveResizeMode::Move;
    // Each request below changes one requested field; a published Move also commits one change.
    const quint64 writes = 1;
    switch (scenario) {
    case 3:
        subject->m_geometryRequestGeneration = exhausted - 1;
        break;
    case 4:
        subject->m_geometryRequestGeneration = exhausted - 2;
        break;
    case 5:
        subject->m_geometryUpdateGeneration = exhausted - 1;
        break;
    case 6:
        subject->m_geometryUpdateGeneration = exhausted - 2;
        break;
    case 7:
        subject->m_placementContinuationRevision = exhausted - 1 - writes;
        break;
    case 8:
        subject->m_placementContinuationRevision = exhausted - 2 - writes;
        break;
    case 9:
        // The queued branch would need one write, but the reservation is for the whole request.
        subject->m_placementContinuationRevision = exhausted - 2;
        break;
    }
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence((scenario == 1 ? other : subject)->makeTileAssignmentFence()));
    if (scenario == 0) {
        subject->setGeometryRestore(RectF(10, 20, 200, 100));
    }
    QSignalSpy aboutToChange(subject, &Window::frameGeometryAboutToChange);
    QSignalSpy requestedChanged(subject, &Window::requestedGeometryChanged);
    const auto generation = subject->m_geometryRequestGeneration;
    const auto publication = subject->m_geometryUpdateGeneration;
    const auto revision = subject->m_placementContinuationRevision;
    const auto requested = subject->moveResizeGeometry();
    Window::AssignmentGeometryRequest request;
    if (scenario == 2) {
        // An active transient request owns the geometry path. The guarded request made inside the
        // hook that request runs is refused without effects on either of them.
        RequestTestTile tile(window);
        QObject owner;
        auto authority = std::make_shared<RequestAuthority>();
        authority->owner = &owner;
        window->setGeometryRestore(RectF(30, 40, 200, 100));
        QObject context;
        bool refused = false;
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
            QVERIFY(window->m_activeTransientRequest);
            auto hookBefore = window->makeTileAssignmentFence();
            request = window->requestMoveResizeForAssignment(desired, mode, hookBefore);
            refused = request.result.outcome == Window::AssignmentStepOutcome::Refused && !request.result.checkpoint
                && hookBefore.isCurrentFor(*window);
        });
        const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
        QVERIFY(refused);
        QCOMPARE(receipt.outcome, Window::TransientRequestOutcome::Applied);
        QCOMPARE(window->requestedTile(), &tile);
        QCOMPARE(request.dispatch, Window::AssignmentRequestDispatch::None);
        QCOMPARE(request.requestGeneration, quint64(0));
        return;
    }
    request = subject->requestMoveResizeForAssignment(desired, mode, *before);
    auto expected = Window::AssignmentStepOutcome::AppliedCurrent;
    if (scenario == 0 || scenario == 1) {
        expected = Window::AssignmentStepOutcome::Superseded;
    } else if (scenario == 3 || scenario == 5 || scenario == 7 || scenario == 9 || scenario == 10) {
        expected = Window::AssignmentStepOutcome::Refused;
    }
    QCOMPARE(request.result.outcome, expected);
    QCOMPARE(bool(request.result.checkpoint), expected == Window::AssignmentStepOutcome::AppliedCurrent);
    if (expected != Window::AssignmentStepOutcome::AppliedCurrent) {
        // No effects: nothing allocated, synchronized, staged, published or announced.
        QCOMPARE(request.dispatch, Window::AssignmentRequestDispatch::None);
        QCOMPARE(request.requestGeneration, quint64(0));
        QCOMPARE(subject->m_geometryRequestGeneration, generation);
        QCOMPARE(subject->m_geometryUpdateGeneration, publication);
        QCOMPARE(subject->m_placementContinuationRevision, revision);
        QCOMPARE(subject->moveResizeGeometry(), requested);
        QCOMPARE(subject->frameGeometry(), frame);
        QVERIFY(aboutToChange.isEmpty() && requestedChanged.isEmpty());
        QVERIFY(!subject->m_geometryResizePending);
        if (scenario == 9) {
            QVERIFY(!window->hasPendingNativeGeometry());
        }
        return;
    }
    QVERIFY(request.result.checkpoint->isCurrentFor(*subject));
    QCOMPARE(request.dispatch, Window::AssignmentRequestDispatch::Published);
    QCOMPARE(request.requestGeneration, generation + 1);
    QCOMPARE(subject->frameGeometry(), desired);
    QCOMPARE(subject->moveResizeGeometry(), desired);
    QCOMPARE(subject->m_geometryUpdateGeneration, publication + 1);
    QCOMPARE(subject->m_placementContinuationRevision, revision + writes + 1);
    QCOMPARE(requestedChanged.count(), 1);
    if (scenario == 4) {
        QCOMPARE(subject->m_geometryRequestGeneration, exhausted - 1);
    } else if (scenario == 6) {
        QCOMPARE(subject->m_geometryUpdateGeneration, exhausted - 1);
    } else if (scenario == 8) {
        QCOMPARE(subject->m_placementContinuationRevision, exhausted - 1);
    }
}

void TestXdgShellWindow::testGuardedGeometryRequestRefusesOwnership()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.backend-request"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const QRectF floating = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.backend-request");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, floating, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    // An owned record with no request on the stack: only the ownership check can refuse this.
    QVERIFY(window->m_transientOwnership && !window->m_activeTransientRequest);
    const auto geometry = window->moveResizeGeometry();
    const auto output = window->moveResizeOutput();
    const auto frame = window->frameGeometry();
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryRequestGeneration;
    const auto publication = window->m_geometryUpdateGeneration;
    QSignalSpy aboutToChange(window, &Window::frameGeometryAboutToChange);
    QSignalSpy requestedChanged(window, &Window::requestedGeometryChanged);
    {
        auto before = window->makeTileAssignmentFence();
        auto request = window->requestMoveResizeForAssignment(RectF(1400, 100, 200, 100), Window::MoveResizeMode::Move, before);
        QCOMPARE(request.result.outcome, Window::AssignmentStepOutcome::Refused);
        QVERIFY(!request.result.checkpoint);
        QCOMPARE(request.dispatch, Window::AssignmentRequestDispatch::None);
        QCOMPARE(request.requestGeneration, quint64(0));
        QVERIFY(before.isCurrentFor(*window));
    }
    QCOMPARE(window->moveResizeGeometry(), geometry);
    QCOMPARE(window->moveResizeOutput(), output);
    QCOMPARE(window->frameGeometry(), frame);
    QCOMPARE(window->m_placementContinuationRevision, revision);
    QCOMPARE(window->m_geometryRequestGeneration, generation);
    QCOMPARE(window->m_geometryUpdateGeneration, publication);
    QVERIFY(aboutToChange.isEmpty() && requestedChanged.isEmpty());
    QVERIFY(!window->m_geometryResizePending);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), floating);
    QCOMPARE(window->requestedTile(), controller.tile());
    QVERIFY(writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), floating);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testRequestedGeometryPropertyDrift()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    window->setPreferredColorDescription(ColorDescription::BT2020PQ);
    const auto revision = window->m_placementContinuationRevision;
    window->setMoveResizeOutput(window->moveResizeOutput());
    QCOMPARE(window->preferredColorDescription(), ColorDescription::BT2020PQ);
    QCOMPARE(window->m_placementContinuationRevision, revision);
    auto before = window->makeTileAssignmentFence();
    auto result = window->setMoveResizeGeometryForAssignment(window->moveResizeGeometry(), before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->preferredColorDescription(), window->moveResizeOutput()->blendingColor());
    QCOMPARE(window->m_placementContinuationRevision, revision + 3);
    QVERIFY(!window->m_outputPropertiesPending);
}

void TestXdgShellWindow::testRequestedGeometryNullOutput()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto originalOutput = window->moveResizeOutput();
    window->setOutputPropertiesPending(true);
    const auto revision = window->m_placementContinuationRevision;
    const auto scale = window->nextTargetScale();
    const auto transform = window->preferredBufferTransform();
    const auto color = window->preferredColorDescription();
    window->setMoveResizeOutput(nullptr);
    QVERIFY(!window->moveResizeOutput() && !window->m_outputPropertiesPending);
    QCOMPARE(window->m_placementContinuationRevision, revision + 2);
    QCOMPARE(window->nextTargetScale(), scale);
    QCOMPARE(window->preferredBufferTransform(), transform);
    QCOMPARE(window->preferredColorDescription(), color);
    window->setMoveResizeOutput(nullptr);
    QCOMPARE(window->m_placementContinuationRevision, revision + 2);
    window->setMoveResizeOutput(originalOutput);
}

void TestXdgShellWindow::testRequestedGeometryRefusesOwnership()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.requested-state"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const QRectF floating = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.requested-state");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, floating, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    const auto geometry = window->moveResizeGeometry();
    const auto output = window->moveResizeOutput();
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryRequestGeneration;
    {
        auto before = window->makeTileAssignmentFence();
        auto result = window->setMoveResizeGeometryForAssignment(RectF(1400, 100, 200, 100), before);
        QCOMPARE(result.outcome, Window::AssignmentStepOutcome::Refused);
        QVERIFY(!result.checkpoint);
        QVERIFY(before.isCurrentFor(*window));
    }
    QCOMPARE(window->moveResizeGeometry(), geometry);
    QCOMPARE(window->moveResizeOutput(), output);
    QCOMPARE(window->m_placementContinuationRevision, revision);
    QCOMPARE(window->m_geometryRequestGeneration, generation);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), floating);
    QCOMPARE(window->requestedTile(), controller.tile());
    QVERIFY(writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), floating);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testRequestedGeometryRefusesActiveRequest()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    window->setGeometryRestore(RectF(30, 40, 200, 100));
    QObject context;
    bool refused = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        QVERIFY(window->m_activeTransientRequest);
        QVERIFY(!window->m_transientOwnership);
        const auto geometry = window->moveResizeGeometry();
        auto before = window->makeTileAssignmentFence();
        auto result = window->setMoveResizeGeometryForAssignment(RectF(1400, 100, 200, 100), before);
        QCOMPARE(result.outcome, Window::AssignmentStepOutcome::Refused);
        QVERIFY(!result.checkpoint && before.isCurrentFor(*window));
        QCOMPARE(window->moveResizeGeometry(), geometry);
        refused = true;
    });
    const auto result = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QVERIFY(refused);
    QCOMPARE(result.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(window->requestedTile(), &tile);
    QCOMPARE(window->moveResizeGeometry(), tile.windowGeometry());
}

void TestXdgShellWindow::testGuardedGeometryPublication()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<WaylandWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const RectF oldFrame = window->frameGeometry();
    const RectF oldClient = window->clientGeometry();
    const RectF oldBuffer = window->bufferGeometry();
    const RectF requested = window->moveResizeGeometry();
    const RectF target(QPointF(1400, 100), oldFrame.size());
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryUpdateGeneration;
    QStringList notifications;
    QObject context;
    connect(window, &Window::bufferGeometryChanged, &context, [&]() {
        notifications << QStringLiteral("buffer");
    });
    connect(window, &Window::clientGeometryChanged, &context, [&]() {
        notifications << QStringLiteral("client");
    });
    connect(window, &Window::frameGeometryChanged, &context, [&]() {
        notifications << QStringLiteral("frame");
    });
    connect(window, &Window::outputChanged, &context, [&]() {
        notifications << QStringLiteral("output");
    });
    auto before = window->makeTileAssignmentFence();
    auto result = window->updateGeometryForAssignment(target, before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QVERIFY(!before.isCurrentFor(*window));
    QCOMPARE(window->frameGeometry(), target);
    QCOMPARE(window->clientGeometry(), oldClient.translated(target.topLeft() - oldFrame.topLeft()));
    QCOMPARE(window->bufferGeometry(), oldBuffer.translated(target.topLeft() - oldFrame.topLeft()));
    QCOMPARE(window->output(), workspace()->outputs()[1]);
    QCOMPARE(window->moveResizeGeometry(), requested);
    QCOMPARE(window->m_placementContinuationRevision, revision + 1);
    QCOMPARE(window->m_geometryUpdateGeneration, generation + 1);
    QCOMPARE(notifications, QStringList({QStringLiteral("buffer"), QStringLiteral("client"), QStringLiteral("frame"), QStringLiteral("output")}));
    notifications.clear();
    auto noop = window->updateGeometryForAssignment(target, *result.checkpoint);
    QCOMPARE(noop.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(noop.checkpoint && noop.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->m_placementContinuationRevision, revision + 1);
    QCOMPARE(window->m_geometryUpdateGeneration, generation + 1);
    QVERIFY(notifications.isEmpty());
}

void TestXdgShellWindow::testGuardedGeometryPublicationReentry_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("restore") << 0;
    QTest::newRow("committed-tile") << 1;
    QTest::newRow("equal-request") << 2;
    QTest::newRow("nested-legacy") << 3;
    QTest::newRow("nested-drain") << 4;
    QTest::newRow("last-notification") << 5;
    QTest::newRow("input-owner") << 6;
    QTest::newRow("input-owner-close") << 7;
    QTest::newRow("copied-input") << 8;
    QTest::newRow("output-roundtrip") << 9;
    QTest::newRow("same-output") << 10;
}

void TestXdgShellWindow::testGuardedGeometryPublicationReentry()
{
    QFETCH(int, action);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<WaylandWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto tile = std::make_unique<Tile>(workspace()->tileManager(window->output()));
    const RectF target(QPointF(1400, 100), window->size());
    const RectF replacement(QPointF(1500, 200), window->size());
    RectF callerValue = target;
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryUpdateGeneration;
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence(window->makeTileAssignmentFence()));
    QPointer<Window> guard(window);
    QSignalSpy buffers(window, &Window::bufferGeometryChanged);
    QSignalSpy clients(window, &Window::clientGeometryChanged);
    QSignalSpy frames(window, &Window::frameGeometryChanged);
    QSignalSpy outputs(window, &Window::outputChanged);
    QObject context;
    bool called = false;
    bool nestedApplied = false;
    const auto interrupt = [&]() {
        if (called) {
            return;
        }
        called = true;
        switch (action) {
        case 0:
        case 5:
            window->setGeometryRestore(RectF(30, 40, 200, 100));
            break;
        case 1:
            window->commitTile(tile.get());
            break;
        case 2:
            window->moveResize(window->frameGeometry());
            break;
        case 3:
            window->updateGeometry(replacement);
            break;
        case 4: {
            auto nestedBefore = window->makeTileAssignmentFence();
            auto nested = window->updateGeometryForAssignment(target, nestedBefore);
            nestedApplied = nested.outcome == Window::AssignmentStepOutcome::AppliedCurrent && nested.checkpoint && nested.checkpoint->isCurrentFor(*window);
            break;
        }
        case 6:
            before.reset();
            break;
        case 7:
            before.reset();
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            QVERIFY(guard);
            break;
        case 8:
            callerValue = replacement;
            break;
        case 9:
            window->setOutput(workspace()->outputs()[0]);
            window->setOutput(workspace()->outputs()[1]);
            break;
        case 10:
            window->setOutput(window->output());
            break;
        }
    };
    if (action == 5) {
        connect(window, &Window::outputChanged, &context, interrupt);
    } else {
        connect(window, &Window::bufferGeometryChanged, &context, interrupt);
    }
    auto result = window->updateGeometryForAssignment(callerValue, *before);
    QVERIFY(called);
    const bool current = action == 6 || action == 8 || action == 10;
    QCOMPARE(result.outcome, current ? Window::AssignmentStepOutcome::AppliedCurrent : Window::AssignmentStepOutcome::Superseded);
    QCOMPARE(bool(result.checkpoint), current);
    if (action == 7) {
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    QCOMPARE(window->frameGeometry(), action == 3 ? replacement : target);
    const bool interruptedBeforeClient = action < 3 || action == 9;
    QCOMPARE(clients.count(), interruptedBeforeClient ? 0 : 1);
    QCOMPARE(frames.count(), interruptedBeforeClient ? 0 : 1);
    int outputCount = action < 3 ? 0 : 1;
    if (action == 9) {
        outputCount = 2;
    }
    QCOMPARE(outputs.count(), outputCount);
    const int bufferCount = action == 3 ? 2 : 1;
    QCOMPARE(buffers.count(), bufferCount);
    if (action == 4) {
        QVERIFY(nestedApplied);
        QCOMPARE(window->m_placementContinuationRevision, revision + 1);
        QCOMPARE(window->m_geometryUpdateGeneration, generation + 2);
    }
    auto drainBefore = window->makeTileAssignmentFence();
    auto drain = window->updateGeometryForAssignment(window->frameGeometry(), drainBefore);
    QCOMPARE(drain.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(drain.checkpoint && drain.checkpoint->isCurrentFor(*window));
    QCOMPARE(buffers.count(), bufferCount);
    QCOMPARE(clients.count(), 1);
    QCOMPARE(frames.count(), 1);
    QCOMPARE(outputs.count(), action == 9 ? 3 : 1);
    QVERIFY(!window->m_pendingGeometryNotifications.buffer && !window->m_pendingGeometryNotifications.client
            && !window->m_pendingGeometryNotifications.frame && !window->m_pendingGeometryNotifications.outputPending);
}

void TestXdgShellWindow::testGuardedGeometryPublicationBoundaries_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("stale") << 0;
    QTest::newRow("wrong-window") << 1;
    QTest::newRow("placement-headroom") << 2;
    QTest::newRow("publication-headroom") << 3;
    QTest::newRow("last-safe-write") << 4;
    QTest::newRow("saturated-noop") << 5;
    QTest::newRow("invalid-input") << 6;
    QTest::newRow("placement-noop") << 7;
    QTest::newRow("drain-refused") << 8;
    QTest::newRow("last-safe-drain") << 9;
    QTest::newRow("net-zero-drain") << 10;
}

void TestXdgShellWindow::testGuardedGeometryPublicationBoundaries()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = static_cast<WaylandWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::Surface> otherSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> otherShell(Test::createXdgToplevelSurface(otherSurface.get()));
    auto other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(other);
    const auto exhausted = std::numeric_limits<quint64>::max();
    if (scenario == 2 || scenario == 7) {
        window->m_placementContinuationRevision = exhausted - 1;
    } else if (scenario == 6) {
        window->m_placementContinuationRevision = exhausted;
    } else if (scenario == 4) {
        window->m_placementContinuationRevision = exhausted - 2;
    }
    if (scenario == 3 || scenario == 8) {
        window->m_geometryUpdateGeneration = exhausted - 1;
    } else if (scenario == 5) {
        window->m_geometryUpdateGeneration = exhausted;
    } else if (scenario == 4 || scenario == 9) {
        window->m_geometryUpdateGeneration = exhausted - 2;
    }
    const auto original = window->frameGeometry();
    const bool pending = scenario >= 8;
    if (pending) {
        window->m_pendingGeometryNotifications.frame = scenario == 10 ? original : original.translated(-10, 0);
    }
    auto before = (scenario == 1 ? other : window)->makeTileAssignmentFence();
    if (scenario == 0) {
        window->setGeometryRestore(RectF(10, 20, 200, 100));
    }
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryUpdateGeneration;
    const bool change = scenario < 5;
    const RectF target = change ? original.translated(20, 30) : original;
    QSignalSpy frames(window, &Window::frameGeometryChanged);
    auto result = window->updateGeometryForAssignment(target, before);
    auto expected = Window::AssignmentStepOutcome::AppliedCurrent;
    if (scenario == 0 || scenario == 1 || scenario == 6) {
        expected = Window::AssignmentStepOutcome::Superseded;
    } else if (scenario == 2 || scenario == 3 || scenario == 8) {
        expected = Window::AssignmentStepOutcome::Refused;
    }
    QCOMPARE(result.outcome, expected);
    QCOMPARE(bool(result.checkpoint), expected == Window::AssignmentStepOutcome::AppliedCurrent);
    if (result.checkpoint) {
        QVERIFY(result.checkpoint->isCurrentFor(*window));
    }
    const bool wrote = scenario == 4;
    const bool drained = scenario == 9 || scenario == 10;
    QCOMPARE(window->frameGeometry(), wrote ? target : original);
    QCOMPARE(window->m_placementContinuationRevision, revision + (wrote ? 1 : 0));
    QCOMPARE(window->m_geometryUpdateGeneration, generation + (wrote || drained ? 1 : 0));
    QCOMPARE(frames.count(), wrote || scenario == 9 ? 1 : 0);
    QCOMPARE(window->m_pendingGeometryNotifications.frame.has_value(), scenario == 8);
}

void TestXdgShellWindow::testGuardedGeometryPublicationOutput()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = static_cast<WaylandWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    const auto geometry = window->frameGeometry();
    const auto expected = workspace()->outputAt(geometry.center());
    window->m_output = workspace()->outputs()[1];
    QVERIFY(window->m_output != expected);
    auto before = window->makeTileAssignmentFence();
    const auto revision = window->m_placementContinuationRevision;
    QSignalSpy outputs(window, &Window::outputChanged);
    QSignalSpy frames(window, &Window::frameGeometryChanged);
    auto result = window->updateGeometryForAssignment(geometry, before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->output(), expected);
    QCOMPARE(window->frameGeometry(), geometry);
    QCOMPARE(window->m_placementContinuationRevision, revision + 1);
    QCOMPARE(outputs.count(), 1);
    QVERIFY(frames.isEmpty());
}

void TestXdgShellWindow::testGuardedGeometryPublicationOutputRemoval()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = static_cast<WaylandWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    QPointer<LogicalOutput> destination = workspace()->outputs()[1];
    const RectF target(QPointF(1400, 100), window->size());
    auto before = window->makeTileAssignmentFence();
    QObject context;
    bool called = false;
    connect(window, &Window::bufferGeometryChanged, &context, [&]() {
        if (!called) {
            called = true;
            Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
        }
    });
    auto result = window->updateGeometryForAssignment(target, before);
    QVERIFY(called);
    QVERIFY(!destination);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::Superseded);
    QVERIFY(!result.checkpoint);
    QCOMPARE(workspace()->outputs().size(), 1);
}

void TestXdgShellWindow::testGeometryPublicationLegacy()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = static_cast<WaylandWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    const auto original = window->frameGeometry();
    const auto revision = window->m_placementContinuationRevision;
    const auto generation = window->m_geometryUpdateGeneration;
    window->m_pendingGeometryNotifications.frame = original.translated(-10, 0);
    QSignalSpy frames(window, &Window::frameGeometryChanged);
    window->updateGeometry(original);
    QVERIFY(frames.isEmpty());
    QVERIFY(window->m_pendingGeometryNotifications.frame);
    QCOMPARE(window->m_placementContinuationRevision, revision);
    QCOMPARE(window->m_geometryUpdateGeneration, generation);
    const auto target = original.translated(30, 40);
    window->updateGeometry(target);
    QCOMPARE(window->frameGeometry(), target);
    QCOMPARE(window->m_placementContinuationRevision, revision + 1);
    QCOMPARE(window->m_geometryUpdateGeneration, generation + 1);
    QCOMPARE(frames.count(), 1);
    QVERIFY(!window->m_pendingGeometryNotifications.frame);
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

void TestXdgShellWindow::testOwnedRequestAuthority_data()
{
    QTest::addColumn<int>("boundary");
    QTest::newRow("control") << 0;
    QTest::newRow("restore-notification") << 1;
    QTest::newRow("scale-synchronization") << 2;
    QTest::newRow("frame-about-to-change") << 3;
    QTest::newRow("buffer-publication") << 4;
    QTest::newRow("requested-tile-notification") << 5;
    QTest::newRow("requested-tile-restore-change") << 6;
    QTest::newRow("equal-target-scale-synchronization") << 7;
}

void TestXdgShellWindow::testOwnedRequestAuthority()
{
    // The request's external authority can lapse without any Window counter changing. Every owned
    // child step must notice that next to the fence and stop where it stands, without rollback.
    QFETCH(int, boundary);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    if (boundary == 2 || boundary == 7) {
        // Settle the configure the scale change schedules; admission refuses pending native geometry.
        configure.clear();
        window->setNextTargetScale(3.0);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), QSize(200, 100), Qt::red);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->nextTargetScale(), 3.0);
    }
    const RectF original = window->frameGeometry();
    QCOMPARE(window->moveResizeGeometry(), original);
    RequestTestTile tile(window);
    if (boundary == 4) {
        // A same-size move publishes immediately instead of staging a configure.
        tile.setTargetForTest(RectF(640, 512, 200, 100));
    } else if (boundary == 7) {
        // An equal target writes no rectangle; the carrier must still join the synchronization.
        tile.setTargetForTest(original);
    }
    const RectF target = tile.windowGeometry();
    if (boundary == 7) {
        QCOMPARE(target, original);
    } else {
        QVERIFY(target != original);
    }
    QCOMPARE(target.size() == original.size(), boundary == 4 || boundary == 7);
    QVERIFY(static_cast<Window *>(window)->canStartTransientTileRequest(target));
    window->setGeometryRestore(RectF(20, 20, 200, 100));
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QObject context;
    const RectF replaced(30, 30, 200, 100);
    bool intervened = false;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        if (boundary == 6) {
            // Fenced state changes with the authority intact; the request predicate alone misses it.
            window->setGeometryRestore(replaced);
        } else {
            authority->valid = false;
        }
    };
    switch (boundary) {
    case 1:
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, intervene);
        break;
    case 2:
    case 7:
        connect(window, &Window::nextTargetScaleChanged, &context, intervene);
        break;
    case 3:
        connect(window, &Window::frameGeometryAboutToChange, &context, intervene);
        break;
    case 4:
        connect(window, &Window::bufferGeometryChanged, &context, intervene);
        break;
    case 5:
    case 6:
        connect(window, &Window::requestedTileChanged, &context, intervene);
        break;
    }
    QSignalSpy bufferChanged(window, &Window::bufferGeometryChanged);
    QSignalSpy clientChanged(window, &Window::clientGeometryChanged);
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    QSignalSpy requestedChanged(window, &Window::requestedTileChanged);
    const auto revision = window->m_requestedTileChange;
    const auto generation = window->m_geometryRequestGeneration;
    configure.clear();
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    QCOMPARE(intervened, boundary != 0);
    QCOMPARE(authority->valid, boundary == 0 || boundary == 6);
    QCOMPARE(receipt.outcome, boundary == 0 ? Window::TransientRequestOutcome::Applied : Window::TransientRequestOutcome::Superseded);
    // Progress is published at allocation and stays Requested whatever happens afterwards.
    QCOMPARE(receipt.progress, boundary == 1 ? Window::TransientRequestProgress::NoRequest : Window::TransientRequestProgress::Requested);
    QVERIFY(receipt.nativeSequence);
    QCOMPARE(bool(receipt.noRequestProof), boundary == 1);
    QVERIFY(!window->m_activeTransientRequest);
    // Saved A survives every outcome once written and a handler's newer value is kept; only this
    // attempt's association is removed.
    QCOMPARE(window->geometryRestore(), boundary == 6 ? replaced : original);
    QCOMPARE(window->requestedTile(), boundary == 0 ? &tile : nullptr);
    // No install before the restore notification; install only on success; install and removal otherwise.
    quint64 associationChanges = 2;
    if (boundary == 0) {
        associationChanges = 1;
    } else if (boundary == 1) {
        associationChanges = 0;
    }
    QCOMPARE(window->m_requestedTileChange, revision + associationChanges);
    // Removal announces once more; an intervention at the notification itself sees both announcements.
    QCOMPARE(quint64(requestedChanged.count()), boundary == 5 || boundary == 6 ? 2 : qMin<quint64>(associationChanges, 1));
    QCOMPARE(window->m_geometryRequestGeneration, generation + (boundary == 1 ? 0 : 1));
    QVERIFY(!window->m_geometryResizePending);
    QCOMPARE(window->moveResizeGeometry(), boundary == 1 ? original : target);
    QCOMPARE(window->frameGeometry(), boundary == 4 ? target : original);
    // Output properties: the lapse inside synchronization leaves the obligation for a later request.
    QCOMPARE(window->m_outputPropertiesPending, boundary == 2 || boundary == 7);
    QCOMPARE(window->nextTargetScale(), window->moveResizeOutput()->scale());
    // Every boundary past the association write publishes the quick-tile state, which schedules a configure; row 1
    // stops before any write. Since 6.8 every configure carries moveResizeGeometry() as its bounds, so the dispatcher's
    // own staging (rows 0, 5 and 6) is not separately observable; the configure delivers the rectangle requested so far.
    QCOMPARE(window->m_configureTimer->isActive(), boundary != 1);
    // Publication stops after the notification in which the authority lapsed; the rest stays pending.
    QCOMPARE(bufferChanged.count(), boundary == 4 ? 1 : 0);
    QCOMPARE(clientChanged.count(), 0);
    QCOMPARE(frameChanged.count(), 0);
    QCOMPARE(window->m_pendingGeometryNotifications.client.has_value(), boundary == 4);
    QCOMPARE(window->m_pendingGeometryNotifications.frame.has_value(), boundary == 4);
    if (boundary != 0) {
        return;
    }
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->frameGeometry(), target);
    QCOMPARE(window->requestedTile(), &tile);
    // The transient commit is gated on the ownership record; a direct request establishes none.
    QVERIFY(!window->tile());
}

void TestXdgShellWindow::testOwnedRequestExhaustion_data()
{
    QTest::addColumn<int>("counter");
    QTest::newRow("request-generation") << 0;
    QTest::newRow("association-revision") << 1;
    QTest::newRow("placement-revision") << 2;
    QTest::newRow("publication-generation") << 3;
    QTest::newRow("placement-after-restore") << 4;
}

void TestXdgShellWindow::testOwnedRequestExhaustion()
{
    // Headroom known before any effect is refused as Rejected. A child refusal after the restore
    // and association writes ends as Superseded with only this attempt's association removed.
    QFETCH(int, counter);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = static_cast<XdgToplevelWindow *>(Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red));
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile tile(window);
    const RectF original = window->frameGeometry();
    const RectF target = tile.windowGeometry();
    QVERIFY(static_cast<Window *>(window)->canStartTransientTileRequest(target));
    const RectF stale(20, 20, 200, 100);
    window->setGeometryRestore(stale);
    const auto exhausted = std::numeric_limits<quint64>::max();
    const auto requestGeneration = window->m_geometryRequestGeneration;
    const auto associationRevision = window->m_requestedTileChange;
    const auto placementRevision = window->m_placementContinuationRevision;
    const auto publicationGeneration = window->m_geometryUpdateGeneration;
    const auto restoreCounters = qScopeGuard([&]() {
        window->m_geometryRequestGeneration = requestGeneration;
        window->m_requestedTileChange = associationRevision;
        window->m_placementContinuationRevision = placementRevision;
        window->m_geometryUpdateGeneration = publicationGeneration;
    });
    switch (counter) {
    case 0:
        window->m_geometryRequestGeneration = exhausted - 1;
        break;
    case 1:
        window->m_requestedTileChange = exhausted - 1;
        break;
    case 2:
        window->m_placementContinuationRevision = exhausted - 1;
        break;
    case 3:
        window->m_geometryUpdateGeneration = exhausted - 1;
        break;
    case 4:
        window->m_placementContinuationRevision = exhausted - 2;
        break;
    }
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy requestedChanged(window, &Window::requestedTileChanged);
    const auto sequence = window->m_transientRequestSequence;
    // The request captures the request generation now, after the sentinel is in place.
    const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &tile, authority));
    const bool rejected = counter <= 2;
    QCOMPARE(receipt.outcome, rejected ? Window::TransientRequestOutcome::Rejected : Window::TransientRequestOutcome::Superseded);
    QCOMPARE(receipt.progress, Window::TransientRequestProgress::NoRequest);
    // Counter sentinels are refused before the sequence is consumed; the restore writer's refusal is not.
    QCOMPARE(receipt.nativeSequence, counter <= 1 ? 0 : sequence + 1);
    QCOMPARE(bool(receipt.noRequestProof), counter >= 2);
    QVERIFY(!window->m_activeTransientRequest);
    QCOMPARE(window->geometryRestore(), rejected ? stale : original);
    QCOMPARE(restoreChanged.count(), rejected ? 0 : 1);
    QVERIFY(!window->requestedTile());
    QCOMPARE(requestedChanged.count(), rejected ? 0 : 1);
    QCOMPARE(window->moveResizeGeometry(), original);
    QCOMPARE(window->frameGeometry(), original);
    QVERIFY(!window->m_outputPropertiesPending);
    QVERIFY(!window->m_geometryResizePending);
    // A rejected request schedules no configure at all; a superseded one published its association's install and
    // removal (the quick-tile state), whose configure carries moveResizeGeometry(), the original rectangle above.
    QCOMPARE(window->m_configureTimer->isActive(), !rejected);
    QCOMPARE(window->m_geometryRequestGeneration, counter == 0 ? exhausted - 1 : requestGeneration);
    QCOMPARE(window->m_requestedTileChange, counter == 1 ? exhausted - 1 : associationRevision + (rejected ? 0 : 2));
    QCOMPARE(window->m_placementContinuationRevision, counter == 2 || counter == 4 ? exhausted - 1 : placementRevision + (rejected ? 0 : 1));
    QCOMPARE(window->m_geometryUpdateGeneration, counter == 3 ? exhausted - 1 : publicationGeneration);
}

void TestXdgShellWindow::testOwnedMaximize_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("committed");
    QTest::newRow("full-committed") << int(MaximizeFull) << true;
    QTest::newRow("full-requested") << int(MaximizeFull) << false;
    QTest::newRow("horizontal-committed") << int(MaximizeHorizontal) << true;
    QTest::newRow("vertical-committed") << int(MaximizeVertical) << true;
}

void TestXdgShellWindow::testOwnedMaximize()
{
    // An ordinary maximize toggle under live ownership keeps ownership, the association and the
    // saved floating frame, and returns to the managed rectangle; an explicit untile still releases.
    QFETCH(int, mode);
    QFETCH(bool, committed);
    const auto maximizeMode = MaximizeMode(mode);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.maximize"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
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
    const auto key = QStringLiteral("org.example.maximize");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    auto state = writer.snapshot();
    configure.clear();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership);
    const auto generation = window->m_transientOwnership->generation;
    const QRectF managed = leaf->windowGeometry();
    if (committed) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), managed.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf);
    }
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    const QRectF area = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    QRectF expected = managed;
    if (maximizeMode & MaximizeHorizontal) {
        expected.setLeft(area.left());
        expected.setRight(area.right());
    }
    if (maximizeMode & MaximizeVertical) {
        expected.setTop(area.top());
        expected.setBottom(area.bottom());
    }
    const auto owned = [&]() {
        return window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
            && writer.ownsReservation(id, QString::number(generation)) && window->requestedTile() == leaf && leaf->windows().contains(window)
            && QRectF(window->geometryRestore()) == original && writer.snapshot().entries.value(key) == original;
    };
    configure.clear();
    window->maximize(maximizeMode);
    QVERIFY(owned());
    QCOMPARE(window->requestedMaximizeMode(), maximizeMode);
    QCOMPARE(QRectF(window->moveResizeGeometry()), expected);
    QCOMPARE(restoreChanged.count(), 0);
    QCOMPARE(requestedTileChanged.count(), 0);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::green);
    QVERIFY(Test::waylandSync());
    QCOMPARE(QRectF(window->frameGeometry()), expected);
    QCOMPARE(window->maximizeMode(), maximizeMode);
    if (maximizeMode == MaximizeFull) {
        QVERIFY(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Maximized);
    }
    // The acknowledgement commits a kept association without unmaximizing.
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
    QVERIFY(window->m_transientOwnership->committed);
    configure.clear();
    window->maximize(MaximizeRestore);
    QVERIFY(owned());
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(leaf->windowGeometry()));
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::yellow);
    QVERIFY(Test::waylandSync());
    QCOMPARE(QRectF(window->frameGeometry()), QRectF(leaf->windowGeometry()));
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Maximized));
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
    QCOMPARE(restoreChanged.count(), 0);
    QCOMPARE(requestedTileChanged.count(), 0);
    // The explicit untile-or-restore action still releases through the existing routing.
    workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
    QVERIFY(!window->requestedTile());
    QVERIFY(!writer.ownsReservation(id, QString::number(generation)));
    QVERIFY(!window->m_transientOwnership || window->m_transientOwnership->phase != Window::TransientOwnershipPhase::Owned);
    QCOMPARE(QRectF(window->geometryRestore()), original);
}

void TestXdgShellWindow::testOwnedMaximizeLegacy_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("restore-parameter") << 0;
    QTest::newRow("electric-border") << 1;
    QTest::newRow("executing-record") << 2;
    QTest::newRow("no-ownership") << 3;
    QTest::newRow("changed-minimum") << 4;
}

void TestXdgShellWindow::testOwnedMaximizeLegacy()
{
    // Initial ineligibility selects the legacy path before any owned mutation; it releases as before.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.maximize"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const QRectF original = window->frameGeometry();
    const RectF supplied(10, 10, 100, 50);
    if (scenario == 3) {
        window->maximize(MaximizeFull);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(QRectF(window->geometryRestore()), original);
        return;
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.maximize");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    QObject context;
    bool intervened = false;
    if (scenario == 2) {
        // The record is executing its adoption request: a maximize from that boundary is legacy.
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                QVERIFY(window->m_transientOwnership && window->m_transientOwnership->executing);
                window->maximize(MaximizeFull);
            }
        });
    }
    auto state = writer.snapshot();
    configure.clear();
    const auto admission = controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision);
    if (scenario == 2) {
        QVERIFY(intervened);
        QCOMPARE(admission, Result::Superseded);
    } else {
        QCOMPARE(admission, Result::Accepted);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
        if (scenario == 4) {
            // Owned maximize first; then the client raises its minimum width above the managed
            // width, so the released axis can no longer return to the managed rectangle.
            const QRectF managed = leaf->windowGeometry();
            QVERIFY(!configure.isEmpty() || configure.wait());
            shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
            Test::render(surface.get(), managed.size().toSize(), Qt::blue);
            QVERIFY(Test::waylandSync());
            QCOMPARE(window->tile(), leaf);
            configure.clear();
            window->maximize(MaximizeFull);
            QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
            QCOMPARE(window->requestedTile(), leaf);
            QVERIFY(!configure.isEmpty() || configure.wait());
            shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
            Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::green);
            QVERIFY(Test::waylandSync());
            QCOMPARE(window->maximizeMode(), MaximizeFull);
            shell->set_min_size(int(managed.width()) + 100, 0);
            surface->commit(KWayland::Client::Surface::CommitFlag::None);
            QVERIFY(Test::waylandSync());
            QVERIFY(window->minSize().width() > managed.width());
            window->maximize(MaximizeRestore);
        } else {
            if (scenario == 1) {
                // An electric-border maximize in progress: the mode must be set before the outline shows.
                window->setElectricBorderMode(ElectricBorderMode(MaximizeFull));
                window->setElectricBorderMaximizing(true);
                QVERIFY(window->isElectricBorderMaximizing());
            }
            window->maximize(MaximizeFull, scenario == 0 ? supplied : RectF());
            if (scenario == 1) {
                window->setElectricBorderMaximizing(false);
                window->setElectricBorderMode(std::nullopt);
            }
        }
    }
    QCOMPARE(window->requestedMaximizeMode(), scenario == 4 ? MaximizeRestore : MaximizeFull);
    QVERIFY(!window->requestedTile());
    QVERIFY(!leaf->windows().contains(window));
    QVERIFY(!window->m_transientOwnership || window->m_transientOwnership->phase != Window::TransientOwnershipPhase::Owned);
    if (scenario == 0) {
        QCOMPARE(QRectF(window->geometryRestore()), QRectF(supplied));
    } else {
        QCOMPARE(QRectF(window->geometryRestore()), original);
    }
}

void TestXdgShellWindow::testOwnedMaximizeSupersession_data()
{
    QTest::addColumn<int>("boundary");
    QTest::newRow("revoke-at-about-to-change") << 0;
    QTest::newRow("restore-at-about-to-change") << 1;
    QTest::newRow("round-trip-at-about-to-change") << 2;
    QTest::newRow("revoke-at-requested-geometry") << 3;
    QTest::newRow("round-trip-at-requested-geometry") << 4;
    QTest::newRow("close-at-about-to-change") << 5;
    QTest::newRow("close-at-requested-geometry") << 6;
    QTest::newRow("reservation-lost-at-scale-synchronization") << 7;
    QTest::newRow("desktop-change-at-about-to-change") << 8;
    QTest::newRow("round-trip-without-geometry-at-about-to-change") << 9;
    QTest::newRow("minimum-raised-at-about-to-change") << 10;
    QTest::newRow("minimum-raised-at-frame-about-to-change") << 11;
}

void TestXdgShellWindow::testOwnedMaximizeSupersession()
{
    // After the owned path is entered, a handler's newer intent ends it where it stands: no legacy
    // geometry, no exit from the tile, no restore write; only a current mode write is published.
    QFETCH(int, boundary);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.maximize"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
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
    const auto key = QStringLiteral("org.example.maximize");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    auto state = writer.snapshot();
    configure.clear();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership);
    const auto generation = window->m_transientOwnership->generation;
    const QRectF managed = leaf->windowGeometry();
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), managed.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), leaf);
    if (boundary == 7) {
        // Settle the configure the scale change schedules so the owned request synchronizes it.
        configure.clear();
        window->setNextTargetScale(3.0);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), managed.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->nextTargetScale(), 3.0);
    }
    const QRectF area = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    const RectF replaced(30, 30, 200, 100);
    QPointer<Window> guard(window);
    VirtualDesktop *other = nullptr;
    if (boundary == 8) {
        VirtualDesktopManager::self()->setCount(2);
        other = VirtualDesktopManager::self()->desktops().at(1);
    }
    const auto desktopsReset = qScopeGuard([boundary]() {
        if (boundary == 8) {
            VirtualDesktopManager::self()->setCount(1);
        }
    });
    // Row 9: nested owned geometry requests refuse before mutation for publication headroom, so the
    // nested mode writes change no fenced state and only the requested-mode revision sees them.
    const auto publicationGeneration = window->m_geometryUpdateGeneration;
    if (boundary == 9) {
        window->m_geometryUpdateGeneration = std::numeric_limits<quint64>::max() - 1;
    }
    const auto publicationReset = qScopeGuard([&]() {
        if (guard) {
            window->m_geometryUpdateGeneration = publicationGeneration;
        }
    });
    if (boundary >= 10) {
        // Rows 10 and 11 start owned and maximized; the outer call is the unmaximize.
        configure.clear();
        window->maximize(MaximizeFull);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(window->requestedTile(), leaf);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::green);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        QCOMPARE(QRectF(window->frameGeometry()), area);
    }
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QObject context;
    bool intervened = false;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (boundary) {
        case 0:
        case 3:
            leaf->forget(window);
            break;
        case 10:
        case 11:
            // The client's minimum width rises above the managed width as a property-only change
            // (a real commit would also notify the requested geometry): neither the fence nor the
            // ownership record observes it, only the operation's own feasibility.
            static_cast<XdgToplevelWindow *>(window)->m_minimumSize = QSizeF(managed.width() + 100, 0);
            QVERIFY(window->minSize().width() > managed.width());
            break;
        case 1:
            window->setGeometryRestore(replaced);
            break;
        case 2:
        case 9:
            window->maximize(MaximizeFull);
            window->maximize(MaximizeRestore);
            break;
        case 4:
            window->maximize(MaximizeRestore);
            break;
        case 5:
        case 6:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        case 7:
            // Validity lapses in the writer alone; no Window counter changes.
            QVERIFY(writer.forgetWindow(id));
            break;
        case 8:
            // The window leaves the recorded desktop; no fence and no release notices.
            window->setDesktops({other});
            break;
        }
    };
    if (boundary <= 2 || boundary == 5 || boundary == 8 || boundary == 9 || boundary == 10) {
        connect(window, &Window::maximizedAboutToChange, &context, intervene);
    } else if (boundary == 7) {
        connect(window, &Window::nextTargetScaleChanged, &context, intervene);
    } else if (boundary == 11) {
        connect(window, &Window::frameGeometryAboutToChange, &context, intervene);
    } else {
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    }
    configure.clear();
    toplevelConfigure.clear();
    window->maximize(boundary >= 10 ? MaximizeRestore : MaximizeFull);
    QVERIFY(intervened);
    if (boundary == 5 || boundary == 6) {
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    const bool owned = window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
        && writer.ownsReservation(id, QString::number(generation)) && window->requestedTile() == leaf && leaf->windows().contains(window);
    QCOMPARE(owned, boundary != 0 && boundary != 3 && boundary != 7);
    QCOMPARE(QRectF(window->geometryRestore()), boundary == 1 ? QRectF(replaced) : original);
    QCOMPARE(writer.snapshot().entries.value(key), original);
    switch (boundary) {
    case 0:
    case 1:
    case 8:
    case 9:
        // The handler's intent precedes the mode write: the outer operation writes no mode and
        // requests no geometry. Row 9's nested round trip leaves Restore requested; only the
        // revision could tell it from an untouched mode.
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
        break;
    case 2:
    case 4:
        // The nested round trip holds the newer intent; the outer operation publishes nothing.
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 2);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(leaf->windowGeometry()));
        break;
    case 3:
        // The mode was written before the revocation: it is published, the geometry stays requested,
        // nothing falls through to the legacy path.
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        break;
    case 7:
        // The lapse is visible only through the ownership provenance: synchronization stops with its
        // obligation pending, the dispatcher is never reached, the mode is still published.
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        QVERIFY(window->m_outputPropertiesPending);
        // Since 6.8 the published state's configure carries moveResizeGeometry() as its bounds, asserted above; no separate position staging.
        break;
    case 10:
        // Infeasible before the mode write: the unmaximize writes nothing, requests nothing, keeps
        // ownership and stays maximized; no legacy fallthrough.
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        break;
    case 11:
        // Infeasible at the dispatcher boundary: the request's own constraint stops the dispatch
        // before it stages anything; the written mode is still published, ownership is kept.
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
        // Since 6.8 the published state's configure carries moveResizeGeometry() as its bounds, asserted above; no separate position staging.
        break;
    }
    if (boundary == 8) {
        // The record is structurally present but no longer valid authority; the handler's desktop stands.
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        QCOMPARE(window->desktops(), QList<VirtualDesktop *>{other});
    }
    QCOMPARE(restoreChanged.count(), boundary == 1 ? 1 : 0);
    // Whatever was published reflects the final requested mode, never an older one.
    if (configure.isEmpty()) {
        configure.wait(200);
    }
    if (!toplevelConfigure.isEmpty()) {
        const auto states = toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>();
        QCOMPARE(bool(states & Test::XdgToplevel::State::Maximized), window->requestedMaximizeMode() == MaximizeFull);
    } else {
        QCOMPARE(window->requestedMaximizeMode(), boundary == 10 ? MaximizeFull : MaximizeRestore);
    }
    if (boundary == 2 || boundary == 4) {
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::yellow);
        QVERIFY(Test::waylandSync());
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(leaf->windowGeometry()));
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QVERIFY(window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned);
    }
}

void TestXdgShellWindow::testOwnedMaximizeDecorated_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("control") << 0;
    QTest::newRow("revoke-at-first-signal") << 1;
}

void TestXdgShellWindow::testOwnedMaximizeDecorated()
{
    // A server-decorated owner: each decoration signal is a callback boundary of its own.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    shell->set_app_id(QStringLiteral("org.example.maximize"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(configure.count() >= 2 || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.maximize");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    auto state = writer.snapshot();
    configure.clear();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership);
    const auto generation = window->m_transientOwnership->generation;
    const QRectF managed = leaf->windowGeometry();
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->frameSizeToClientSize(managed.size()).toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), leaf);
    QCOMPARE(QRectF(window->frameGeometry()), managed);
    auto decorated = window->nextDecoration()->window();
    QVERIFY(decorated);
    QSignalSpy vertical(decorated, &KDecoration3::DecoratedWindow::maximizedVerticallyChanged);
    QSignalSpy horizontal(decorated, &KDecoration3::DecoratedWindow::maximizedHorizontallyChanged);
    QSignalSpy full(decorated, &KDecoration3::DecoratedWindow::maximizedChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QObject context;
    bool intervened = false;
    if (scenario == 1) {
        connect(decorated, &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                leaf->forget(window);
            }
        });
    }
    const auto owned = [&]() {
        return window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
            && writer.ownsReservation(id, QString::number(generation)) && window->requestedTile() == leaf && leaf->windows().contains(window)
            && QRectF(window->geometryRestore()) == original && writer.snapshot().entries.value(key) == original;
    };
    const QRectF area = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    configure.clear();
    toplevelConfigure.clear();
    window->maximize(MaximizeFull);
    QCOMPARE(vertical.count(), 1);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    QCOMPARE(QRectF(window->geometryRestore()), original);
    if (scenario == 1) {
        // The first handler's revocation stops the remaining signals and the geometry; the mode
        // write, already made, is still published.
        QVERIFY(intervened);
        QCOMPARE(horizontal.count(), 0);
        QCOMPARE(full.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        QVERIFY(!owned());
        QVERIFY(!window->requestedTile());
        QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
        QVERIFY(!configure.isEmpty() || configure.wait());
        QVERIFY(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Maximized);
        return;
    }
    QCOMPARE(horizontal.count(), 1);
    QCOMPARE(full.count(), 1);
    QVERIFY(owned());
    QCOMPARE(QRectF(window->moveResizeGeometry()), area);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->frameSizeToClientSize(window->moveResizeGeometry().size()).toSize(), Qt::green);
    QVERIFY(Test::waylandSync());
    QCOMPARE(QRectF(window->frameGeometry()), area);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
    configure.clear();
    window->maximize(MaximizeRestore);
    QVERIFY(owned());
    QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(leaf->windowGeometry()));
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->frameSizeToClientSize(window->moveResizeGeometry().size()).toSize(), Qt::yellow);
    QVERIFY(Test::waylandSync());
    QCOMPARE(QRectF(window->frameGeometry()), QRectF(leaf->windowGeometry()));
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QVERIFY(owned());
}

void TestXdgShellWindow::testOwnedFullScreen_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("committed") << 0;
    QTest::newRow("requested") << 1;
    QTest::newRow("from-full") << 2;
    QTest::newRow("from-horizontal") << 3;
    QTest::newRow("from-vertical") << 4;
    QTest::newRow("client-requested-same-output") << 5;
    QTest::newRow("fast-toggle-stale-ack") << 6;
    QTest::newRow("untile-first-press") << 7;
    QTest::newRow("restore-replaced-before-exit") << 8;
    QTest::newRow("untile-close-during-exit") << 9;
    QTest::newRow("untile-newer-enter-during-exit") << 10;
    QTest::newRow("untile-while-entry-pending") << 11;
    QTest::newRow("untile-round-trip-during-exit") << 12;
}

void TestXdgShellWindow::testOwnedFullScreen()
{
    // An ordinary fullscreen toggle under live ownership keeps ownership, the association and the
    // saved floating frame, stores the rectangle the exit returns to as the fullscreen restore,
    // and returns to it: the managed rectangle, or the maximize-area extents on maximized axes.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.fullscreen"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
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
    const auto key = QStringLiteral("org.example.fullscreen");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    auto state = writer.snapshot();
    configure.clear();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership);
    const auto generation = window->m_transientOwnership->generation;
    const QRectF managed = leaf->windowGeometry();
    if (scenario != 1) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), managed.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf);
    }
    MaximizeMode maximizeMode = MaximizeRestore;
    if (scenario == 2) {
        maximizeMode = MaximizeFull;
    } else if (scenario == 3) {
        maximizeMode = MaximizeHorizontal;
    } else if (scenario == 4) {
        maximizeMode = MaximizeVertical;
    }
    const QRectF maximizeArea = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    QRectF target = managed;
    if (maximizeMode != MaximizeRestore) {
        // The owner enters fullscreen while maximized; the exit returns to the owned maximized geometry.
        configure.clear();
        window->maximize(maximizeMode);
        QCOMPARE(window->requestedMaximizeMode(), maximizeMode);
        QCOMPARE(window->requestedTile(), leaf);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::cyan);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->maximizeMode(), maximizeMode);
        if (maximizeMode & MaximizeHorizontal) {
            target.setLeft(maximizeArea.left());
            target.setRight(maximizeArea.right());
        }
        if (maximizeMode & MaximizeVertical) {
            target.setTop(maximizeArea.top());
            target.setBottom(maximizeArea.bottom());
        }
        QCOMPARE(QRectF(window->moveResizeGeometry()), target);
    }
    const auto owned = [&]() {
        return window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
            && writer.ownsReservation(id, QString::number(generation)) && window->requestedTile() == leaf && leaf->windows().contains(window)
            && QRectF(window->geometryRestore()) == original && writer.snapshot().entries.value(key) == original;
    };
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy fullscreenRestoreChanged(window, &Window::fullscreenGeometryRestoreChanged);
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QSignalSpy fullScreenChanged(window, &Window::fullScreenChanged);
    const QRectF area = workspace()->clientArea(FullScreenArea, window, window->moveResizeOutput());
    const auto outputs = workspace()->outputs();
    configure.clear();
    if (scenario == 5) {
        // The client asks for the output the ownership lives on: the same owned path.
        shell->set_fullscreen(*Test::waylandOutput(outputs[0]->name()));
        QVERIFY(configure.wait());
    } else {
        window->setFullScreen(true);
    }
    QVERIFY(owned());
    QVERIFY(window->isRequestedFullScreen());
    QCOMPARE(QRectF(window->moveResizeGeometry()), area);
    QCOMPARE(QRectF(window->fullscreenGeometryRestore()), target);
    QCOMPARE(fullscreenRestoreChanged.count(), 1);
    QCOMPARE(restoreChanged.count(), 0);
    QCOMPARE(requestedTileChanged.count(), 0);
    QCOMPARE(window->requestedMaximizeMode(), maximizeMode);
    if (scenario == 11) {
        // The explicit action while the entry is still unacknowledged: the owning layer leaves the
        // pending entry as the owner and releases in the same step. The stale entry configure,
        // acknowledged later, commits no association; the newer placement lands with its own.
        QVERIFY(!configure.isEmpty() || configure.wait());
        const quint32 stale = configure.last().at(0).value<quint32>();
        const QSize staleSize = toplevelConfigure.last().at(0).value<QSize>();
        QVERIFY(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen);
        configure.clear();
        workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(QRectF(window->moveResizeGeometry()), target);
        QVERIFY(!window->requestedTile());
        QVERIFY(!writer.ownsReservation(id, QString::number(generation)));
        QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
        QCOMPARE(QRectF(window->geometryRestore()), original);
        QCOMPARE(QRectF(window->fullscreenGeometryRestore()), target);
        QVERIFY(!configure.isEmpty() || configure.wait());
        const quint32 newer = configure.last().at(0).value<quint32>();
        QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
        shell->xdgSurface()->ack_configure(stale);
        Test::render(surface.get(), staleSize, Qt::green);
        QVERIFY(Test::waylandSync());
        QVERIFY(window->isFullScreen());
        QVERIFY(!window->isRequestedFullScreen());
        QVERIFY(!window->tile());
        QVERIFY(!window->requestedTile());
        shell->xdgSurface()->ack_configure(newer);
        Test::render(surface.get(), target.size().toSize(), Qt::yellow);
        QVERIFY(Test::waylandSync());
        QVERIFY(!window->isFullScreen());
        QCOMPARE(QRectF(window->frameGeometry()), target);
        QVERIFY(!window->tile());
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
        return;
    }
    if (scenario == 6) {
        // The exit is requested while the fullscreen configure is still unacknowledged. The stale
        // acknowledgement commits the kept association and the lagging acknowledged state; the newer
        // placement stands and lands with its own acknowledgement.
        QVERIFY(!configure.isEmpty() || configure.wait());
        const quint32 stale = configure.last().at(0).value<quint32>();
        const QSize staleSize = toplevelConfigure.last().at(0).value<QSize>();
        QVERIFY(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen);
        configure.clear();
        window->setFullScreen(false);
        QVERIFY(owned());
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(QRectF(window->moveResizeGeometry()), target);
        QCOMPARE(fullscreenRestoreChanged.count(), 1);
        QVERIFY(!configure.isEmpty() || configure.wait());
        const quint32 newer = configure.last().at(0).value<quint32>();
        QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
        shell->xdgSurface()->ack_configure(stale);
        Test::render(surface.get(), staleSize, Qt::green);
        QVERIFY(Test::waylandSync());
        QVERIFY(window->isFullScreen());
        QVERIFY(!window->isRequestedFullScreen());
        QVERIFY(owned());
        QCOMPARE(window->tile(), leaf);
        QCOMPARE(QRectF(window->moveResizeGeometry()), target);
        shell->xdgSurface()->ack_configure(newer);
        Test::render(surface.get(), target.size().toSize(), Qt::yellow);
        QVERIFY(Test::waylandSync());
        QVERIFY(!window->isFullScreen());
        QCOMPARE(QRectF(window->frameGeometry()), target);
        QVERIFY(owned());
        QCOMPARE(window->tile(), leaf);
        QCOMPARE(requestedTileChanged.count(), 0);
        return;
    }
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    QVERIFY(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen);
    Test::render(surface.get(), area.size().toSize(), Qt::green);
    QVERIFY(Test::waylandSync());
    QVERIFY(window->isFullScreen());
    QCOMPARE(QRectF(window->frameGeometry()), area);
    QCOMPARE(window->maximizeMode(), maximizeMode);
    // The acknowledgement commits a kept association.
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
    QVERIFY(window->m_transientOwnership->committed);
    if (scenario == 9) {
        // The exit's callback closes the window: the explicit action touches nothing afterwards.
        QPointer<Window> guard(window);
        QObject context;
        connect(window, &Window::requestedGeometryChanged, &context, [window]() {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        });
        workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
        QTRY_VERIFY(!guard);
        return;
    }
    if (scenario == 10) {
        // The exit's callback asks for fullscreen again (a newer owned entry): the explicit action
        // does not release, because the exit it asked for is no longer the requested state.
        QObject context;
        bool intervened = false;
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                window->setFullScreen(true);
            }
        });
        workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
        QVERIFY(intervened);
        QVERIFY(window->isRequestedFullScreen());
        QVERIFY(owned());
        QCOMPARE(window->ownedTransientPlacementGeneration(), generation);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        QCOMPARE(QRectF(window->fullscreenGeometryRestore()), target);
        return;
    }
    if (scenario == 12) {
        // The exit's callback runs a newer owned round trip that ends not fullscreen: equal final
        // values prove nothing, and the explicit action releases only an exit that completed as the
        // current intent, so the newer operation survives with its ownership.
        QObject context;
        bool intervened = false;
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                window->setFullScreen(true);
                window->setFullScreen(false);
            }
        });
        workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
        QVERIFY(intervened);
        QVERIFY(!window->isRequestedFullScreen());
        QVERIFY(owned());
        QCOMPARE(window->ownedTransientPlacementGeneration(), generation);
        QCOMPARE(QRectF(window->moveResizeGeometry()), target);
        QCOMPARE(window->requestedTile(), leaf);
        return;
    }
    if (scenario == 7) {
        // The explicit untile-or-restore action releases on its first press: the owned exit to the
        // managed rectangle and the release happen in one dispatch.
        configure.clear();
        workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(QRectF(window->moveResizeGeometry()), target);
        QVERIFY(!window->requestedTile());
        QVERIFY(!writer.ownsReservation(id, QString::number(generation)));
        QVERIFY(!window->m_transientOwnership || window->m_transientOwnership->phase != Window::TransientOwnershipPhase::Owned);
        QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
        QCOMPARE(QRectF(window->geometryRestore()), original);
        QCOMPARE(writer.snapshot().entries.value(key), original);
        QCOMPARE(QRectF(window->fullscreenGeometryRestore()), target);
        return;
    }
    const RectF replaced(30, 30, 200, 100);
    if (scenario == 8) {
        // A legacy consumer replaces the restore field while fullscreen (as the placement tracker
        // may): the owned exit still returns to the managed rectangle, not to that field.
        window->setFullscreenGeometryRestore(replaced);
        QCOMPARE(fullscreenRestoreChanged.count(), 2);
        QVERIFY(owned());
    }
    configure.clear();
    if (scenario == 5) {
        shell->unset_fullscreen();
        QVERIFY(configure.wait());
    } else {
        window->setFullScreen(false);
    }
    QVERIFY(owned());
    QVERIFY(!window->isRequestedFullScreen());
    QCOMPARE(QRectF(window->moveResizeGeometry()), target);
    QCOMPARE(window->requestedMaximizeMode(), maximizeMode);
    // The exit leaves the restore field alone and returns to the frozen rectangle, not to that field.
    QCOMPARE(fullscreenRestoreChanged.count(), scenario == 8 ? 2 : 1);
    QCOMPARE(QRectF(window->fullscreenGeometryRestore()), scenario == 8 ? QRectF(replaced) : target);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
    Test::render(surface.get(), target.size().toSize(), Qt::yellow);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->isFullScreen());
    QCOMPARE(QRectF(window->frameGeometry()), target);
    QCOMPARE(window->maximizeMode(), maximizeMode);
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
    QCOMPARE(restoreChanged.count(), 0);
    QCOMPARE(requestedTileChanged.count(), 0);
    QCOMPARE(fullScreenChanged.count(), 2);
}

void TestXdgShellWindow::testOwnedFullScreenLegacy_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("no-ownership") << 0;
    QTest::newRow("executing-record") << 1;
    QTest::newRow("requested-output-differs") << 2;
    QTest::newRow("exit-infeasible") << 3;
    QTest::newRow("untile-non-owner") << 4;
}

void TestXdgShellWindow::testOwnedFullScreenLegacy()
{
    // Initial ineligibility selects the legacy path before any owned mutation; it releases as
    // before. A window that is not an owner keeps the one-step semantics of the explicit untile.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.fullscreen"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const QRectF original = window->frameGeometry();
    const auto outputs = workspace()->outputs();
    if (scenario == 0) {
        window->setFullScreen(true);
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(QRectF(window->fullscreenGeometryRestore()), original);
        QCOMPARE(QRectF(window->moveResizeGeometry()), workspace()->clientArea(FullScreenArea, window, outputs[0]));
        return;
    }
    if (scenario == 4) {
        // A quick-tiled, fullscreen window that is not an owner: the first press only leaves fullscreen.
        configure.clear();
        window->setQuickTileModeAtCurrentPosition(QuickTileFlag::Left);
        QCOMPARE(window->requestedQuickTileMode(), QuickTileMode(QuickTileFlag::Left));
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        const QRectF tiled = window->frameGeometry();
        configure.clear();
        window->setFullScreen(true);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::green);
        QVERIFY(Test::waylandSync());
        QVERIFY(window->isFullScreen());
        QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
        workspace()->performWindowOperation(window, Options::UntileOrRestoreOp);
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(window->requestedQuickTileMode(), QuickTileMode(QuickTileFlag::Left));
        QCOMPARE(QRectF(window->moveResizeGeometry()), tiled);
        return;
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.fullscreen");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    QObject context;
    bool intervened = false;
    if (scenario == 1) {
        // The record is executing its adoption request: a fullscreen from that boundary is legacy.
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                QVERIFY(window->m_transientOwnership && window->m_transientOwnership->executing);
                window->setFullScreen(true);
            }
        });
    }
    auto state = writer.snapshot();
    configure.clear();
    const auto admission = controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision);
    if (scenario == 1) {
        QVERIFY(intervened);
        QCOMPARE(admission, Result::Superseded);
        QVERIFY(window->isRequestedFullScreen());
        // The legacy geometry ran: the restore field holds the rectangle the adoption had requested
        // and the window was moved to the fullscreen area.
        QCOMPARE(QRectF(window->fullscreenGeometryRestore()), QRectF(leaf->windowGeometry()));
        QCOMPARE(QRectF(window->moveResizeGeometry()), workspace()->clientArea(FullScreenArea, window, outputs[0]));
    } else {
        QCOMPARE(admission, Result::Accepted);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
        const QRectF managed = leaf->windowGeometry();
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), managed.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf);
        configure.clear();
        if (scenario == 2) {
            // The client asks for another output: that leaves the ownership context, so the legacy
            // path moves the window there and releases as before.
            shell->set_fullscreen(*Test::waylandOutput(outputs[1]->name()));
            QVERIFY(configure.wait());
            QVERIFY(window->isRequestedFullScreen());
            QCOMPARE(QRectF(window->moveResizeGeometry()), workspace()->clientArea(FullScreenArea, window, outputs[1]));
            QCOMPARE(QRectF(window->fullscreenGeometryRestore()), managed);
        } else {
            // Owned fullscreen first; then the client raises its minimum width above the managed
            // width, so the exit can no longer return to the managed rectangle.
            window->setFullScreen(true);
            QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
            QCOMPARE(window->requestedTile(), leaf);
            QVERIFY(!configure.isEmpty() || configure.wait());
            shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
            Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::green);
            QVERIFY(Test::waylandSync());
            QVERIFY(window->isFullScreen());
            shell->set_min_size(int(managed.width()) + 100, 0);
            surface->commit(KWayland::Client::Surface::CommitFlag::None);
            QVERIFY(Test::waylandSync());
            QVERIFY(window->minSize().width() > managed.width());
            window->setFullScreen(false);
            QVERIFY(!window->isRequestedFullScreen());
            const QRectF expected(managed.topLeft(), window->constrainFrameSize(managed.size()));
            QVERIFY(expected.width() > managed.width());
            QCOMPARE(QRectF(window->moveResizeGeometry()), expected);
        }
    }
    QVERIFY(!window->requestedTile());
    QVERIFY(!leaf->windows().contains(window));
    QVERIFY(!window->m_transientOwnership || window->m_transientOwnership->phase != Window::TransientOwnershipPhase::Owned);
    QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
    QCOMPARE(QRectF(window->geometryRestore()), original);
}

void TestXdgShellWindow::testOwnedFullScreenSupersession_data()
{
    QTest::addColumn<int>("boundary");
    QTest::newRow("revoke-at-restore") << 0;
    QTest::newRow("replace-at-restore") << 1;
    QTest::newRow("round-trip-at-restore") << 2;
    QTest::newRow("revoke-at-requested-geometry") << 3;
    QTest::newRow("round-trip-at-requested-geometry") << 4;
    QTest::newRow("close-at-restore") << 5;
    QTest::newRow("close-at-requested-geometry") << 6;
    QTest::newRow("reservation-lost-at-scale-synchronization") << 7;
    QTest::newRow("output-request-at-restore") << 8;
    QTest::newRow("output-change-at-restore") << 9;
    QTest::newRow("maximize-at-restore") << 10;
    QTest::newRow("minimum-raised-at-frame-about-to-change-exit") << 11;
    QTest::newRow("revoke-at-requested-geometry-exit") << 12;
    QTest::newRow("round-trip-at-requested-geometry-exit") << 13;
    QTest::newRow("equal-restore-write-at-restore") << 14;
    QTest::newRow("away-and-back-restore-at-restore") << 15;
    QTest::newRow("desktop-change-at-restore") << 16;
    QTest::newRow("output-request-at-frame-about-to-change") << 17;
    QTest::newRow("maximize-at-frame-about-to-change-exit") << 18;
}

void TestXdgShellWindow::testOwnedFullScreenSupersession()
{
    // After the owned path is entered, a handler's newer intent ends it where it stands: no legacy
    // geometry, no exit from the tile, no further restore write; the state write is published.
    QFETCH(int, boundary);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.fullscreen"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
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
    const auto key = QStringLiteral("org.example.fullscreen");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    auto state = writer.snapshot();
    configure.clear();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership);
    const auto generation = window->m_transientOwnership->generation;
    const QRectF managed = leaf->windowGeometry();
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), managed.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), leaf);
    const bool exit = (boundary >= 11 && boundary <= 13) || boundary == 18;
    if (boundary == 7) {
        // Settle the configure the scale change schedules so the owned request synchronizes it.
        configure.clear();
        window->setNextTargetScale(3.0);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), managed.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->nextTargetScale(), 3.0);
    }
    const QRectF area = workspace()->clientArea(FullScreenArea, window, window->moveResizeOutput());
    const QRectF maximizeArea = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    const RectF replaced(30, 30, 200, 100);
    const auto outputs = workspace()->outputs();
    QPointer<Window> guard(window);
    VirtualDesktop *other = nullptr;
    if (boundary == 16) {
        VirtualDesktopManager::self()->setCount(2);
        other = VirtualDesktopManager::self()->desktops().at(1);
    }
    const auto desktopsReset = qScopeGuard([boundary]() {
        if (boundary == 16) {
            VirtualDesktopManager::self()->setCount(1);
        }
    });
    if (exit) {
        // The exit rows start owned and fullscreen; the outer call is the exit.
        configure.clear();
        window->setFullScreen(true);
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(window->requestedTile(), leaf);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::green);
        QVERIFY(Test::waylandSync());
        QVERIFY(window->isFullScreen());
        QCOMPARE(QRectF(window->frameGeometry()), area);
    }
    const auto xdg = static_cast<XdgToplevelWindow *>(window);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy fullscreenRestoreChanged(window, &Window::fullscreenGeometryRestoreChanged);
    QObject context;
    bool intervened = false;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (boundary) {
        case 0:
        case 3:
        case 12:
            leaf->forget(window);
            break;
        case 1:
            window->setFullscreenGeometryRestore(replaced);
            break;
        case 2:
            window->setFullScreen(false);
            window->setFullScreen(true);
            break;
        case 4:
            window->setFullScreen(false);
            window->setFullScreen(true);
            break;
        case 5:
        case 6:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        case 7:
            // Validity lapses in the writer alone; no Window counter changes.
            QVERIFY(writer.forgetWindow(id));
            break;
        case 8:
            // A client fullscreen request with the state already requested, delivered through the
            // interface's signal: the real handler rewrites the output intent and returns early from
            // the state write.
            Q_EMIT xdg->m_shellSurface->fullscreenRequested(nullptr);
            break;
        case 9:
            // The output intent changes identity without any write: the stand-in for the requested
            // output vanishing under the guarded pointer, which only the identity clause observes.
            xdg->m_fullScreenRequestedOutput = outputs[1];
            break;
        case 17:
            // The same request at the dispatch boundary: only the request's own constraint
            // observes it, before anything is staged.
            Q_EMIT xdg->m_shellSurface->fullscreenRequested(nullptr);
            break;
        case 10:
        case 18:
            window->maximize(MaximizeFull);
            break;
        case 11:
            // The client's minimum width rises above the managed width as a property-only change:
            // only the operation's own feasibility, carried by the request, observes it.
            xdg->m_minimumSize = QSizeF(managed.width() + 100, 0);
            QVERIFY(window->minSize().width() > managed.width());
            break;
        case 13:
            window->setFullScreen(true);
            window->setFullScreen(false);
            break;
        case 14:
            // An equal-value write: no notification, no revision, the operation continues.
            window->setFullscreenGeometryRestore(managed);
            break;
        case 15:
            // Away and back: the field ends with the owner's value, the revisions do not.
            window->setFullscreenGeometryRestore(replaced);
            window->setFullscreenGeometryRestore(managed);
            break;
        case 16:
            // The window leaves the recorded desktop; no fence and no release notices.
            window->setDesktops({other});
            break;
        }
    };
    if (boundary <= 2 || boundary == 5 || boundary == 8 || boundary == 9 || boundary == 10 || (boundary >= 14 && boundary <= 16)) {
        connect(window, &Window::fullscreenGeometryRestoreChanged, &context, intervene);
    } else if (boundary == 7) {
        connect(window, &Window::nextTargetScaleChanged, &context, intervene);
    } else if (boundary == 11 || boundary == 17 || boundary == 18) {
        connect(window, &Window::frameGeometryAboutToChange, &context, intervene);
    } else {
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    }
    configure.clear();
    toplevelConfigure.clear();
    window->setFullScreen(!exit);
    QVERIFY(intervened);
    if (boundary == 5 || boundary == 6) {
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    const bool owned = window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
        && writer.ownsReservation(id, QString::number(generation)) && window->requestedTile() == leaf && leaf->windows().contains(window);
    QCOMPARE(owned, boundary != 0 && boundary != 3 && boundary != 7 && boundary != 12);
    QCOMPARE(QRectF(window->geometryRestore()), original);
    QCOMPARE(writer.snapshot().entries.value(key), original);
    QCOMPARE(restoreChanged.count(), 0);
    QCOMPARE(QRectF(window->fullscreenGeometryRestore()), boundary == 1 ? QRectF(replaced) : managed);
    int expectedRestoreWrites = exit ? 0 : 1;
    if (boundary == 1) {
        expectedRestoreWrites = 2;
    } else if (boundary == 15) {
        expectedRestoreWrites = 3;
    }
    QCOMPARE(fullscreenRestoreChanged.count(), expectedRestoreWrites);
    switch (boundary) {
    case 0:
    case 1:
    case 8:
    case 9:
    case 15:
    case 16:
        // The handler's intent precedes the geometry: the state write stands and is published, the
        // restore write stood, no geometry is requested.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
        break;
    case 2:
        // The nested round trip holds the newer intent (its exit requested the current rectangle,
        // which notifies nothing; its enter requested the area); the outer operation stops there.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        break;
    case 3:
        // Revoked after the geometry was requested: nothing falls through to the legacy path.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        break;
    case 4:
    case 13:
        // The nested round trip after the outer request holds the newer intent.
        QCOMPARE(window->isRequestedFullScreen(), boundary == 4);
        QCOMPARE(requestedGeometry.count(), 3);
        QCOMPARE(QRectF(window->moveResizeGeometry()), boundary == 4 ? area : managed);
        break;
    case 7:
        // The lapse is visible only through the ownership provenance: synchronization stops with its
        // obligation pending, the dispatcher is never reached, the state is still published.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        QVERIFY(window->m_outputPropertiesPending);
        // Since 6.8 the published state's configure carries moveResizeGeometry() as its bounds, asserted above; no separate position staging.
        break;
    case 17:
        // The intent changed at the dispatcher boundary without any fenced change: the request's own
        // constraint stops the dispatch before it stages anything; the state is still published.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        // Since 6.8 the published state's configure carries moveResizeGeometry() as its bounds, asserted above; no separate position staging.
        break;
    case 10:
        // A maximize while the fullscreen state is requested is ignored (the window is not
        // resizable): nothing supersedes the operation, which requests the area.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        break;
    case 18:
        // The nested owned maximize at the exit's dispatch boundary holds the newer intent: its
        // geometry generation supersedes the outer request; the exit state is still published. The
        // maximize area equals the fullscreen area here, the last notified rectangle, so the nested
        // request notifies nothing and the outer one never reaches its notification.
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), maximizeArea);
        QCOMPARE(maximizeArea, area);
        break;
    case 11:
        // Infeasible at the dispatcher boundary: the request's own constraint stops the dispatch
        // before it stages anything; the state write is still published, ownership is kept.
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
        // Since 6.8 the published state's configure carries moveResizeGeometry() as its bounds, asserted above; no separate position staging.
        break;
    case 12:
        QVERIFY(!window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
        break;
    case 14:
        // Nothing superseded the operation: the area was requested.
        QVERIFY(window->isRequestedFullScreen());
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), area);
        break;
    }
    if (boundary == 16) {
        // The record is structurally present but no longer valid authority; the handler's desktop stands.
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        QCOMPARE(window->desktops(), QList<VirtualDesktop *>{other});
    }
    // Whatever was published reflects the final requested state, never an older one.
    if (configure.isEmpty()) {
        configure.wait(200);
    }
    if (!toplevelConfigure.isEmpty()) {
        const auto states = toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>();
        QCOMPARE(bool(states & Test::XdgToplevel::State::Fullscreen), window->isRequestedFullScreen());
    } else {
        QCOMPARE(window->isRequestedFullScreen(), !exit);
    }
    if (boundary == 2 || boundary == 4 || boundary == 14) {
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), window->moveResizeGeometry().size().toSize(), Qt::yellow);
        QVERIFY(Test::waylandSync());
        QCOMPARE(QRectF(window->frameGeometry()), area);
        QVERIFY(window->isFullScreen());
        QVERIFY(window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned);
        QCOMPARE(window->tile(), leaf);
    }
}

void TestXdgShellWindow::testOwnedFullScreenDecorated_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("control") << 0;
    QTest::newRow("exit-at-minimum") << 1;
    QTest::newRow("exit-infeasible-after-configuration") << 2;
}

void TestXdgShellWindow::testOwnedFullScreenDecorated()
{
    // A server-decorated owner: the decoration leaves with the fullscreen state and returns on the
    // exit, whose exact feasibility is known only once that decoration exists again.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    shell->set_app_id(QStringLiteral("org.example.fullscreen"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QVERIFY(configure.count() >= 2 || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.fullscreen");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    auto state = writer.snapshot();
    configure.clear();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QVERIFY(window->m_transientOwnership);
    const auto generation = window->m_transientOwnership->generation;
    const QRectF managed = leaf->windowGeometry();
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->frameSizeToClientSize(managed.size()).toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->tile(), leaf);
    QCOMPARE(QRectF(window->frameGeometry()), managed);
    // The client size the managed rectangle leaves under the decoration that will return.
    const QSizeF decoratedClient = window->frameSizeToClientSize(managed.size());
    QVERIFY(decoratedClient.height() < managed.height());
    const auto owned = [&]() {
        return window->m_transientOwnership && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
            && writer.ownsReservation(id, QString::number(generation)) && window->requestedTile() == leaf && leaf->windows().contains(window)
            && QRectF(window->geometryRestore()) == original && writer.snapshot().entries.value(key) == original;
    };
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    const QRectF area = workspace()->clientArea(FullScreenArea, window, window->moveResizeOutput());
    configure.clear();
    toplevelConfigure.clear();
    window->setFullScreen(true);
    QVERIFY(owned());
    QVERIFY(window->isRequestedFullScreen());
    QVERIFY(!window->nextDecoration());
    QCOMPARE(QRectF(window->moveResizeGeometry()), area);
    QCOMPARE(QRectF(window->fullscreenGeometryRestore()), managed);
    QCOMPARE(requestedGeometry.count(), 1);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::green);
    QVERIFY(Test::waylandSync());
    QVERIFY(window->isFullScreen());
    QVERIFY(!window->isDecorated());
    QCOMPARE(QRectF(window->frameGeometry()), area);
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
    if (scenario >= 1) {
        // While fullscreen the enforced minimum is the undecorated one and the managed height is a
        // feasible client height; with the decoration restored the client is smaller by the borders.
        // Row 1 sets the minimum exactly at that boundary, row 2 one past it.
        shell->set_min_size(0, int(decoratedClient.height()) + (scenario == 2 ? 1 : 0));
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->minSize().height(), decoratedClient.height() + (scenario == 2 ? 1 : 0));
        QCOMPARE(window->constrainFrameSize(managed.size()), managed.size());
    }
    configure.clear();
    toplevelConfigure.clear();
    requestedGeometry.clear();
    window->setFullScreen(false);
    QVERIFY(!window->isRequestedFullScreen());
    QVERIFY(window->nextDecoration());
    if (scenario == 2) {
        // Infeasible only under the restored decoration, with the client's limits unchanged: the
        // legacy exit took the operation over and released; it clamps as before.
        QVERIFY(!owned());
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
        const QRectF expected(managed.topLeft(), window->constrainFrameSize(managed.size()));
        QVERIFY(expected.height() > managed.height());
        QCOMPARE(QRectF(window->moveResizeGeometry()), expected);
        QCOMPARE(QRectF(window->geometryRestore()), original);
        QVERIFY(!configure.isEmpty() || configure.wait());
        QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
        return;
    }
    QVERIFY(owned());
    QCOMPARE(QRectF(window->moveResizeGeometry()), managed);
    QCOMPARE(requestedGeometry.count(), 1);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
    Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::yellow);
    QVERIFY(Test::waylandSync());
    QVERIFY(!window->isFullScreen());
    QVERIFY(window->isDecorated());
    QCOMPARE(QRectF(window->frameGeometry()), managed);
    QVERIFY(owned());
    QCOMPARE(window->tile(), leaf);
}

void TestXdgShellWindow::testNormalizeMaximize_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("full") << 0;
    QTest::newRow("horizontal") << 1;
    QTest::newRow("vertical") << 2;
    QTest::newRow("decorated-full") << 3;
    QTest::newRow("restore-no-op") << 4;
    QTest::newRow("no-op-lagging-acknowledgement") << 5;
    QTest::newRow("quick-tile-skips-capture") << 6;
    QTest::newRow("invalid-restore") << 7;
    QTest::newRow("captures-only-unmaximized-axis") << 8;
    QTest::newRow("stale-fence") << 9;
    QTest::newRow("lapsed-provenance") << 10;
    QTest::newRow("refused-owner") << 11;
    QTest::newRow("refused-unresizable") << 12;
    QTest::newRow("refused-fullscreen") << 13;
    QTest::newRow("refused-rule-forced") << 14;
    QTest::newRow("refused-electric-border") << 15;
    QTest::newRow("refused-headroom") << 16;
    QTest::newRow("refused-recursion-at-decoration-signal") << 17;
    QTest::newRow("refused-placement-headroom") << 18;
    QTest::newRow("placement-budget-boundary") << 19;
    QTest::newRow("chained-provenance") << 20;
}

void TestXdgShellWindow::testNormalizeMaximize()
{
    // The guarded normalization makes its own recorded writes and guarded child calls, refuses
    // without effect where the backend would refuse, and returns a receipt whose validity every
    // later step evaluates.
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    const bool decorated = scenario == 3 || scenario == 17;
    KSharedConfig::Ptr rules;
    if (scenario == 14) {
        rules = KSharedConfig::openConfig(QStringLiteral("kwinrulesrc"), KConfig::SimpleConfig);
        for (const QString &group : rules->groupList()) {
            rules->deleteGroup(group);
        }
        workspace()->rulebook()->setConfig(rules);
        rules->group(QStringLiteral("General")).writeEntry("rules", QStringList{QStringLiteral("normalize-rule")});
        KConfigGroup group = rules->group(QStringLiteral("normalize-rule"));
        group.writeEntry("maximizehoriz", true);
        group.writeEntry("maximizehorizrule", int(Rules::Force));
        group.writeEntry("maximizevert", true);
        group.writeEntry("maximizevertrule", int(Rules::Force));
        group.writeEntry("wmclass", "org.example.normalize");
        group.writeEntry("wmclasscomplete", false);
        group.writeEntry("wmclassmatch", int(Rules::ExactMatch));
        group.sync();
        workspace()->slotReconfigure();
    }
    const auto rulesReset = qScopeGuard([&]() {
        if (rules) {
            for (const QString &group : rules->groupList()) {
                rules->deleteGroup(group);
            }
            rules->sync();
            workspace()->slotReconfigure();
        }
    });
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), decorated ? Test::CreationSetup::CreateOnly : Test::CreationSetup::CreateAndConfigure));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration;
    shell->set_app_id(QStringLiteral("org.example.normalize"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    if (decorated) {
        decoration = Test::createXdgToplevelDecorationV1(shell.get());
        decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    }
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(configure.count() >= (decorated ? 2 : 1) || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>().isEmpty() ? QSize(200, 100) : toplevelConfigure.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(Test::waylandSync());
    const auto xdg = static_cast<XdgToplevelWindow *>(window);
    const QRectF original = window->frameGeometry();
    const QRectF area = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    const auto ack = [&](const QColor &color) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), color);
        QVERIFY(Test::waylandSync());
    };
    MaximizeMode mode = MaximizeFull;
    if (scenario == 1) {
        mode = MaximizeHorizontal;
    } else if (scenario == 2) {
        mode = MaximizeVertical;
    }
    // Rows that start maximized through the public path, acknowledged.
    if (scenario <= 3 || scenario == 5 || scenario == 7 || scenario == 9 || scenario == 10 || scenario == 15 || scenario == 16 || scenario == 17 || scenario == 20) {
        configure.clear();
        window->maximize(mode);
        QCOMPARE(window->requestedMaximizeMode(), mode);
        ack(Qt::green);
        QCOMPARE(window->maximizeMode(), mode);
    }
    if (scenario == 14) {
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    TransientTileController controller;
    Tile *leaf = nullptr;
    auto *const quickTile = workspace()->tileManager(window->moveResizeOutput())->quickTile(QuickTileFlag::Left);
    QVERIFY(quickTile);
    switch (scenario) {
    case 5:
        // The requested mode is Restore while the acknowledged one still lags.
        window->maximize(MaximizeRestore);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        break;
    case 6:
        // Tiled, then horizontally maximized by a property-only write: the vertical extents of the
        // tile differ from the restore's, so a capture would be visible; it must be skipped.
        configure.clear();
        window->requestTile(quickTile);
        QCOMPARE(window->requestedTile(), quickTile);
        ack(Qt::blue);
        QVERIFY(QRectF(window->moveResizeGeometry()).height() != original.height());
        xdg->m_requestedMaximizeMode = MaximizeHorizontal;
        ++xdg->m_requestedMaximizeRevision;
        break;
    case 7:
        window->setGeometryRestore(RectF());
        QVERIFY(!window->geometryRestore().isValid());
        break;
    case 8:
    case 18:
    case 19:
        // Horizontally maximized, then moved vertically: only the vertical axis is captured. The
        // capture changes the restore, so the placement budget is exercised in full: a shortage
        // known at entry (the capture would leave the request short) is refused before anything is
        // written, and the last admissible value completes.
        configure.clear();
        window->maximize(MaximizeHorizontal);
        ack(Qt::green);
        QCOMPARE(window->maximizeMode(), MaximizeHorizontal);
        configure.clear();
        window->moveResize(RectF(area.x(), 50, area.width(), 300));
        ack(Qt::cyan);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(area.x(), 50, area.width(), 300));
        QCOMPARE(QRectF(window->geometryRestore()), original);
        if (scenario == 18) {
            window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - 3;
        } else if (scenario == 19) {
            window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - 10;
        }
        break;
    case 11: {
        const auto token = writer.registerProvider(&provider);
        const auto key = QStringLiteral("org.example.normalize");
        QVERIFY(writer.trackWindow(window->internalId(), key));
        QCOMPARE(writer.record(token, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
        auto manager = workspace()->tileManager(window->moveResizeOutput());
        auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
        QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        const auto state = writer.snapshot();
        configure.clear();
        QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        QVERIFY(window->ownedTransientPlacementGeneration());
        break;
    }
    case 12:
        shell->set_min_size(200, 100);
        shell->set_max_size(200, 100);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(Test::waylandSync());
        QVERIFY(!window->isResizable());
        xdg->m_requestedMaximizeMode = MaximizeFull;
        ++xdg->m_requestedMaximizeRevision;
        break;
    case 13:
        configure.clear();
        window->setFullScreen(true);
        QVERIFY(window->isRequestedFullScreen());
        QVERIFY(!window->isResizable());
        xdg->m_requestedMaximizeMode = MaximizeFull;
        ++xdg->m_requestedMaximizeRevision;
        break;
    case 15:
        window->setElectricBorderMode(ElectricBorderMode(MaximizeFull));
        window->setElectricBorderMaximizing(true);
        QVERIFY(window->isElectricBorderMaximizing());
        break;
    case 16:
        xdg->m_requestedMaximizeRevision = std::numeric_limits<quint64>::max() - 1;
        break;
    }
    const auto electricReset = qScopeGuard([&]() {
        if (scenario == 15) {
            window->setElectricBorderMaximizing(false);
            window->setElectricBorderMode(std::nullopt);
        }
    });
    const MaximizeMode before = window->requestedMaximizeMode();
    const auto revisionBefore = xdg->m_requestedMaximizeRevision;
    const QRectF restoreBefore = window->geometryRestore();
    const QRectF geometryBefore = window->moveResizeGeometry();
    QSignalSpy aboutToChange(window, &Window::maximizedAboutToChange);
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    auto fence = window->makeTileAssignmentFence();
    if (scenario == 9) {
        // A later public request makes the input stale before the step runs.
        window->moveResize(RectF(0, 0, 300, 200));
        QVERIFY(!fence.isCurrentFor(*window));
    }
    if (scenario == 17) {
        // A public unmaximize of a decorated window emits its decoration signals under the recursion
        // guard; a private entry from there is refused without effect.
        auto decoratedWindow = window->nextDecoration()->window();
        QVERIFY(decoratedWindow);
        QObject context;
        std::optional<Outcome> nested;
        connect(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedChanged, &context, [&]() {
            if (!nested) {
                auto inner = window->makeTileAssignmentFence();
                nested = window->normalizeMaximizeForAssignment(inner).outcome;
            }
        });
        configure.clear();
        window->maximize(MaximizeRestore);
        QVERIFY(nested.has_value());
        QCOMPARE(*nested, Outcome::Refused);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(aboutToChange.count(), 1);
        return;
    }
    Window::OwnedProvenance provenance;
    bool incoming = true;
    if (scenario == 10) {
        provenance = provenance.withConstraint([]() {
            return false;
        });
    } else if (scenario == 20) {
        // An incoming constraint that holds; the receipt must be chained onto it, not replace it.
        provenance = provenance.withConstraint([&incoming]() {
            return incoming;
        });
    }
    // Only what the step itself publishes is read afterwards.
    configure.clear();
    toplevelConfigure.clear();
    const auto result = window->normalizeMaximizeForAssignment(fence, provenance);
    if (scenario == 9 || scenario == 10) {
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), before);
        QCOMPARE(xdg->m_requestedMaximizeRevision, revisionBefore);
        QCOMPARE(aboutToChange.count(), 0);
        QCOMPARE(restoreChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), scenario == 9 ? 1 : 0);
        return;
    }
    if (scenario >= 11 && scenario <= 18) {
        // Refused: nothing written, nothing notified, no checkpoint.
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), before);
        QCOMPARE(xdg->m_requestedMaximizeRevision, revisionBefore);
        QCOMPARE(QRectF(window->geometryRestore()), restoreBefore);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        QCOMPARE(aboutToChange.count(), 0);
        QCOMPARE(restoreChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        if (scenario == 11) {
            QVERIFY(window->ownedTransientPlacementGeneration());
            QCOMPARE(window->requestedTile(), leaf);
        }
        return;
    }
    QCOMPARE(result.outcome, Outcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QVERIFY(window->assignmentResultIsCurrent(result));
    QCOMPARE(result.owned.kind, Window::OwnedProvenance::Kind::Operation);
    QVERIFY(result.owned.constraint);
    QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
    if (scenario == 4 || scenario == 5) {
        // The operation-owned no-op: nothing recorded, the requested mode governs.
        QCOMPARE(xdg->m_requestedMaximizeRevision, revisionBefore);
        QCOMPARE(aboutToChange.count(), 0);
        QCOMPARE(restoreChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        // A later requested-mode change still invalidates the receipt while the fence stays current.
        xdg->m_requestedMaximizeMode = MaximizeFull;
        ++xdg->m_requestedMaximizeRevision;
        QVERIFY(result.checkpoint->isCurrentFor(*window));
        QVERIFY(!window->assignmentResultIsCurrent(result));
        xdg->m_requestedMaximizeMode = MaximizeRestore;
        return;
    }
    QCOMPARE(xdg->m_requestedMaximizeRevision, revisionBefore + 1);
    QCOMPARE(aboutToChange.count(), 1);
    QCOMPARE(requestedGeometry.count(), 1);
    QRectF expectedRestore = restoreBefore;
    QRectF expected = geometryBefore;
    switch (scenario) {
    case 0:
    case 1:
    case 2:
    case 20:
        // The restore was captured by the public maximize; the released axes return to it.
        expected = original;
        if (mode == MaximizeHorizontal) {
            expected.setTop(geometryBefore.top());
            expected.setBottom(geometryBefore.bottom());
        } else if (mode == MaximizeVertical) {
            expected.setLeft(geometryBefore.left());
            expected.setRight(geometryBefore.right());
        }
        break;
    case 3:
        // Decorated: the released axes return to the restore, clamped to the decorated minimum of
        // 150 on each axis around the restore's centre.
        expected = QRectF(0, 0, std::max(original.width(), 150.0), std::max(original.height(), 150.0));
        expected.moveCenter(original.center());
        break;
    case 6:
        // Capture skipped under the requested quick tile: the restore kept the original extents,
        // the horizontal axis returns to it and the vertical axis keeps the tile's extents.
        expected = QRectF(window->geometryRestore().x(), geometryBefore.y(), window->geometryRestore().width(), geometryBefore.height());
        QCOMPARE(restoreChanged.count(), 0);
        break;
    case 7:
        // An invalid restore: the client chooses its size from the maximize area's origin.
        expected = QRectF(area.x(), area.y(), 0, 0);
        break;
    case 8:
    case 19:
        // Only the vertical axis, not maximized before, was refreshed from the old geometry.
        expectedRestore = QRectF(original.x(), 50, original.width(), 300);
        expected = expectedRestore;
        break;
    }
    QCOMPARE(QRectF(window->geometryRestore()), expectedRestore);
    QCOMPARE(QRectF(window->moveResizeGeometry()), expected);
    if (scenario != 6) {
        QCOMPARE(restoreChanged.count(), expectedRestore == restoreBefore ? 0 : 1);
    }
    // The mode write was published.
    QVERIFY(!configure.isEmpty() || configure.wait());
    QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Maximized));
    if (scenario == 7) {
        QCOMPARE(toplevelConfigure.last().at(0).value<QSize>(), QSize(0, 0));
    }
    if (scenario == 20) {
        // Both constraints must hold: the incoming one lapsing invalidates the receipt on its own.
        incoming = false;
        QVERIFY(result.checkpoint->isCurrentFor(*window));
        QVERIFY(!window->assignmentResultIsCurrent(result));
        incoming = true;
        QVERIFY(window->assignmentResultIsCurrent(result));
    }
    // A later requested-mode change invalidates the receipt while the fence stays current.
    xdg->m_requestedMaximizeMode = MaximizeFull;
    ++xdg->m_requestedMaximizeRevision;
    QVERIFY(result.checkpoint->isCurrentFor(*window));
    QVERIFY(!window->assignmentResultIsCurrent(result));
    xdg->m_requestedMaximizeMode = MaximizeRestore;
}

void TestXdgShellWindow::testNormalizeMaximizeSupersession_data()
{
    QTest::addColumn<int>("boundary");
    QTest::newRow("nested-unmaximize-at-about-to-change") << 0;
    QTest::newRow("nested-manage-at-about-to-change") << 1;
    QTest::newRow("close-at-about-to-change") << 2;
    QTest::newRow("replace-restore-at-decoration-signal") << 3;
    QTest::newRow("replace-restore-at-restore-signal") << 4;
    QTest::newRow("away-and-back-restore-at-restore-signal") << 5;
    QTest::newRow("equal-restore-write-at-restore-signal") << 6;
    QTest::newRow("round-trip-at-requested-geometry") << 7;
    QTest::newRow("close-at-requested-geometry") << 8;
    QTest::newRow("membership-no-op-at-restore-signal") << 9;
    QTest::newRow("mode-away-and-back-at-restore-signal") << 10;
    QTest::newRow("minimum-raised-at-frame-about-to-change") << 11;
    QTest::newRow("decoration-cleared-at-decoration-signal") << 12;
    QTest::newRow("borderless-restored") << 13;
    QTest::newRow("mode-written-at-decoration-policy-change") << 14;
    QTest::newRow("fence-destroyed-at-about-to-change") << 15;
    QTest::newRow("minimum-raised-at-about-to-change") << 16;
    QTest::newRow("placement-exhausted-at-about-to-change") << 17;
    QTest::newRow("borderless-reverted-at-decoration-policy-change") << 18;
    QTest::newRow("minimum-raised-at-decoration-policy-change") << 19;
}

void TestXdgShellWindow::testNormalizeMaximizeSupersession()
{
    // After the mode write, a handler's newer intent ends the normalization where it stands: the
    // written mode is published, nothing falls through, nothing is rolled back.
    QFETCH(int, boundary);
    using Outcome = Window::AssignmentStepOutcome;
    const bool decorated = boundary == 3 || boundary == 12;
    const bool borderless = boundary == 13 || boundary == 14 || boundary == 18 || boundary == 19;
    if (borderless) {
        options->setBorderlessMaximizedWindows(true);
    }
    const auto optionsReset = qScopeGuard([borderless]() {
        if (borderless) {
            options->setBorderlessMaximizedWindows(false);
        }
    });
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), (decorated || borderless) ? Test::CreationSetup::CreateOnly : Test::CreationSetup::CreateAndConfigure));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration;
    shell->set_app_id(QStringLiteral("org.example.normalize"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    if (decorated || borderless) {
        decoration = Test::createXdgToplevelDecorationV1(shell.get());
        decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    }
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(configure.count() >= ((decorated || borderless) ? 2 : 1) || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>().isEmpty() ? QSize(200, 100) : toplevelConfigure.last().at(0).value<QSize>(), Qt::red);
    QVERIFY(Test::waylandSync());
    const auto xdg = static_cast<XdgToplevelWindow *>(window);
    const QRectF original = window->frameGeometry();
    const QRectF area = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    const auto ack = [&](const QColor &color) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), color);
        QVERIFY(Test::waylandSync());
    };
    // The restore-signal rows need a capture that changes the restore: a fully maximized window
    // refreshes no axis, so those rows start horizontally maximized and vertically moved.
    const bool partial = boundary == 4 || boundary == 5 || boundary == 6 || boundary == 9 || boundary == 10;
    configure.clear();
    window->maximize(partial ? MaximizeHorizontal : MaximizeFull);
    QCOMPARE(window->requestedMaximizeMode(), partial ? MaximizeHorizontal : MaximizeFull);
    ack(Qt::green);
    QCOMPARE(window->maximizeMode(), partial ? MaximizeHorizontal : MaximizeFull);
    if (borderless) {
        QVERIFY(window->noBorder());
    }
    if (partial) {
        configure.clear();
        window->moveResize(RectF(area.x(), 50, area.width(), 300));
        ack(Qt::cyan);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(area.x(), 50, area.width(), 300));
    }
    const QRectF captured(original.x(), 50, original.width(), 300);
    const QRectF geometryBefore = window->moveResizeGeometry();
    auto *const quickTile = workspace()->tileManager(window->moveResizeOutput())->quickTile(QuickTileFlag::Left);
    QVERIFY(quickTile);
    const RectF replaced(30, 30, 200, 100);
    QPointer<Window> guard(window);
    KDecoration3::DecoratedWindow *decoratedWindow = decorated ? window->nextDecoration()->window() : nullptr;
    const auto revisionBefore = xdg->m_requestedMaximizeRevision;
    QSignalSpy aboutToChange(window, &Window::maximizedAboutToChange);
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    // The later decoration signals must stay suppressed once a decoration boundary ends the step.
    std::unique_ptr<QSignalSpy> horizontalSignals;
    std::unique_ptr<QSignalSpy> fullSignals;
    if (decoratedWindow) {
        horizontalSignals = std::make_unique<QSignalSpy>(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedHorizontallyChanged);
        fullSignals = std::make_unique<QSignalSpy>(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedChanged);
    }
    // The caller's fence, owned here so a handler can destroy it under the step.
    std::unique_ptr<TileAssignmentFence> ownedFence;
    QObject context;
    bool intervened = false;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (boundary) {
        case 0:
            window->maximize(MaximizeRestore);
            break;
        case 1:
            QVERIFY(quickTile->manage(window));
            break;
        case 2:
        case 8:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        case 3:
        case 4:
            window->setGeometryRestore(replaced);
            break;
        case 5:
            window->setGeometryRestore(replaced);
            window->setGeometryRestore(captured);
            break;
        case 6:
            window->setGeometryRestore(window->geometryRestore());
            break;
        case 7:
            window->maximize(MaximizeFull);
            window->maximize(MaximizeRestore);
            break;
        case 9:
            // A public membership no-op: only the membership revision observes it.
            QVERIFY(!quickTile->unmanage(window));
            break;
        case 10:
        case 14:
            // Property-only: the requested mode away and back, no fenced change.
            xdg->m_requestedMaximizeMode = MaximizeFull;
            ++xdg->m_requestedMaximizeRevision;
            xdg->m_requestedMaximizeMode = MaximizeRestore;
            ++xdg->m_requestedMaximizeRevision;
            break;
        case 11:
        case 16:
        case 19:
            xdg->m_minimumSize = QSizeF(original.width() + 100, 0);
            QVERIFY(window->minSize().width() > original.width());
            break;
        case 12:
        case 18:
            window->setNoBorder(true);
            break;
        case 15:
            ownedFence.reset();
            break;
        case 17:
            window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - 3;
            break;
        }
    };
    if (boundary <= 2 || (boundary >= 15 && boundary <= 17)) {
        connect(window, &Window::maximizedAboutToChange, &context, intervene);
    } else if (boundary == 3 || boundary == 12) {
        connect(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, &context, intervene);
    } else if (boundary >= 4 && boundary <= 6 || boundary == 9 || boundary == 10) {
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, intervene);
    } else if (boundary == 7 || boundary == 8) {
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    } else if (boundary == 11) {
        connect(window, &Window::frameGeometryAboutToChange, &context, intervene);
    } else if (boundary == 14 || boundary == 18 || boundary == 19) {
        connect(window, &Window::decorationPolicyChanged, &context, intervene);
    }
    configure.clear();
    toplevelConfigure.clear();
    ownedFence = window->makeTileAssignmentCheckpoint();
    const auto result = window->normalizeMaximizeForAssignment(*ownedFence);
    QCOMPARE(intervened, boundary != 13);
    if (boundary == 2 || boundary == 8) {
        // The step stopped at the close. The caller's fence still holds a reference to the
        // logically deleted window, so it is released before the deletion can be observed.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(guard && guard->isDeleted());
        ownedFence.reset();
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    switch (boundary) {
    case 0:
        // The nested public unmaximize holds the newer intent; the outer wrote nothing.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(aboutToChange.count(), 2);
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), original);
        break;
    case 1:
        // The nested management (legacy unmaximize and tiling) holds the newer intent.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(window->requestedTile(), quickTile);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(quickTile->windowGeometry()));
        break;
    case 3:
    case 4:
    case 5:
    case 9:
    case 10:
    case 14:
        // Superseded after the mode write: published, no geometry requested, nothing rolled back.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        {
            QRectF expectedRestore = partial ? captured : original;
            int expectedWrites = 1;
            if (boundary == 3 || boundary == 4) {
                expectedRestore = QRectF(replaced);
                expectedWrites = boundary == 3 ? 1 : 2;
            } else if (boundary == 5) {
                expectedWrites = 3;
            } else if (boundary == 14) {
                expectedWrites = 0;
            }
            QCOMPARE(QRectF(window->geometryRestore()), expectedRestore);
            QCOMPARE(restoreChanged.count(), expectedWrites);
        }
        if (boundary == 10 || boundary == 14) {
            QCOMPARE(xdg->m_requestedMaximizeRevision, revisionBefore + 3);
        }
        break;
    case 6:
        // An equal write is a no-op: the normalization completed with the captured restore.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->geometryRestore()), captured);
        QCOMPARE(QRectF(window->moveResizeGeometry()), captured);
        QCOMPARE(restoreChanged.count(), 1);
        break;
    case 7:
        // The nested public round trip holds the newer intent.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 3);
        QCOMPARE(QRectF(window->moveResizeGeometry()), original);
        break;
    case 11:
        // The frozen size limits no longer hold at the dispatch boundary: the request stops before
        // staging anything; the mode is published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), original);
        // Since 6.8 the published state's configure carries moveResizeGeometry() as its bounds, asserted above; no separate position staging.
        break;
    case 12:
        // The decoration was cleared by the handler: its identity no longer holds.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 0);
        QVERIFY(window->noBorder());
        window->setNoBorder(false);
        break;
    case 13: {
        // Borderless maximizing: the border returns as the operation's own transition, the receipt
        // is established against the decorated minimum of 150 on each axis, and the normalization
        // completes with the restore clamped to it around its centre.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!window->noBorder());
        QCOMPARE(requestedGeometry.count(), 1);
        QRectF expected(0, 0, std::max(original.width(), 150.0), std::max(original.height(), 150.0));
        expected.moveCenter(original.center());
        QCOMPARE(QRectF(window->moveResizeGeometry()), expected);
        break;
    }
    case 15:
        // Destroying the caller's fence is not a state change: the step reads its own snapshot.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), original);
        break;
    case 18:
    case 19:
        // The operation's own decoration transition was crossed by a foreign change (the policy it
        // set reverted, or the client's limits raised): the receipt is not established, nothing is
        // captured or requested, and the written mode is published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(window->noBorder(), boundary == 18);
        QCOMPARE(restoreChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        break;
    case 16:
    case 17:
        // A limit change or a placement shortage introduced at the about-to-change boundary ends
        // the step before the mode write: nothing written, nothing published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(xdg->m_requestedMaximizeRevision, revisionBefore);
        QCOMPARE(aboutToChange.count(), 1);
        QCOMPARE(restoreChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        break;
    }
    if (horizontalSignals) {
        QCOMPARE(horizontalSignals->count(), 0);
        QCOMPARE(fullSignals->count(), 0);
    }
    if (boundary == 10 || boundary == 14) {
        // The handler's property-only writes made the step's mode write stale, so its publication
        // obligation is not discharged: the next states still carry the earlier maximize until a
        // current write publishes them (a decoration change may send them as they are).
        QVERIFY(xdg->m_nextStates & XdgToplevelInterface::State::MaximizedHorizontal);
        return;
    }
    // Whatever was published reflects the final requested mode.
    if (configure.isEmpty()) {
        configure.wait(200);
    }
    if (!toplevelConfigure.isEmpty()) {
        const auto states = toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>();
        QCOMPARE(bool(states & Test::XdgToplevel::State::Maximized), window->requestedMaximizeMode() == MaximizeFull);
    } else {
        QCOMPARE(window->requestedMaximizeMode(), (boundary == 16 || boundary == 17) ? MaximizeFull : MaximizeRestore);
    }
}

void TestXdgShellWindow::testCancellationPreparation_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("no-owner") << 0;
    QTest::newRow("prepared-owner-clean-release") << 1;
    QTest::newRow("admitted-owner-release-cannot-finalize") << 2;
    QTest::newRow("membership-stamp-at-window-removed") << 3;
    QTest::newRow("nested-request-at-requested-tile-changed") << 4;
    QTest::newRow("revision-bump-at-requested-tile-changed") << 5;
    QTest::newRow("close-at-window-removed") << 6;
    QTest::newRow("reentrant-cancellation-during-release") << 7;
    QTest::newRow("exhausted-placement-refused") << 8;
    QTest::newRow("deleted-window") << 9;
    QTest::newRow("legacy-helper-unchanged") << 10;
    QTest::newRow("placement-boundary-applied") << 11;
    QTest::newRow("active-request-without-owner") << 12;
}

void TestXdgShellWindow::testCancellationPreparation()
{
    // The public cancellation boundary as a qualified continuation: the stamp and the release's
    // own writes are inside the checkpoint, the release's notifications are callback boundaries,
    // and one terminal validation decides whether the caller may continue.
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    using Result = TransientPlacementWriter::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.cancellation"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const QRectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.cancellation");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    const auto state = writer.snapshot();
    const bool prepared = scenario == 1;
    const bool admitted = scenario == 2 || (scenario >= 3 && scenario <= 8) || scenario == 10;
    // A Prepared reservation releases through the window-only path and finalizes cleanly; an
    // admitted owner releases through the controller and cannot finalize synchronously (its
    // progress is Requested), so the owner stays for a consumer's entry refusal.
    RequestTestTile requestTile(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    quint64 generation = 0;
    if (prepared) {
        generation = window->prepareTransientOwnership(&writer, token, key, state.providerRevision, state.mapRevision,
                                                       std::make_unique<RequestForTest>(window, &requestTile, authority, 0));
        QVERIFY(generation);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Prepared);
        QVERIFY(!window->m_transientOwnership->controller);
        QVERIFY(writer.isReserved(id));
    }
    TransientTileController controller;
    Tile *leaf = nullptr;
    if (admitted) {
        auto manager = workspace()->tileManager(window->moveResizeOutput());
        auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
        QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        QVERIFY(leaf);
        configure.clear();
        QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        generation = window->ownedTransientPlacementGeneration();
        QVERIFY(generation);
        QCOMPARE(window->requestedTile(), leaf);
        QVERIFY(leaf->windows().contains(window));
        QCOMPARE(window->m_transientOwnership->progress, Window::TransientRequestProgress::Requested);
    }
    auto *const quickTile = workspace()->tileManager(window->moveResizeOutput())->quickTile(QuickTileFlag::Left);
    QVERIFY(quickTile);
    QPointer<Window> guard(window);
    std::unique_ptr<TileAssignmentFence> retained;
    if (scenario == 6 || scenario == 9) {
        // A caller-owned checkpoint retains the window across its close, so the logically deleted
        // window can be observed after the call; it is released before the destruction is awaited.
        retained = window->makeTileAssignmentCheckpoint();
    }
    if (scenario == 9) {
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QVERIFY(guard && guard->isDeleted());
    }
    if (scenario == 8) {
        window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 11) {
        // The last usable value: the stamp lands below exhaustion and the checkpoint is current.
        window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - 2;
    }
    const auto placementBefore = window->m_placementContinuationRevision;
    const auto requestedBefore = window->m_requestedTileChange;
    const auto committedBefore = window->m_committedTileChange;
    const auto membershipBefore = window->m_tileMembershipChange;
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    std::unique_ptr<QSignalSpy> leafRemoved;
    std::unique_ptr<QSignalSpy> leafChanged;
    if (leaf) {
        leafRemoved = std::make_unique<QSignalSpy>(leaf, &Tile::windowRemoved);
        leafChanged = std::make_unique<QSignalSpy>(leaf, &Tile::windowsChanged);
    }
    QObject context;
    bool intervened = false;
    bool hadActiveRequest = false;
    quint64 placementAtInner = 0;
    quint64 placementAfterInner = 0;
    std::optional<Window::AssignmentStepResult> inner;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (scenario) {
        case 12:
            // Inside an in-flight request without ownership: the stamp and the cancellation mark
            // are the caller's own, no release runs, the continuation starts right there.
            hadActiveRequest = window->m_activeTransientRequest != nullptr;
            placementAtInner = window->m_placementContinuationRevision;
            inner = window->cancelTransientRequestForAssignment();
            placementAfterInner = window->m_placementContinuationRevision;
            break;
        case 3:
            // A public membership no-op: only the membership revision observes it.
            QVERIFY(!quickTile->unmanage(window));
            break;
        case 4:
            // A nested public request: the release is already running, so its cancellation
            // acquires nothing and the public path proceeds to tile.
            window->requestTile(quickTile);
            break;
        case 5:
            // Property-only: the requested-association revision alone.
            window->m_requestedTileChange = window->m_requestedTileChange + 1;
            break;
        case 6:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        case 7:
            inner = window->cancelTransientRequestForAssignment();
            break;
        }
    };
    if (scenario == 3 || scenario == 6 || scenario == 7) {
        connect(leaf, &Tile::windowRemoved, &context, intervene);
    } else if (scenario == 4 || scenario == 5) {
        connect(window, &Window::requestedTileChanged, &context, intervene);
    } else if (scenario == 12) {
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    }
    if (scenario == 12) {
        QVERIFY(window->canStartTransientTileRequest(requestTile.windowGeometry()));
        const auto receipt = window->requestTransientTile(std::make_unique<RequestForTest>(window, &requestTile, authority));
        QVERIFY(intervened);
        QVERIFY(hadActiveRequest);
        QVERIFY(inner.has_value());
        QCOMPARE(inner->outcome, Outcome::AppliedCurrent);
        QVERIFY(inner->checkpoint);
        // The stamp is the inner call's only placement write; the request's own writes before
        // and after the handler are its own continuation, not the cancellation's.
        QCOMPARE(placementAfterInner, placementAtInner + 1);
        QVERIFY(window->m_placementContinuationRevision > placementBefore);
        // The cancelled request does not apply; what it clears afterwards is its own continuation.
        QVERIFY(receipt.outcome != Window::TransientRequestOutcome::Applied);
        QVERIFY(!window->m_transientOwnership);
        return;
    }
    if (scenario == 10) {
        // The legacy helper keeps its body and its bool: the release runs as before.
        QVERIFY(window->cancelTransientRequest());
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 1);
        QCOMPARE(leafRemoved->count(), 1);
        QVERIFY(!window->requestedTile());
        QVERIFY(window->m_transientOwnership);
        return;
    }
    const auto result = window->cancelTransientRequestForAssignment();
    QCOMPARE(intervened, scenario >= 3 && scenario <= 7);
    switch (scenario) {
    case 11:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->m_placementContinuationRevision, std::numeric_limits<quint64>::max() - 1);
        break;
    case 0:
        // No owner: the stamp alone, inside the checkpoint.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 1);
        QCOMPARE(window->m_requestedTileChange, requestedBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 1:
        // The Prepared reservation is cancelled through the window-only path and finalized: the
        // owner is gone, the reservation released, and only the stamp was written.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!window->m_transientOwnership);
        QVERIFY(!writer.isReserved(id));
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 1);
        QCOMPARE(window->m_requestedTileChange, requestedBefore);
        QCOMPARE(window->m_committedTileChange, committedBefore);
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        break;
    case 2:
        // The admitted owner's release runs its writes (the transient removal, the requested
        // detachment) and notifications inside the checkpoint but cannot finalize: the owner
        // stays, the continuation is current.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(window->m_transientOwnership && window->m_transientOwnership->generation == generation);
        QVERIFY(!window->requestedTile());
        QVERIFY(!leaf->windows().contains(window));
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 1);
        QCOMPARE(window->m_requestedTileChange, requestedBefore + 1);
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(leafRemoved->count(), 1);
        QCOMPARE(leafChanged->count(), 1);
        break;
    case 3:
    case 4:
    case 5:
        // A foreign change inside the release's notifications: the continuation is stale, the
        // release itself completed its notifications regardless.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(leafRemoved->count(), 1);
        QCOMPARE(leafChanged->count(), 1);
        if (scenario == 3) {
            QCOMPARE(window->m_tileMembershipChange, membershipBefore + 2);
        } else if (scenario == 4) {
            // The handler's intent stands.
            QCOMPARE(window->requestedTile(), quickTile);
        } else {
            QCOMPARE(window->m_requestedTileChange, requestedBefore + 2);
        }
        break;
    case 6:
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(guard && guard->isDeleted());
        break;
    case 7:
        // The reentrant cancellation acquired no release (one was running): its own writes are
        // complete and no callback ran in its frame, so its continuation starts there; the outer
        // continuation is stale because of it.
        QVERIFY(inner.has_value());
        QCOMPARE(inner->outcome, Outcome::AppliedCurrent);
        QVERIFY(inner->checkpoint);
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 2);
        QVERIFY(window->m_transientOwnership);
        QVERIFY(window->assignmentResultIsCurrent(*inner));
        break;
    case 8:
        // Exhaustion known before the stamp: refused without effect, no release ran.
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        QCOMPARE(leafRemoved->count(), 0);
        QCOMPARE(window->requestedTile(), leaf);
        break;
    case 9:
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        break;
    }
    if (scenario == 6 || scenario == 9) {
        retained.reset();
        QTRY_VERIFY(!guard);
    }
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

void TestXdgShellWindow::testTransientReceiptLifetime_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("applied-current") << 0;
    QTest::newRow("stale-after-geometry") << 1;
    QTest::newRow("stale-after-membership-loss") << 2;
    QTest::newRow("stale-after-association-revision") << 3;
    QTest::newRow("stale-after-admission-authority") << 4;
    QTest::newRow("stale-after-writer-loss") << 5;
    QTest::newRow("stale-after-provider-retired") << 6;
    QTest::newRow("stale-after-owned-maximize") << 7;
    QTest::newRow("stale-after-owned-maximize-roundtrip") << 8;
    QTest::newRow("destructor-revokes-authority") << 9;
    QTest::newRow("destructor-moves-window") << 10;
    QTest::newRow("prepared-no-success") << 11;
    QTest::newRow("executing-no-success") << 12;
    QTest::newRow("superseded-at-request-notification") << 13;
    QTest::newRow("superseded-before-request") << 14;
    QTest::newRow("release-no-revival") << 15;
    QTest::newRow("provider-retired-before-apply") << 16;
    QTest::newRow("superseded-by-client-resize") << 17;
}

void TestXdgShellWindow::testTransientReceiptLifetime()
{
    // The application's qualified continuation outlives the request it was validated through:
    // current after the request's destruction while the attempt's checkpoint, the ownership and
    // the attempt's own bindings hold; invalidated by later changes without being refreshed; never
    // a current success for an attempt that did not finish while Owned.
    QFETCH(int, scenario);
    using Result = TransientPlacementWriter::Result;
    using Outcome = Window::AssignmentStepOutcome;
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
    auto provider = std::make_unique<QObject>();
    const auto token = writer->registerProvider(provider.get());
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const RectF target(original.x() + 40, original.y() + 30, original.width(), original.height());
    const QRectF manual(70, 80, 240, 140);
    QVERIFY(writer->trackWindow(id, QStringLiteral("app")));
    QCOMPARE(writer->record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    RequestTestTile tile(window);
    tile.setTargetForTest(target);
    QCOMPARE(tile.windowGeometry(), target);
    QObject owner;
    auto state = std::make_shared<LifetimeState>();
    state->owner = &owner;
    // The caller's stable admission constraint: its own state, consulted with the continuation.
    auto admission = std::make_shared<RequestAuthority>();
    admission->owner = &owner;
    int admissionChecks = 0;
    const std::function<bool()> admissionConstraint = [admission, &admissionChecks]() {
        ++admissionChecks;
        return admission->owner && admission->valid;
    };
    const auto snapshot = writer->snapshot();
    const auto generation = window->prepareTransientOwnership(writer.get(), token, QStringLiteral("app"), snapshot.providerRevision, snapshot.mapRevision,
                                                              std::make_unique<LifetimeRequestForTest>(window, &tile, state));
    QVERIFY(generation);
    QVERIFY(!window->transientOwnershipIsCurrent(generation));
    if (scenario == 11) {
        // Prepared: reservation only. No application starts, nothing is continued, the record stays.
        const auto application = window->applyTransientOwnershipGuarded(generation, admissionConstraint);
        QCOMPARE(application.receipt.outcome, Window::TransientRequestOutcome::Rejected);
        QCOMPARE(application.continuation.outcome, Outcome::Refused);
        QVERIFY(!application.continuation.checkpoint);
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        QCOMPARE(admissionChecks, 0);
        QVERIFY(!state->destroyed);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Prepared);
        QVERIFY(writer->isReserved(id));
        QVERIFY(window->revokeTransientOwnership(generation));
        QVERIFY(!window->m_transientOwnership);
        QVERIFY(state->destroyed);
        QVERIFY(!writer->isReserved(id));
        return;
    }
    QVERIFY(window->activateTransientOwnership(generation));
    QVERIFY(!window->transientOwnershipIsCurrent(generation));
    if (scenario == 16) {
        // The provider retired before the application: revoked before any request, the
        // continuation concluded, the record finalized without a request.
        provider.reset();
        const auto application = window->applyTransientOwnershipGuarded(generation, admissionConstraint);
        QCOMPARE(application.receipt.outcome, Window::TransientRequestOutcome::Rejected);
        QCOMPARE(application.continuation.outcome, Outcome::Superseded);
        QVERIFY(!application.continuation.checkpoint);
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        QVERIFY(state->destroyed);
        QVERIFY(!window->m_transientOwnership);
        QVERIFY(!writer->isReserved(id));
        return;
    }
    QObject context;
    bool observed = false;
    Window::TransientApplication reentrant;
    bool reentrantOwnershipCurrent = true;
    if (scenario == 9) {
        // The request's destructor ends the caller's authority: the terminal validation, which
        // runs after that destruction, sees it.
        state->onDestroy = [admission, window, generation, &reentrantOwnershipCurrent]() {
            admission->valid = false;
            reentrantOwnershipCurrent = window->transientOwnershipIsCurrent(generation);
        };
    } else if (scenario == 10) {
        // The request's destructor moves the window through the public path: the ownership ends
        // while the owner is still executing.
        state->onDestroy = [window, manual]() {
            window->moveResize(manual);
        };
    } else if (scenario == 12) {
        // A reentrant application inside the request's own notification: the owner is executing,
        // so nothing starts and no success is exported.
        connect(window, &Window::frameGeometryChanged, &context, [&]() {
            if (observed) {
                return;
            }
            observed = true;
            reentrantOwnershipCurrent = window->transientOwnershipIsCurrent(generation);
            reentrant = window->applyTransientOwnershipGuarded(generation, admissionConstraint);
        });
    } else if (scenario == 13) {
        // A public move inside the request's notification: superseded after the request, the
        // record retained as the restoration's barrier.
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (observed) {
                return;
            }
            observed = true;
            window->moveResize(manual);
        });
    } else if (scenario == 14) {
        // A public move inside the restore capture's notification: superseded before any request,
        // the record finalized with its NoRequest proof.
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
            if (observed) {
                return;
            }
            observed = true;
            window->moveResize(manual);
        });
    }
    const auto application = window->applyTransientOwnershipGuarded(generation, admissionConstraint);
    // The request's destructor ran inside the application, before its result was concluded.
    QVERIFY(state->destroyed);
    if (scenario == 13 || scenario == 14) {
        QVERIFY(observed);
        QCOMPARE(application.receipt.outcome, Window::TransientRequestOutcome::Superseded);
        QCOMPARE(application.continuation.outcome, Outcome::Superseded);
        QVERIFY(!application.continuation.checkpoint);
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        if (scenario == 13) {
            QCOMPARE(application.receipt.progress, Window::TransientRequestProgress::Requested);
            QVERIFY(!application.receipt.noRequestProof);
            QVERIFY(window->m_transientOwnership);
            QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
            QCOMPARE(window->m_transientOwnership->progress, Window::TransientRequestProgress::Requested);
            QVERIFY(writer->isReserved(id));
        } else {
            QCOMPARE(application.receipt.progress, Window::TransientRequestProgress::NoRequest);
            QVERIFY(application.receipt.noRequestProof);
            QVERIFY(!window->m_transientOwnership);
            QVERIFY(!writer->isReserved(id));
        }
        return;
    }
    QCOMPARE(application.receipt.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(application.receipt.progress, Window::TransientRequestProgress::Requested);
    QCOMPARE(application.receipt.ownershipGeneration, generation);
    QVERIFY(application.receipt.nativeSequence);
    QCOMPARE(application.receipt.nativeSequence, window->m_transientRequestSequence);
    QVERIFY(!application.receipt.noRequestProof);
    QCOMPARE(window->frameGeometry(), target);
    if (scenario == 9) {
        // Applied, then invalidated by the destructor: no current success; the ownership itself
        // stands (the prerequisite changes no release policy; the caller's continuation decides).
        QVERIFY(!reentrantOwnershipCurrent);
        QCOMPARE(application.continuation.outcome, Outcome::Superseded);
        QVERIFY(!application.continuation.checkpoint);
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        QVERIFY(admissionChecks >= 1);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
        QVERIFY(window->transientOwnershipIsCurrent(generation));
        return;
    }
    if (scenario == 10) {
        // Applied, then the ownership ended by the destructor's public move: the record went to
        // its restoration with the attempt's progress retained, the continuation concluded.
        QCOMPARE(application.continuation.outcome, Outcome::Superseded);
        QVERIFY(!application.continuation.checkpoint);
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QCOMPARE(window->m_transientOwnership->progress, Window::TransientRequestProgress::Requested);
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        QCOMPARE(QRectF(window->moveResizeGeometry()), manual);
        return;
    }
    // Current after the request's destruction, bound to this ownership generation.
    QCOMPARE(application.continuation.outcome, Outcome::AppliedCurrent);
    QVERIFY(application.continuation.checkpoint);
    QCOMPARE(application.continuation.owned.kind, Window::OwnedProvenance::Kind::Ownership);
    QCOMPARE(application.continuation.owned.ownership, generation);
    QVERIFY(!application.continuation.owned.request);
    QVERIFY(admissionChecks >= 1);
    QVERIFY(window->transientOwnershipIsCurrent(generation));
    QVERIFY(window->assignmentResultIsCurrent(application.continuation));
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    QVERIFY(!window->m_transientOwnership->request);
    QVERIFY(writer->ownsReservation(id, QString::number(generation)));
    const int checksAfterApplication = admissionChecks;
    QVERIFY(window->assignmentResultIsCurrent(application.continuation));
    QCOMPARE(admissionChecks, checksAfterApplication + 1);
    if (scenario == 12) {
        QVERIFY(observed);
        QVERIFY(!reentrantOwnershipCurrent);
        QCOMPARE(reentrant.receipt.outcome, Window::TransientRequestOutcome::Rejected);
        QCOMPARE(reentrant.continuation.outcome, Outcome::Refused);
        QVERIFY(!reentrant.continuation.checkpoint);
        QVERIFY(!window->assignmentResultIsCurrent(reentrant.continuation));
    }
    if (scenario == 0 || scenario == 12) {
        // Evidence of this attempt, not a replay: a second application is rejected without
        // touching the attempt, and the continuation stays current.
        QCOMPARE(window->applyTransientOwnership(generation).outcome, Window::TransientRequestOutcome::Rejected);
        QCOMPARE(window->m_transientRequestSequence, application.receipt.nativeSequence);
        QVERIFY(window->assignmentResultIsCurrent(application.continuation));
        return;
    }
    // A later change invalidates the continuation; the outcome field is never refreshed.
    switch (scenario) {
    case 1:
        window->moveResize(manual);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        break;
    case 2:
        // The tile forgets its member without any fence counter moving.
        tile.abandonWithoutForgetForTest();
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        break;
    case 3:
        // The association revision alone, the pointers unchanged: the checkpoint sees it.
        window->m_requestedTileChange = window->m_requestedTileChange + 1;
        QVERIFY(window->transientOwnershipIsCurrent(generation));
        break;
    case 4:
        admission->valid = false;
        QVERIFY(window->transientOwnershipIsCurrent(generation));
        break;
    case 5:
        writer.reset();
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        break;
    case 6:
        provider.reset();
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        break;
    case 7:
        // An owner may be maximized: the ownership stays current, this attempt's placement does
        // not (its request generation and its target are gone).
        window->maximize(MaximizeFull);
        QVERIFY(window->transientOwnershipIsCurrent(generation));
        QVERIFY(QRectF(window->moveResizeGeometry()) != QRectF(target));
        break;
    case 8:
        // Back to the managed rectangle through two further requests: the target holds again, the
        // request generation does not.
        window->maximize(MaximizeFull);
        window->maximize(MaximizeRestore);
        QVERIFY(window->transientOwnershipIsCurrent(generation));
        QCOMPARE(window->moveResizeGeometry(), target);
        break;
    case 17: {
        // The client resizes itself: the compositor's intended geometry and the request generation
        // hold and the ownership stays current, but the committed frame leaves the target and its
        // publication advances the placement continuation revision, which the checkpoint observes.
        const auto placementBefore = window->m_placementContinuationRevision;
        QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
        Test::render(surface.get(), QSize(260, 140), Qt::blue);
        QVERIFY(frameChanged.wait());
        QVERIFY(QRectF(window->frameGeometry()) != QRectF(target));
        QCOMPARE(window->moveResizeGeometry(), target);
        QVERIFY(window->m_placementContinuationRevision != placementBefore);
        QVERIFY(window->transientOwnershipIsCurrent(generation));
        break;
    }
    case 15: {
        // A release of the applied attempt keeps the record as the restoration's barrier (no
        // readmission is possible on this window while it stands), and bringing the window back to
        // the exact target through the public path revives nothing.
        QVERIFY(!window->revokeTransientOwnership(generation));
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QVERIFY(!window->transientOwnershipIsCurrent(generation));
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        window->moveResize(target);
        QCOMPARE(window->moveResizeGeometry(), target);
        QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
        QObject replacementProvider;
        const auto replacementToken = writer->registerProvider(&replacementProvider);
        const auto replacement = writer->snapshot();
        QCOMPARE(window->prepareTransientOwnership(writer.get(), replacementToken, QStringLiteral("app"), replacement.providerRevision, replacement.mapRevision,
                                                   std::make_unique<LifetimeRequestForTest>(window, &tile, std::make_shared<LifetimeState>())),
                 quint64(0));
        break;
    }
    default:
        QFAIL("unhandled scenario");
    }
    QCOMPARE(application.continuation.outcome, Outcome::AppliedCurrent);
    QVERIFY(!window->assignmentResultIsCurrent(application.continuation));
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

void TestXdgShellWindow::testTransientAdmissionReceipt_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("accepted") << 0;
    QTest::newRow("superseded-at-window-added-by-desktop") << 1;
    QTest::newRow("superseded-at-windows-changed-by-commit") << 2;
    QTest::newRow("superseded-at-window-added-by-provider") << 3;
    QTest::newRow("superseded-at-last-request-notification") << 4;
    QTest::newRow("refused-headroom-membership") << 5;
    QTest::newRow("refused-headroom-association") << 6;
    QTest::newRow("refused-headroom-placement") << 7;
    QTest::newRow("ineligible-requested-association") << 8;
    QTest::newRow("ineligible-unsettled-geometry") << 9;
}

void TestXdgShellWindow::testTransientAdmissionReceipt()
{
    // The admission as a transaction: a complete preflight refuses before any provisional state, the
    // publication runs on a checkpoint sealed after the admission's own writes with the admission's
    // currentness as provenance, the native request's continuation decides acceptance beyond the
    // request's destruction, and any later failure releases this attempt's membership and
    // association only.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    auto desktops = VirtualDesktopManager::self();
    const auto oldCount = desktops->count();
    const auto restoreDesktops = qScopeGuard([desktops, oldCount]() {
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
    configure.clear();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const QRectF original = window->frameGeometry();
    const auto key = QStringLiteral("org.example.admission");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto current = desktops->currentDesktop(window->moveResizeOutput());
    auto other = desktops->desktops().first() == current ? desktops->desktops().last() : desktops->desktops().first();
    QVERIFY(other != current);
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, current, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto generation = controller.generation();
    QPointer<Tile> leaf(controller.tile());
    const auto target = leaf->windowGeometry();
    Tile ordinary(manager);
    QSignalSpy added(leaf, &Tile::windowAdded);
    QSignalSpy removed(leaf, &Tile::windowRemoved);
    QSignalSpy windowsChanged(leaf, &Tile::windowsChanged);
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    QObject context;
    // The publication's own second notification, apart from the one the release publishes with
    // the removal.
    int changedBeforeRemoval = 0;
    connect(leaf, &Tile::windowsChanged, &context, [&]() {
        if (removed.isEmpty()) {
            ++changedBeforeRemoval;
        }
    });
    const auto exhausted = std::numeric_limits<quint64>::max();
    const auto membershipOriginal = window->m_tileMembershipChange;
    const auto associationOriginal = window->m_requestedTileChange;
    const auto placementOriginal = window->m_placementContinuationRevision;
    QMetaObject::Connection desktopChange;
    switch (scenario) {
    case 1:
        // A counter-free context change inside the first notification: only the admission's
        // currentness sees it, through the publication's provenance.
        desktopChange = connect(leaf, &Tile::windowAdded, &context, [&]() {
            window->setDesktops({other});
        });
        break;
    case 2:
        // A committed association installed inside the second notification: the checkpoint sees it,
        // and the admission's cleanup leaves that manual placement in place.
        connect(leaf, &Tile::windowsChanged, &context, [&]() {
            window->commitTile(&ordinary);
        });
        break;
    case 3:
        // The provider retired inside the first notification: the writer's own release runs inside
        // the publication, and the admission's currentness ends with the provider.
        connect(leaf, &Tile::windowAdded, &context, [&]() {
            provider.reset();
        });
        break;
    case 4:
        // The admission's context changed inside the request's last notification: the request's
        // own authority ends it after the request was made, and the barrier is kept.
        connect(window, &Window::requestedTileChanged, &context, [&]() {
            window->setDesktops({other});
        });
        break;
    case 5:
        window->m_tileMembershipChange = exhausted - 2;
        break;
    case 6:
        window->m_requestedTileChange = exhausted - 2;
        break;
    case 7:
        window->m_placementContinuationRevision = exhausted - Window::assignmentPlacementBudget;
        break;
    case 8:
        // A requested association without a membership, as a public request leaves it: ineligible
        // before any reservation.
        window->m_requestedTile = &ordinary;
        break;
    case 9:
        // A compositor resize the client has not acknowledged: the native geometry is not settled.
        window->moveResize(RectF(original.x(), original.y(), original.width() + 60, original.height() + 40));
        QVERIFY(QRectF(window->moveResizeGeometry()) != QRectF(window->frameGeometry()));
        break;
    default:
        break;
    }
    // The counters as the admission finds them (a public resize in the setup advances the
    // placement continuation revision on its own).
    const auto membershipBefore = window->m_tileMembershipChange;
    const auto associationBefore = window->m_requestedTileChange;
    const auto placementBefore = window->m_placementContinuationRevision;
    const auto state = writer.snapshot();
    const auto result = controller.admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision);
    if (scenario >= 5) {
        // Refused for a known shortage, or ineligible: before any reservation, membership or
        // association, and before any provisional admission state (a second admission is not
        // Busy, it is refused the same way).
        QCOMPARE(result, scenario <= 7 ? Result::Refused : Result::Ineligible);
        QCOMPARE(controller.admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision), result);
        if (scenario == 8) {
            window->m_requestedTile = nullptr;
        }
        QCOMPARE(added.count(), 0);
        QCOMPARE(windowsChanged.count(), 0);
        QCOMPARE(geometry.count(), scenario == 9 ? 1 : 0);
        QVERIFY(!writer.isReserved(id));
        QVERIFY(!window->m_transientOwnership);
        QVERIFY(!leaf->windows().contains(window));
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        if (scenario == 5) {
            window->m_tileMembershipChange = membershipOriginal;
        } else if (scenario == 6) {
            window->m_requestedTileChange = associationOriginal;
        } else if (scenario == 7) {
            window->m_placementContinuationRevision = placementOriginal;
        }
        return;
    }
    if (scenario == 0) {
        QCOMPARE(result, Result::Accepted);
        QCOMPARE(added.count(), 1);
        QCOMPARE(windowsChanged.count(), 1);
        QVERIFY(leaf->windows().contains(window));
        QCOMPARE(window->requestedTile(), leaf.data());
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
        QCOMPARE(window->m_transientOwnership->progress, Window::TransientRequestProgress::Requested);
        QVERIFY(writer.ownsReservation(id, QString::number(window->m_transientOwnership->generation)));
        QCOMPARE(window->moveResizeGeometry(), target);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), target.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf.data());
        return;
    }
    QCOMPARE(result, Result::Superseded);
    QCOMPARE(added.count(), 1);
    // The release publishes the removal once, with its own notification.
    QCOMPARE(removed.count(), 1);
    QCOMPARE(windowsChanged.count(), changedBeforeRemoval + 1);
    QVERIFY(!leaf->windows().contains(window));
    QVERIFY(!window->requestedTile());
    if (scenario == 4) {
        // After the request: the record stays as the restoration's barrier with its progress, the
        // reservation with it; the attempt's membership and association are released.
        QCOMPARE(changedBeforeRemoval, 1);
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QCOMPARE(window->m_transientOwnership->progress, Window::TransientRequestProgress::Requested);
        QVERIFY(writer.isReserved(id));
        QCOMPARE(window->desktops(), (QList<VirtualDesktop *>{other}));
        return;
    }
    // Before any request: no further publication after the superseding notification, nothing
    // requested, the record finalized with its NoRequest proof, the reservation gone, the frame
    // untouched.
    QCOMPARE(changedBeforeRemoval, scenario == 2 ? 1 : 0);
    QCOMPARE(geometry.count(), 0);
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(QRectF(window->frameGeometry()), original);
    if (scenario == 2) {
        // The manual placement a callback installed survives the admission's cleanup, and the
        // admission holds no provisional state: a new admission is refused for that placement,
        // not Busy.
        QCOMPARE(window->tile(), &ordinary);
        QCOMPARE(controller.admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Ineligible);
        return;
    }
    if (scenario == 3) {
        QVERIFY(controller.admit(generation, window, &writer, token, key, state.providerRevision, state.mapRevision) != Result::Busy);
        return;
    }
    if (scenario == 1) {
        // Released and readmitted: a new attempt with its own generation, the old one never revived.
        const auto oldGeneration = window->m_lastTransientOwnershipGeneration;
        QVERIFY(oldGeneration);
        // Only the first attempt is superseded; the second must run without the injected move.
        QVERIFY(disconnect(desktopChange));
        window->setDesktops({current});
        // The desktop changes scheduled configures for the activation state; the admission refuses
        // pending native geometry, so the client settles them first. The first attempt's
        // finalization advanced the writer, so the readmission freezes it anew.
        for (int settled = 0; settled < 3 && !window->transientAdmissionEligible(target); ++settled) {
            QVERIFY(!configure.isEmpty() || configure.wait());
            shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
            configure.clear();
            Test::render(surface.get(), QSize(200, 100), Qt::red);
            QVERIFY(Test::waylandSync());
        }
        QVERIFY(window->transientAdmissionEligible(target));
        const auto fresh = writer.snapshot();
        QCOMPARE(controller.admit(generation, window, &writer, token, key, fresh.providerRevision, fresh.mapRevision), Result::Accepted);
        QVERIFY(window->m_transientOwnership);
        QVERIFY(window->m_transientOwnership->generation > oldGeneration);
        QVERIFY(leaf->windows().contains(window));
        QCOMPARE(window->moveResizeGeometry(), target);
    }
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

void TestXdgShellWindow::testControllerForeignMembership_data()
{
    QTest::addColumn<bool>("activeDesktop");
    QTest::addColumn<bool>("otherOutput");
    QTest::newRow("inactive-desktop") << false << false;
    QTest::newRow("active-desktop") << true << false;
    QTest::newRow("other-output-inactive-desktop") << false << true;
}

void TestXdgShellWindow::testControllerForeignMembership()
{
    QFETCH(bool, activeDesktop);
    QFETCH(bool, otherOutput);
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
    // Only acknowledge a configure produced by the membership setup below, not the old activation.
    configure.clear();
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto current = desktops->currentDesktop(window->moveResizeOutput());
    auto other = desktops->desktops().first() == current ? desktops->desktops().last() : desktops->desktops().first();
    window->setOnAllDesktops(true);
    auto foreignManager = manager;
    if (otherOutput) {
        const auto outputs = workspace()->outputs();
        auto output = outputs.first() == window->moveResizeOutput() ? outputs.last() : outputs.first();
        QVERIFY(output != window->moveResizeOutput());
        foreignManager = workspace()->tileManager(output);
    }
    auto foreign = foreignManager->rootTile(activeDesktop ? current : other)->pick(foreignManager->output()->geometryF().center());
    QVERIFY(foreign);
    QCOMPARE(foreign->isActive(), activeDesktop);
    QVERIFY(foreign->manage(window));
    window->setDesktops({current});
    QVERIFY(foreign->windows().contains(window));
    if (activeDesktop) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), foreign->windowGeometry().size().toSize(), Qt::red);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->requestedTile(), foreign);
        QCOMPARE(window->tile(), foreign);
    } else {
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
        QCOMPARE(window->moveResizeOutput(), manager->output());
    }
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
    QSignalSpy added(controller.tile(), &Tile::windowAdded);
    QSignalSpy changed(controller.tile(), &Tile::windowsChanged);
    QSignalSpy removed(foreign, &Tile::windowRemoved);
    const auto requested = window->requestedTile();
    const auto committed = window->tile();
    const auto membership = window->m_tileMembershipChange;
    const auto geometry = window->moveResizeGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Ineligible);
    QVERIFY(foreign->windows().contains(window));
    QCOMPARE(added.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(window->requestedTile(), requested);
    QCOMPARE(window->tile(), committed);
    QCOMPARE(window->m_tileMembershipChange, membership);
    QCOMPARE(window->moveResizeGeometry(), geometry);
    QVERIFY(!writer.isReserved(id));
    QVERIFY(!window->m_transientOwnership);
    QVERIFY(foreign->unmanage(window));
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testControllerClearAfterWriterLoss_data()
{
    QTest::addColumn<int>("stage");
    QTest::newRow("after-admission") << 0;
    QTest::newRow("during-window-added") << 1;
    QTest::newRow("during-windows-changed") << 2;
}

void TestXdgShellWindow::testControllerClearAfterWriterLoss()
{
    QFETCH(int, stage);
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
    QSignalSpy added(controller.tile(), &Tile::windowAdded);
    QSignalSpy removed(controller.tile(), &Tile::windowRemoved);
    QSignalSpy changed(controller.tile(), &Tile::windowsChanged);
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    QObject context;
    const auto loseWriter = [&]() {
        writer.reset();
    };
    if (stage == 1) {
        connect(controller.tile(), &Tile::windowAdded, &context, loseWriter);
    } else if (stage == 2) {
        connect(controller.tile(), &Tile::windowsChanged, &context, loseWriter);
    }
    const auto state = writer->snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, writer.get(), token, key, state.providerRevision, state.mapRevision),
             stage == 0 ? Result::Accepted : Result::Superseded);
    QCOMPARE(added.count(), 1);
    if (stage == 0) {
        writer.reset();
    } else {
        QVERIFY(!writer);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(changed.count(), stage == 1 ? 1 : 2);
        QCOMPARE(geometry.count(), 0);
        QVERIFY(controller.tile()->windows().isEmpty());
        QCOMPARE(QRectF(window->frameGeometry()), original);
    }
    QCOMPARE(controller.clearTree(controller.generation()), Result::Accepted);
    QVERIFY(!window->requestedTile());
    QVERIFY(!window->tile());
    QVERIFY(window->m_transientOwnership);
    QVERIFY(!window->m_transientOwnership->controller);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QVERIFY(!window->m_transientOwnership->writer);
    QVERIFY(!window->m_transientOwnership->releaseWriterReady);
    QCOMPARE(window->m_transientOwnership->progress,
             stage == 0 ? Window::TransientRequestProgress::Requested : Window::TransientRequestProgress::NoRequest);
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

void TestXdgShellWindow::testTransientConfigurePublication_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("buffer-callback-manual-move") << 0;
    QTest::newRow("client-callback-manual-resize") << 1;
    QTest::newRow("frame-callback-native-reentry") << 2;
    QTest::newRow("frame-callback-close") << 3;
    QTest::newRow("frame-callback-new-manual-tile") << 4;
    QTest::newRow("frame-callback-native-committed-association") << 5;
}

void TestXdgShellWindow::testTransientConfigurePublication()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.configure-publication"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const RectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.configure-publication");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, QRectF(original), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto automatic = controller.tile()->windowGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), automatic.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const auto owner = window->ownedTransientPlacementGeneration();
    auto claim = window->claimTransientRestore(owner);
    QCOMPARE(claim.continuation.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    auto request = window->requestTransientRestore(std::move(claim));
    QCOMPARE(request.result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(!configure.isEmpty() || configure.wait());
    const quint32 serial = configure.last().at(0).value<quint32>();
    configure.clear();
    Tile *manualTile = nullptr;
    if (scenario == 4 || scenario == 5) {
        manualTile = manager->rootTile(desktop)->childTile(0);
        QVERIFY(manualTile && !manualTile->isTransient());
        QVERIFY(manualTile->windowGeometry().size() != original.size());
    }
    const RectF manual(90, 100, 300, 200);
    bool entered = false;
    QObject context;
    const auto intervene = [&]() {
        if (entered) {
            return;
        }
        entered = true;
        if (scenario == 0) {
            window->move(manual.topLeft());
        } else if (scenario == 1) {
            window->moveResize(manual);
        } else if (scenario == 2) {
            Q_EMIT window->surface()->committed();
        } else if (scenario == 3) {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        } else if (scenario == 4) {
            window->setTileCompatibility(manualTile);
            QCOMPARE(window->requestedTile(), manualTile);
        } else {
            // Native association seam, separate from the public assignment/ACK row. An
            // obsolete null configure must not erase this newer committed association.
            window->commitTile(manualTile);
            QCOMPARE(window->tile(), manualTile);
            QVERIFY(!window->requestedTile());
        }
    };
    if (scenario == 0) {
        connect(window, &Window::bufferGeometryChanged, &context, intervene);
    } else if (scenario == 1) {
        connect(window, &Window::clientGeometryChanged, &context, intervene);
    } else {
        connect(window, &Window::frameGeometryChanged, &context, intervene);
    }
    QPointer<Window> guard(window);
    shell->xdgSurface()->ack_configure(serial);
    Test::render(surface.get(), original.size().toSize(), Qt::green);
    QVERIFY(Test::waylandSync());
    QVERIFY(entered);
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QVERIFY(writer.isReserved(id));
    if (scenario == 3) {
        QVERIFY(window->isDeleted());
    } else if (scenario == 0) {
        const RectF moved(manual.topLeft(), original.size());
        QCOMPARE(window->frameGeometry(), moved);
        QCOMPARE(window->moveResizeGeometry(), moved);
    } else if (scenario == 1) {
        QCOMPARE(window->frameGeometry(), original);
        QCOMPARE(window->moveResizeGeometry(), manual);
    } else if (scenario == 2) {
        QCOMPARE(window->frameGeometry(), original);
        QCOMPARE(window->moveResizeGeometry(), original);
        QVERIFY(!static_cast<XdgSurfaceWindow *>(window)->m_pendingContentState);
    } else if (scenario == 4) {
        QCOMPARE(window->requestedTile(), manualTile);
        QVERIFY(!window->tile());
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), manualTile->windowGeometry().size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), manualTile);
        QCOMPARE(window->frameGeometry(), manualTile->windowGeometry());
    } else {
        QCOMPARE(window->tile(), manualTile);
        QVERIFY(!window->requestedTile());
    }
    request.result.checkpoint.reset();
    if (guard && !guard->isDeleted()) {
        guard->setSkipCloseAnimation(true);
        guard->destroyWindow();
    }
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testConfigureContentHandoff_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("states-no-ack-reentry") << 0;
    QTest::newRow("states-newer-ack-reentry") << 1;
    QTest::newRow("states-unknown-ack-reentry") << 2;
    QTest::newRow("prefix-disposal-unchanged-content") << 3;
    QTest::newRow("decoration-apply-no-ack-reentry") << 4;
    QTest::newRow("scale-no-ack-reentry") << 5;
    QTest::newRow("pending-content-blocks-admission") << 6;
    QTest::newRow("pending-fullscreen-blocks-interactive-placement") << 7;
}

void TestXdgShellWindow::testConfigureContentHandoff()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    configure.clear();
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!window->decoration());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    auto xdg = static_cast<XdgToplevelWindow *>(window);
    QVERIFY(xdg->m_configureEvents.isEmpty());
    QPointer<KDecoration3::Decoration> watched;
    if (scenario == 3 || scenario == 4 || scenario == 6) {
        decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
        QVERIFY(configure.wait());
        watched = xdg->m_nextDecoration.get();
        QVERIFY(watched);
        if (scenario == 3) {
            configure.clear();
            decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);
            QVERIFY(configure.wait());
            QVERIFY(!xdg->m_nextDecoration);
        }
    } else if (scenario == 5 || scenario == 7) {
        window->setNextTargetScale(2.0);
        QVERIFY(configure.wait());
    } else {
        window->moveResize(RectF(60, 70, 300, 200));
        QVERIFY(configure.wait());
    }
    const quint32 selected = configure.last().at(0).value<quint32>();
    auto event = static_cast<XdgToplevelConfigure *>(xdg->m_configureEvents.last());
    // This is a native content-handler fixture: inject combined committed states so one
    // synchronous callback can interrupt processing between maximize and fullscreen.
    event->states |= XdgToplevelInterface::State::Maximized | XdgToplevelInterface::State::FullScreen;
    if (scenario >= 3) {
        event->scale = 2.0;
    }
    auto expectedStates = event->states;
    quint32 successor = 0;
    if (scenario == 1) {
        configure.clear();
        window->moveResize(RectF(90, 100, 400, 300));
        QVERIFY(configure.wait());
        successor = configure.last().at(0).value<quint32>();
        expectedStates = static_cast<XdgToplevelConfigure *>(xdg->m_configureEvents.last())->states;
    }
    bool entered = false;
    bool admissionChecked = false;
    QObject context;
    const auto reenter = [&]() {
        if (entered) {
            return;
        }
        entered = true;
        if (scenario == 1 || scenario == 2) {
            Q_EMIT xdg->XdgSurfaceWindow::m_shellSurface->configureAcknowledged(
                scenario == 1 ? successor : std::numeric_limits<quint32>::max());
        }
        Q_EMIT window->surface()->committed();
    };
    if (scenario == 3) {
        connect(watched, &QObject::destroyed, &context, reenter);
    } else if (scenario == 4 || scenario == 6) {
        connect(watched, &KDecoration3::Decoration::currentStateChanged, &context, reenter);
    } else if (scenario == 5 || scenario == 7) {
        connect(window, &Window::targetScaleChanged, &context, reenter);
    } else {
        connect(window, &Window::maximizedChanged, &context, reenter);
    }
    if (scenario == 6) {
        connect(window, &Window::decorationChanged, &context, [&]() {
            if (!entered) {
                return;
            }
            admissionChecked = true;
            QVERIFY(xdg->m_pendingContentState);
            QVERIFY(!xdg->m_lastAcknowledgedConfigure);
            QVERIFY(!xdg->m_lastAcknowledgedConfigureSerial);
            QVERIFY(xdg->m_configureEvents.isEmpty());
            QVERIFY(!xdg->m_configureTimer->isActive());
            QVERIFY(window->isTransientTileTargetFeasible(RectF(0, 0, 300, 200)));
            QVERIFY(!window->transientAdmissionEligible(RectF(0, 0, 300, 200)));
        });
    }
    const QPointF originalPosition = window->pos();
    if (scenario == 7) {
        // An interactive move (6.8: Gravity::Center, no resize edge).
        window->setInteractiveMoveResizeGravity(Gravity::Center);
        window->setInteractiveMoveResizeAnchor(QPointF(400, 300));
        window->setInteractiveMoveOffset(QPointF(0.5, 0.5));
        window->setUnrestrictedInteractiveMoveResize(true);
        QVERIFY(window->startInteractiveMoveResize());
    }
    shell->xdgSurface()->ack_configure(selected);
    Test::render(surface.get(), scenario >= 3 ? QSize(200, 100) : QSize(300, 200), Qt::blue);
    QVERIFY(Test::waylandSync());
    QVERIFY(entered);
    QCOMPARE(window->maximizeMode(), scenario == 1 ? MaximizeRestore : MaximizeFull);
    QCOMPARE(window->isFullScreen(), scenario != 1);
    QCOMPARE(xdg->m_acknowledgedStates, expectedStates);
    QVERIFY(!xdg->m_pendingContentState);
    if (scenario >= 3) {
        QCOMPARE(window->targetScale(), 2.0);
    }
    if (scenario == 4 || scenario == 6) {
        QCOMPARE(window->decoration(), watched.data());
    }
    if (scenario == 6) {
        QVERIFY(admissionChecked);
    }
    if (scenario == 7) {
        QCOMPARE(window->pos(), originalPosition);
        QCOMPARE(window->moveResizeGeometry().topLeft(), originalPosition);
        window->finishInteractiveMoveResize(false);
    }
    if (scenario == 2) {
        QCOMPARE(xdg->m_lastContentOrigin.kind, Window::GeometryRequestOrigin::Kind::Unclassified);
    }
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testTransientConfigurePlacement_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("admission-after-claim") << 0;
    QTest::newRow("admission-after-restore-request") << 1;
    QTest::newRow("restore-after-manual-resize") << 2;
    QTest::newRow("restore-after-manual-move") << 3;
    QTest::newRow("cancelled-coalesced-restore") << 4;
    QTest::newRow("current-restore-different-content") << 5;
    QTest::newRow("current-restore-no-ack-content") << 6;
    QTest::newRow("unknown-ack-then-no-ack-content") << 7;
    QTest::newRow("fresh-ordinary-after-cancel") << 8;
    QTest::newRow("fresh-ordinary-no-ack-content") << 9;
    QTest::newRow("provider-retired-before-commit") << 10;
    QTest::newRow("root-removed-current-restore") << 11;
    QTest::newRow("scale-callback-manual-resize") << 12;
    QTest::newRow("scale-callback-native-reentry") << 13;
    QTest::newRow("generic-release-control") << 14;
    QTest::newRow("owned-control") << 15;
    QTest::newRow("synchronization-callback-claim") << 16;
    QTest::newRow("missing-configure-origin") << 17;
    QTest::newRow("wrong-origin-owner") << 18;
    QTest::newRow("wrong-origin-restore-sequence") << 19;
    QTest::newRow("wrong-origin-geometry-generation") << 20;
    QTest::newRow("wrong-origin-kind") << 21;
    QTest::newRow("restore-origin-without-record") << 22;
    QTest::newRow("interactive-obsolete-content") << 23;
    QTest::newRow("ambiguous-ack-content-lineage") << 24;
    QTest::newRow("wrapped-admission-before-new-request") << 25;
    QTest::newRow("wrapped-restore-before-new-request") << 26;
}

void TestXdgShellWindow::testTransientConfigurePlacement()
{
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.configure-origin"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const RectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.configure-origin");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, QRectF(original), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    TransientTileController controller;
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    QPointer<Tile> root(controller.rootTile());
    const RectF automatic = controller.tile()->windowGeometry();
    QVERIFY(automatic != original);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    const auto owner = window->ownedTransientPlacementGeneration();
    QVERIFY(!configure.isEmpty() || configure.wait());
    QCOMPARE(window->m_geometryRequestOrigin.kind, Window::GeometryRequestOrigin::Kind::OwnedGeometry);
    QCOMPARE(window->m_geometryRequestOrigin.geometryGeneration, window->geometryRequestGeneration());
    QCOMPARE(window->m_geometryRequestOrigin.ownershipGeneration, owner);
    quint32 serial = configure.last().at(0).value<quint32>();
    auto xdg = static_cast<XdgToplevelWindow *>(window);
    wl_resource *wireSurface = nullptr;
    if (scenario == 25 || scenario == 26) {
        const auto objectId = wl_proxy_get_id(reinterpret_cast<wl_proxy *>(shell->xdgSurface()->object()));
        wireSurface = wl_client_get_object(window->surface()->client()->client(), objectId);
        QVERIFY(wireSurface);
    }
    if (scenario == 25) {
        // Use the qualified wire-value fixture with protected admission provenance, not just
        // an ordinary configure queue. This does not alter libwayland's serial allocator.
        serial = std::numeric_limits<quint32>::max();
        xdg->m_configureEvents.last()->serial = serial;
        xdg_surface_send_configure(wireSurface, serial);
        QVERIFY(Test::waylandSync());
        QCOMPARE(configure.last().at(0).value<quint32>(), serial);
    }
    const bool admissionContent = scenario == 0 || scenario == 1 || (scenario >= 12 && scenario <= 15) || scenario == 25;
    if (!admissionContent) {
        shell->xdgSurface()->ack_configure(serial);
        Test::render(surface.get(), automatic.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->frameGeometry(), automatic);
        configure.clear();
    }
    Window::TransientRestoreClaim claim;
    Window::AssignmentGeometryRequest request;
    if (scenario == 14) {
        QVERIFY(window->cancelTransientRequest());
    } else if (scenario != 15 && scenario != 16) {
        claim = window->claimTransientRestore(owner);
        QCOMPARE(claim.continuation.outcome, Outcome::AppliedCurrent);
        if (scenario != 0) {
            request = window->requestTransientRestore(std::move(claim));
            QCOMPARE(request.result.outcome, Outcome::AppliedCurrent);
            QCOMPARE(window->m_geometryRequestOrigin.kind, Window::GeometryRequestOrigin::Kind::Restoration);
            QCOMPARE(window->m_geometryRequestOrigin.geometryGeneration, request.requestGeneration);
            QCOMPARE(window->m_geometryRequestOrigin.ownershipGeneration, owner);
            if (scenario == 4) {
                QVERIFY(window->cancelTransientRequest());
            }
            if (!admissionContent) {
                QVERIFY(!configure.isEmpty() || configure.wait());
                serial = configure.last().at(0).value<quint32>();
                if (scenario == 26) {
                    serial = std::numeric_limits<quint32>::max();
                    xdg->m_configureEvents.last()->serial = serial;
                    xdg_surface_send_configure(wireSurface, serial);
                    QVERIFY(Test::waylandSync());
                    QCOMPARE(configure.last().at(0).value<quint32>(), serial);
                }
                configure.clear();
            }
        }
    }
    const RectF manual(90, 120, 300, 200);
    std::unique_ptr<Window::TransientOwnership> heldOwnership;
    if (scenario == 2 || scenario == 8 || scenario == 9 || scenario == 25 || scenario == 26) {
        configure.clear();
        window->moveResize(manual);
        QVERIFY(!configure.isEmpty() || configure.wait());
        if (scenario == 25 || scenario == 26) {
            xdg->m_configureEvents.last()->serial = 0;
            xdg_surface_send_configure(wireSurface, 0);
            QVERIFY(Test::waylandSync());
            QCOMPARE(configure.last().at(0).value<quint32>(), 0u);
        }
        if (scenario == 8 || scenario == 9) {
            serial = configure.last().at(0).value<quint32>();
        }
    } else if (scenario == 3) {
        window->move(manual.topLeft());
    } else if (scenario == 10) {
        provider.reset();
    } else if (scenario == 11) {
        delete root.data();
        QVERIFY(!root);
    } else if (scenario == 16) {
        window->setNextTargetScale(3.0);
        QVERIFY(!configure.isEmpty() || configure.wait());
        serial = configure.last().at(0).value<quint32>();
        // Force a same-output obligation without scheduling another configure.
        window->m_nextTargetScale = 2.0;
        window->m_outputPropertiesPending = true;
    } else if (scenario == 22) {
        heldOwnership = std::move(window->m_transientOwnership);
    } else if (scenario == 23) {
        // An interactive move (6.8: Gravity::Center, no resize edge).
        window->setInteractiveMoveResizeGravity(Gravity::Center);
        window->setInteractiveMoveResizeAnchor(QPointF(400, 300));
        window->setInteractiveMoveOffset(QPointF(0.5, 0.5));
        window->setUnrestrictedInteractiveMoveResize(true);
        QVERIFY(window->startInteractiveMoveResize());
        QVERIFY(window->isInteractiveMove());
    } else if (scenario == 24) {
        xdg->m_configureEvents.append(new XdgToplevelConfigure(*static_cast<XdgToplevelConfigure *>(xdg->m_configureEvents.last())));
    }
    if (scenario == 2 || scenario == 3 || scenario == 8 || scenario == 9 || scenario == 25 || scenario == 26) {
        QCOMPARE(window->m_geometryRequestOrigin.kind, Window::GeometryRequestOrigin::Kind::Ordinary);
    }
    Window::GeometryRequestOrigin expectedContentOrigin;
    bool foundConfigure = false;
    for (auto event : std::as_const(xdg->m_configureEvents)) {
        if (event->serial != serial) {
            continue;
        }
        if (scenario == 4) {
            QCOMPARE(event->origin.kind, Window::GeometryRequestOrigin::Kind::Restoration);
        } else if (scenario == 17) {
            event->origin = {};
        } else if (scenario == 18) {
            ++event->origin.ownershipGeneration;
        } else if (scenario == 19) {
            ++event->origin.restoreSequence;
        } else if (scenario == 20) {
            ++event->origin.geometryGeneration;
        } else if (scenario == 21) {
            event->origin.kind = Window::GeometryRequestOrigin::Kind::Ordinary;
        }
        expectedContentOrigin = event->origin;
        foundConfigure = true;
    }
    QVERIFY(foundConfigure);

    QSize content = admissionContent ? automatic.size().toSize() : original.size().toSize();
    QPointF expectedOrigin = window->pos();
    RectF expectedRequested = window->moveResizeGeometry();
    if (scenario == 5) {
        content += QSize(20, 10);
    }
    if (scenario == 5 || scenario == 6 || scenario == 7 || scenario == 11) {
        expectedOrigin = original.topLeft();
    } else if (scenario == 8 || scenario == 9) {
        content = manual.size().toSize() + QSize(20, 10);
        expectedOrigin = manual.topLeft();
        expectedRequested = RectF(expectedOrigin, content);
    } else if (scenario == 14 || scenario == 15) {
        expectedOrigin = automatic.topLeft();
    } else if (scenario == 16) {
        content = automatic.size().toSize() + QSize(30, 15);
        expectedRequested = RectF(expectedOrigin, content);
    }

    bool callback = false;
    QObject context;
    if (scenario == 12 || scenario == 13) {
        for (auto event : std::as_const(xdg->m_configureEvents)) {
            if (event->serial == serial) {
                event->scale = 2.0;
            }
        }
        connect(window, &Window::targetScaleChanged, &context, [&]() {
            if (callback) {
                return;
            }
            callback = true;
            if (scenario == 12) {
                window->moveResize(manual);
            } else {
                // Native dispatch reentry, not a second wire transaction.
                Q_EMIT window->surface()->committed();
            }
        });
        if (scenario == 12) {
            expectedRequested = manual;
        }
    } else if (scenario == 16) {
        connect(window, &Window::nextTargetScaleChanged, &context, [&]() {
            if (callback) {
                return;
            }
            callback = true;
            const auto geometryGeneration = window->geometryRequestGeneration();
            claim = window->claimTransientRestore(owner);
            QCOMPARE(claim.continuation.outcome, Outcome::AppliedCurrent);
            QCOMPARE(window->geometryRequestGeneration(), geometryGeneration);
        });
    }

    shell->xdgSurface()->ack_configure(serial);
    Test::render(surface.get(), content, Qt::green);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->frameGeometry().size().toSize(), content);
    QCOMPARE(window->pos(), expectedOrigin);
    QCOMPARE(window->moveResizeGeometry(), expectedRequested);
    QVERIFY(xdg->m_lastContentOrigin == (scenario == 24 ? Window::GeometryRequestOrigin{} : expectedContentOrigin));
    if (scenario == 25 || scenario == 26) {
        QCOMPARE(xdg->m_configureEvents.size(), 1);
        QCOMPARE(xdg->m_configureEvents.first()->serial, 0u);
        QCOMPARE(xdg->m_configureEvents.first()->origin.kind, Window::GeometryRequestOrigin::Kind::Ordinary);
        QVERIFY(xdg->m_configureEvents.first()->origin == window->m_geometryRequestOrigin);
    }
    if (scenario == 12 || scenario == 13 || scenario == 16) {
        QVERIFY(callback);
    }
    if (scenario == 16) {
        QVERIFY(window->m_outputPropertiesPending);
    }
    if (scenario == 6 || scenario == 7 || scenario == 9 || scenario == 24) {
        if (scenario == 7) {
            shell->xdgSurface()->ack_configure(std::numeric_limits<quint32>::max());
        }
        // The client's own resizes, no ACK. Without placement authority (rows 7 and 24: no current
        // content origin) the content lands where the window is. With it (rows 6 and 9) the frame
        // follows 6.8's rule for a client's own resize: the spontaneous gravity on the previous
        // frame (the centre for an ordinary window), confined like any committed frame.
        const bool spontaneous = scenario == 6 || scenario == 9;
        const auto spontaneousOrigin = [&](const RectF &previous) {
            RectF frame = xdg->m_spontaneousGravity.apply(RectF(previous.topLeft(), QSizeF(content)), previous);
            if (!xdg->m_isUnrestricted) {
                if (const auto anchor = window->confineInteractiveMove(frame)) {
                    frame.moveTopLeft(*anchor);
                }
            }
            return frame.topLeft();
        };
        RectF previous;
        content += QSize(30, 15);
        previous = window->frameGeometry();
        Test::render(surface.get(), content, Qt::blue);
        QVERIFY(Test::waylandSync());
        if (spontaneous) {
            expectedOrigin = spontaneousOrigin(previous);
            // Anchored to a literal rule: an ordinary window's spontaneous gravity is the centre, and
            // nothing confines it horizontally here, so it grows by half the width to the left.
            QCOMPARE(expectedOrigin.x(), previous.x() - (content.width() - previous.width()) / 2.0);
        }
        if (scenario == 9) {
            expectedRequested = RectF(expectedOrigin, content);
        }
        QCOMPARE(window->pos(), expectedOrigin);
        QCOMPARE(window->frameGeometry().size().toSize(), content);
        QCOMPARE(window->moveResizeGeometry(), expectedRequested);
        QVERIFY(xdg->m_lastContentOrigin == ((scenario == 7 || scenario == 24) ? Window::GeometryRequestOrigin{} : expectedContentOrigin));
        content += QSize(10, 10);
        previous = window->frameGeometry();
        Test::render(surface.get(), content, Qt::blue);
        QVERIFY(Test::waylandSync());
        if (spontaneous) {
            expectedOrigin = spontaneousOrigin(previous);
            // Anchored to a literal rule: an ordinary window's spontaneous gravity is the centre, and
            // nothing confines it horizontally here, so it grows by half the width to the left.
            QCOMPARE(expectedOrigin.x(), previous.x() - (content.width() - previous.width()) / 2.0);
        }
        if (scenario == 9) {
            expectedRequested = RectF(expectedOrigin, content);
        }
        QCOMPARE(window->pos(), expectedOrigin);
        QCOMPARE(window->frameGeometry().size().toSize(), content);
        QCOMPARE(window->moveResizeGeometry(), expectedRequested);
    }
    if (heldOwnership) {
        window->m_transientOwnership = std::move(heldOwnership);
    }
    if (scenario == 23) {
        window->finishInteractiveMoveResize(false);
    }
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->generation, owner);
    QCOMPARE(window->m_transientOwnership->floating, original);
    QVERIFY(writer.isReserved(id));
    if (scenario != 15) {
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    }
    QPointer<Window> guard(window);
    request.result.checkpoint.reset();
    claim.continuation.checkpoint.reset();
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testConfigureDiscardReentry_data()
{
    QTest::addColumn<bool>("close");
    QTest::addColumn<bool>("tail");
    QTest::addColumn<bool>("exhaust");
    QTest::newRow("obsolete-decoration-close") << true << false << false;
    QTest::newRow("obsolete-decoration-nested-commit") << false << false << false;
    QTest::newRow("nested-commit-exhaustion") << false << false << true;
    QTest::newRow("selected-decoration-tail-close") << true << true << false;
}

void TestXdgShellWindow::testConfigureDiscardReentry()
{
    QFETCH(bool, close);
    QFETCH(bool, tail);
    QFETCH(bool, exhaust);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    configure.clear();
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!window->decoration());
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto xdg = static_cast<XdgToplevelWindow *>(window);
    QVERIFY(xdg->m_configureEvents.isEmpty());
    configure.clear();
    // The nested commit takes the buffer as the client's own resize, which since 6.8 keeps the frame's
    // centre for an ordinary window. Pin the top-left anchor, as 6.7 had, so the position shows that the
    // outer commit's selected configure is never applied.
    xdg->m_spontaneousGravity = Gravity::BottomRight;

    // This decoration is queued but never installed. Its last reference belongs to the
    // first configure, so consuming a later prefix must handle its destruction callback.
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
    QVERIFY(configure.wait());
    const quint32 decorated = configure.last().at(0).value<quint32>();
    QPointer<KDecoration3::Decoration> obsolete = xdg->m_nextDecoration.get();
    QVERIFY(obsolete);
    QVERIFY(!window->decoration());
    configure.clear();
    decoration->set_mode(Test::XdgToplevelDecorationV1::mode_client_side);
    QVERIFY(configure.wait());
    QVERIFY(!xdg->m_nextDecoration);
    QVERIFY(obsolete);
    configure.clear();
    window->moveResize(RectF(80, 90, 300, 200));
    QVERIFY(configure.wait());
    const quint32 selected = tail ? decorated : configure.last().at(0).value<quint32>();
    configure.clear();
    const RectF newer(100, 110, 400, 300);
    window->moveResize(newer);
    QVERIFY(configure.wait());
    const quint32 successor = configure.last().at(0).value<quint32>();
    QCOMPARE(xdg->m_configureEvents.size(), 4);
    QVERIFY(!static_cast<XdgToplevelConfigure *>(xdg->m_configureEvents.last())->decoration);
    const QPointF origin = window->pos();
    QPointer<Window> guard(window);
    auto held = window->makeTileAssignmentCheckpoint();
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    bool destroyed = false;
    bool nested = false;
    QObject context;
    if (tail) {
        connect(window, &Window::frameGeometryChanged, &context, [&]() {
            if (window->decoration() == obsolete) {
                // The selected configure keeps the decoration alive until tail disposal.
                window->setDecoration(nullptr);
            }
        });
    }
    if (exhaust) {
        xdg->m_configureCommitGeneration = std::numeric_limits<quint64>::max() - 2;
    }
    connect(obsolete, &QObject::destroyed, &context, [&]() {
        destroyed = true;
        if (close) {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        } else {
            // Explicit native signal reentry, not a second wire transaction. The outer ACK
            // must already be consumed, and its tail must not clear a later pending ACK.
            Q_EMIT window->surface()->committed();
            nested = true;
            Q_EMIT xdg->XdgSurfaceWindow::m_shellSurface->configureAcknowledged(successor);
        }
    });
    shell->xdgSurface()->ack_configure(selected);
    Test::render(surface.get(), QSize(300, 200), Qt::blue);
    QVERIFY(Test::waylandSync());
    QVERIFY(destroyed);
    QVERIFY(!obsolete);
    if (close) {
        QVERIFY(window->isDeleted());
        if (tail) {
            QVERIFY(frameChanged.count() > 0);
        } else {
            QCOMPARE(frameChanged.count(), 0);
        }
    } else {
        QVERIFY(nested);
        QCOMPARE(window->pos(), origin);
        QCOMPARE(window->frameGeometry().size().toSize(), exhaust ? QSize(200, 100) : QSize(300, 200));
        if (exhaust) {
            QCOMPARE(xdg->m_configureCommitGeneration, std::numeric_limits<quint64>::max());
        }
        QCOMPARE(window->moveResizeGeometry(), newer);
        QCOMPARE(xdg->m_lastAcknowledgedConfigureSerial, std::optional<quint32>(successor));
        QCOMPARE(xdg->m_configureEvents.size(), 1);
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
    }
    held.reset();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testConfigureCommitExhaustion_data()
{
    QTest::addColumn<quint64>("generation");
    QTest::addColumn<bool>("applied");
    QTest::newRow("already-exhausted") << std::numeric_limits<quint64>::max() << false;
    QTest::newRow("would-exhaust") << std::numeric_limits<quint64>::max() - 1 << false;
    QTest::newRow("last-usable") << std::numeric_limits<quint64>::max() - 2 << true;
}

void TestXdgShellWindow::testConfigureCommitExhaustion()
{
    QFETCH(quint64, generation);
    QFETCH(bool, applied);
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
    const RectF before = window->frameGeometry();
    const RectF target(80, 90, 300, 200);
    window->moveResize(target);
    QVERIFY(configure.wait());
    auto xdg = static_cast<XdgToplevelWindow *>(window);
    xdg->m_configureCommitGeneration = generation;
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), target.size().toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(window->frameGeometry(), applied ? target : before);
    QCOMPARE(xdg->m_configureCommitGeneration, std::numeric_limits<quint64>::max() - quint64(applied));
    QCOMPARE(xdg->m_configureEvents.size(), applied ? 0 : 1);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testConfigureSerialMatching_data()
{
    QTest::addColumn<bool>("wrap");
    QTest::addColumn<int>("selected");
    QTest::newRow("ordinary-first") << false << 0;
    QTest::newRow("ordinary-middle") << false << 1;
    QTest::newRow("ordinary-last") << false << 2;
    QTest::newRow("ordinary-unknown") << false << -1;
    QTest::newRow("ordinary-interior-hole") << false << -3;
    QTest::newRow("wrap-first-preserves-successors") << true << 0;
    QTest::newRow("wrap-max-preserves-zero") << true << 1;
    QTest::newRow("wrap-zero-consumes-prefix") << true << 2;
    QTest::newRow("wrap-unknown") << true << -1;
    QTest::newRow("wrap-ambiguous") << true << -2;
}

void TestXdgShellWindow::testConfigureSerialMatching()
{
    QFETCH(bool, wrap);
    QFETCH(int, selected);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    auto xdg = static_cast<XdgToplevelWindow *>(window);
    QVERIFY(xdg->m_configureEvents.isEmpty());
    configure.clear();
    // An unmatched commit is the client's own resize, which since 6.8 keeps the frame's centre for an
    // ordinary window (preferredSpontaneousGravity). Pin the top-left anchor, as 6.7 had, so the position
    // isolates the matcher: only a selected configure moves the frame, to that configure's bounds.
    xdg->m_spontaneousGravity = Gravity::BottomRight;
    QSignalSpy acknowledged(xdg->XdgSurfaceWindow::m_shellSurface, &XdgSurfaceInterface::configureAcknowledged);
    const auto objectId = wl_proxy_get_id(reinterpret_cast<wl_proxy *>(shell->xdgSurface()->object()));
    wl_resource *resource = wl_client_get_object(window->surface()->client()->client(), objectId);
    QVERIFY(resource);
    QCOMPARE(XdgSurfaceInterface::get(resource), xdg->XdgSurfaceWindow::m_shellSurface);
    std::array<quint32, 3> serials;
    std::array<RectF, 3> bounds;
    quint32 hole = 0;
    for (int i = 0; i < 3; ++i) {
        if (selected == -3 && i == 1) {
            hole = waylandServer()->display()->nextSerial();
        }
        const auto count = configure.count();
        bounds[i] = RectF(60 + 20 * i, 70 + 20 * i, 300 + 50 * i, 200 + 50 * i);
        window->moveResize(bounds[i]);
        QCOMPARE(window->moveResizeGeometry(), bounds[i]);
        // The display flushes at aboutToBlock; enter an event loop rather than polling events.
        QVERIFY(configure.count() > count || configure.wait());
        QCOMPARE(configure.count(), count + 1);
        QCOMPARE(xdg->m_configureEvents.size(), i + 1);
        auto event = xdg->m_configureEvents.last();
        serials[i] = event->serial;
        if (wrap) {
            // Reuse production snapshots, but deliver chosen uint32 boundary values over the
            // actual wire. This exercises ACK/commit matching, not libwayland's serial counter.
            serials[i] = std::numeric_limits<quint32>::max() - 1 + quint32(i);
            if (selected == -2 && i == 2) {
                serials[i] = serials[1];
            }
            event->serial = serials[i];
            xdg_surface_send_configure(resource, serials[i]);
        }
        QVERIFY(Test::waylandSync());
        QCOMPARE(configure.last().at(0).value<quint32>(), serials[i]);
    }
    quint32 serial = wrap ? 17 : std::numeric_limits<quint32>::max();
    if (selected >= 0) {
        serial = serials[selected];
    } else if (selected == -2) {
        serial = serials[1];
    } else if (selected == -3) {
        QVERIFY(serials[0] < hole && hole < serials[1]);
        serial = hole;
    }
    const QSize content = selected >= 0 ? bounds[selected].size().toSize() : QSize(240, 160);
    const QPointF origin = window->pos();
    shell->xdgSurface()->ack_configure(serial);
    Test::render(surface.get(), content, Qt::blue);
    QVERIFY(Test::waylandSync());
    QCOMPARE(acknowledged.count(), 1);
    QCOMPARE(acknowledged.last().at(0).value<quint32>(), serial);
    QCOMPARE(xdg->m_configureEvents.size(), selected >= 0 ? 2 - selected : 3);
    for (int i = selected >= 0 ? selected + 1 : 0; i < 3; ++i) {
        QCOMPARE(quint32(xdg->m_configureEvents[i - (selected >= 0 ? selected + 1 : 0)]->serial), serials[i]);
    }
    QCOMPARE(window->frameGeometry().size().toSize(), content);
    QCOMPARE(window->pos(), selected >= 0 ? bounds[selected].topLeft() : origin);
    QCOMPARE(window->moveResizeGeometry(), bounds[2]);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testMaximizeRestoreNormalization_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("committed");
    QTest::addColumn<bool>("decorated");
    QTest::addColumn<int>("scenario");
    QTest::newRow("horizontal-pending") << int(MaximizeHorizontal) << false << false << 0;
    QTest::newRow("horizontal-committed") << int(MaximizeHorizontal) << true << false << 0;
    QTest::newRow("vertical-pending") << int(MaximizeVertical) << false << false << 0;
    QTest::newRow("vertical-committed") << int(MaximizeVertical) << true << false << 0;
    QTest::newRow("full-pending") << int(MaximizeFull) << false << false << 0;
    QTest::newRow("full-committed") << int(MaximizeFull) << true << false << 0;
    QTest::newRow("ssd-horizontal") << int(MaximizeHorizontal) << true << true << 0;
    QTest::newRow("ssd-vertical") << int(MaximizeVertical) << true << true << 0;
    QTest::newRow("ssd-full") << int(MaximizeFull) << true << true << 0;
    QTest::newRow("requested-restore-committed-maximize") << int(MaximizeFull) << true << false << 1;
    QTest::newRow("normal-noop") << int(MaximizeRestore) << false << false << 0;
    QTest::newRow("normal-claim-remains-narrow") << int(MaximizeFull) << false << false << 2;
    QTest::newRow("borderless-refused") << int(MaximizeFull) << false << true << 4;
    QTest::newRow("mode-headroom-refused") << int(MaximizeFull) << false << false << 5;
    QTest::newRow("duplicate-during-normalization") << int(MaximizeFull) << false << false << 6;
    QTest::newRow("cancel-before-mode-write") << int(MaximizeFull) << false << false << 7;
    QTest::newRow("cancel-after-mode-write") << int(MaximizeFull) << true << true << 8;
    QTest::newRow("maximize-revision-stale") << int(MaximizeFull) << false << false << 9;
    QTest::newRow("fullscreen-intent-stale") << int(MaximizeFull) << false << false << 10;
    QTest::newRow("provider-lost-callback") << int(MaximizeFull) << false << false << 11;
    QTest::newRow("close-callback") << int(MaximizeFull) << false << false << 12;
    QTest::newRow("noop-without-write-headroom") << int(MaximizeRestore) << false << false << 13;
    QTest::newRow("scale-mismatch-refused") << int(MaximizeFull) << false << false << 14;
    QTest::newRow("lower-dispatch-during-normalization") << int(MaximizeFull) << false << false << 15;
    QTest::newRow("old-maximize-content") << int(MaximizeFull) << false << false << 16;
    QTest::newRow("lower-dispatch-after-mode-write") << int(MaximizeFull) << true << true << 17;
    QTest::newRow("exhausted-fullscreen-intent") << int(MaximizeRestore) << false << false << 18;
    QTest::newRow("exhausted-maximize-revision") << int(MaximizeRestore) << false << false << 19;
    QTest::newRow("exhausted-fullscreen-revision") << int(MaximizeRestore) << false << false << 20;
    QTest::newRow("normal-claim-exhausted-intent") << int(MaximizeRestore) << false << false << 21;
    QTest::newRow("ssd-bounded-client-maximum") << int(MaximizeFull) << true << true << 22;
    QTest::newRow("ssd-post-transition-impossible") << int(MaximizeFull) << true << true << 23;
    QTest::newRow("output-intent-after-dispatch") << int(MaximizeFull) << false << false << 24;
    QTest::newRow("maximize-roundtrip-after-dispatch") << int(MaximizeFull) << false << false << 25;
    QTest::newRow("fullscreen-roundtrip-after-dispatch") << int(MaximizeFull) << false << false << 26;
    QTest::newRow("scale-change-during-normalization") << int(MaximizeFull) << false << false << 27;
    QTest::newRow("constraints-change-after-mode-write") << int(MaximizeFull) << true << true << 28;
    QTest::newRow("decoration-replaced-after-mode-write") << int(MaximizeFull) << true << true << 29;
    QTest::newRow("decoration-border-snapshot-disagrees") << int(MaximizeFull) << true << true << 30;
    QTest::newRow("pending-decoration-refused") << int(MaximizeFull) << true << true << 31;
    QTest::newRow("fullscreen-pending") << int(MaximizeRestore) << false << false << 32;
    QTest::newRow("fullscreen-committed") << int(MaximizeRestore) << true << false << 33;
    QTest::newRow("fullscreen-ssd-returning-decoration") << int(MaximizeRestore) << true << true << 34;
    QTest::newRow("fullscreen-constraint-refused") << int(MaximizeRestore) << true << false << 35;
    QTest::newRow("fullscreen-and-maximize-pending") << int(MaximizeFull) << false << false << 36;
    QTest::newRow("fullscreen-old-content") << int(MaximizeRestore) << false << false << 37;
    QTest::newRow("fullscreen-named-output-cleared") << int(MaximizeRestore) << true << false << 38;
    QTest::newRow("fullscreen-superseded-after-exit-write") << int(MaximizeFull) << false << false << 39;
    QTest::newRow("fullscreen-superseded-before-normalize") << int(MaximizeRestore) << false << false << 40;
}

void TestXdgShellWindow::testMaximizeRestoreNormalization()
{
    QFETCH(int, mode);
    QFETCH(bool, committed);
    QFETCH(bool, decorated);
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    const bool previousBorderless = options->borderlessMaximizedWindows();
    options->setBorderlessMaximizedWindows(scenario == 4);
    const auto resetOption = qScopeGuard([previousBorderless]() {
        options->setBorderlessMaximizedWindows(previousBorderless);
    });
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> decoration(Test::createXdgToplevelDecorationV1(shell.get()));
    shell->set_app_id(QStringLiteral("org.example.normalize-restore"));
    decoration->set_mode(decorated ? Test::XdgToplevelDecorationV1::mode_server_side : Test::XdgToplevelDecorationV1::mode_client_side);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    configure.clear();
    const QSize clientSize = decorated ? QSize(320, 240) : QSize(200, 100);
    Window *window = Test::renderAndWaitForShown(surface.get(), clientSize, Qt::red);
    QVERIFY(window);
    QCOMPARE(window->isDecorated(), decorated);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), clientSize, Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const RectF original = window->frameGeometry();
    const auto originalBorders = window->decoration() ? window->decoration()->currentState()->borders() : QMarginsF();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.normalize-restore");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, QRectF(original), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    TransientTileController controller;
    // Keep the old full-size content's center on this output while separating its stale
    // maximize origin from the current committed origin by more than just the tile padding.
    const RectF region = scenario == 16 ? RectF(0.25, 0, 0.5, 1) : RectF(0, 0, 0.5, 1);
    QCOMPARE(controller.createTree(manager, desktop, region), TransientTileController::Result::Accepted);
    const auto automatic = controller.tile()->windowGeometry();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), window->nextFrameSizeToClientSize(automatic.size()).toSize(), Qt::blue);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const auto owner = window->ownedTransientPlacementGeneration();
    quint32 oldMaximizeSerial = 0;
    QSize oldMaximizeContent;
    quint32 oldFullScreenSerial = 0;
    QSize oldFullScreenContent;
    if (mode != int(MaximizeRestore)) {
        window->maximize(MaximizeMode(mode));
        QCOMPARE(window->ownedTransientPlacementGeneration(), owner);
        QVERIFY(!configure.isEmpty() || configure.wait());
        oldMaximizeSerial = configure.last().at(0).value<quint32>();
        oldMaximizeContent = toplevelConfigure.last().at(0).value<QSize>();
        if (committed) {
            shell->xdgSurface()->ack_configure(oldMaximizeSerial);
            Test::render(surface.get(), oldMaximizeContent, Qt::blue);
            QVERIFY(Test::waylandSync());
            QCOMPARE(window->maximizeMode(), MaximizeMode(mode));
        }
        configure.clear();
    }
    const bool fullScreenRow = scenario >= 32 && scenario <= 38;
    if (fullScreenRow) {
        if (scenario == 38) {
            // The client names the output the ownership lives on: the admitted combination, which
            // records the requested output the exit under the explicit attempt must clear.
            const auto outputs = workspace()->outputs();
            shell->set_fullscreen(*Test::waylandOutput(outputs[0]->name()));
        } else {
            window->setFullScreen(true);
        }
        QCOMPARE(window->ownedTransientPlacementGeneration(), owner);
        QVERIFY(!configure.isEmpty() || configure.wait());
        oldFullScreenSerial = configure.last().at(0).value<quint32>();
        oldFullScreenContent = toplevelConfigure.last().at(0).value<QSize>();
        QVERIFY(window->isRequestedFullScreen());
        if (committed) {
            shell->xdgSurface()->ack_configure(oldFullScreenSerial);
            Test::render(surface.get(), oldFullScreenContent, Qt::blue);
            QVERIFY(Test::waylandSync());
            QVERIFY(window->isFullScreen());
        }
        configure.clear();
        if (scenario == 35) {
            // A's client extent cannot satisfy the constraints the restore must respect: the
            // claim refuses before any mutation, geometry or mode write.
            static_cast<XdgToplevelWindow *>(window)->m_minimumSize = QSizeF(400, 400);
        }
    }
    if (scenario == 1) {
        window->maximize(MaximizeRestore);
        QVERIFY(!configure.isEmpty() || configure.wait());
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        configure.clear();
    } else if (scenario == 14) {
        window->setNextTargetScale(3.0);
    } else if (scenario == 31) {
        window->invalidateDecoration();
    }
    auto xdg = static_cast<XdgToplevelWindow *>(window);
    if (scenario == 5 || scenario == 13) {
        xdg->m_requestedMaximizeRevision = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 18 || scenario == 21) {
        xdg->m_fullScreenIntentRevision = std::numeric_limits<quint64>::max();
    } else if (scenario == 19) {
        xdg->m_requestedMaximizeRevision = std::numeric_limits<quint64>::max();
    } else if (scenario == 20) {
        xdg->m_requestedFullScreenRevision = std::numeric_limits<quint64>::max();
    } else if (scenario == 22) {
        QVERIFY(xdg->m_nextDecoration);
        QVERIFY(original.width() > clientSize.width() || original.height() > clientSize.height());
        xdg->m_maximumSize = clientSize;
        auto state = xdg->m_nextDecoration->nextState();
        state->setBorders(QMarginsF());
        Q_EMIT xdg->m_nextDecoration->nextStateChanged(state);
        QCOMPARE(xdg->m_nextDecorationState->borders(), QMarginsF());
    }
    const RectF mutableRestore(17, 29, 410, 270);
    const RectF mutableFullscreen(31, 43, 430, 290);
    window->setGeometryRestore(mutableRestore);
    window->setFullscreenGeometryRestore(mutableFullscreen);
    const auto generationBefore = window->geometryRequestGeneration();
    const auto modeBefore = window->requestedMaximizeMode();
    auto held = window->makeTileAssignmentCheckpoint();
    QPointer<Window> guard(window);
    auto claim = (scenario == 2 || scenario == 21) ? window->claimTransientRestore(owner) : window->claimTransientRestoreWithMaximizeNormalization(owner);
    const bool refused = scenario == 2 || scenario == 4 || scenario == 5 || scenario == 14 || (scenario >= 18 && scenario <= 21) || scenario == 31 || scenario == 35;
    if (scenario == 18 || scenario == 21) {
        // Exercise the real output-intent writer even though requested fullscreen is already
        // false. At saturation, the write cannot advance the revision and must not be trusted.
        shell->unset_fullscreen();
        QVERIFY(Test::waylandSync());
        QCOMPARE(xdg->m_fullScreenIntentRevision, std::numeric_limits<quint64>::max());
        QVERIFY(!xdg->m_fullScreenRequestedOutput);
    }
    if (refused) {
        QCOMPARE(claim.continuation.outcome, Outcome::Refused);
        QVERIFY(!window->m_transientOwnership->restoreAttempt);
        QCOMPARE(window->ownedTransientPlacementGeneration(), owner);
        QCOMPARE(window->geometryRequestGeneration(), generationBefore);
    } else {
        QCOMPARE(claim.continuation.outcome, Outcome::AppliedCurrent);
        const auto sequence = claim.sequence;
        QCOMPARE(window->m_transientOwnership->restoreAttempt->stage, Window::TransientRestoreStage::Claimed);
        if (scenario == 9) {
            xdg->m_requestedMaximizeRevision += 2;
        } else if (scenario == 10) {
            xdg->m_fullScreenIntentRevision += 2;
        } else if (scenario == 40) {
            // The client holds the newer intent before normalization starts; the real writer's
            // revisions supersede the claimed attempt's captured state.
            shell->unset_fullscreen();
            QVERIFY(Test::waylandSync());
        }
        bool callback = false;
        QObject context;
        QSignalSpy about(window, &Window::maximizedAboutToChange);
        connect(window, &Window::maximizedAboutToChange, &context, [&]() {
            if (callback) {
                return;
            }
            callback = true;
            QCOMPARE(window->m_transientOwnership->restoreAttempt->stage, Window::TransientRestoreStage::Normalizing);
            QCOMPARE(window->m_transientOwnership->restoreAttempt->requestGeneration, 0u);
            if (scenario == 36) {
                // The fullscreen exit runs before the maximize write: its requested state is
                // already clear when the maximize boundary fires.
                QVERIFY(!window->isRequestedFullScreen());
            }
            if (scenario == 6) {
                Window::TransientRestoreClaim duplicate;
                duplicate.windowId = id;
                duplicate.ownershipGeneration = owner;
                duplicate.sequence = sequence;
                duplicate.continuation.outcome = Outcome::AppliedCurrent;
                duplicate.continuation.checkpoint = window->makeTileAssignmentCheckpoint();
                const auto nested = window->requestTransientRestore(std::move(duplicate));
                QCOMPARE(nested.result.outcome, Outcome::Refused);
                QCOMPARE(window->geometryRequestGeneration(), generationBefore);
            } else if (scenario == 7 || scenario == 39) {
                // Scenario 39 fires here after the fullscreen exit's writes and publication: the
                // cancellation supersedes the attempt mid-normalization, before the maximize
                // write, keeping the already-published fullscreen exit.
                window->cancelTransientRequest();
            } else if (scenario == 11) {
                provider.reset();
            } else if (scenario == 12) {
                window->setSkipCloseAnimation(true);
                window->destroyWindow();
            } else if (scenario == 15) {
                const auto before = window->makeTileAssignmentCheckpoint();
                const auto nested = window->requestMoveResizeGuarded(original, Window::MoveResizeMode::MoveResize, *before,
                                                                     Window::OwnedProvenance::forRestoration(owner, sequence));
                QCOMPARE(nested.result.outcome, Outcome::Refused);
                QCOMPARE(nested.requestGeneration, 0u);
                QCOMPARE(window->geometryRequestGeneration(), generationBefore);
            } else if (scenario == 27) {
                window->setNextTargetScale(2.0);
            }
        });
        bool postWrite = false;
        bool borderResponse = false;
        if (scenario == 22 || scenario == 23 || scenario == 30) {
            connect(xdg->m_nextDecoration->window(), &KDecoration3::DecoratedWindow::maximizedChanged, &context, [&](bool maximized) {
                if (maximized) {
                    return;
                }
                borderResponse = true;
                if (scenario == 30) {
                    xdg->m_nextDecorationState = xdg->m_nextDecorationState->clone();
                    xdg->m_nextDecorationState->setBorders(QMarginsF(400, 400, 400, 400));
                } else {
                    auto state = xdg->m_nextDecoration->nextState();
                    state->setBorders(scenario == 22 ? originalBorders : QMarginsF(400, 400, 400, 400));
                    Q_EMIT xdg->m_nextDecoration->nextStateChanged(state);
                }
            });
        }
        if (scenario == 8 || scenario == 28 || scenario == 29) {
            QVERIFY(xdg->m_nextDecoration);
            connect(xdg->m_nextDecoration->window(), &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, &context, [&]() {
                postWrite = true;
                QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
                QCOMPARE(window->m_transientOwnership->restoreAttempt->stage, Window::TransientRestoreStage::Normalizing);
                if (scenario == 8) {
                    window->cancelTransientRequest();
                } else if (scenario == 28) {
                    xdg->m_minimumSize = QSizeF(2000, 2000);
                } else {
                    window->invalidateDecoration();
                }
            });
        }
        if (scenario == 17) {
            connect(xdg->m_nextDecoration->window(), &KDecoration3::DecoratedWindow::maximizedChanged, &context, [&](bool maximized) {
                if (maximized) {
                    return;
                }
                postWrite = true;
                QCOMPARE(window->m_transientOwnership->restoreAttempt->stage, Window::TransientRestoreStage::Normalizing);
                QVERIFY(window->transientRestoreOperationIsCurrent(owner, sequence));
                QVERIFY(window->canStartTransientRestoreRequest(original));
                const auto before = window->makeTileAssignmentCheckpoint();
                const auto nested = window->requestMoveResizeGuarded(original, Window::MoveResizeMode::MoveResize, *before,
                                                                     Window::OwnedProvenance::forRestoration(owner, sequence));
                QCOMPARE(nested.result.outcome, Outcome::Refused);
                QCOMPARE(nested.requestGeneration, 0u);
                QCOMPARE(window->geometryRequestGeneration(), generationBefore);
            });
        }
        auto result = window->requestTransientRestore(std::move(claim));
        const bool superseded = scenario == 7 || scenario == 8 || scenario == 9 || scenario == 10 || scenario == 11 || scenario == 12
            || scenario == 23 || scenario == 27 || scenario == 28 || scenario == 29 || scenario == 30 || scenario == 39 || scenario == 40;
        QCOMPARE(result.result.outcome, superseded ? Outcome::Superseded : Outcome::AppliedCurrent);
        QCOMPARE(window->m_transientOwnership->restoreAttempt->stage, Window::TransientRestoreStage::Returned);
        QCOMPARE(window->m_transientOwnership->floating, original);
        QCOMPARE(window->geometryRestore(), mutableRestore);
        QCOMPARE(window->fullscreenGeometryRestore(), mutableFullscreen);
        if (superseded) {
            QCOMPARE(result.requestGeneration, 0u);
            QCOMPARE(window->geometryRequestGeneration(), generationBefore);
            QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            if (scenario == 39) {
                // The fullscreen exit's write stands published; the maximize write never happened.
                QVERIFY(!window->isRequestedFullScreen());
                QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
                QVERIFY(!configure.isEmpty() || configure.wait());
                QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
            }
        } else {
            QCOMPARE(result.requestGeneration, generationBefore + 1);
            QCOMPARE(window->moveResizeGeometry(), original);
            QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
            if (fullScreenRow) {
                QVERIFY(!window->isRequestedFullScreen());
                if (scenario == 38) {
                    // The named output does not survive the exit; the owned and legacy exits
                    // clear it too.
                    QVERIFY(!xdg->m_fullScreenRequestedOutput);
                }
            }
            QCOMPARE(about.count(), modeBefore == MaximizeRestore ? 0 : 1);
            QVERIFY(!configure.isEmpty() || configure.wait());
            const auto serial = configure.last().at(0).value<quint32>();
            if (scenario == 16) {
                const auto committedOrigin = window->pos();
                bool foundOldConfigure = false;
                for (auto event : std::as_const(xdg->m_configureEvents)) {
                    if (event->serial == oldMaximizeSerial) {
                        QVERIFY(event->bounds.topLeft() != committedOrigin);
                        foundOldConfigure = true;
                    }
                }
                QVERIFY(foundOldConfigure);
                shell->xdgSurface()->ack_configure(oldMaximizeSerial);
                Test::render(surface.get(), oldMaximizeContent, Qt::blue);
                QVERIFY(Test::waylandSync());
                QCOMPARE(window->frameGeometry().size().toSize(), oldMaximizeContent);
                QCOMPARE(window->pos(), committedOrigin);
                QCOMPARE(window->maximizeMode(), MaximizeFull);
                QCOMPARE(window->output(), manager->output());
                QCOMPARE(window->moveResizeGeometry(), original);
                QVERIFY(window->transientRestoreRequestIsCurrent(owner, sequence));
            } else if (scenario == 37) {
                // The old fullscreen configure may deliver its content and committed state, but
                // it must not regain placement authority over the pending restore.
                const auto committedOrigin = window->pos();
                bool foundOldConfigure = false;
                for (auto event : std::as_const(xdg->m_configureEvents)) {
                    if (event->serial == oldFullScreenSerial) {
                        QVERIFY(event->bounds.topLeft() != committedOrigin);
                        foundOldConfigure = true;
                    }
                }
                QVERIFY(foundOldConfigure);
                shell->xdgSurface()->ack_configure(oldFullScreenSerial);
                Test::render(surface.get(), oldFullScreenContent, Qt::blue);
                QVERIFY(Test::waylandSync());
                QCOMPARE(window->frameGeometry().size().toSize(), oldFullScreenContent);
                QCOMPARE(window->pos(), committedOrigin);
                QCOMPARE(window->isFullScreen(), true);
                QCOMPARE(window->output(), manager->output());
                QCOMPARE(window->moveResizeGeometry(), original);
                QVERIFY(window->transientRestoreRequestIsCurrent(owner, sequence));
            }
            const bool supersededAfterDispatch = scenario == 24 || scenario == 25 || scenario == 26;
            if (scenario == 24) {
                shell->unset_fullscreen();
                QVERIFY(Test::waylandSync());
                QCOMPARE(window->geometryRequestGeneration(), generationBefore + 1);
            } else if (scenario == 25) {
                window->maximize(MaximizeFull);
                window->maximize(MaximizeRestore);
            } else if (scenario == 26) {
                window->setFullScreen(true);
                window->setFullScreen(false);
            }
            const auto newerRequested = window->moveResizeGeometry();
            const auto newerPosition = window->pos();
            if (supersededAfterDispatch) {
                QVERIFY(!window->transientRestoreRequestIsCurrent(owner, sequence));
            }
            shell->xdgSurface()->ack_configure(serial);
            Test::render(surface.get(), clientSize, Qt::green);
            QVERIFY(Test::waylandSync());
            if (supersededAfterDispatch) {
                QCOMPARE(window->moveResizeGeometry(), newerRequested);
                QCOMPARE(window->pos(), newerPosition);
                QVERIFY(!window->transientRestoreRequestIsCurrent(owner, sequence));
            } else {
                QCOMPARE(window->frameGeometry(), original);
                QCOMPARE(window->maximizeMode(), MaximizeRestore);
                if (fullScreenRow) {
                    QVERIFY(!window->isFullScreen());
                    QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Fullscreen));
                    if (scenario == 34) {
                        // The returning server decoration's borders are part of the exact frame:
                        // A is restored as the frame, not recaptured from the managed extent.
                        QVERIFY(window->isDecorated());
                        QCOMPARE(window->decoration()->currentState()->borders(), originalBorders);
                    }
                }
            }
        }
        if (scenario == 6 || scenario == 7 || scenario == 11 || scenario == 12 || scenario == 15 || scenario == 27 || scenario == 39) {
            QVERIFY(callback);
        }
        if (scenario == 8 || scenario == 28 || scenario == 29 || scenario == 30) {
            QVERIFY(scenario == 30 ? borderResponse : postWrite);
            QVERIFY(!configure.isEmpty() || configure.wait());
            QVERIFY(!(toplevelConfigure.last().at(1).value<Test::XdgToplevel::States>() & Test::XdgToplevel::State::Maximized));
        }
        if (scenario == 17) {
            QVERIFY(postWrite);
        }
        if (scenario == 22 || scenario == 23 || scenario == 30) {
            QVERIFY(borderResponse);
        }
        QVERIFY(writer.isReserved(id));
        result.result.checkpoint.reset();
    }
    claim.continuation.checkpoint.reset();
    held.reset();
    if (guard && !guard->isDeleted()) {
        guard->setSkipCloseAnimation(true);
        guard->destroyWindow();
    }
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testMaximizeRestoreRecursion()
{
    struct Fixture
    {
        std::unique_ptr<KWayland::Client::Surface> surface;
        std::unique_ptr<Test::XdgToplevel> shell;
        std::unique_ptr<Test::XdgToplevelDecorationV1> decoration;
        std::unique_ptr<QObject> provider;
        std::unique_ptr<TransientPlacementWriter> writer;
        std::unique_ptr<TransientTileController> controller;
        Window *window = nullptr;
        Window::TransientRestoreClaim claim;
        quint64 geometryBefore = 0;
    };
    using Outcome = Window::AssignmentStepOutcome;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::array<Fixture, 2> fixtures;
    for (int i = 0; i < 2; ++i) {
        auto &fixture = fixtures[i];
        fixture.surface = Test::createSurface();
        fixture.shell = Test::createXdgToplevelSurface(fixture.surface.get(), Test::CreationSetup::CreateOnly);
        fixture.decoration = Test::createXdgToplevelDecorationV1(fixture.shell.get());
        const auto key = QStringLiteral("org.example.restore-recursion-%1").arg(i);
        fixture.shell->set_app_id(key);
        fixture.decoration->set_mode(Test::XdgToplevelDecorationV1::mode_server_side);
        QSignalSpy configure(fixture.shell->xdgSurface(), &Test::XdgSurface::configureRequested);
        fixture.surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(configure.wait());
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        configure.clear();
        fixture.window = Test::renderAndWaitForShown(fixture.surface.get(), QSize(320, 240), Qt::red);
        QVERIFY(fixture.window);
        QVERIFY(!configure.isEmpty() || configure.wait());
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(fixture.surface.get(), QSize(320, 240), Qt::red);
        QVERIFY(Test::waylandSync());
        configure.clear();
        const auto outputs = workspace()->outputs();
        QVERIFY(outputs.size() >= 2);
        fixture.window->move(outputs[i]->geometryF().topLeft() + QPointF(50, 50));
        QVERIFY(Test::waylandSync());
        fixture.provider = std::make_unique<QObject>();
        fixture.writer = std::make_unique<TransientPlacementWriter>(directory.filePath(QStringLiteral("placement-%1rc").arg(i)));
        const auto token = fixture.writer->registerProvider(fixture.provider.get());
        const auto id = fixture.window->internalId();
        QVERIFY(fixture.writer->trackWindow(id, key));
        QCOMPARE(fixture.writer->record(token, 1, {id, QRectF(fixture.window->frameGeometry()), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
        auto manager = workspace()->tileManager(fixture.window->moveResizeOutput());
        auto desktop = VirtualDesktopManager::self()->currentDesktop(fixture.window->moveResizeOutput());
        fixture.controller = std::make_unique<TransientTileController>();
        QCOMPARE(fixture.controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        const auto state = fixture.writer->snapshot();
        QCOMPARE(fixture.controller->admit(fixture.controller->generation(), fixture.window, fixture.writer.get(), token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        QVERIFY(!configure.isEmpty() || configure.wait());
        fixture.shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(fixture.surface.get(), fixture.window->nextFrameSizeToClientSize(fixture.controller->tile()->windowGeometry().size()).toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        configure.clear();
        fixture.window->maximize(MaximizeFull);
        QVERIFY(!configure.isEmpty() || configure.wait());
        fixture.geometryBefore = fixture.window->geometryRequestGeneration();
        fixture.claim = fixture.window->claimTransientRestoreWithMaximizeNormalization(fixture.window->ownedTransientPlacementGeneration());
        QCOMPARE(fixture.claim.continuation.outcome, Outcome::AppliedCurrent);
    }
    bool entered = false;
    Window::AssignmentGeometryRequest nested;
    QObject context;
    auto first = static_cast<XdgToplevelWindow *>(fixtures[0].window);
    connect(first->m_nextDecoration->window(), &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, &context, [&]() {
        if (entered) {
            return;
        }
        entered = true;
        nested = fixtures[1].window->requestTransientRestore(std::move(fixtures[1].claim));
        // A rejected nested consumer must not clear the first normalizer's recursion guard.
        fixtures[0].window->maximize(MaximizeFull);
        QCOMPARE(fixtures[0].window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(nested.result.outcome, Outcome::Superseded);
        QCOMPARE(fixtures[1].window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(fixtures[1].window->geometryRequestGeneration(), fixtures[1].geometryBefore);
    });
    auto result = first->requestTransientRestore(std::move(fixtures[0].claim));
    QVERIFY(entered);
    QCOMPARE(result.result.outcome, Outcome::AppliedCurrent);
    QCOMPARE(first->geometryRequestGeneration(), fixtures[0].geometryBefore + 1);
    QVERIFY(!first->m_transientOwnership->restoreAttempt->cancelled);
    QVERIFY(fixtures[1].window->m_transientOwnership->restoreAttempt->cancelled);
    result.result.checkpoint.reset();
    nested.result.checkpoint.reset();
    for (auto &fixture : fixtures) {
        fixture.claim.continuation.checkpoint.reset();
        QPointer<Window> guard(fixture.window);
        fixture.window->setSkipCloseAnimation(true);
        fixture.window->destroyWindow();
        QTRY_VERIFY(!guard);
    }
}

void TestXdgShellWindow::testTransientRestoreRequest_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("pending-published") << 0;
    QTest::newRow("committed-staged") << 1;
    QTest::newRow("stale-checkpoint") << 2;
    QTest::newRow("foreign-window") << 3;
    QTest::newRow("foreign-owner") << 4;
    QTest::newRow("foreign-sequence") << 5;
    QTest::newRow("duplicate-after") << 6;
    QTest::newRow("duplicate-in-callback") << 7;
    QTest::newRow("cancel-first-callback") << 8;
    QTest::newRow("cancel-final-callback") << 9;
    QTest::newRow("manual-move-callback") << 10;
    QTest::newRow("close-callback") << 11;
    QTest::newRow("provider-first-callback") << 12;
    QTest::newRow("output-callback") << 13;
    QTest::newRow("root-removed-before-request") << 14;
    QTest::newRow("request-headroom-refused") << 15;
    QTest::newRow("publication-headroom-refused") << 16;
    QTest::newRow("placement-headroom-refused") << 17;
    QTest::newRow("pending-gravity-refused") << 18;
    QTest::newRow("gravity-in-callback") << 19;
    QTest::newRow("lower-no-wrapper") << 20;
    QTest::newRow("lower-wrong-target") << 21;
    QTest::newRow("lower-wrong-mode") << 22;
    QTest::newRow("lower-reuse") << 23;
    QTest::newRow("provider-final-callback") << 24;
    QTest::newRow("old-content-stale-claim") << 26;
    QTest::newRow("lower-consumed-no-wrapper") << 27;
    QTest::newRow("lower-reuse-in-callback") << 28;
    QTest::newRow("generation-before-sync-callback") << 29;
    QTest::newRow("cancel-sync-callback") << 30;
}

void TestXdgShellWindow::testTransientRestoreRequest()
{
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    using Dispatch = Window::AssignmentRequestDispatch;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.restore-request"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    const RectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto writer = std::make_unique<TransientPlacementWriter>(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto providerToken = writer->registerProvider(provider.get());
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.restore-request");
    QVERIFY(writer->trackWindow(id, key));
    QCOMPARE(writer->record(providerToken, 1, {id, QRectF(original), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    auto controller = std::make_unique<TransientTileController>();
    QCOMPARE(controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    QPointer<Tile> root(controller->rootTile());
    QPointer<Tile> leaf(controller->tile());
    const RectF automatic = leaf->windowGeometry();
    QVERIFY(automatic != original);
    const auto state = writer->snapshot();
    QCOMPARE(controller->admit(controller->generation(), window, writer.get(), providerToken, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    const auto owner = window->ownedTransientPlacementGeneration();
    const bool staged = scenario == 1 || scenario == 9 || scenario == 24;
    // Keep a real older admission configure outstanding in the pending cases, rather than
    // observing a later coalesced configure that already represents the release.
    QVERIFY(!configure.isEmpty() || configure.wait());
    if (staged) {
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), automatic.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf.data());
        configure.clear();
    }
    window->setGeometryRestore(RectF(11, 22, 333, 222));
    window->setFullscreenGeometryRestore(RectF(33, 44, 444, 333));
    if (scenario == 15) {
        window->m_geometryRequestGeneration = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 16) {
        window->m_geometryUpdateGeneration = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 17) {
        window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 18) {
        static_cast<XdgSurfaceWindow *>(window)->m_configureGravity = Gravity::TopLeft;
    } else if (scenario == 29 || scenario == 30) {
        // Force same-output synchronization before backend dispatch, while A remains feasible.
        window->setNextTargetScale(3.0);
        QCOMPARE(window->nextTargetScale(), 3.0);
    }
    auto claim = window->claimTransientRestore(owner);
    QCOMPARE(claim.continuation.outcome, Outcome::AppliedCurrent);
    const auto sequence = claim.sequence;
    const auto generationBefore = window->geometryRequestGeneration();
    const auto makeClaim = [&]() {
        Window::TransientRestoreClaim copy;
        copy.windowId = id;
        copy.ownershipGeneration = owner;
        copy.sequence = sequence;
        copy.continuation.outcome = Outcome::AppliedCurrent;
        copy.continuation.checkpoint = window->makeTileAssignmentCheckpoint();
        return copy;
    };
    if (scenario >= 3 && scenario <= 5) {
        auto foreign = makeClaim();
        if (scenario == 3) {
            foreign.windowId = QUuid::createUuid();
        } else if (scenario == 4) {
            ++foreign.ownershipGeneration;
        } else {
            ++foreign.sequence;
        }
        const auto rejected = window->requestTransientRestore(std::move(foreign));
        QCOMPARE(rejected.result.outcome, Outcome::Superseded);
        QCOMPARE(rejected.requestGeneration, 0u);
        QVERIFY(!window->m_transientOwnership->restoreAttempt->requestConsumed());
        QVERIFY(window->transientRestoreClaimIsCurrent(claim));
    }
    if (scenario == 2) {
        window->setGeometryRestore(RectF(22, 33, 344, 233));
    } else if (scenario == 14) {
        delete root.data();
        QVERIFY(!root && !leaf);
    } else if (scenario == 26) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.first().at(0).value<quint32>());
        Test::render(surface.get(), automatic.size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QVERIFY(window->transientRestoreAttemptIsCurrent(owner, sequence));
        QVERIFY(!window->transientRestoreClaimIsCurrent(claim));
    }
    QPointer<Window> guard(window);
    std::unique_ptr<TileAssignmentFence> held;
    if (scenario == 11) {
        held = window->makeTileAssignmentCheckpoint();
    }
    bool firstCallback = false;
    bool syncCallback = false;
    bool finalCallback = false;
    std::optional<Window::AssignmentGeometryRequest> nested;
    QObject callbacks;
    connect(window, &Window::frameGeometryAboutToChange, &callbacks, [&]() {
        if (firstCallback) {
            return;
        }
        firstCallback = true;
        QVERIFY(window->m_transientOwnership);
        const auto &attempt = *window->m_transientOwnership->restoreAttempt;
        QVERIFY(attempt.requestConsumed() && attempt.requesting());
        QVERIFY(attempt.requestGeneration != 0);
        QCOMPARE(attempt.requestGeneration, window->geometryRequestGeneration());
        QCOMPARE(window->m_geometryRequestOrigin.kind, Window::GeometryRequestOrigin::Kind::Restoration);
        QCOMPARE(window->m_geometryRequestOrigin.geometryGeneration, attempt.requestGeneration);
        QCOMPARE(window->m_geometryRequestOrigin.ownershipGeneration, owner);
        QCOMPARE(window->m_geometryRequestOrigin.restoreSequence, sequence);
        if (scenario == 7) {
            nested = window->requestTransientRestore(makeClaim());
            QCOMPARE(nested->result.outcome, Outcome::Refused);
            QCOMPARE(window->geometryRequestGeneration(), attempt.requestGeneration);
        } else if (scenario == 28) {
            auto before = window->makeTileAssignmentCheckpoint();
            const auto generation = attempt.requestGeneration;
            nested = window->requestMoveResizeGuarded(original, Window::MoveResizeMode::MoveResize,
                                                      *before, Window::OwnedProvenance::forRestoration(owner, sequence));
            QCOMPARE(nested->result.outcome, Outcome::Refused);
            QCOMPARE(nested->requestGeneration, 0u);
            QCOMPARE(window->geometryRequestGeneration(), generation);
            QVERIFY(!window->m_transientOwnership->restoreAttempt->cancelled);
        } else if (scenario == 8) {
            window->cancelTransientRequest();
        } else if (scenario == 10) {
            window->move(QPointF(70, 80));
        } else if (scenario == 11) {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        } else if (scenario == 12) {
            provider.reset();
        } else if (scenario == 13) {
            const auto outputs = workspace()->outputs();
            window->setMoveResizeOutput(outputs.first() == manager->output() ? outputs.last() : outputs.first());
        } else if (scenario == 19) {
            static_cast<XdgSurfaceWindow *>(window)->m_configureGravity = Gravity::TopLeft;
        }
    });
    if (scenario == 29 || scenario == 30) {
        connect(window, &Window::nextTargetScaleChanged, &callbacks, [&]() {
            QVERIFY(!syncCallback);
            syncCallback = true;
            QVERIFY(!firstCallback);
            QVERIFY(window->m_transientOwnership);
            const auto &attempt = *window->m_transientOwnership->restoreAttempt;
            QVERIFY(attempt.requestConsumed() && attempt.requesting());
            QVERIFY(attempt.requestGeneration != 0);
            QCOMPARE(attempt.requestGeneration, window->geometryRequestGeneration());
            QCOMPARE(window->nextTargetScale(), manager->output()->scale());
            if (scenario == 30) {
                window->cancelTransientRequest();
            }
        });
    }
    if (scenario == 9 || scenario == 24) {
        connect(window, &Window::requestedGeometryChanged, &callbacks, [&]() {
            if (finalCallback) {
                return;
            }
            finalCallback = true;
            QCOMPARE(window->m_transientOwnership->restoreAttempt->requestGeneration, window->geometryRequestGeneration());
            if (scenario == 9) {
                window->cancelTransientRequest();
            } else {
                provider.reset();
            }
        });
    }
    Window::AssignmentGeometryRequest result;
    if ((scenario >= 20 && scenario <= 22) || scenario == 27) {
        auto &attempt = *window->m_transientOwnership->restoreAttempt;
        if (scenario != 20) {
            attempt.stage = scenario == 27 ? Window::TransientRestoreStage::Returned : Window::TransientRestoreStage::Dispatching;
        }
        const auto target = scenario == 21 ? RectF(70, 80, 300, 200) : original;
        const auto mode = scenario == 22 ? Window::MoveResizeMode::Move : Window::MoveResizeMode::MoveResize;
        result = window->requestMoveResizeGuarded(target, mode, *claim.continuation.checkpoint, Window::OwnedProvenance::forRestoration(owner, sequence));
        QCOMPARE(result.result.outcome, scenario == 20 || scenario == 27 ? Outcome::Superseded : Outcome::Refused);
        QCOMPARE(result.requestGeneration, 0u);
        QCOMPARE(window->geometryRequestGeneration(), generationBefore);
        QCOMPARE(attempt.requestGeneration, 0u);
        QVERIFY(!firstCallback);
        if (scenario != 20) {
            attempt.stage = Window::TransientRestoreStage::Returned;
        }
    } else {
        result = window->requestTransientRestore(std::move(claim));
        const bool preflightFailure = scenario == 2 || (scenario >= 15 && scenario <= 18) || scenario == 26;
        const bool callbackFailure = (scenario >= 8 && scenario <= 13) || scenario == 19 || scenario == 24 || scenario == 30;
        if (preflightFailure || callbackFailure) {
            QCOMPARE(result.result.outcome, Outcome::Superseded);
            QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            QCOMPARE(firstCallback, !preflightFailure && scenario != 30);
            if (preflightFailure) {
                QCOMPARE(result.requestGeneration, 0u);
                QCOMPARE(window->geometryRequestGeneration(), generationBefore);
            } else {
                QVERIFY(result.requestGeneration != 0);
            }
        } else {
            QCOMPARE(result.result.outcome, Outcome::AppliedCurrent);
            QCOMPARE(result.dispatch, staged ? Dispatch::Queued : Dispatch::Published);
            QVERIFY(firstCallback);
            QVERIFY(window->assignmentResultIsCurrent(result.result));
            QCOMPARE(window->moveResizeGeometry(), original);
            QVERIFY(!window->m_transientOwnership->restoreAttempt->cancelled);
            if (scenario == 6 || scenario == 23) {
                const auto requestGeneration = window->geometryRequestGeneration();
                if (scenario == 6) {
                    nested = window->requestTransientRestore(makeClaim());
                } else {
                    nested = window->requestMoveResizeGuarded(original, Window::MoveResizeMode::MoveResize,
                                                              *result.result.checkpoint, result.result.owned);
                }
                QCOMPARE(nested->result.outcome, Outcome::Refused);
                QCOMPARE(window->geometryRequestGeneration(), requestGeneration);
                QVERIFY(!window->m_transientOwnership->restoreAttempt->cancelled);
                QVERIFY(window->assignmentResultIsCurrent(result.result));
            }
            if (scenario == 1) {
                QVERIFY(!configure.isEmpty() || configure.wait());
                shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
                Test::render(surface.get(), original.size().toSize(), Qt::green);
                QVERIFY(Test::waylandSync());
                QCOMPARE(window->frameGeometry(), original);
            } else {
                QCOMPARE(window->frameGeometry(), original);
            }
        }
        if (scenario == 9 || scenario == 24) {
            QVERIFY(finalCallback);
            QCOMPARE(result.dispatch, Dispatch::Queued);
        }
        if (scenario == 29 || scenario == 30) {
            QVERIFY(syncCallback);
            if (scenario == 30) {
                QCOMPARE(result.dispatch, Dispatch::None);
            }
        }
        if (scenario == 10) {
            QCOMPARE(window->moveResizeGeometry().topLeft(), QPointF(70, 80));
            QVERIFY(window->geometryRequestGeneration() != result.requestGeneration);
        }
        QCOMPARE(window->m_transientOwnership->restoreAttempt->requestGeneration, result.requestGeneration);
        QVERIFY(window->m_transientOwnership->restoreAttempt->requestConsumed());
    }
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QVERIFY(!window->m_transientOwnership->restoreAttempt->requesting());
    QCOMPARE(window->m_transientOwnership->generation, owner);
    QCOMPARE(window->m_transientOwnership->restoreAttempt->sequence, sequence);
    QCOMPARE(window->m_transientOwnership->floating, original);
    if (writer) {
        QVERIFY(writer->isReserved(id));
    }
    result.result.checkpoint.reset();
    claim.continuation.checkpoint.reset();
    held.reset();
    if (guard && !guard->isDeleted()) {
        guard->setSkipCloseAnimation(true);
        guard->destroyWindow();
    }
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testTransientRestoreClaim_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("pending") << 0;
    QTest::newRow("committed") << 1;
    QTest::newRow("no-owner") << 2;
    QTest::newRow("stale-owner-generation") << 3;
    QTest::newRow("generic-restoring") << 4;
    QTest::newRow("duplicate-during-release") << 5;
    QTest::newRow("cancel-during-release") << 6;
    QTest::newRow("public-commit-during-release") << 7;
    QTest::newRow("commit-roundtrip-during-release") << 8;
    QTest::newRow("desktop-roundtrip-during-release") << 9;
    QTest::newRow("close-during-release") << 10;
    QTest::newRow("writer-lost-during-release") << 11;
    QTest::newRow("provider-retired-during-release") << 12;
    QTest::newRow("root-destroyed-during-release") << 13;
    QTest::newRow("sequence-exhausted") << 14;
    QTest::newRow("last-usable-sequence") << 15;
    QTest::newRow("maximized-refused") << 16;
    QTest::newRow("fullscreen-refused") << 17;
    QTest::newRow("pre-request-refused") << 18;
    QTest::newRow("native-commit-roundtrip") << 19;
    QTest::newRow("native-transient-target-refused") << 20;
    QTest::newRow("public-commit-after-claim") << 21;
    QTest::newRow("cancel-after-claim") << 22;
    QTest::newRow("desktop-roundtrip-after-claim") << 23;
    QTest::newRow("requested-output-roundtrip") << 24;
    QTest::newRow("committed-output-roundtrip") << 25;
    QTest::newRow("token-bindings") << 26;
    QTest::newRow("old-native-content") << 27;
    QTest::newRow("provider-retired-after-claim") << 28;
    QTest::newRow("writer-lost-after-claim") << 29;
    QTest::newRow("reservation-lost-after-claim") << 32;
    QTest::newRow("restore-field-during-release") << 33;
    QTest::newRow("guarded-cancel-after-claim") << 34;
    QTest::newRow("membership-headroom-refused") << 35;
    QTest::newRow("requested-headroom-refused") << 36;
    QTest::newRow("committed-headroom-refused") << 37;
    QTest::newRow("placement-exhausted") << 38;
#if KWIN_BUILD_ACTIVITIES
    if (qEnvironmentVariableIsSet("KWIN_TEST_RESTORE_ACTIVITIES")) {
        QTest::newRow("activity-membership-roundtrip") << 30;
    }
#endif
}

void TestXdgShellWindow::testTransientRestoreClaim()
{
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    auto desktops = VirtualDesktopManager::self();
    const auto oldCount = desktops->count();
    const auto restoreDesktops = qScopeGuard([desktops, oldCount]() {
        desktops->setCount(oldCount);
    });
    desktops->setCount(std::max(oldCount, uint(2)));
    QString firstActivity;
    QString secondActivity;
#if KWIN_BUILD_ACTIVITIES
    if (scenario == 30) {
        // Service activation must leave the compositor event loop running. A blocking proxy
        // construction can otherwise wait for a client that is waiting for this compositor.
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                      QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
        message.setArguments({QStringLiteral("org.kde.ActivityManager"), uint(0)});
        QDBusPendingReply<uint> started = QDBusConnection::sessionBus().asyncCall(message, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(started.isFinished(), 16000);
        QVERIFY2(!started.isError(), qPrintable(started.error().message()));
        QDBusInterface activityControl(QStringLiteral("org.kde.ActivityManager"), QStringLiteral("/ActivityManager/Activities"),
                                       QStringLiteral("org.kde.ActivityManager.Activities"), QDBusConnection::sessionBus());
        QVERIFY(activityControl.isValid());
        QDBusPendingReply<QString> first = activityControl.asyncCall(QStringLiteral("AddActivity"), QStringLiteral("Restore claim first"));
        QDBusPendingReply<QString> second = activityControl.asyncCall(QStringLiteral("AddActivity"), QStringLiteral("Restore claim second"));
        QTRY_VERIFY_WITH_TIMEOUT(first.isFinished() && second.isFinished(), 10000);
        QVERIFY(!first.isError() && !second.isError());
        firstActivity = first.value();
        secondActivity = second.value();
        QVERIFY(!firstActivity.isEmpty() && !secondActivity.isEmpty() && firstActivity != secondActivity);
        QDBusPendingReply<bool> selected = activityControl.asyncCall(QStringLiteral("SetCurrentActivity"), firstActivity);
        QTRY_VERIFY_WITH_TIMEOUT(selected.isFinished(), 10000);
        QVERIFY(!selected.isError() && selected.value());
    }
#endif
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.restore-claim"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    configure.clear();
    if (scenario == 30) {
        const auto membershipReady = [&]() {
            window->setOnActivities({firstActivity});
            return window->activities() == QStringList{firstActivity} && window->isOnCurrentActivity();
        };
        QTRY_VERIFY_WITH_TIMEOUT(membershipReady(), 10000);
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto writer = std::make_unique<TransientPlacementWriter>(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto providerToken = writer->registerProvider(provider.get());
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.restore-claim");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer->trackWindow(id, key));
    QCOMPARE(writer->record(providerToken, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = desktops->currentDesktop(window->moveResizeOutput());
    auto otherDesktop = desktops->desktops().first() == desktop ? desktops->desktops().last() : desktops->desktops().first();
    auto controller = std::make_unique<TransientTileController>();
    QCOMPARE(controller->createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    QPointer<Tile> root(controller->rootTile());
    QPointer<Tile> leaf(controller->tile());
    for (int attempt = 0; attempt < 3 && !window->canStartTransientTileRequest(leaf->windowGeometry()); ++attempt) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        configure.clear();
        Test::render(surface.get(), QSize(200, 100), Qt::red);
        QVERIFY(Test::waylandSync());
    }
    QVERIFY(window->canStartTransientTileRequest(leaf->windowGeometry()));
    std::optional<Window::TransientRestoreClaim> nested;
    QObject callbacks;
    QMetaObject::Connection beforeRequest;
    if (scenario == 18) {
        beforeRequest = connect(leaf, &Tile::windowAdded, &callbacks, [&]() {
            nested = window->claimTransientRestore(window->ownedTransientPlacementGeneration());
        });
    }
    quint64 generation = 0;
    if (scenario != 2) {
        const auto state = writer->snapshot();
        QCOMPARE(controller->admit(controller->generation(), window, writer.get(), providerToken, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        generation = window->ownedTransientPlacementGeneration();
        QVERIFY(generation);
    }
    if (scenario == 18) {
        QVERIFY(nested);
        QCOMPARE(nested->continuation.outcome, Outcome::Refused);
        QCOMPARE(nested->sequence, 0u);
        QCOMPARE(window->m_transientRestoreSequence, 0u);
        QVERIFY(!window->m_transientOwnership->restoreAttempt);
        disconnect(beforeRequest);
    } else if (scenario == 1) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), leaf->windowGeometry().size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf.data());
    }
    Tile ordinary(manager);
    window->setGeometryRestore(RectF(11, 22, 333, 222));
    if (scenario == 4) {
        window->cancelTransientRequest();
    } else if (scenario == 14 || scenario == 15) {
        window->m_transientRestoreSequence = std::numeric_limits<quint64>::max() - (scenario == 14 ? 1 : 2);
    } else if (scenario == 16) {
        window->maximize(MaximizeFull);
    } else if (scenario == 17) {
        window->setFullScreen(true);
    } else if (scenario == 35) {
        window->m_tileMembershipChange = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 36) {
        window->m_requestedTileChange = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 37) {
        window->m_committedTileChange = std::numeric_limits<quint64>::max() - 1;
    } else if (scenario == 38) {
        window->m_placementContinuationRevision = std::numeric_limits<quint64>::max();
    }
    QPointer<Window> guard(window);
    std::unique_ptr<TileAssignmentFence> retained;
    if (scenario == 10) {
        retained = window->makeTileAssignmentCheckpoint();
    }
    bool intervened = false;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (scenario) {
        case 5:
            nested = window->claimTransientRestore(generation);
            break;
        case 6:
            window->cancelTransientRequest();
            break;
        case 7:
        case 8:
            window->commitTile(&ordinary);
            if (scenario == 8) {
                window->commitTile(nullptr);
            }
            break;
        case 9:
            window->setDesktops({otherDesktop});
            window->setDesktops({desktop});
            break;
        case 10:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        case 11:
            writer.reset();
            break;
        case 12:
            provider.reset();
            break;
        case 13:
            delete root.data();
            break;
        case 33:
            window->setGeometryRestore(RectF(21, 32, 343, 232));
            break;
        }
    };
    if ((scenario >= 5 && scenario <= 13) || scenario == 33) {
        if (scenario == 7 || scenario == 8) {
            connect(window, &Window::requestedTileChanged, &callbacks, intervene);
        } else {
            connect(leaf, &Tile::windowRemoved, &callbacks, intervene);
        }
    }
    const auto sequenceBefore = window->m_transientRestoreSequence;
    const auto geometryBefore = window->geometryRequestGeneration();
    const auto requestedBefore = window->requestedTile();
    const auto committedBefore = window->tile();
    const auto membershipBefore = window->m_tileMembershipChange;
    const auto requestedChangeBefore = window->m_requestedTileChange;
    const auto committedChangeBefore = window->m_committedTileChange;
    const auto placementBefore = window->m_placementContinuationRevision;
    const auto ownershipBefore = window->m_transientOwnership.get();
    const bool reservedBefore = writer && writer->isReserved(id);
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    auto claim = scenario == 18 ? Window::TransientRestoreClaim() : window->claimTransientRestore(scenario == 3 ? generation + 1 : generation);
    if (scenario == 2 || scenario == 3 || scenario == 4 || scenario == 14 || scenario == 16 || scenario == 17 || scenario == 18 || scenario >= 35) {
        QCOMPARE(claim.continuation.outcome, Outcome::Refused);
        QCOMPARE(claim.sequence, 0u);
        QCOMPARE(window->m_transientRestoreSequence, sequenceBefore);
        QCOMPARE(window->requestedTile(), requestedBefore);
        QCOMPARE(window->tile(), committedBefore);
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(window->m_requestedTileChange, requestedChangeBefore);
        QCOMPARE(window->m_committedTileChange, committedChangeBefore);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        QCOMPARE(window->m_transientOwnership.get(), ownershipBefore);
        QCOMPARE(bool(writer && writer->isReserved(id)), reservedBefore);
    } else {
        const bool superseded = (scenario >= 6 && scenario <= 12) || scenario == 33;
        QCOMPARE(claim.continuation.outcome, superseded ? Outcome::Superseded : Outcome::AppliedCurrent);
        QCOMPARE(claim.windowId, id);
        QCOMPARE(claim.ownershipGeneration, generation);
        QCOMPARE(claim.sequence, sequenceBefore + 1);
        QVERIFY(window->m_transientOwnership);
        QVERIFY(window->m_transientOwnership->restoreAttempt);
        QCOMPARE(window->m_transientOwnership->restoreAttempt->sequence, claim.sequence);
        QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        QVERIFY(!window->m_transientOwnership->controller);
        QCOMPARE(window->transientRestoreClaimIsCurrent(claim), !superseded);
        QCOMPARE(window->m_transientOwnership->restoreAttempt->cancelled, superseded);
        if (scenario == 5) {
            QVERIFY(nested);
            QCOMPARE(nested->continuation.outcome, Outcome::Refused);
            QCOMPARE(nested->sequence, 0u);
        }
        if (!superseded) {
            const auto duplicate = window->claimTransientRestore(generation);
            QCOMPARE(duplicate.continuation.outcome, Outcome::Refused);
            QCOMPARE(window->m_transientRestoreSequence, claim.sequence);
            if (scenario == 19) {
                window->commitTileFromConfigure(&ordinary, 0);
                window->commitTileFromConfigure(nullptr, 0);
                QVERIFY(!window->m_transientOwnership->restoreAttempt->cancelled);
                QVERIFY(window->transientRestoreAttemptIsCurrent(generation, claim.sequence));
                QVERIFY(!window->assignmentResultIsCurrent(claim.continuation));
            } else if (scenario == 20) {
                window->commitTileFromConfigure(leaf, 0);
                QVERIFY(!window->tile());
                QVERIFY(!window->m_transientOwnership->restoreAttempt->cancelled);
            } else if (scenario == 21) {
                window->commitTile(&ordinary);
                QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            } else if (scenario == 22) {
                window->cancelTransientRequest();
                QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            } else if (scenario == 23) {
                window->setDesktops({otherDesktop});
                window->setDesktops({desktop});
                QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            } else if (scenario == 24 || scenario == 25) {
                const auto outputs = workspace()->outputs();
                auto originalOutput = window->moveResizeOutput();
                auto otherOutput = outputs.first() == originalOutput ? outputs.last() : outputs.first();
                if (scenario == 24) {
                    window->setMoveResizeOutput(otherOutput);
                    window->setMoveResizeOutput(originalOutput);
                } else {
                    window->setOutput(otherOutput);
                    window->setOutput(originalOutput);
                }
                QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            } else if (scenario == 26) {
                claim.windowId = QUuid::createUuid();
                QVERIFY(!window->transientRestoreClaimIsCurrent(claim));
                claim.windowId = id;
                ++claim.ownershipGeneration;
                QVERIFY(!window->transientRestoreClaimIsCurrent(claim));
                claim.ownershipGeneration = generation;
                ++claim.sequence;
                QVERIFY(!window->transientRestoreClaimIsCurrent(claim));
                --claim.sequence;
            } else if (scenario == 27) {
                QVERIFY(!configure.isEmpty() || configure.wait());
                shell->xdgSurface()->ack_configure(configure.first().at(0).value<quint32>());
                Test::render(surface.get(), leaf->windowGeometry().size().toSize(), Qt::blue);
                QVERIFY(Test::waylandSync());
                QVERIFY(!window->m_transientOwnership->restoreAttempt->cancelled);
                QVERIFY(window->transientRestoreAttemptIsCurrent(generation, claim.sequence));
                QVERIFY(!window->assignmentResultIsCurrent(claim.continuation));
            } else if (scenario == 28) {
                provider.reset();
            } else if (scenario == 29) {
                writer.reset();
            } else if (scenario == 30) {
                window->setOnActivities({secondActivity});
                QCOMPARE(window->activities(), QStringList{secondActivity});
                window->setOnActivities({firstActivity});
                QCOMPARE(window->activities(), QStringList{firstActivity});
                QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            } else if (scenario == 32) {
                QCOMPARE(writer->completeRestore(id, QString::number(generation), 2), TransientPlacementWriter::Result::Accepted);
            } else if (scenario == 34) {
                const auto cancellation = window->cancelTransientRequestForAssignment();
                QCOMPARE(cancellation.outcome, Outcome::AppliedCurrent);
                QCOMPARE(window->m_transientRestoreSequence, claim.sequence);
                QVERIFY(window->m_transientOwnership->restoreAttempt->cancelled);
            }
            const bool staysCurrent = scenario < 19 || scenario == 20 || scenario == 26;
            QCOMPARE(window->transientRestoreClaimIsCurrent(claim), staysCurrent);
        }
    }
    QCOMPARE(window->geometryRequestGeneration(), geometryBefore);
    QCOMPARE(geometry.count(), 0);
    claim.continuation.checkpoint.reset();
    retained.reset();
    if (guard && !guard->isDeleted()) {
        guard->setSkipCloseAnimation(true);
        guard->destroyWindow();
    }
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testControllerReleaseOrdering_data()
{
    QTest::addColumn<bool>("committed");
    QTest::newRow("pending") << false;
    QTest::newRow("committed") << true;
}

void TestXdgShellWindow::testControllerReleaseOrdering()
{
    QFETCH(bool, committed);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.release-ordering"));
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
    const auto key = QStringLiteral("org.example.release-ordering");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    const auto generation = window->m_transientOwnership->generation;
    if (committed) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), leaf->windowGeometry().size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), leaf);
    } else {
        QVERIFY(!window->tile());
    }
    const auto geometryGeneration = window->geometryRequestGeneration();
    QSignalSpy geometry(window, &Window::requestedGeometryChanged);
    QStringList notifications;
    QObject context;
    const auto observe = [&](const QString &name) {
        notifications.append(name);
        QVERIFY(window->m_transientOwnership);
        const auto &ownership = *window->m_transientOwnership;
        QCOMPARE(ownership.generation, generation);
        QCOMPARE(QRectF(ownership.floating), original);
        QCOMPARE(ownership.phase, Window::TransientOwnershipPhase::Restoring);
        QCOMPARE(ownership.progress, Window::TransientRequestProgress::Requested);
        QVERIFY(ownership.releasing);
        QVERIFY(!ownership.controller);
        QVERIFY(leaf->windows().isEmpty());
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
        QVERIFY(writer.isReserved(id));
        QVERIFY(!writer.ownsReservation(id, QString::number(generation)));
        QCOMPARE(writer.snapshot().entries.value(key), original);
        QCOMPARE(window->geometryRequestGeneration(), geometryGeneration);
        QCOMPARE(geometry.count(), 0);
    };
    connect(window, &Window::requestedTileChanged, &context, [&]() {
        observe(QStringLiteral("requested"));
    });
    connect(window, &Window::tileChanged, &context, [&]() {
        observe(QStringLiteral("committed"));
    });
    connect(window, &Window::quickTileModeChanged, &context, [&]() {
        observe(QStringLiteral("mode"));
    });
    connect(leaf, &Tile::windowRemoved, &context, [&]() {
        observe(QStringLiteral("removed"));
    });
    connect(leaf, &Tile::windowsChanged, &context, [&]() {
        observe(QStringLiteral("membership"));
    });
    auto result = window->cancelTransientRequestForAssignment();
    const QStringList expected = committed
        ? QStringList{QStringLiteral("requested"), QStringLiteral("committed"), QStringLiteral("mode"), QStringLiteral("removed"), QStringLiteral("membership")}
        : QStringList{QStringLiteral("requested"), QStringLiteral("removed"), QStringLiteral("membership")};
    QCOMPARE(notifications, expected);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint);
    QVERIFY(window->assignmentResultIsCurrent(result));
    QVERIFY(window->m_transientOwnership);
    QVERIFY(!window->m_transientOwnership->releasing);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QVERIFY(writer.isReserved(id));
    QCOMPARE(geometry.count(), 0);

    // The native record, not a finished restoration, keeps the per-key writer barrier alive.
    const auto other = QUuid::createUuid();
    const QRectF manual(40, 50, 260, 160);
    QVERIFY(writer.trackWindow(other, key));
    QCOMPARE(writer.record(token, 2, {other, manual, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Deferred);
    QCOMPARE(writer.snapshot().entries.value(key), original);
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QVERIFY(guard && guard->isDeleted());
    QVERIFY(!window->assignmentResultIsCurrent(result));
    QVERIFY(window->m_transientOwnership);
    QVERIFY(writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), original);
    // Logical close cannot release the barrier while the caller's checkpoint retains the Window.
    result.checkpoint.reset();
    QTRY_VERIFY(!guard);
    QVERIFY(!writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), manual);
}

void TestXdgShellWindow::testCompatibilityCancellation_data()
{
    QTest::addColumn<bool>("committed");
    QTest::addColumn<int>("action");
    QTest::addColumn<bool>("viaProperty");
    for (bool committed : {false, true}) {
        const QByteArray prefix = committed ? "committed-" : "pending-";
        QTest::newRow((prefix + "unchanged").constData()) << committed << 0 << false;
        QTest::newRow((prefix + "equal-null").constData()) << committed << 1 << false;
        QTest::newRow((prefix + "manual").constData()) << committed << 2 << false;
        QTest::newRow((prefix + "equal-geometry").constData()) << committed << 3 << false;
        QTest::newRow((prefix + "commit-only").constData()) << committed << 4 << false;
        QTest::newRow((prefix + "commit-roundtrip").constData()) << committed << 5 << false;
        QTest::newRow((prefix + "target-destroyed").constData()) << committed << 6 << false;
        QTest::newRow((prefix + "window-closed").constData()) << committed << 7 << false;
        QTest::newRow((prefix + "helper-window-closed").constData()) << committed << 8 << false;
        QTest::newRow((prefix + "saturation-next").constData()) << committed << 9 << false;
        QTest::newRow((prefix + "saturation-current").constData()) << committed << 10 << false;
        QTest::newRow((prefix + "outer-null-new-manual").constData()) << committed << 11 << false;
        QTest::newRow((prefix + "property-unchanged").constData()) << committed << 0 << true;
        QTest::newRow((prefix + "property-equal-null").constData()) << committed << 1 << true;
        QTest::newRow((prefix + "property-commit-only").constData()) << committed << 4 << true;
        QTest::newRow((prefix + "property-commit-roundtrip").constData()) << committed << 5 << true;
    }
}

void TestXdgShellWindow::testCompatibilityCancellation()
{
    QFETCH(bool, committed);
    QFETCH(int, action);
    QFETCH(bool, viaProperty);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.compatibility"));
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
    const auto key = QStringLiteral("org.example.compatibility");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), Result::Accepted);
    QCOMPARE(window->requestedTile(), controller.tile());
    if (committed) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), controller.tile()->windowGeometry().size().toSize(), Qt::blue);
        QVERIFY(Test::waylandSync());
        QCOMPARE(window->tile(), controller.tile());
    } else {
        QVERIFY(!window->tile());
    }
    auto targets = std::make_unique<TileManager>(window->moveResizeOutput());
    QPointer<Tile> target = targets->quickTile(QuickTileFlag::Left);
    QPointer<Tile> other = targets->quickTile(QuickTileFlag::Right);
    QVERIFY(target && other);
    if (action == 9 || action == 10) {
        window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - (action == 9 ? 1 : 0);
    }
    QSignalSpy targetAdded(target, &Tile::windowAdded);
    QObject context;
    QPointer<Window> guard(window);
    bool called = false;
    connect(window, &Window::requestedTileChanged, &context, [&]() {
        if (called) {
            return;
        }
        called = true;
        QVERIFY(window->m_transientOwnership);
        QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
        switch (action) {
        case 1:
            window->setTileCompatibility(nullptr);
            break;
        case 2:
        case 11:
            QVERIFY(other->manage(window));
            break;
        case 3:
            window->moveResize(window->moveResizeGeometry());
            break;
        case 4:
            window->commitTile(other.data());
            break;
        case 5:
            window->commitTile(other.data());
            window->commitTile(nullptr);
            break;
        case 6:
            targets.reset();
            break;
        case 7:
        case 8:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        }
    });
    if (action == 8) {
        QVERIFY(!window->cancelTransientRequest());
    } else if (viaProperty) {
        // True means the meta-object dispatched the setter, not that placement succeeded.
        QVERIFY(window->setProperty("tile", QVariant::fromValue(target.data())));
    } else {
        window->setTileCompatibility(action == 11 ? nullptr : target.data());
    }
    QVERIFY(called);
    QCOMPARE(targetAdded.count(), action == 0 ? 1 : 0);
    if (action == 7 || action == 8) {
        QVERIFY(!guard);
        QVERIFY(target->windows().isEmpty());
        QVERIFY(!writer.isReserved(id));
        QCOMPARE(writer.snapshot().entries.value(key), original);
        return;
    }
    QVERIFY(guard);
    if (action == 6) {
        QVERIFY(!target && !other);
    }
    if (target) {
        QCOMPARE(target->windows().contains(window), action == 0);
    } else {
        QCOMPARE(action, 6);
    }
    Tile *expectedTile = action == 0 ? target.data() : nullptr;
    if (action == 2 || action == 11) {
        expectedTile = other.data();
        QVERIFY(other->windows().contains(window));
    }
    QCOMPARE(window->requestedTile(), expectedTile);
    if (action == 4 || action == 5) {
        QCOMPARE(window->tile(), action == 4 ? other.data() : nullptr);
    }
    if (action == 9 || action == 10) {
        QCOMPARE(window->m_placementContinuationRevision, std::numeric_limits<quint64>::max());
        QVERIFY(!window->cancelTransientRequest());
        QCOMPARE(window->m_placementContinuationRevision, std::numeric_limits<quint64>::max());
    }
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Restoring);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QVERIFY(writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), original);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
}

void TestXdgShellWindow::testTileAssignmentFenceNeutral()
{
    static_assert(!std::is_copy_constructible_v<TileAssignmentFence>);
    static_assert(!std::is_move_constructible_v<TileAssignmentFence>);
    static_assert(!std::is_copy_assignable_v<TileAssignmentFence>);
    static_assert(!std::is_move_assignable_v<TileAssignmentFence>);
    static_assert(!std::is_constructible_v<TileAssignmentFence, Window &>);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::Surface> otherSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> otherShell(Test::createXdgToplevelSurface(otherSurface.get()));
    auto other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(other);
    const std::array fields = {&Window::m_placementContinuationRevision, &Window::m_requestedTileChange,
                               &Window::m_committedTileChange, &Window::m_geometryRequestGeneration};
    // Colliding versions make wrong-Window refusal independent of the counter comparisons.
    for (auto field : fields) {
        const auto revision = std::max(window->*field, other->*field);
        window->*field = revision;
        other->*field = revision;
    }
    const auto revisions = std::array{window->m_placementContinuationRevision, window->m_requestedTileChange,
                                      window->m_committedTileChange, window->m_geometryRequestGeneration};
    const auto geometry = window->moveResizeGeometry();
    const int references = window->m_refCount;
    QSignalSpy requested(window, &Window::requestedTileChanged);
    QSignalSpy committed(window, &Window::tileChanged);
    QSignalSpy geometryChanged(window, &Window::requestedGeometryChanged);
    {
        auto fence = window->makeTileAssignmentFence();
        QCOMPARE(window->m_refCount, references + 1);
        QVERIFY(fence.isCurrentFor(*window));
        QVERIFY(!fence.isCurrentFor(*other));
        QVERIFY(fence.isCurrentFor(*window));
        {
            auto second = window->makeTileAssignmentFence();
            QCOMPARE(window->m_refCount, references + 2);
            QVERIFY(second.isCurrentFor(*window));
            QVERIFY(fence.isCurrentFor(*window));
        }
        QCOMPARE(window->m_refCount, references + 1);
        QCOMPARE(window->m_placementContinuationRevision, revisions[0]);
        QCOMPARE(window->m_requestedTileChange, revisions[1]);
        QCOMPARE(window->m_committedTileChange, revisions[2]);
        QCOMPARE(window->m_geometryRequestGeneration, revisions[3]);
        QCOMPARE(window->moveResizeGeometry(), geometry);
        QVERIFY(!window->requestedTile() && !window->tile());
        QVERIFY(requested.isEmpty() && committed.isEmpty() && geometryChanged.isEmpty());
    }
    QCOMPARE(window->m_refCount, references);
}

void TestXdgShellWindow::testTileAssignmentFenceInvalidation_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("equal-null") << 0;
    QTest::newRow("equal-geometry") << 1;
    QTest::newRow("requested-roundtrip") << 2;
    QTest::newRow("committed-roundtrip") << 3;
}

void TestXdgShellWindow::testTileAssignmentFenceInvalidation()
{
    QFETCH(int, action);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto tile = workspace()->tileManager(window->moveResizeOutput())->quickTile(QuickTileFlag::Left);
    QVERIFY(tile);
    auto fence = window->makeTileAssignmentFence();
    QVERIFY(fence.isCurrentFor(*window));
    switch (action) {
    case 0:
        window->setTileCompatibility(nullptr);
        break;
    case 1:
        window->moveResize(window->moveResizeGeometry());
        break;
    case 2:
        window->requestTile(tile);
        window->requestTile(nullptr);
        break;
    case 3:
        window->commitTile(tile);
        window->commitTile(nullptr);
        break;
    }
    QVERIFY(!fence.isCurrentFor(*window));
    QVERIFY(!fence.isCurrentFor(*window));
    QVERIFY(!window->requestedTile() && !window->tile());
}

void TestXdgShellWindow::testTileAssignmentFenceAssociations_data()
{
    QTest::addColumn<bool>("requested");
    QTest::addColumn<bool>("committed");
    QTest::newRow("requested-only") << true << false;
    QTest::newRow("committed-only") << false << true;
    QTest::newRow("both") << true << true;
}

void TestXdgShellWindow::testTileAssignmentFenceAssociations()
{
    QFETCH(bool, requested);
    QFETCH(bool, committed);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto tile = std::make_unique<Tile>(workspace()->tileManager(window->moveResizeOutput()));
    tile->setRelativeGeometry(RectF(0, 0, 0.5, 1));
    if (requested) {
        window->requestTile(tile.get());
    }
    if (committed) {
        window->commitTile(tile.get());
    }
    // No membership means destruction only clears QPointers, without ordinary unmanage calls.
    QVERIFY(tile->windows().isEmpty());
    const auto revisions = std::array{window->m_placementContinuationRevision, window->m_requestedTileChange,
                                      window->m_committedTileChange, window->m_geometryRequestGeneration};
    QSignalSpy requestedChanged(window, &Window::requestedTileChanged);
    QSignalSpy committedChanged(window, &Window::tileChanged);
    auto fence = window->makeTileAssignmentFence();
    QVERIFY(fence.isCurrentFor(*window));
    tile.reset();
    QVERIFY(!window->requestedTile() && !window->tile());
    QCOMPARE(window->m_placementContinuationRevision, revisions[0]);
    QCOMPARE(window->m_requestedTileChange, revisions[1]);
    QCOMPARE(window->m_committedTileChange, revisions[2]);
    QCOMPARE(window->m_geometryRequestGeneration, revisions[3]);
    QVERIFY(requestedChanged.isEmpty() && committedChanged.isEmpty());
    QVERIFY(!fence.isCurrentFor(*window));
}

void TestXdgShellWindow::testTileAssignmentFenceClose()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QPointer<Window> guard(window);
    QSignalSpy destroyed(window, &QObject::destroyed);
    {
        auto fence = window->makeTileAssignmentFence();
        QVERIFY(fence.isCurrentFor(*window));
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QVERIFY(guard);
        QVERIFY(window->isDeleted());
        QVERIFY(!fence.isCurrentFor(*window));
        // Matching snapshots still cannot make a logically closed Window current.
        auto closedFence = window->makeTileAssignmentFence();
        QVERIFY(!closedFence.isCurrentFor(*window));
        QCOMPARE(destroyed.count(), 0);
    }
    QTRY_VERIFY(!guard);
    QCOMPARE(destroyed.count(), 1);
}

void TestXdgShellWindow::testTileAssignmentFenceCounters_data()
{
    QTest::addColumn<int>("field");
    QTest::addColumn<int>("boundary");
    const std::array names = {QByteArray("placement"), QByteArray("requested"), QByteArray("committed"), QByteArray("geometry")};
    for (int field = 0; field < int(names.size()); ++field) {
        QTest::newRow((names[field] + "-change").constData()) << field << 0;
        QTest::newRow((names[field] + "-last-value").constData()) << field << 1;
        QTest::newRow((names[field] + "-exhausted").constData()) << field << 2;
    }
}

void TestXdgShellWindow::testTileAssignmentFenceCounters()
{
    QFETCH(int, field);
    QFETCH(int, boundary);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const std::array fields = {&Window::m_placementContinuationRevision, &Window::m_requestedTileChange,
                               &Window::m_committedTileChange, &Window::m_geometryRequestGeneration};
    auto &revision = window->*fields[field];
    const auto maximum = std::numeric_limits<quint64>::max();
    if (boundary) {
        revision = maximum - (boundary == 1 ? 1 : 0);
    }
    const auto before = std::array{window->m_placementContinuationRevision, window->m_requestedTileChange,
                                   window->m_committedTileChange, window->m_geometryRequestGeneration};
    auto fence = window->makeTileAssignmentFence();
    QCOMPARE(fence.isCurrentFor(*window), boundary != 2);
    if (boundary != 2) {
        // Isolate each comparison so another changed counter cannot mask its absence.
        ++revision;
        for (int other = 0; other < int(fields.size()); ++other) {
            if (other != field) {
                QCOMPARE(window->*fields[other], before[other]);
            }
        }
    }
    QVERIFY(!fence.isCurrentFor(*window));
    if (boundary) {
        auto tile = workspace()->tileManager(window->moveResizeOutput())->quickTile(QuickTileFlag::Left);
        QVERIFY(tile);
        switch (field) {
        case 0:
            window->cancelTransientRequest();
            window->cancelTransientRequest();
            break;
        case 1:
            window->requestTile(tile);
            window->requestTile(nullptr);
            break;
        case 2:
            window->commitTile(tile);
            window->commitTile(nullptr);
            break;
        case 3:
            window->moveResize(RectF(30, 40, 260, 130));
            window->moveResize(RectF(40, 50, 270, 140));
            break;
        }
        QCOMPARE(revision, maximum);
    }
    QVERIFY(!fence.isCurrentFor(*window));
}

void TestXdgShellWindow::testGuardedTileCommit()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto tile = std::make_unique<Tile>(workspace()->tileManager(window->moveResizeOutput()));
    tile->setRelativeGeometry(RectF(0, 0, 0.5, 1));
    tile->setQuickTileMode(QuickTileFlag::Custom);
    window->requestTile(tile.get());
    QVERIFY(!window->tile());
    const auto requestedRevision = window->m_requestedTileChange;
    const auto committedRevision = window->m_committedTileChange;
    const auto placementRevision = window->m_placementContinuationRevision;
    const auto geometryRevision = window->m_geometryRequestGeneration;
    const auto requestedGeometry = window->moveResizeGeometry();
    const auto frameGeometry = window->frameGeometry();
    auto before = window->makeTileAssignmentFence();
    QStringList notifications;
    QObject context;
    connect(window, &Window::tileChanged, &context, [&]() {
        notifications.append(QStringLiteral("tile"));
    });
    connect(window, &Window::quickTileModeChanged, &context, [&]() {
        notifications.append(QStringLiteral("mode"));
    });
    auto result = window->commitTileForAssignment(tile.get(), before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint);
    QVERIFY(result.checkpoint->isCurrentFor(*window));
    QVERIFY(!before.isCurrentFor(*window));
    QCOMPARE(window->tile(), tile.get());
    QCOMPARE(window->requestedTile(), tile.get());
    QCOMPARE(window->m_requestedTileChange, requestedRevision);
    QCOMPARE(window->m_committedTileChange, committedRevision + 1);
    QCOMPARE(window->m_placementContinuationRevision, placementRevision + 1);
    QCOMPARE(window->m_geometryRequestGeneration, geometryRevision);
    QCOMPARE(window->moveResizeGeometry(), requestedGeometry);
    QCOMPARE(window->frameGeometry(), frameGeometry);
    QCOMPARE(notifications, QStringList({QStringLiteral("tile"), QStringLiteral("mode")}));
    notifications.clear();
    auto unchanged = window->commitTileForAssignment(tile.get(), *result.checkpoint);
    QCOMPARE(unchanged.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(unchanged.checkpoint && unchanged.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->m_committedTileChange, committedRevision + 1);
    QCOMPARE(window->m_placementContinuationRevision, placementRevision + 1);
    QVERIFY(notifications.isEmpty());
    window->forgetTile(tile.get());
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->tile(), tile.get());
    auto beforeNull = window->makeTileAssignmentFence();
    auto toNull = window->commitTileForAssignment(nullptr, beforeNull);
    QCOMPARE(toNull.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(toNull.checkpoint && toNull.checkpoint->isCurrentFor(*window));
    QVERIFY(!window->tile());
    QCOMPARE(notifications, QStringList({QStringLiteral("tile"), QStringLiteral("mode")}));
    notifications.clear();
    auto nullNoOp = window->commitTileForAssignment(nullptr, *toNull.checkpoint);
    QCOMPARE(nullNoOp.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(nullNoOp.checkpoint && nullNoOp.checkpoint->isCurrentFor(*window));
    QVERIFY(notifications.isEmpty());
}

void TestXdgShellWindow::testGuardedTileCommitRefusal_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("mismatched-target") << 0;
    QTest::newRow("stale-fence") << 1;
    QTest::newRow("wrong-window") << 2;
    QTest::newRow("raw-destroyed-target") << 3;
    QTest::newRow("captured-destroyed-target") << 4;
    QTest::newRow("closed-window") << 5;
}

void TestXdgShellWindow::testGuardedTileCommitRefusal()
{
    QFETCH(int, action);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::Surface> otherSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> otherShell(Test::createXdgToplevelSurface(otherSurface.get()));
    auto other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(other);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto target = std::make_unique<Tile>(manager);
    auto alternative = std::make_unique<Tile>(manager);
    target->setRelativeGeometry(RectF(0, 0, 0.5, 1));
    alternative->setRelativeGeometry(RectF(0.5, 0, 0.5, 1));
    alternative->setQuickTileMode(QuickTileFlag::Custom);
    window->requestTile(target.get());
    Tile *rawTarget = target.get();
    if (action == 3) {
        target.reset();
    }
    auto before = (action == 2 ? other : window)->makeTileAssignmentFence();
    if (action == 1) {
        window->setTileCompatibility(nullptr);
    } else if (action == 4) {
        target.reset();
    } else if (action == 5) {
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QVERIFY(window->isDeleted());
    }
    const auto revisions = std::array{window->m_placementContinuationRevision, window->m_requestedTileChange,
                                      window->m_committedTileChange, window->m_geometryRequestGeneration};
    const auto requested = window->requestedTile();
    const auto committed = window->tile();
    const auto geometry = window->moveResizeGeometry();
    QSignalSpy requestedChanged(window, &Window::requestedTileChanged);
    QSignalSpy tileChanged(window, &Window::tileChanged);
    QSignalSpy modeChanged(window, &Window::quickTileModeChanged);
    auto result = window->commitTileForAssignment(action == 0 ? alternative.get() : rawTarget, before);
    QCOMPARE(result.outcome, action == 0 || action == 3 ? Window::AssignmentStepOutcome::Refused : Window::AssignmentStepOutcome::Superseded);
    QVERIFY(!result.checkpoint);
    QCOMPARE(window->m_placementContinuationRevision, revisions[0]);
    QCOMPARE(window->m_requestedTileChange, revisions[1]);
    QCOMPARE(window->m_committedTileChange, revisions[2]);
    QCOMPARE(window->m_geometryRequestGeneration, revisions[3]);
    QCOMPARE(window->requestedTile(), requested);
    QCOMPARE(window->tile(), committed);
    QCOMPARE(window->moveResizeGeometry(), geometry);
    QVERIFY(requestedChanged.isEmpty() && tileChanged.isEmpty() && modeChanged.isEmpty());
    if (action == 0) {
        // Ordinary commits deliberately keep their legacy, less restrictive contract.
        window->commitTile(alternative.get());
        QCOMPARE(window->tile(), alternative.get());
        QCOMPARE(window->requestedTile(), target.get());
    }
}

void TestXdgShellWindow::testGuardedTileCommitCapacity_data()
{
    QTest::addColumn<bool>("placement");
    QTest::addColumn<int>("scenario");
    for (bool placement : {false, true}) {
        const QByteArray prefix = placement ? "placement-" : "committed-";
        QTest::newRow((prefix + "last-change-refused").constData()) << placement << 0;
        QTest::newRow((prefix + "penultimate-change").constData()) << placement << 1;
        QTest::newRow((prefix + "last-noop").constData()) << placement << 2;
        QTest::newRow((prefix + "exhausted-input").constData()) << placement << 3;
    }
}

void TestXdgShellWindow::testGuardedTileCommitCapacity()
{
    QFETCH(bool, placement);
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto tile = std::make_unique<Tile>(workspace()->tileManager(window->moveResizeOutput()));
    tile->setRelativeGeometry(RectF(0, 0, 0.5, 1));
    tile->setQuickTileMode(QuickTileFlag::Custom);
    window->requestTile(tile.get());
    if (scenario >= 2) {
        window->commitTile(tile.get());
    }
    auto &limited = placement ? window->m_placementContinuationRevision : window->m_committedTileChange;
    const auto maximum = std::numeric_limits<quint64>::max();
    quint64 headroom = 1;
    if (scenario == 1) {
        headroom = 2;
    } else if (scenario == 3) {
        headroom = 0;
    }
    limited = maximum - headroom;
    const auto oldCommitted = window->m_committedTileChange;
    const auto oldPlacement = window->m_placementContinuationRevision;
    auto before = window->makeTileAssignmentFence();
    QSignalSpy tileChanged(window, &Window::tileChanged);
    QSignalSpy modeChanged(window, &Window::quickTileModeChanged);
    auto result = window->commitTileForAssignment(tile.get(), before);
    auto expected = Window::AssignmentStepOutcome::AppliedCurrent;
    if (scenario == 0) {
        expected = Window::AssignmentStepOutcome::Refused;
    } else if (scenario == 3) {
        expected = Window::AssignmentStepOutcome::Superseded;
    }
    QCOMPARE(result.outcome, expected);
    QCOMPARE(bool(result.checkpoint), scenario == 1 || scenario == 2);
    if (result.checkpoint) {
        QVERIFY(result.checkpoint->isCurrentFor(*window));
    }
    QCOMPARE(window->m_committedTileChange, oldCommitted + (scenario == 1 ? 1 : 0));
    QCOMPARE(window->m_placementContinuationRevision, oldPlacement + (scenario == 1 ? 1 : 0));
    QCOMPARE(tileChanged.count(), scenario == 1 ? 1 : 0);
    QCOMPARE(modeChanged.count(), scenario == 1 ? 1 : 0);
    QCOMPARE(window->requestedTile(), tile.get());
    QCOMPARE(window->tile(), scenario == 0 ? nullptr : tile.get());
}

void TestXdgShellWindow::testGuardedTileCommitTransientTarget()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    RequestTestTile target(window);
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    const auto request = window->requestTransientTile(std::make_unique<RequestForTest>(window, &target, authority));
    QCOMPARE(request.outcome, Window::TransientRequestOutcome::Applied);
    QCOMPARE(window->requestedTile(), &target);
    auto before = window->makeTileAssignmentFence();
    QVERIFY(before.isCurrentFor(*window));
    QSignalSpy committed(window, &Window::tileChanged);
    auto result = window->commitTileForAssignment(&target, before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::Refused);
    QVERIFY(!result.checkpoint);
    QVERIFY(before.isCurrentFor(*window));
    QCOMPARE(window->requestedTile(), &target);
    QVERIFY(!window->tile());
    QVERIFY(committed.isEmpty());
}

void TestXdgShellWindow::testGuardedTileCommitReentry_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("equal-requested") << 0;
    QTest::newRow("equal-geometry") << 1;
    QTest::newRow("committed-roundtrip") << 2;
    QTest::newRow("target-destroyed") << 3;
    QTest::newRow("window-closed") << 4;
    QTest::newRow("input-owner-close") << 5;
    QTest::newRow("mode-notification") << 6;
    QTest::newRow("new-requested") << 7;
    QTest::newRow("input-owner-only") << 8;
    QTest::newRow("restore-change") << 9;
    QTest::newRow("restore-same") << 10;
}

void TestXdgShellWindow::testGuardedTileCommitReentry()
{
    QFETCH(int, action);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto target = std::make_unique<Tile>(manager);
    auto other = std::make_unique<Tile>(manager);
    target->setRelativeGeometry(RectF(0, 0, 0.5, 1));
    other->setRelativeGeometry(RectF(0.5, 0, 0.5, 1));
    target->setQuickTileMode(QuickTileFlag::Custom);
    other->setQuickTileMode(QuickTileFlag::Custom);
    window->requestTile(target.get());
    // Direct prvalue initialization preserves the fence's nonmovable type.
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence(window->makeTileAssignmentFence()));
    QPointer<Window> guard(window);
    QSignalSpy modeChanged(window, &Window::quickTileModeChanged);
    bool invoked = false;
    QObject context;
    const auto callback = [&]() {
        if (invoked) {
            return;
        }
        invoked = true;
        switch (action) {
        case 0:
            window->setTileCompatibility(target.get());
            break;
        case 1:
            window->moveResize(window->moveResizeGeometry());
            break;
        case 2:
            window->commitTile(other.get());
            window->commitTile(target.get());
            break;
        case 3:
            target.reset();
            break;
        case 5:
            before.reset();
            [[fallthrough]];
        case 4:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            QVERIFY(guard);
            break;
        case 6:
            window->setTileCompatibility(nullptr);
            break;
        case 7:
            window->requestTile(other.get());
            break;
        case 8:
            before.reset();
            break;
        case 9:
            window->setGeometryRestore(RectF(60, 70, 220, 130));
            break;
        case 10:
            window->setGeometryRestore(window->geometryRestore());
            break;
        }
    };
    if (action == 6) {
        connect(window, &Window::quickTileModeChanged, &context, callback);
    } else {
        connect(window, &Window::tileChanged, &context, callback);
    }
    auto result = window->commitTileForAssignment(target.get(), *before);
    QVERIFY(invoked);
    const bool current = action == 8 || action == 10;
    QCOMPARE(result.outcome, current ? Window::AssignmentStepOutcome::AppliedCurrent : Window::AssignmentStepOutcome::Superseded);
    QCOMPARE(bool(result.checkpoint), current);
    QCOMPARE(modeChanged.count(), action == 6 || current ? 1 : 0);
    if (action == 4 || action == 5) {
        if (action == 4) {
            QVERIFY(guard && window->isDeleted());
            before.reset();
        }
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    if (action == 3) {
        QVERIFY(!window->requestedTile() && !window->tile());
    } else if (action == 7) {
        QCOMPARE(window->requestedTile(), other.get());
    } else if (action == 2) {
        QCOMPARE(window->tile(), target.get());
    } else if (current) {
        QCOMPARE(!before, action == 8);
        QVERIFY(result.checkpoint->isCurrentFor(*window));
    }
}

void TestXdgShellWindow::testGuardedGeometryRestore()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const RectF original(10, 20, 200, 100);
    const RectF desired(30, 40, 240, 160);
    window->setGeometryRestore(original);
    const auto revision = window->m_placementContinuationRevision;
    const auto requestedRevision = window->m_requestedTileChange;
    const auto committedRevision = window->m_committedTileChange;
    const auto geometryRevision = window->m_geometryRequestGeneration;
    const auto requestedGeometry = window->moveResizeGeometry();
    const auto frameGeometry = window->frameGeometry();
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy requestedChanged(window, &Window::requestedGeometryChanged);
    QSignalSpy tileChanged(window, &Window::tileChanged);
    auto before = window->makeTileAssignmentFence();
    auto result = window->setGeometryRestoreForAssignment(desired, before);
    QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QVERIFY(!before.isCurrentFor(*window));
    QCOMPARE(window->geometryRestore(), desired);
    QCOMPARE(window->m_placementContinuationRevision, revision + 1);
    QCOMPARE(window->m_requestedTileChange, requestedRevision);
    QCOMPARE(window->m_committedTileChange, committedRevision);
    QCOMPARE(window->m_geometryRequestGeneration, geometryRevision);
    QCOMPARE(window->moveResizeGeometry(), requestedGeometry);
    QCOMPARE(window->frameGeometry(), frameGeometry);
    QCOMPARE(restoreChanged.count(), 1);
    QVERIFY(requestedChanged.isEmpty() && tileChanged.isEmpty());
    auto unchanged = window->setGeometryRestoreForAssignment(desired, *result.checkpoint);
    QCOMPARE(unchanged.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
    QVERIFY(unchanged.checkpoint && unchanged.checkpoint->isCurrentFor(*window));
    QCOMPARE(window->m_placementContinuationRevision, revision + 1);
    QCOMPARE(restoreChanged.count(), 1);
}

void TestXdgShellWindow::testGuardedGeometryRestoreRefusal_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("stale") << 0;
    QTest::newRow("wrong-window") << 1;
    QTest::newRow("closed") << 2;
    QTest::newRow("last-change-refused") << 3;
    QTest::newRow("penultimate-change") << 4;
    QTest::newRow("last-noop") << 5;
    QTest::newRow("exhausted-input") << 6;
}

void TestXdgShellWindow::testGuardedGeometryRestoreRefusal()
{
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    std::unique_ptr<KWayland::Client::Surface> otherSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> otherShell(Test::createXdgToplevelSurface(otherSurface.get()));
    auto other = Test::renderAndWaitForShown(otherSurface.get(), QSize(200, 100), Qt::blue);
    QVERIFY(other);
    const RectF original(10, 20, 200, 100);
    const RectF desired = scenario == 5 || scenario == 6 ? original : RectF(30, 40, 240, 160);
    window->setGeometryRestore(original);
    if (scenario >= 3) {
        quint64 headroom = 1;
        if (scenario == 4) {
            headroom = 2;
        } else if (scenario == 6) {
            headroom = 0;
        }
        window->m_placementContinuationRevision = std::numeric_limits<quint64>::max() - headroom;
    }
    auto owner = scenario == 1 ? other : window;
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence(owner->makeTileAssignmentFence()));
    QPointer<Window> guard(window);
    if (scenario == 0) {
        window->cancelTransientRequest();
    } else if (scenario == 2) {
        window->setSkipCloseAnimation(true);
        window->destroyWindow();
        QVERIFY(guard);
    }
    const auto revision = window->m_placementContinuationRevision;
    const auto geometry = window->moveResizeGeometry();
    QSignalSpy changed(window, &Window::maximizeGeometryRestoreChanged);
    auto result = window->setGeometryRestoreForAssignment(desired, *before);
    auto expected = Window::AssignmentStepOutcome::Superseded;
    if (scenario == 3) {
        expected = Window::AssignmentStepOutcome::Refused;
    } else if (scenario == 4 || scenario == 5) {
        expected = Window::AssignmentStepOutcome::AppliedCurrent;
    }
    QCOMPARE(result.outcome, expected);
    QCOMPARE(bool(result.checkpoint), scenario == 4 || scenario == 5);
    if (result.checkpoint) {
        QVERIFY(result.checkpoint->isCurrentFor(*window));
    }
    QCOMPARE(window->geometryRestore(), scenario == 4 ? desired : original);
    QCOMPARE(window->m_placementContinuationRevision, revision + (scenario == 4 ? 1 : 0));
    QCOMPARE(changed.count(), scenario == 4 ? 1 : 0);
    QCOMPARE(window->moveResizeGeometry(), geometry);
    if (scenario == 2) {
        before.reset();
        QTRY_VERIFY(!guard);
    }
}

void TestXdgShellWindow::testGuardedGeometryRestoreReentry_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("replacement") << 0;
    QTest::newRow("roundtrip") << 1;
    QTest::newRow("same-value") << 2;
    QTest::newRow("equal-geometry") << 3;
    QTest::newRow("cancel") << 4;
    QTest::newRow("commit") << 5;
    QTest::newRow("close") << 6;
    QTest::newRow("input-owner-close") << 7;
    QTest::newRow("input-owner-only") << 8;
    QTest::newRow("caller-value-change") << 9;
}

void TestXdgShellWindow::testGuardedGeometryRestoreReentry()
{
    QFETCH(int, action);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    auto tile = std::make_unique<Tile>(workspace()->tileManager(window->moveResizeOutput()));
    tile->setRelativeGeometry(RectF(0, 0, 0.5, 1));
    const RectF requested(30, 40, 240, 160);
    const RectF replacement(70, 80, 260, 180);
    RectF callerValue = requested;
    std::unique_ptr<TileAssignmentFence> before(new TileAssignmentFence(window->makeTileAssignmentFence()));
    QPointer<Window> guard(window);
    QSignalSpy changed(window, &Window::maximizeGeometryRestoreChanged);
    QObject context;
    bool called = false;
    connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
        if (called) {
            return;
        }
        called = true;
        switch (action) {
        case 0:
            window->setGeometryRestore(replacement);
            break;
        case 1:
            window->setGeometryRestore(replacement);
            window->setGeometryRestore(requested);
            break;
        case 2:
            window->setGeometryRestore(requested);
            break;
        case 3:
            window->moveResize(window->moveResizeGeometry());
            break;
        case 4:
            window->setTileCompatibility(nullptr);
            break;
        case 5:
            window->commitTile(tile.get());
            break;
        case 7:
            before.reset();
            [[fallthrough]];
        case 6:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            QVERIFY(guard);
            break;
        case 8:
            before.reset();
            break;
        case 9:
            callerValue = replacement;
            break;
        }
    });
    auto result = window->setGeometryRestoreForAssignment(callerValue, *before);
    QVERIFY(called);
    const bool current = action == 2 || action == 8 || action == 9;
    QCOMPARE(result.outcome, current ? Window::AssignmentStepOutcome::AppliedCurrent : Window::AssignmentStepOutcome::Superseded);
    QCOMPARE(bool(result.checkpoint), current);
    if (action == 6 || action == 7) {
        if (action == 6) {
            QVERIFY(guard && window->isDeleted());
            before.reset();
        }
        QTRY_VERIFY(!guard);
        return;
    }
    QVERIFY(guard);
    QCOMPARE(window->geometryRestore(), action == 0 ? replacement : requested);
    if (result.checkpoint) {
        QVERIFY(result.checkpoint->isCurrentFor(*window));
    }
    if (action == 9) {
        QCOMPARE(callerValue, replacement);
    }
    int notifications = 1;
    if (action == 0) {
        notifications = 2;
    } else if (action == 1) {
        notifications = 3;
    }
    QCOMPARE(changed.count(), notifications);
}

void TestXdgShellWindow::testGuardedGeometryRestoreSentinels()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    const auto geometry = window->moveResizeGeometry();
    const auto frame = window->frameGeometry();
    for (const RectF &desired : {RectF(), RectF(1, 2, -3, 4), RectF(5, 6, 0, 0)}) {
        const bool changed = window->geometryRestore() != desired;
        const auto revision = window->m_placementContinuationRevision;
        auto before = window->makeTileAssignmentFence();
        QSignalSpy notification(window, &Window::maximizeGeometryRestoreChanged);
        auto result = window->setGeometryRestoreForAssignment(desired, before);
        QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
        QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
        QCOMPARE(window->geometryRestore(), desired);
        QCOMPARE(window->m_placementContinuationRevision, revision + (changed ? 1 : 0));
        QCOMPARE(notification.count(), changed ? 1 : 0);
        QCOMPARE(window->moveResizeGeometry(), geometry);
        QCOMPARE(window->frameGeometry(), frame);
    }
}

void TestXdgShellWindow::testGeometryRestorePreservesOwnership()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    shell->set_app_id(QStringLiteral("org.example.restore-field"));
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(window);
    QVERIFY(!configure.isEmpty() || configure.wait());
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(200, 100), Qt::red);
    QVERIFY(Test::waylandSync());
    const QRectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.restore-field");
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto manager = workspace()->tileManager(window->moveResizeOutput());
    auto desktop = VirtualDesktopManager::self()->currentDesktop(window->moveResizeOutput());
    QCOMPARE(controller.createTree(manager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
    const auto state = writer.snapshot();
    QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
    const auto geometry = window->moveResizeGeometry();
    auto leaf = controller.tile();
    QSignalSpy requested(window, &Window::requestedGeometryChanged);
    QSignalSpy removed(leaf, &Tile::windowRemoved);
    const RectF stored(40, 50, 210, 120);
    window->setGeometryRestore(stored);
    const auto revision = window->m_placementContinuationRevision;
    window->setGeometryRestore(stored);
    QCOMPARE(window->m_placementContinuationRevision, revision);
    {
        auto before = window->makeTileAssignmentFence();
        auto result = window->setGeometryRestoreForAssignment(RectF(60, 70, 220, 130), before);
        QCOMPARE(result.outcome, Window::AssignmentStepOutcome::AppliedCurrent);
        QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    }
    QVERIFY(window->m_transientOwnership);
    QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Owned);
    QCOMPARE(QRectF(window->m_transientOwnership->floating), original);
    QCOMPARE(window->requestedTile(), leaf);
    QCOMPARE(window->moveResizeGeometry(), geometry);
    QVERIFY(writer.isReserved(id));
    QCOMPARE(writer.snapshot().entries.value(key), original);
    QVERIFY(requested.isEmpty() && removed.isEmpty());
    QPointer<Window> guard(window);
    window->setSkipCloseAnimation(true);
    window->destroyWindow();
    QTRY_VERIFY(!guard);
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
