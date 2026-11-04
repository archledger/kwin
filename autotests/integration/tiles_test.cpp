/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "atoms.h"
#include "core/output.h"
#include "pointer_input.h"
#include "rules.h"
#include "tiles/tilemanager.h"
#include "tiles/transientplacementwriter.h"
#include "tiles/transienttilecontroller.h"
#include "utils/gravity.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
#include "xdgshellwindow.h"

#if KWIN_BUILD_X11
#include "utils/xcbutils.h"
#include "x11window.h"
#include <KDecoration3/DecoratedWindow>
#include <KDecoration3/Decoration>
#include <netwm.h>
#include <xcb/xcb_icccm.h>
#endif

#include <KConfigGroup>
#include <KDecoration3/Decoration>
#include <KSharedConfig>
#include <QAbstractItemModelTester>
#include <QChildEvent>
#include <QPointer>

#include <functional>
#include <limits>

Q_DECLARE_METATYPE(KWin::QuickTileMode)
Q_DECLARE_METATYPE(KWin::MaximizeMode)

namespace KWin
{

class ChildEventFilter : public QObject
{
public:
    std::function<void(QObject *, QObject *)> childAdded;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::ChildAdded) {
            QPointer<QObject> guard(object);
            childAdded(object, static_cast<QChildEvent *>(event)->child());
            return !guard;
        }
        return false;
    }
};

class NativeTestTile : public Tile
{
public:
    explicit NativeTestTile(TileManager *manager)
        : Tile(manager)
    {
    }

    void attachForTest(Tile *child)
    {
        insertChild(0, child);
    }
};

class TransientTestTile : public Tile
{
public:
    explicit TransientTestTile(TileManager *manager, Tile *parent = nullptr)
        : Tile(manager, parent, Type::Transient)
    {
        m_desktop = VirtualDesktopManager::self()->currentDesktop(manager->output());
        m_relativeGeometry = RectF(0.1, 0.1, 0.4, 0.4);
    }

    void attachForTest(Tile *child)
    {
        insertChild(0, child);
    }

    void clearMembershipForTest()
    {
        m_windows.clear();
    }

    void seedMembershipForTest(Window *window)
    {
        m_windows.append(window);
    }
};

#if KWIN_BUILD_X11
static X11Window *createWindow(xcb_connection_t *connection, const Rect &geometry, std::function<void(xcb_window_t)> setup = {})
{
    xcb_window_t windowId = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, windowId, rootWindow(),
                      geometry.x(),
                      geometry.y(),
                      geometry.width(),
                      geometry.height(),
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);

    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof(hints));
    xcb_icccm_size_hints_set_position(&hints, 1, geometry.x(), geometry.y());
    xcb_icccm_size_hints_set_size(&hints, 1, geometry.width(), geometry.height());
    xcb_icccm_set_wm_normal_hints(connection, windowId, &hints);

    if (setup) {
        setup(windowId);
    }

    xcb_map_window(connection, windowId);
    xcb_flush(connection);

    QSignalSpy windowCreatedSpy(workspace(), &Workspace::windowAdded);
    if (!windowCreatedSpy.wait()) {
        return nullptr;
    }
    return windowCreatedSpy.last().first().value<X11Window *>();
}
#endif

class TilesTest : public QObject
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
        RequestForTest(Window *window, Tile *tile, std::shared_ptr<RequestAuthority> authority, quint64 generation = 0)
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
    void testWindowInteraction();
    void testAssignedTileDeletion();
    void resizeTileFromWindow();
    void shortcuts();
    void testPerDesktopTiles();
    void sendToOutput();
    void sendToOutputX11();
    void tileAndMaximize();
    void evacuateFromRemovedDesktop();
    void splitFromSingleTileBelowRoot();
    void requestedGeometryAfterUntileBeforeCommit();
    void requestedGeometryChangedAfterX11Tile();
    void transientPublicMutations_data();
    void transientPublicMutations();
    void transientRefusesDirectManagement();
    void compatibilityAssignmentRefusal_data();
    void compatibilityAssignmentRefusal();
    void compatibilityAssignmentAcrossDesktops_data();
    void compatibilityAssignmentAcrossDesktops();
    void compatibilityPreviousTileDestruction_data();
    void compatibilityPreviousTileDestruction();
    void transientMembershipMutations_data();
    void transientMembershipMutations();
    void membershipRevision_data();
    void membershipRevision();
    void membershipDuringTransientAdmission_data();
    void membershipDuringTransientAdmission();
    void requestTileForAssignment_data();
    void requestTileForAssignment();
    void forgetTileForAssignment_data();
    void forgetTileForAssignment();
    void publicRequestTileBoundary_data();
    void publicRequestTileBoundary();
    void manageForAssignment_data();
    void manageForAssignment();
    void unmanageForAssignment_data();
    void unmanageForAssignment();
    void publicTileEntryPoints_data();
    void publicTileEntryPoints();
    void setTileCompatibility_data();
    void setTileCompatibility();
    void x11GuardedAssignment_data();
    void x11GuardedAssignment();
    void x11NormalizeMaximizeForAssignment_data();
    void x11NormalizeMaximizeForAssignment();
    void workspacePositionDecision_data();
    void workspacePositionDecision();
    void decorationGeneration_data();
    void decorationGeneration();
    void transientChildDestruction_data();
    void transientChildDestruction();
    void transientControllerGeometry();
    void transientControllerInvalidGeometry_data();
    void transientControllerInvalidGeometry();
    void transientControllerRevocation_data();
    void transientControllerRevocation();
    void transientControllerShutdown();
    void transientControllerConstructionContext_data();
    void transientControllerConstructionContext();
    void transientControllerLeafThenRootDeletion();
    void transientControllerCreationReentry();
    void transientControllerUnpublishedRootCleanup();
    void transientControllerIgnoresRetiredNotification();
    void transientControllerRemovedDesktop_data();
    void transientControllerRemovedDesktop();

private:
    void createSimpleLayout();
    void createComplexLayout();

    LogicalOutput *m_output;
    TileManager *m_tileManager;
    CustomTile *m_rootTile;
};

void TilesTest::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    QVERIFY(waylandServer()->init(qAppName()));

    kwinApp()->start();
    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
        Rect(1280, 0, 1280, 1024),
    });
    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.count(), 2);
    QCOMPARE(outputs[0]->geometry(), Rect(0, 0, 1280, 1024));
    QCOMPARE(outputs[1]->geometry(), Rect(1280, 0, 1280, 1024));
    setenv("QT_QPA_PLATFORM", "wayland", true);
}

void TilesTest::init()
{
    QVERIFY(Test::setupWaylandConnection());

    workspace()->setActiveOutput(QPoint(640, 512));
    input()->pointer()->warp(QPoint(640, 512));
    m_output = workspace()->activeOutput();
    m_tileManager = workspace()->tileManager(m_output);
    m_rootTile = workspace()->rootTile(m_output);
    QVERIFY(!m_rootTile->isTransient());
    QAbstractItemModelTester(m_tileManager->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);

    VirtualDesktopManager::self()->setCount(3);
    VirtualDesktopManager::self()->setCurrent(1);
    createSimpleLayout();
}

void TilesTest::cleanup()
{
    Test::destroyWaylandConnection();
}

void TilesTest::transientPublicMutations_data()
{
    QTest::addColumn<int>("operation");
    QTest::newRow("relative-property") << 0;
    QTest::newRow("padding-property") << 1;
    QTest::newRow("minimum-property") << 2;
    QTest::newRow("quick-mode") << 3;
    QTest::newRow("absolute-geometry") << 4;
    QTest::newRow("window-geometry") << 5;
    QTest::newRow("pixel-resize") << 6;
    QTest::newRow("gravity-resize") << 7;
    QTest::newRow("create-child") << 8;
    QTest::newRow("destroy-child") << 9;
}

void TilesTest::transientPublicMutations()
{
    QFETCH(int, operation);
    auto root = std::make_unique<TransientTestTile>(m_tileManager);
    auto tile = new TransientTestTile(m_tileManager, root.get());
    root->attachForTest(tile);
    QVERIFY(tile->isTransient());
    QVERIFY(!tile->canBeRemoved());
    QVERIFY(!tile->supportsResizeGravity(Gravity::BottomRight));
    const auto geometry = tile->relativeGeometry();
    const auto padding = tile->padding();
    const auto minimum = tile->minimumSize();
    const auto mode = tile->quickTileMode();
    QCOMPARE(mode, QuickTileMode(QuickTileFlag::Custom));
    QSignalSpy geometryChanged(tile, &Tile::relativeGeometryChanged);
    QSignalSpy absoluteChanged(tile, &Tile::absoluteGeometryChanged);
    QSignalSpy windowGeometryChanged(tile, &Tile::windowGeometryChanged);
    QSignalSpy paddingChanged(tile, &Tile::paddingChanged);
    QSignalSpy minimumChanged(tile, &Tile::minimumSizeChanged);
    QSignalSpy childrenChanged(tile, &Tile::childTilesChanged);
    QSignalSpy rootChildrenChanged(root.get(), &Tile::childTilesChanged);
    QSignalSpy layoutChanged(tile, &Tile::isLayoutChanged);
    switch (operation) {
    case 0:
        QVERIFY(tile->setProperty("relativeGeometry", QVariant::fromValue(RectF(0.2, 0.2, 0.5, 0.5))));
        break;
    case 1:
        QVERIFY(tile->setProperty("padding", 24.0));
        break;
    case 2:
        QVERIFY(tile->setProperty("minimumSize", QSizeF(0.6, 0.6)));
        break;
    case 3:
        tile->setQuickTileMode(QuickTileFlag::Right);
        break;
    case 4:
        tile->setGeometryFromAbsolute(RectF(10, 20, 600, 500));
        break;
    case 5:
        tile->setGeometryFromWindow(RectF(10, 20, 600, 500));
        break;
    case 6:
        tile->resizeByPixels(100, Qt::RightEdge);
        break;
    case 7:
        tile->resizeFromGravity(Gravity::BottomRight, 900, 700);
        break;
    case 8: {
        std::unique_ptr<TransientTestTile> created(tile->createChildAt<TransientTestTile>(RectF(0.1, 0.1, 0.2, 0.2), 0));
        QVERIFY(!created);
        break;
    }
    case 9: {
        QPointer<TransientTestTile> guard(tile);
        root->destroyChild(tile);
        QVERIFY(guard);
        break;
    }
    }
    QCOMPARE(tile->relativeGeometry(), geometry);
    QCOMPARE(tile->padding(), padding);
    QCOMPARE(tile->minimumSize(), minimum);
    QCOMPARE(tile->quickTileMode(), mode);
    QCOMPARE(tile->childCount(), 0);
    QCOMPARE(geometryChanged.count(), 0);
    QCOMPARE(absoluteChanged.count(), 0);
    QCOMPARE(windowGeometryChanged.count(), 0);
    QCOMPARE(paddingChanged.count(), 0);
    QCOMPARE(minimumChanged.count(), 0);
    QCOMPARE(childrenChanged.count(), 0);
    QCOMPARE(rootChildrenChanged.count(), 0);
    QCOMPARE(layoutChanged.count(), 0);
}

void TilesTest::transientRefusesDirectManagement()
{
    TransientTestTile tile(m_tileManager);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    QVERIFY(window);
    auto manual = workspace()->rootTile(m_output)->childTile(0);
    QVERIFY(manual->manage(window));
    QVERIFY(!manual->isTransient());
    const auto manualWindows = manual->windows();
    const auto requestedGeometry = window->moveResizeGeometry();
    QSignalSpy added(&tile, &Tile::windowAdded);
    QSignalSpy transientWindowsChanged(&tile, &Tile::windowsChanged);
    QSignalSpy manualWindowsChanged(manual, &Tile::windowsChanged);
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    const bool accepted = tile.manage(window);
    const bool keptManual = window->requestedTile() == manual;
    const bool keptMembership = manual->windows() == manualWindows;
    const bool keptGeometry = window->moveResizeGeometry() == requestedGeometry;
    const bool empty = tile.windows().isEmpty();
    const bool silent = added.isEmpty() && transientWindowsChanged.isEmpty() && manualWindowsChanged.isEmpty() && requestedTileChanged.isEmpty();
    // Clean a failed control without leaving a stale test-only membership.
    tile.clearMembershipForTest();
    window->requestTile(nullptr);
    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
    QVERIFY(!accepted);
    QVERIFY(keptManual);
    QVERIFY(keptMembership);
    QVERIFY(keptGeometry);
    QVERIFY(empty);
    QVERIFY(silent);
}

void TilesTest::compatibilityAssignmentRefusal_data()
{
    QTest::addColumn<bool>("propertyWrite");
    QTest::addColumn<bool>("x11");
    QTest::newRow("wayland-direct") << false << false;
    QTest::newRow("wayland-property") << true << false;
#if KWIN_BUILD_X11
    QTest::newRow("x11-direct") << false << true;
    QTest::newRow("x11-property") << true << true;
#endif
}

void TilesTest::compatibilityAssignmentRefusal()
{
    QFETCH(bool, propertyWrite);
    QFETCH(bool, x11);
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
#if KWIN_BUILD_X11
    Test::XcbConnectionPtr connection;
#endif
    Window *window = nullptr;
    if (x11) {
#if KWIN_BUILD_X11
        connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        window = createWindow(connection.get(), Rect(0, 0, 430, 310));
#endif
    } else {
        surface = Test::createSurface();
        shell = Test::createXdgToplevelSurface(surface.get());
        window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    }
    QVERIFY(window);
    auto previous = m_rootTile->childTile(0);
    auto destination = m_tileManager->rootTile(VirtualDesktopManager::self()->desktops()[1])->childTile(0);
    QVERIFY(previous && destination);
    QVERIFY(!window->isOnDesktop(destination->desktop()));
    QVERIFY(previous->manage(window));
    const auto requestedGeometry = window->moveResizeGeometry();
    const auto committedGeometry = window->frameGeometry();
    const auto restoreGeometry = window->geometryRestore();
    const auto committedTile = window->tile();
    QSignalSpy previousWindows(previous, &Tile::windowsChanged);
    QSignalSpy destinationWindows(destination, &Tile::windowsChanged);
    QSignalSpy requestedChanged(window, &Window::requestedTileChanged);
    QSignalSpy committedChanged(window, &Window::tileChanged);

    if (propertyWrite) {
        QVERIFY(window->setProperty("tile", QVariant::fromValue(destination)));
    } else {
        window->setTileCompatibility(destination);
    }

    QCOMPARE(window->requestedTile(), previous);
    QCOMPARE(window->tile(), committedTile);
    QCOMPARE(previous->windows(), QList<Window *>{window});
    QVERIFY(destination->windows().isEmpty());
    QCOMPARE(window->moveResizeGeometry(), requestedGeometry);
    QCOMPARE(window->frameGeometry(), committedGeometry);
    QCOMPARE(window->geometryRestore(), restoreGeometry);
    QVERIFY(previousWindows.isEmpty() && destinationWindows.isEmpty());
    QVERIFY(requestedChanged.isEmpty() && committedChanged.isEmpty());
}

void TilesTest::compatibilityAssignmentAcrossDesktops_data()
{
    QTest::addColumn<bool>("propertyWrite");
    QTest::newRow("direct") << false;
    QTest::newRow("property") << true;
}

void TilesTest::compatibilityAssignmentAcrossDesktops()
{
    QFETCH(bool, propertyWrite);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shell(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    QVERIFY(window);
    window->setOnAllDesktops(true);
    auto previous = m_rootTile->childTile(0);
    auto inactive = m_tileManager->rootTile(VirtualDesktopManager::self()->desktops()[1])->childTile(0);
    auto active = m_rootTile->childTile(1);
    QVERIFY(previous && inactive && active);
    QVERIFY(previous->manage(window));
    const auto assign = [&](Tile *tile) {
        if (propertyWrite) {
            return window->setProperty("tile", QVariant::fromValue(tile));
        }
        window->setTileCompatibility(tile);
        return true;
    };

    QVERIFY(assign(inactive));
    QVERIFY(previous->windows().isEmpty());
    QCOMPARE(inactive->windows(), QList<Window *>{window});
    QVERIFY(!window->requestedTile());
    QVERIFY(assign(active));
    QCOMPARE(window->requestedTile(), active);
    QCOMPARE(active->windows(), QList<Window *>{window});
    QCOMPARE(inactive->windows(), QList<Window *>{window});
    QVERIFY(assign(nullptr));
    QVERIFY(!window->requestedTile());
    QVERIFY(active->windows().isEmpty());
    QCOMPARE(inactive->windows(), QList<Window *>{window});
}

void TilesTest::compatibilityPreviousTileDestruction_data()
{
    QTest::addColumn<bool>("propertyWrite");
    QTest::addColumn<bool>("x11");
    QTest::newRow("wayland-direct") << false << false;
    QTest::newRow("wayland-property") << true << false;
#if KWIN_BUILD_X11
    QTest::newRow("x11-direct") << false << true;
    QTest::newRow("x11-property") << true << true;
#endif
}

void TilesTest::compatibilityPreviousTileDestruction()
{
    QFETCH(bool, propertyWrite);
    QFETCH(bool, x11);
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
#if KWIN_BUILD_X11
    Test::XcbConnectionPtr connection;
#endif
    Window *window = nullptr;
    if (x11) {
#if KWIN_BUILD_X11
        connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        window = createWindow(connection.get(), Rect(0, 0, 430, 310));
#endif
    } else {
        surface = Test::createSurface();
        shell = Test::createXdgToplevelSurface(surface.get());
        window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    }
    QVERIFY(window);
    QPointer<Tile> previous(m_rootTile->childTile(0));
    QPointer<Tile> target(m_rootTile->childTile(1));
    QVERIFY(previous && target);
    QVERIFY(previous->manage(window));
    const RectF requestedBefore = window->moveResizeGeometry();
    QSignalSpy added(target, &Tile::windowAdded);
    QObject context;
    bool destroyedAfterEvacuation = false;
    connect(target, &Tile::windowAdded, &context, [&](Window *addedWindow) {
        QCOMPARE(addedWindow, window);
        QVERIFY(previous);
        QVERIFY(!previous->windows().contains(window));
        destroyedAfterEvacuation = true;
        delete previous.data();
    });

    if (propertyWrite) {
        QVERIFY(window->setProperty("tile", QVariant::fromValue(target.data())));
    } else {
        window->setTileCompatibility(target.data());
    }

    QVERIFY(destroyedAfterEvacuation);
    QVERIFY(!previous);
    QVERIFY(target);
    QCOMPARE(added.count(), 1);
    QCOMPARE(target->windows(), QList<Window *>{window});
    // The guarded ingress is superseded on both backends: the window's requested tile was
    // destroyed after its evacuation, so the association vanished without a stamp (the fence
    // observes its presence). The membership stands, no association is published over it, the
    // geometry is untouched, and the setter treats the refused destination as not releasing
    // anything.
    QVERIFY(!window->requestedTile());
    QCOMPARE(window->moveResizeGeometry(), requestedBefore);
    if (x11) {
        QVERIFY(!window->tile());
    }
}

void TilesTest::transientMembershipMutations_data()
{
    QTest::addColumn<int>("operation");
    QTest::newRow("manage-existing") << 0;
    QTest::newRow("unmanage") << 1;
    QTest::newRow("forget") << 2;
}

void TilesTest::transientMembershipMutations()
{
    QFETCH(int, operation);
    TransientTestTile tile(m_tileManager);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    QVERIFY(window);
    tile.seedMembershipForTest(window);
    QSignalSpy removed(&tile, &Tile::windowRemoved);
    QSignalSpy windowsChanged(&tile, &Tile::windowsChanged);
    bool accepted = false;
    if (operation == 0) {
        accepted = tile.manage(window);
    } else if (operation == 1) {
        accepted = tile.unmanage(window);
    } else {
        tile.forget(window);
    }
    const bool retained = tile.windows() == QList<Window *>{window};
    // The controller is not installed in this unit; clear the test-only membership explicitly.
    tile.clearMembershipForTest();
    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(window));
    QVERIFY(!accepted);
    QVERIFY(retained);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(windowsChanged.count(), 0);
}

void TilesTest::membershipRevision_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("manage-inactive-desktop-member") << 0;
    QTest::newRow("manage-existing-member") << 1;
    QTest::newRow("unmanage-member") << 2;
    QTest::newRow("unmanage-non-member") << 3;
    QTest::newRow("forget-non-member") << 4;
    QTest::newRow("manage-refused-transient") << 5;
    QTest::newRow("manage-refused-off-desktop") << 6;
    QTest::newRow("evacuation-miss-single-stamp") << 7;
    QTest::newRow("evacuation-moves-membership") << 8;
    QTest::newRow("saturates") << 9;
    QTest::newRow("exhausted-fence") << 10;
    QTest::newRow("unmanage-null") << 11;
    QTest::newRow("forget-null") << 12;
}

void TilesTest::membershipRevision()
{
    // Every actual membership change and every otherwise-valid public membership no-op advances the
    // window's membership revision before any notification, so a fence taken earlier is no longer
    // current; refusals and internal evacuation misses record nothing.
    QFETCH(int, scenario);
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    QVERIFY(window);
    const auto maximum = std::numeric_limits<quint64>::max();
    auto inactiveDesktop = VirtualDesktopManager::self()->desktops().constLast();
    QVERIFY(inactiveDesktop != VirtualDesktopManager::self()->currentDesktop(m_output));
    Tile *inactive = m_tileManager->quickRootTile(inactiveDesktop)->tileForMode(QuickTileFlag::Left);
    QVERIFY(inactive && !inactive->isActive());
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second && first->isActive());
    if (scenario == 0) {
        window->setOnAllDesktops(true);
    }
    if (scenario == 1 || scenario == 2 || scenario == 8) {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
    }
    if (scenario == 9) {
        window->m_tileMembershipChange = maximum - 1;
    } else if (scenario == 10) {
        window->m_tileMembershipChange = maximum;
    }
    const auto membership = window->m_tileMembershipChange;
    const auto requested = window->m_requestedTileChange;
    const auto committed = window->m_committedTileChange;
    const auto placement = window->m_placementContinuationRevision;
    const auto geometry = window->m_geometryRequestGeneration;
    QSignalSpy addedFirst(first, &Tile::windowAdded);
    QSignalSpy removedFirst(first, &Tile::windowRemoved);
    QSignalSpy changedFirst(first, &Tile::windowsChanged);
    auto fence = window->makeTileAssignmentFence();
    QCOMPARE(fence.isCurrentFor(*window), scenario != 10);
    // The stamp precedes the notification: inside the membership callbacks the fence is already stale.
    bool staleInsideCallbacks = false;
    QObject context;
    if (scenario == 7 || scenario == 8) {
        staleInsideCallbacks = true;
        connect(second, &Tile::windowAdded, &context, [&]() {
            staleInsideCallbacks = staleInsideCallbacks && !fence.isCurrentFor(*window);
        });
        connect(first, &Tile::windowRemoved, &context, [&]() {
            staleInsideCallbacks = staleInsideCallbacks && !fence.isCurrentFor(*window);
        });
    }
    bool accepted = false;
    switch (scenario) {
    case 0:
        // A membership on another desktop is added without any request: only the revision sees it.
        accepted = inactive->manage(window);
        QVERIFY(accepted);
        QVERIFY(inactive->windows().contains(window));
        QVERIFY(!window->requestedTile());
        break;
    case 1:
        accepted = first->manage(window);
        QVERIFY(accepted);
        QCOMPARE(addedFirst.count(), 0);
        QCOMPARE(changedFirst.count(), 0);
        break;
    case 2:
        accepted = first->unmanage(window);
        QVERIFY(accepted);
        QCOMPARE(removedFirst.count(), 1);
        QVERIFY(!window->requestedTile());
        break;
    case 3:
        accepted = first->unmanage(window);
        QVERIFY(!accepted);
        QCOMPARE(removedFirst.count(), 0);
        QCOMPARE(changedFirst.count(), 0);
        break;
    case 4:
        first->forget(window);
        QCOMPARE(removedFirst.count(), 0);
        QCOMPARE(changedFirst.count(), 0);
        break;
    case 5: {
        TransientTestTile transient(m_tileManager);
        accepted = transient.manage(window);
        QVERIFY(!accepted);
        break;
    }
    case 6:
        QVERIFY(!window->isOnDesktop(inactiveDesktop));
        accepted = inactive->manage(window);
        QVERIFY(!accepted);
        QVERIFY(!inactive->windows().contains(window));
        break;
    case 7:
        // The evacuation visits every tile of the desktop and finds nothing: one stamp, the add's.
        accepted = second->manage(window);
        QVERIFY(accepted);
        QCOMPARE(window->requestedTile(), second);
        break;
    case 8:
        // The evacuation removes the membership from the first tile, then the second adds it.
        accepted = second->manage(window);
        QVERIFY(accepted);
        QCOMPARE(removedFirst.count(), 1);
        QVERIFY(!first->windows().contains(window));
        QVERIFY(second->windows().contains(window));
        break;
    case 9:
    case 10:
        accepted = first->unmanage(window);
        QVERIFY(!accepted);
        break;
    case 11:
        // A null argument (reachable from scripting) stays a plain miss: nothing recorded, nothing notified.
        accepted = first->unmanage(nullptr);
        QVERIFY(!accepted);
        QCOMPARE(removedFirst.count(), 0);
        QCOMPARE(changedFirst.count(), 0);
        break;
    case 12:
        first->forget(nullptr);
        QCOMPARE(removedFirst.count(), 0);
        QCOMPARE(changedFirst.count(), 0);
        break;
    }
    quint64 expected = membership;
    switch (scenario) {
    case 0:
    case 1:
    case 3:
    case 4:
    case 7:
        expected = membership + 1;
        break;
    case 2:
        expected = membership + 1;
        break;
    case 8:
        expected = membership + 2;
        break;
    case 9:
    case 10:
        expected = maximum;
        break;
    }
    QCOMPARE(window->m_tileMembershipChange, expected);
    QCOMPARE(fence.isCurrentFor(*window), scenario == 5 || scenario == 6 || scenario >= 11);
    if (scenario == 7 || scenario == 8) {
        // The ordering was observed inside the notifications themselves.
        QVERIFY(staleInsideCallbacks);
    }
    if (scenario == 0 || scenario == 1 || scenario == 3 || scenario == 4 || scenario == 5 || scenario == 6 || scenario >= 11) {
        // The membership revision alone changed (or nothing did): the other counters are untouched,
        // except that a new membership through the public entry point carries that operation's
        // own stamp (the qualified cancellation) even on an inactive desktop; the existing-member,
        // miss, refusal and null paths stamp membership only.
        QCOMPARE(window->m_requestedTileChange, requested);
        QCOMPARE(window->m_committedTileChange, committed);
        QCOMPARE(window->m_placementContinuationRevision, scenario == 0 ? placement + 1 : placement);
        QCOMPARE(window->m_geometryRequestGeneration, geometry);
    }
    if (scenario == 9 || scenario == 10) {
        // Exhaustion fails closed: no later fence of this window is current again.
        auto later = window->makeTileAssignmentFence();
        QVERIFY(!later.isCurrentFor(*window));
    }
    if (scenario == 0) {
        QVERIFY(inactive->unmanage(window));
    }
}

void TilesTest::membershipDuringTransientAdmission_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("control") << 0;
    QTest::newRow("unmanage-no-op-at-window-added") << 1;
    QTest::newRow("forget-no-op-at-windows-changed") << 2;
    QTest::newRow("foreign-manage-at-window-added") << 3;
    QTest::newRow("exhausted-membership") << 4;
}

void TilesTest::membershipDuringTransientAdmission()
{
    // The transient membership is published before the admission takes any fence: a membership
    // change or an otherwise-valid public no-op made by a publication callback is newer intent the
    // admission must not absorb.
    QFETCH(int, scenario);
    using Result = TransientTileController::Result;
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    shellSurface->set_app_id(QStringLiteral("org.example.membership"));
    QSignalSpy configure(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    QVERIFY(window);
    // Settle the configure the activation scheduled: admission refuses pending native geometry.
    QVERIFY(!configure.isEmpty() || configure.wait());
    shellSurface->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(430, 310), Qt::blue);
    QVERIFY(Test::waylandSync());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto id = window->internalId();
    const auto key = QStringLiteral("org.example.membership");
    const QRectF original = window->frameGeometry();
    QVERIFY(writer.trackWindow(id, key));
    QCOMPARE(writer.record(token, 1, {id, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    TransientTileController controller;
    auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
    QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    Tile *leaf = controller.tile();
    Tile *first = m_rootTile->childTile(0);
    QVERIFY(leaf && first);
    // Admission readiness, the production check itself, before any intervention is installed.
    QVERIFY(window->canStartTransientTileRequest(leaf->windowGeometry()));
    const auto maximum = std::numeric_limits<quint64>::max();
    if (scenario == 4) {
        // The transient add saturates the revision: every later fence of this window is exhausted.
        window->m_tileMembershipChange = maximum - 1;
    }
    QSignalSpy leafAdded(leaf, &Tile::windowAdded);
    QSignalSpy leafRemoved(leaf, &Tile::windowRemoved);
    QSignalSpy leafChanged(leaf, &Tile::windowsChanged);
    QObject context;
    bool intervened = false;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (scenario) {
        case 1:
            QVERIFY(!first->unmanage(window));
            break;
        case 2:
            first->forget(window);
            break;
        case 3:
            QVERIFY(first->manage(window));
            break;
        }
    };
    if (scenario == 1 || scenario == 3) {
        connect(leaf, &Tile::windowAdded, &context, intervene);
    } else if (scenario == 2) {
        connect(leaf, &Tile::windowsChanged, &context, intervene);
    }
    const auto state = writer.snapshot();
    const auto membership = window->m_tileMembershipChange;
    const auto admission = controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision);
    QCOMPARE(intervened, scenario >= 1 && scenario <= 3);
    QCOMPARE(leafAdded.count(), 1);
    if (scenario == 0) {
        QCOMPARE(admission, Result::Accepted);
        QVERIFY(window->ownedTransientPlacementGeneration());
        QCOMPARE(window->requestedTile(), leaf);
        QVERIFY(leaf->windows().contains(window));
        // The transient membership itself was recorded once and published once.
        QCOMPARE(window->m_tileMembershipChange, membership + 1);
        QCOMPARE(leafChanged.count(), 1);
        QCOMPARE(leafRemoved.count(), 0);
        return;
    }
    QCOMPARE(admission, Result::Superseded);
    QCOMPARE(window->ownedTransientPlacementGeneration(), 0u);
    QVERIFY(!leaf->windows().contains(window));
    // The release publishes its removal once; a superseded admission publishes nothing further
    // after the boundary that superseded it (row 2 intervenes at the Owned-phase windowsChanged
    // itself, row 4 is superseded only by its exhausted fences after a complete publication).
    QCOMPARE(leafRemoved.count(), 1);
    QCOMPARE(leafChanged.count(), scenario == 2 || scenario == 4 ? 2 : 1);
    if (scenario == 4) {
        QCOMPARE(window->m_tileMembershipChange, maximum);
        QVERIFY(!window->requestedTile());
        return;
    }
    // The transient add, the handler's stamp and the release's transient removal.
    QCOMPARE(window->m_tileMembershipChange, membership + 3);
    if (scenario == 3) {
        // The handler's management stands.
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(first->windows().contains(window));
    } else {
        QVERIFY(!window->requestedTile());
        QVERIFY(!first->windows().contains(window));
    }
}

namespace
{

// A transient tile a reservation can name, as in the Xdg suite: the request's target predicate
// requires a transient tile on the current desktop whose geometry the window can take.
class RequestTestTile : public Tile
{
public:
    explicit RequestTestTile(Window *window)
        : Tile(workspace()->tileManager(window->moveResizeOutput()), nullptr, Type::Transient)
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

private:
    QPointer<Window> m_window;
};

// An Xdg window shown and settled: the activation's configure acknowledged and rendered, so no
// native geometry is pending when a guarded step starts.
Window *showSettledXdgWindow(std::unique_ptr<KWayland::Client::Surface> &surface, std::unique_ptr<Test::XdgToplevel> &shell, const QString &appId = {})
{
    surface = Test::createSurface();
    shell = Test::createXdgToplevelSurface(surface.get());
    if (!appId.isEmpty()) {
        shell->set_app_id(appId);
    }
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    Window *window = Test::renderAndWaitForShown(surface.get(), QSize(430, 310), Qt::blue);
    if (!window || (configure.isEmpty() && !configure.wait())) {
        return nullptr;
    }
    shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
    const QSize size = toplevelConfigure.last().at(0).value<QSize>();
    Test::render(surface.get(), size.isEmpty() ? QSize(430, 310) : size, Qt::blue);
    return Test::waylandSync() ? window : nullptr;
}

XdgToplevelInterface::States tiledStates(Qt::Edges anchors)
{
    XdgToplevelInterface::States states;
    if (anchors & Qt::LeftEdge) {
        states |= XdgToplevelInterface::State::TiledLeft;
    }
    if (anchors & Qt::RightEdge) {
        states |= XdgToplevelInterface::State::TiledRight;
    }
    if (anchors & Qt::TopEdge) {
        states |= XdgToplevelInterface::State::TiledTop;
    }
    if (anchors & Qt::BottomEdge) {
        states |= XdgToplevelInterface::State::TiledBottom;
    }
    return states;
}

constexpr XdgToplevelInterface::States allTiledStates = XdgToplevelInterface::State::TiledLeft | XdgToplevelInterface::State::TiledRight
    | XdgToplevelInterface::State::TiledTop | XdgToplevelInterface::State::TiledBottom;

}

void TilesTest::requestTileForAssignment_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("x11-applied") << 0;
    QTest::newRow("transient-tile-refused") << 1;
    QTest::newRow("owner-refused") << 2;
    QTest::newRow("interactive-refused") << 3;
    QTest::newRow("rule-forced-maximize-refused") << 4;
    QTest::newRow("marker-refused") << 5;
    QTest::newRow("headroom-placement-refused") << 6;
    QTest::newRow("headroom-association-refused") << 7;
    QTest::newRow("headroom-geometry-refused") << 8;
    QTest::newRow("equal-no-op") << 9;
    QTest::newRow("applied-from-floating") << 10;
    QTest::newRow("applied-from-maximized") << 11;
    QTest::newRow("applied-null") << 12;
    QTest::newRow("aggregate-headroom-refused") << 13;
    QTest::newRow("aggregate-boundary-applied") << 14;
    QTest::newRow("superseded-at-restore-signal") << 15;
    QTest::newRow("nested-request-at-requested-geometry") << 16;
    QTest::newRow("nested-null-at-requested-tile-changed") << 17;
    QTest::newRow("close-at-requested-geometry") << 18;
    QTest::newRow("partial-failure-published") << 19;
    QTest::newRow("target-destroyed-at-requested-geometry") << 20;
    QTest::newRow("provenance-lapsed-at-normalization") << 21;
    QTest::newRow("child-stop-after-normalization") << 22;
    QTest::newRow("equal-nested-at-requested-geometry") << 23;
    QTest::newRow("restore-changed-at-requested-tile-changed") << 24;
    QTest::newRow("stale-fence") << 25;
}

void TilesTest::requestTileForAssignment()
{
    // The guarded requested-association step: refusals without effect, operation-owned no-ops,
    // its own recorded writes and guarded child calls, the obligation of the association write
    // discharged while that write is current, and one terminal validation.
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    using Result = TransientPlacementWriter::Result;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second && first->isActive());
    if (scenario == 0) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        // The guarded request publishes the geometry synchronously and the association write's
        // obligation commits the tile as the step's own publication: applied, current, committed.
        QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
        QSignalSpy tileChanged(window, &Window::tileChanged);
        const auto committed = window->m_committedTileChange;
        const auto publication = window->m_geometryUpdateGeneration;
        QVERIFY(window->canRequestTileForAssignment(first));
        auto fence = window->makeTileAssignmentFence();
        const auto result = window->requestTileForAssignment(first, fence);
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(window->m_committedTileChange, committed + 1);
        QCOMPARE(window->m_geometryUpdateGeneration, publication + 1);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tileChanged.count(), 1);
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    KSharedConfig::Ptr rules;
    if (scenario == 4) {
        rules = KSharedConfig::openConfig(QStringLiteral("kwinrulesrc"), KConfig::SimpleConfig);
        for (const QString &group : rules->groupList()) {
            rules->deleteGroup(group);
        }
        workspace()->rulebook()->setConfig(rules);
        rules->group(QStringLiteral("General")).writeEntry("rules", QStringList{QStringLiteral("association-rule")});
        KConfigGroup group = rules->group(QStringLiteral("association-rule"));
        group.writeEntry("maximizehoriz", true);
        group.writeEntry("maximizehorizrule", int(Rules::Force));
        group.writeEntry("maximizevert", true);
        group.writeEntry("maximizevertrule", int(Rules::Force));
        group.writeEntry("wmclass", "org.example.association");
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
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell, QStringLiteral("org.example.association"));
    QVERIFY(window);
    auto *const xdg = static_cast<XdgToplevelWindow *>(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    const auto ack = [&](const QColor &color) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), color);
        QVERIFY(Test::waylandSync());
        configure.clear();
    };
    const QRectF original = window->frameGeometry();
    if (scenario == 4) {
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
    }
    // Owners: the controller admission fixture.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    TransientTileController controller;
    Tile *leaf = nullptr;
    QObject owner;
    auto authority = std::make_shared<RequestAuthority>();
    authority->owner = &owner;
    quint64 reservation = 0;
    std::unique_ptr<RequestTestTile> reservationTile;
    if (scenario == 1 || scenario == 2) {
        const auto token = writer.registerProvider(&provider);
        const auto key = QStringLiteral("org.example.association");
        QVERIFY(writer.trackWindow(window->internalId(), key));
        QCOMPARE(writer.record(token, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
        QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        QVERIFY(leaf && leaf->isTransient());
        if (scenario == 2) {
            // An owner without an association: a Prepared reservation, whose transient marker is
            // zero, so only the owner clause can refuse it.
            const auto state = writer.snapshot();
            reservationTile = std::make_unique<RequestTestTile>(window);
            reservation = window->prepareTransientOwnership(&writer, token, key, state.providerRevision, state.mapRevision,
                                                            std::make_unique<RequestForTest>(window, reservationTile.get(), authority));
            QVERIFY(reservation);
            QCOMPARE(window->m_transientOwnership->phase, Window::TransientOwnershipPhase::Prepared);
            QCOMPARE(window->m_requestedTransientTileGeneration, quint64(0));
        }
    }
    const auto reservationReset = qScopeGuard([&]() {
        if (reservation && window->m_transientOwnership) {
            window->revokeTransientOwnership(reservation);
        }
    });
    // Starting states.
    const bool maximized = scenario == 11 || scenario == 13 || scenario == 14 || scenario == 19 || scenario == 21 || scenario == 22;
    const bool tiledFirst = scenario == 9 || scenario == 12;
    QRectF maximizedFrame;
    if (maximized) {
        window->maximize(MaximizeFull);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        ack(Qt::green);
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        maximizedFrame = window->frameGeometry();
    }
    if (tiledFirst) {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        ack(Qt::cyan);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(first->windowGeometry()));
    }
    if (scenario == 3) {
        workspace()->activateWindow(window);
        workspace()->slotWindowMove();
        QVERIFY(window->isInteractiveMove());
    }
    const auto interactiveReset = qScopeGuard([&]() {
        if (scenario == 3 && window->isInteractiveMove()) {
            window->keyPressEvent(Qt::Key_Escape);
        }
    });
    switch (scenario) {
    case 5:
        window->m_requestedTransientTileGeneration = 5;
        break;
    case 6:
        window->m_placementContinuationRevision = maximum - 18;
        break;
    case 7:
        window->m_requestedTileChange = maximum - 1;
        break;
    case 8:
        window->m_geometryRequestGeneration = maximum - 2;
        break;
    case 13:
        // The normalization alone would pass its own budget; the whole path does not.
        window->m_placementContinuationRevision = maximum - 15;
        QVERIFY(window->canNormalizeMaximizeForAssignment());
        break;
    case 14:
        window->m_placementContinuationRevision = maximum - 19;
        break;
    }
    Tile *tile = first;
    if (scenario == 1) {
        tile = leaf;
    } else if (scenario == 12) {
        tile = nullptr;
    }
    const QPointer<Tile> targetGuard(tile);
    const auto placementBefore = window->m_placementContinuationRevision;
    const auto associationBefore = window->m_requestedTileChange;
    const auto modeBefore = window->requestedMaximizeMode();
    const QRectF restoreBefore = window->geometryRestore();
    const QRectF geometryBefore = window->moveResizeGeometry();
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    QSignalSpy aboutToChange(window, &Window::maximizedAboutToChange);
    QObject context;
    bool intervened = false;
    int frameAboutToChange = 0;
    bool incoming = true;
    const RectF other(40, 40, 200, 100);
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (scenario) {
        case 15:
        case 24:
            window->setGeometryRestore(other);
            break;
        case 16:
            window->requestTile(second);
            break;
        case 17:
            window->requestTile(nullptr);
            break;
        case 18:
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
            break;
        case 19:
            xdg->m_minimumSize = QSizeF(original.width() + 100, 0);
            break;
        case 20:
            // The destination deleted synchronously by its parent.
            m_rootTile->destroyChild(first);
            break;
        case 21:
            incoming = false;
            break;
        case 22:
            window->m_placementContinuationRevision = window->m_placementContinuationRevision + 1;
            break;
        case 23:
            window->requestTile(first);
            break;
        }
    };
    if (scenario == 15) {
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, intervene);
    } else if (scenario == 16 || scenario == 18 || scenario == 20 || scenario == 23) {
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    } else if (scenario == 17 || scenario == 24) {
        connect(window, &Window::requestedTileChanged, &context, intervene);
    } else if (scenario == 19) {
        // The second geometry request of the path: the association's, after the normalization's.
        connect(window, &Window::frameGeometryAboutToChange, &context, [&]() {
            if (++frameAboutToChange == 2) {
                intervene();
            }
        });
    } else if (scenario == 21) {
        connect(window, &Window::maximizedAboutToChange, &context, intervene);
    } else if (scenario == 22) {
        // The normalization's own geometry request: a foreign placement change right after it.
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    }
    Window::OwnedProvenance provenance;
    if (scenario == 21) {
        provenance = provenance.withConstraint([&incoming]() {
            return incoming;
        });
    }
    QPointer<Window> guard(window);
    // The caller's fence, owned here: the one reference the close row releases before awaiting the
    // destruction it observed logically.
    std::unique_ptr<TileAssignmentFence> fencePtr = window->makeTileAssignmentCheckpoint();
    const TileAssignmentFence &fence = *fencePtr;
    if (scenario == 25) {
        window->moveResize(RectF(10, 10, 300, 200));
        QVERIFY(!fence.isCurrentFor(*window));
    }
    const bool expectRefusal = scenario <= 8 || scenario == 13;
    // The preflight takes no fence: a stale fence is the step's supersession, not a refusal.
    QCOMPARE(window->canRequestTileForAssignment(tile), !expectRefusal);
    configure.clear();
    const auto result = window->requestTileForAssignment(tile, fence, provenance);
    QCOMPARE(intervened, scenario >= 15 && scenario <= 24);
    if (scenario == 18) {
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(guard && guard->isDeleted());
        fencePtr.reset();
        QTRY_VERIFY(!guard);
        return;
    }
    if (expectRefusal) {
        // Refused: nothing written, nothing notified, no checkpoint.
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(window->requestedMaximizeMode(), modeBefore);
        QCOMPARE(QRectF(window->geometryRestore()), restoreBefore);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(aboutToChange.count(), 0);
        if (scenario == 2) {
            QVERIFY(window->m_transientOwnership && window->m_transientOwnership->generation == reservation);
            QVERIFY(!window->requestedTile());
        } else if (scenario == 5) {
            // The legacy public path still consumes the marker.
            window->requestTile(first);
            QCOMPARE(window->requestedTile(), first);
            QCOMPARE(window->m_requestedTransientTileGeneration, quint64(0));
        }
        return;
    }
    if (scenario == 25) {
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        return;
    }
    const auto tiled = xdg->m_nextStates & allTiledStates;
    switch (scenario) {
    case 9:
        // An operation-owned no-op: nothing recorded, a fresh current checkpoint.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        break;
    case 10:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(result.owned.kind, Window::OwnedProvenance::Kind::None);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_requestedTileChange, associationBefore + 1);
        QCOMPARE(QRectF(window->geometryRestore()), original);
        QCOMPARE(restoreChanged.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(first->windowGeometry()));
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, tiledStates(first->anchors()));
        break;
    case 11:
    case 14:
        // The normalization inside: the mode restored, its receipt carried as the provenance.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(result.owned.kind, Window::OwnedProvenance::Kind::Operation);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(aboutToChange.count(), 1);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(first->windowGeometry()));
        QCOMPARE(requestedGeometry.count(), 2);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, tiledStates(first->anchors()));
        break;
    case 12:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!window->requestedTile());
        QCOMPARE(QRectF(window->moveResizeGeometry()), original);
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, XdgToplevelInterface::States());
        break;
    case 15:
        // A foreign restore write during this step's capture: superseded before the association
        // write; nothing of the association happened.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(QRectF(window->geometryRestore()), QRectF(other));
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        break;
    case 16:
        // The nested public request of another tile holds the newer intent: the outer's
        // obligation sees a newer association and skips, so only the nested publication happens.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(second->windowGeometry()));
        QCOMPARE(tiled, tiledStates(second->anchors()));
        break;
    case 17:
        // The nested public untiling inside the outer's own notification.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 2);
        QCOMPARE(tiled, XdgToplevelInterface::States());
        break;
    case 19:
        // The association's geometry request stops on the chained receipt (a limit raised at its
        // boundary), the association stands, and its obligation is still discharged.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, tiledStates(first->anchors()));
        // The request wrote its rectangle before the boundary stopped it: nothing rolls back, the
        // committed frame is still the maximized one, and only the normalization's request notified.
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(first->windowGeometry()));
        QCOMPARE(QRectF(window->frameGeometry()), maximizedFrame);
        break;
    case 20:
        // The destination destroyed inside the geometry request: no publication of an explicit
        // null, the operation ends.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!targetGuard);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 21:
        // The incoming provenance lapsed inside the normalization: nothing written at all.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        QCOMPARE(aboutToChange.count(), 1);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        break;
    case 22:
        // The normalization ran, then a foreign change ended the path: its effect stands, no
        // association, no fallback.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeRestore);
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 1);
        break;
    case 23:
        // The equal nested public request is a no-op for the association but stamps the placement:
        // the association's publication is discharged exactly once, the result is stale.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, tiledStates(first->anchors()));
        break;
    case 24:
        // The notification's own callback changed only the restore: the association stands, the
        // continuation is stale.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(QRectF(window->geometryRestore()), QRectF(other));
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    }
}

void TilesTest::forgetTileForAssignment_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("applied") << 0;
    QTest::newRow("no-op-other-tile") << 1;
    QTest::newRow("x11-applied") << 2;
    QTest::newRow("owner-refused") << 3;
    QTest::newRow("marker-refused") << 4;
    QTest::newRow("headroom-refused") << 5;
    QTest::newRow("boundary-applied") << 6;
    QTest::newRow("nested-request-at-requested-tile-changed") << 7;
    QTest::newRow("stale-fence") << 8;
}

void TilesTest::forgetTileForAssignment()
{
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    using Result = TransientPlacementWriter::Result;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second);
    if (scenario == 2) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
        QSignalSpy tileChanged(window, &Window::tileChanged);
        const auto association = window->m_requestedTileChange;
        auto fence = window->makeTileAssignmentFence();
        const auto result = window->forgetTileForAssignment(first, fence);
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
        QCOMPARE(window->m_requestedTileChange, association + 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tileChanged.count(), 1);
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell, QStringLiteral("org.example.forget"));
    QVERIFY(window);
    auto *const xdg = static_cast<XdgToplevelWindow *>(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    const QRectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    TransientTileController controller;
    Tile *tile = first;
    if (scenario == 3) {
        const auto token = writer.registerProvider(&provider);
        const auto key = QStringLiteral("org.example.forget");
        QVERIFY(writer.trackWindow(window->internalId(), key));
        QCOMPARE(writer.record(token, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
        QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        const auto state = writer.snapshot();
        QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        QVERIFY(window->ownedTransientPlacementGeneration());
        tile = controller.tile();
    } else {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::cyan);
        QVERIFY(Test::waylandSync());
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(first->windowGeometry()));
    }
    switch (scenario) {
    case 1:
        tile = second;
        break;
    case 4:
        window->m_requestedTransientTileGeneration = 5;
        break;
    case 5:
        window->m_requestedTileChange = maximum - 1;
        break;
    case 6:
        window->m_requestedTileChange = maximum - 2;
        break;
    }
    const auto associationBefore = window->m_requestedTileChange;
    const QRectF geometryBefore = window->moveResizeGeometry();
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QObject context;
    bool intervened = false;
    if (scenario == 7) {
        connect(window, &Window::requestedTileChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                window->requestTile(second);
            }
        });
    }
    auto fence = window->makeTileAssignmentFence();
    if (scenario == 8) {
        window->moveResize(RectF(10, 10, 300, 200));
        QVERIFY(!fence.isCurrentFor(*window));
    }
    const auto result = window->forgetTileForAssignment(tile, fence);
    const auto tiled = xdg->m_nextStates & allTiledStates;
    switch (scenario) {
    case 0:
    case 6:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_requestedTileChange, associationBefore + 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        QCOMPARE(tiled, XdgToplevelInterface::States());
        break;
    case 1:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 3:
    case 4:
    case 5:
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), tile);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 7:
        QVERIFY(intervened);
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(requestedTileChanged.count(), 2);
        break;
    case 8:
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        break;
    }
}

void TilesTest::publicRequestTileBoundary_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("x11-guarded") << 0;
    QTest::newRow("interactive-legacy") << 1;
    QTest::newRow("exhaustion-edge-legacy") << 2;
    QTest::newRow("reentrancy-closed") << 3;
    QTest::newRow("target-destroyed-during-cancellation") << 4;
    QTest::newRow("superseded-preparation-stops") << 5;
    QTest::newRow("marker-consumed-by-legacy") << 6;
}

void TilesTest::publicRequestTileBoundary()
{
    // The public entry point establishes its own operation: one stamp through the qualified
    // cancellation, the guarded step on a current continuation, the legacy remainder on a refusal
    // without effect, a stop on supersession; X11, interactive and exhaustion stay legacy.
    QFETCH(int, scenario);
    using Result = TransientPlacementWriter::Result;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second);
    if (scenario == 0) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
        const auto placement = window->m_placementContinuationRevision;
        window->requestTile(first);
        // The guarded sequence: the qualified cancellation, the association, the immediate commit
        // as the step's own publication, one notification.
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        QVERIFY(window->m_placementContinuationRevision > placement);
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell, QStringLiteral("org.example.boundary"));
    QVERIFY(window);
    auto *const xdg = static_cast<XdgToplevelWindow *>(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    const QRectF original = window->frameGeometry();
    const auto ack = [&](const QColor &color) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), color);
        QVERIFY(Test::waylandSync());
        configure.clear();
    };
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    TransientTileController controller;
    Tile *leaf = nullptr;
    if (scenario == 4 || scenario == 5) {
        const auto token = writer.registerProvider(&provider);
        const auto key = QStringLiteral("org.example.boundary");
        QVERIFY(writer.trackWindow(window->internalId(), key));
        QCOMPARE(writer.record(token, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
        QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        const auto state = writer.snapshot();
        QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        QVERIFY(window->ownedTransientPlacementGeneration());
        QCOMPARE(window->requestedTile(), leaf);
    }
    if (scenario == 1) {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        ack(Qt::cyan);
        workspace()->activateWindow(window);
        workspace()->slotWindowMove();
        QVERIFY(window->isInteractiveMove());
    }
    const auto interactiveReset = qScopeGuard([&]() {
        if (scenario == 1 && window->isInteractiveMove()) {
            window->keyPressEvent(Qt::Key_Escape);
        }
    });
    if (scenario == 2) {
        window->m_placementContinuationRevision = maximum - 1;
    } else if (scenario == 6) {
        window->m_requestedTransientTileGeneration = 5;
    }
    const QPointer<Tile> firstGuard(first);
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QObject context;
    bool intervened = false;
    if (scenario == 3) {
        connect(window, &Window::requestedGeometryChanged, &context, [&]() {
            if (!intervened) {
                intervened = true;
                window->requestTile(second);
            }
        });
    } else if (scenario == 4 || scenario == 5) {
        connect(leaf, &Tile::windowRemoved, &context, [&]() {
            if (!intervened) {
                intervened = true;
                if (scenario == 4) {
                    m_rootTile->destroyChild(first);
                } else {
                    QVERIFY(!second->unmanage(window));
                }
            }
        });
    }
    const auto placementBefore = window->m_placementContinuationRevision;
    configure.clear();
    window->requestTile(scenario == 1 ? nullptr : first);
    const auto tiled = xdg->m_nextStates & allTiledStates;
    switch (scenario) {
    case 1:
        // Interactive: the guarded step refuses, the legacy remainder untiles with its adjustment.
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, XdgToplevelInterface::States());
        break;
    case 2:
        // The exhaustion edge keeps the legacy cancellation (saturating) and the legacy body.
        QCOMPARE(window->m_placementContinuationRevision, maximum);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 3:
        // Reentrancy closed on the guarded path: the nested request's publication only.
        QVERIFY(intervened);
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, tiledStates(second->anchors()));
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(second->windowGeometry()));
        break;
    case 4:
        // The destination disappeared inside the cancellation's release: the entry point stops
        // after the release's own publication; nothing names an explicit null.
        QVERIFY(intervened);
        QVERIFY(!firstGuard);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 1);
        break;
    case 5:
        // A foreign stamp inside the release: the preparation is stale and the entry point stops.
        QVERIFY(intervened);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore + 1);
        break;
    case 6:
        // The marker refuses the guarded step; the legacy remainder consumes it.
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_requestedTransientTileGeneration, quint64(0));
        QCOMPARE(requestedTileChanged.count(), 1);
        QVERIFY(window->m_placementContinuationRevision > placementBefore);
        break;
    }
}

void TilesTest::manageForAssignment_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("x11-applied") << 0;
    QTest::newRow("transient-destination-refused") << 1;
    QTest::newRow("unresizable-refused") << 2;
    QTest::newRow("off-desktop-refused") << 3;
    QTest::newRow("existing-member-no-op") << 4;
    QTest::newRow("applied-active-evacuating") << 5;
    QTest::newRow("applied-active-floating") << 6;
    QTest::newRow("applied-inactive-evacuating") << 7;
    QTest::newRow("applied-inactive-no-evacuation") << 8;
    QTest::newRow("destination-destroyed-at-window-removed") << 9;
    QTest::newRow("destination-destroyed-at-window-added") << 10;
    QTest::newRow("unrelated-tile-destroyed-at-window-removed") << 11;
    QTest::newRow("holding-source-destroyed-at-window-removed") << 12;
    QTest::newRow("desktop-switched-at-window-removed") << 13;
    QTest::newRow("window-desktops-changed-at-window-added") << 14;
    QTest::newRow("geometry-changed-at-window-added") << 15;
    QTest::newRow("nested-manage-at-window-added") << 16;
    QTest::newRow("association-refused-after-mutation") << 17;
    QTest::newRow("membership-headroom-refused") << 18;
    QTest::newRow("stale-fence") << 19;
    QTest::newRow("x11-inactive-applied") << 20;
    QTest::newRow("interactive-refused-at-entry") << 21;
    QTest::newRow("desktop-switched-at-requested-geometry") << 22;
    QTest::newRow("geometry-changed-at-requested-tile-changed") << 23;
    QTest::newRow("provenance-lapsed-at-window-removed") << 24;
}

void TilesTest::manageForAssignment()
{
    // The guarded membership ingress: refusals before any mutation, operation-owned no-ops, the
    // evacuation over a worklist with the destination's context frozen and validated after every
    // notification, the addition sealed by its own checkpoint, then the association step.
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second && first->isActive());
    auto inactiveDesktop = VirtualDesktopManager::self()->desktops().constLast();
    QVERIFY(inactiveDesktop != VirtualDesktopManager::self()->currentDesktop(m_output));
    Tile *inactiveLeft = m_tileManager->quickRootTile(inactiveDesktop)->tileForMode(QuickTileFlag::Left);
    Tile *inactiveRight = m_tileManager->quickRootTile(inactiveDesktop)->tileForMode(QuickTileFlag::Right);
    QVERIFY(inactiveLeft && inactiveRight && !inactiveLeft->isActive());
    if (scenario == 0 || scenario == 20) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        // The X11 backend takes the guarded ingress: an active destination's association is
        // committed immediately as the step's own publication; an inactive one without evacuation
        // adds the membership alone.
        Tile *destination = first;
        if (scenario == 20) {
            window->setOnAllDesktops(true);
            destination = inactiveLeft;
        }
        const auto membership = window->m_tileMembershipChange;
        auto fence = window->makeTileAssignmentFence();
        const auto result = destination->manageForAssignment(window, fence);
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(destination->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membership + 1);
        if (scenario == 0) {
            QCOMPARE(window->requestedTile(), first);
            QCOMPARE(window->tile(), first);
        } else {
            QVERIFY(!window->requestedTile());
            QVERIFY(!window->tile());
        }
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell, QStringLiteral("org.example.membership"));
    QVERIFY(window);
    auto *const xdg = static_cast<XdgToplevelWindow *>(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    const auto ack = [&](const QColor &color) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), color);
        QVERIFY(Test::waylandSync());
        configure.clear();
    };
    TransientTileController controller;
    Tile *destination = first;
    Tile *leaf = nullptr;
    Tile *childA = nullptr;
    Tile *childB = nullptr;
    const auto allDesktops = scenario == 7 || scenario == 8;
    if (allDesktops) {
        window->setOnAllDesktops(true);
        QVERIFY(window->isOnDesktop(inactiveDesktop));
    }
    switch (scenario) {
    case 1: {
        auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
        QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        QVERIFY(leaf && leaf->isTransient());
        destination = leaf;
        break;
    }
    case 2:
        shell->set_min_size(430, 310);
        shell->set_max_size(430, 310);
        surface->commit(KWayland::Client::Surface::CommitFlag::None);
        QVERIFY(Test::waylandSync());
        QVERIFY(!window->isResizable());
        break;
    case 3:
        window->setDesktops({inactiveDesktop});
        QVERIFY(!window->isOnDesktop(first->desktop()));
        break;
    case 4:
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        ack(Qt::cyan);
        break;
    case 5:
    case 9:
    case 13:
    case 14:
    case 15:
    case 24:
        QVERIFY(second->manage(window));
        QCOMPARE(window->requestedTile(), second);
        ack(Qt::cyan);
        break;
    case 21:
        workspace()->activateWindow(window);
        workspace()->slotWindowMove();
        QVERIFY(window->isInteractiveMove());
        break;
    case 7:
        // A membership on the inactive desktop without an association.
        QVERIFY(inactiveRight->manage(window));
        QVERIFY(inactiveRight->windows().contains(window));
        QVERIFY(!window->requestedTile());
        destination = inactiveLeft;
        break;
    case 8:
        destination = inactiveLeft;
        break;
    case 11:
    case 12: {
        // Two children under the second root child (a split across the root's direction makes
        // children, along it a sibling): the window held property-only by the first of them
        // (row 11) or by both (row 12); the other child is destroyed during the first one's
        // removal notification. Row 12 keeps the window's centre inside the first child, so the
        // destructor's legacy re-homing of its evacuated member picks that child deterministically.
        static_cast<CustomTile *>(second)->split(CustomTile::LayoutDirection::Vertical);
        QCOMPARE(second->childCount(), 2);
        childA = second->childTile(0);
        childB = second->childTile(1);
        QVERIFY(childA && childB);
        childA->m_windows.append(window);
        window->noteTileMembershipChanged();
        if (scenario == 12) {
            childB->m_windows.append(window);
            window->noteTileMembershipChanged();
            const QPointF centre = childA->absoluteGeometry().center();
            window->moveResize(RectF(centre.x() - 215, centre.y() - 155, 430, 310));
            QVERIFY(childA->absoluteGeometry().contains(window->moveResizeGeometry().center()));
            QVERIFY(!first->absoluteGeometry().contains(window->moveResizeGeometry().center()));
        }
        break;
    }
    case 18:
        window->m_tileMembershipChange = maximum - 1;
        break;
    }
    const QPointer<Tile> destinationGuard(destination);
    const QPointer<Tile> childBGuard(childB);
    const auto membershipBefore = window->m_tileMembershipChange;
    const auto associationBefore = window->m_requestedTileChange;
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QSignalSpy firstAdded(first, &Tile::windowAdded);
    QSignalSpy firstChanged(first, &Tile::windowsChanged);
    QSignalSpy secondRemoved(second, &Tile::windowRemoved);
    QSignalSpy secondChanged(second, &Tile::windowsChanged);
    QObject context;
    bool intervened = false;
    bool incoming = true;
    const auto intervene = [&]() {
        if (intervened) {
            return;
        }
        intervened = true;
        switch (scenario) {
        case 9:
        case 10:
            m_rootTile->destroyChild(first);
            break;
        case 11:
        case 12:
            second->destroyChild(childB);
            break;
        case 13:
            VirtualDesktopManager::self()->setCurrent(2);
            break;
        case 14:
            window->setDesktops({inactiveDesktop});
            break;
        case 15:
            static_cast<CustomTile *>(first)->setRelativeGeometry(RectF(0, 0, 0.3, 1));
            break;
        case 16:
            QVERIFY(second->manage(window));
            break;
        case 17:
            workspace()->activateWindow(window);
            workspace()->slotWindowMove();
            QVERIFY(window->isInteractiveMove());
            break;
        case 22:
            VirtualDesktopManager::self()->setCurrent(2);
            break;
        case 23:
            static_cast<CustomTile *>(first)->setRelativeGeometry(RectF(0, 0, 0.3, 1));
            break;
        case 24:
            incoming = false;
            break;
        }
    };
    if (scenario == 9 || scenario == 13 || scenario == 14 || scenario == 15 || scenario == 24) {
        // Before the window is a member of the destination: the context alone must catch these.
        connect(second, &Tile::windowRemoved, &context, intervene);
    } else if (scenario == 11 || scenario == 12) {
        connect(childA, &Tile::windowRemoved, &context, intervene);
    } else if (scenario == 10 || scenario == 16 || scenario == 17) {
        connect(first, &Tile::windowAdded, &context, intervene);
    } else if (scenario == 22) {
        // Inside the association step's own callbacks: the chained context must hold there too.
        connect(window, &Window::requestedGeometryChanged, &context, intervene);
    } else if (scenario == 23) {
        connect(window, &Window::requestedTileChanged, &context, intervene);
    }
    QSignalSpy childAChanged(childA ? childA : second, &Tile::windowsChanged);
    const auto cleanup = qScopeGuard([&]() {
        if (scenario == 13 || scenario == 22) {
            VirtualDesktopManager::self()->setCurrent(1);
        }
        if ((scenario == 17 || scenario == 21) && window->isInteractiveMove()) {
            window->keyPressEvent(Qt::Key_Escape);
        }
    });
    std::unique_ptr<TileAssignmentFence> fencePtr = window->makeTileAssignmentCheckpoint();
    const TileAssignmentFence &fence = *fencePtr;
    if (scenario == 19) {
        window->moveResize(RectF(10, 10, 300, 200));
        QVERIFY(!fence.isCurrentFor(*window));
    }
    configure.clear();
    // The window's own preflight refuses a transient destination and an interactive operation;
    // resizability and the desktop are the tile step's rejections.
    QCOMPARE(window->canRequestTileForAssignment(destination), scenario != 1 && scenario != 21);
    Window::OwnedProvenance provenance;
    if (scenario == 24) {
        provenance = provenance.withConstraint([&incoming]() {
            return incoming;
        });
    }
    const auto result = destination->manageForAssignment(window, fence, provenance);
    QCOMPARE(intervened, (scenario >= 9 && scenario <= 17) || scenario >= 22);
    const auto tiled = xdg->m_nextStates & allTiledStates;
    switch (scenario) {
    case 1:
    case 2:
    case 3:
    case 18:
    case 21:
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QVERIFY(!destination->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 4:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(firstAdded.count(), 0);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 5:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!second->windows().contains(window));
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 2);
        QCOMPARE(secondRemoved.count(), 1);
        QCOMPARE(secondChanged.count(), 1);
        QCOMPARE(firstAdded.count(), 1);
        QCOMPARE(firstChanged.count(), 1);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(QRectF(window->moveResizeGeometry()), QRectF(first->windowGeometry()));
        QCOMPARE(tiled, tiledStates(first->anchors()));
        break;
    case 6:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tiled, tiledStates(first->anchors()));
        break;
    case 7:
        // Inactive desktop, evacuated: the null association is an operation-owned no-op.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!inactiveRight->windows().contains(window));
        QVERIFY(inactiveLeft->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 2);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 8:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(inactiveLeft->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        break;
    case 9:
        // The destination destroyed inside the evacuation: the removal stands, nothing is added,
        // no explicit null is published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!destinationGuard);
        QVERIFY(!second->windows().contains(window));
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 10:
        // The destination destroyed inside its own addition notification: its destructor re-homes
        // the evacuated member through the public management of the nearest leaf (the newer
        // intent); the outer step publishes nothing over it and ends stale, never an explicit null.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!destinationGuard);
        QCOMPARE(firstAdded.count(), 1);
        QCOMPARE(window->requestedTile(), second);
        QVERIFY(second->windows().contains(window));
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 11:
        // An unrelated tile deleted while the checkpoint still validates: benign.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!childBGuard);
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        break;
    case 12:
        // A not-yet-processed source holding the window was destroyed: its membership changed, the
        // operation ends without adding to the destination. The destructor's re-homing of its
        // evacuated member (the processed source, nearest to the window) is the newer intent; the
        // outer removal's second notification was not published over it, so the single one on
        // that source is the nested addition's.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!childBGuard);
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(firstAdded.count(), 0);
        QCOMPARE(window->requestedTile(), childA);
        QVERIFY(childA->windows().contains(window));
        QCOMPARE(childAChanged.count(), 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 13:
        // The planned active decision no longer holds.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 14:
    case 15:
        // The window's desktop list or the destination's geometry changed between the removal and
        // the addition: nothing is added, no association follows.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(firstAdded.count(), 0);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 16:
        // The nested public management holds the newer intent; the addition's second
        // notification was not published over it (the nested evacuation published its own).
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), second);
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(firstAdded.count(), 1);
        QCOMPARE(firstChanged.count(), 1);
        break;
    case 17:
        // The association step refused after this step's mutations: a supersession, no fallback.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 19:
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!first->windows().contains(window));
        break;
    case 22:
    case 23:
        // The frozen context is held through the association step's own callbacks: the membership
        // and the association stand (published as the association's obligation), the result is
        // stale.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 24:
        // The incoming provenance lapsed at the first removal notification: no further mutation,
        // and the removal's second notification was not published over the lapse.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(secondRemoved.count(), 1);
        QCOMPARE(secondChanged.count(), 0);
        QCOMPARE(firstAdded.count(), 0);
        break;
    }
}

void TilesTest::unmanageForAssignment_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("applied-clears-association") << 0;
    QTest::newRow("applied-membership-only") << 1;
    QTest::newRow("miss-no-op") << 2;
    QTest::newRow("x11-applied") << 3;
    QTest::newRow("nested-manage-at-window-removed") << 4;
    QTest::newRow("stale-fence") << 5;
    QTest::newRow("headroom-refused") << 6;
    QTest::newRow("forget-applied") << 7;
    QTest::newRow("forget-miss-no-op") << 8;
    QTest::newRow("forget-x11-applied") << 9;
    QTest::newRow("clears-preflight-refused") << 10;
    QTest::newRow("forget-preflight-refused") << 11;
    QTest::newRow("x11-membership-only-applied") << 12;
}

void TilesTest::unmanageForAssignment()
{
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second);
    const bool forget = scenario >= 7 && scenario <= 9 || scenario == 11;
    if (scenario == 3 || scenario == 9 || scenario == 12) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        Tile *source = first;
        if (scenario == 12) {
            // A membership without an association on an inactive desktop: the removal clears
            // nothing and publishes nothing.
            window->setOnAllDesktops(true);
            auto inactiveDesktop = VirtualDesktopManager::self()->desktops().constLast();
            source = m_tileManager->quickRootTile(inactiveDesktop)->tileForMode(QuickTileFlag::Left);
            QVERIFY(source && !source->isActive());
            QVERIFY(source->manage(window));
            QVERIFY(!window->requestedTile());
        } else {
            QVERIFY(first->manage(window));
            QCOMPARE(window->requestedTile(), first);
        }
        const auto membership = window->m_tileMembershipChange;
        QSignalSpy tileChanged(window, &Window::tileChanged);
        auto fence = window->makeTileAssignmentFence();
        const auto result = forget ? source->forgetForAssignment(window, fence) : source->unmanageForAssignment(window, fence);
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!source->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membership + 1);
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
        QCOMPARE(tileChanged.count(), scenario == 12 ? 0 : 1);
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell);
    QVERIFY(window);
    auto *const xdg = static_cast<XdgToplevelWindow *>(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    auto inactiveDesktop = VirtualDesktopManager::self()->desktops().constLast();
    Tile *inactiveLeft = m_tileManager->quickRootTile(inactiveDesktop)->tileForMode(QuickTileFlag::Left);
    QVERIFY(inactiveLeft && !inactiveLeft->isActive());
    Tile *tile = first;
    if (scenario == 1) {
        window->setOnAllDesktops(true);
        QVERIFY(inactiveLeft->manage(window));
        QVERIFY(inactiveLeft->windows().contains(window));
        QVERIFY(!window->requestedTile());
        tile = inactiveLeft;
    } else {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::cyan);
        QVERIFY(Test::waylandSync());
    }
    if (scenario == 2 || scenario == 8) {
        tile = second;
    } else if (scenario == 6) {
        window->m_tileMembershipChange = maximum - 1;
    } else if (scenario == 10 || scenario == 11) {
        window->m_requestedTransientTileGeneration = 5;
    }
    const auto membershipBefore = window->m_tileMembershipChange;
    const auto associationBefore = window->m_requestedTileChange;
    const QRectF geometryBefore = window->moveResizeGeometry();
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QSignalSpy requestedGeometry(window, &Window::requestedGeometryChanged);
    QObject context;
    bool intervened = false;
    if (scenario == 4) {
        connect(first, &Tile::windowRemoved, &context, [&]() {
            if (!intervened) {
                intervened = true;
                QVERIFY(second->manage(window));
            }
        });
    }
    auto fence = window->makeTileAssignmentFence();
    if (scenario == 5) {
        window->moveResize(RectF(10, 10, 300, 200));
        QVERIFY(!fence.isCurrentFor(*window));
    }
    const auto result = forget ? tile->forgetForAssignment(window, fence) : tile->unmanageForAssignment(window, fence);
    const auto tiled = xdg->m_nextStates & allTiledStates;
    switch (scenario) {
    case 0:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(requestedGeometry.count(), 1);
        QCOMPARE(tiled, XdgToplevelInterface::States());
        break;
    case 1:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!inactiveLeft->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QCOMPARE(requestedTileChanged.count(), 0);
        QCOMPARE(requestedGeometry.count(), 0);
        break;
    case 2:
    case 8:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 4:
        QVERIFY(intervened);
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), second);
        break;
    case 5:
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(first->windows().contains(window));
        break;
    case 6:
    case 10:
    case 11:
        // Refused before the removal: the membership and the association untouched.
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        break;
    case 7:
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(requestedGeometry.count(), 0);
        QCOMPARE(QRectF(window->moveResizeGeometry()), geometryBefore);
        QCOMPARE(tiled, XdgToplevelInterface::States());
        break;
    }
}

void TilesTest::publicTileEntryPoints_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("x11-guarded-manage") << 0;
    QTest::newRow("manage-superseded-returns-false") << 1;
    QTest::newRow("manage-owner-revoked-first") << 2;
    QTest::newRow("unmanage-superseded-returns-false") << 3;
    QTest::newRow("unmanage-miss-stamp-only") << 4;
    QTest::newRow("forget-miss-stamp-only") << 5;
    QTest::newRow("quick-tile-stops-on-superseded-preparation") << 6;
    QTest::newRow("quick-tile-applied") << 7;
    QTest::newRow("destination-destroyed-during-cancellation") << 8;
    QTest::newRow("context-changed-during-cancellation") << 9;
    QTest::newRow("closed-member-released") << 10;
}

void TilesTest::publicTileEntryPoints()
{
    // The public entry points establish their own operation after the mutation-free rejections:
    // the qualified cancellation, the guarded step, the legacy remainder on a refusal without
    // effect, a stop on supersession; misses keep their membership-only stamps.
    QFETCH(int, scenario);
    using Result = TransientPlacementWriter::Result;
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second);
    if (scenario == 0) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
        QVERIFY(first->manage(window));
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell, QStringLiteral("org.example.entry"));
    QVERIFY(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    const QRectF original = window->frameGeometry();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    TransientTileController controller;
    Tile *leaf = nullptr;
    const bool owner = scenario == 1 || scenario == 2 || scenario == 6 || scenario == 8 || scenario == 9;
    if (owner) {
        const auto token = writer.registerProvider(&provider);
        const auto key = QStringLiteral("org.example.entry");
        QVERIFY(writer.trackWindow(window->internalId(), key));
        QCOMPARE(writer.record(token, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
        QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        const auto state = writer.snapshot();
        QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        QVERIFY(window->ownedTransientPlacementGeneration());
        QCOMPARE(window->requestedTile(), leaf);
    }
    if (scenario == 3 || scenario == 10) {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), Qt::cyan);
        QVERIFY(Test::waylandSync());
    }
    const QPointer<Tile> firstGuard(first);
    const auto membershipBefore = window->m_tileMembershipChange;
    const auto placementBefore = window->m_placementContinuationRevision;
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QStringList order;
    QObject context;
    bool intervened = false;
    if (leaf) {
        connect(leaf, &Tile::windowRemoved, &context, [&]() {
            order.append(QStringLiteral("leaf-removed"));
            if (intervened) {
                return;
            }
            intervened = true;
            if (scenario == 1 || scenario == 6) {
                // A membership-only no-op inside the release: the preparation is stale.
                QVERIFY(!second->unmanage(window));
            } else if (scenario == 8) {
                m_rootTile->destroyChild(first);
            } else if (scenario == 9) {
                // The destination's geometry changed inside the release without touching the
                // window's counters: the public operation's context is stale.
                static_cast<CustomTile *>(first)->setRelativeGeometry(RectF(0, 0, 0.3, 1));
            }
        });
    }
    connect(first, &Tile::windowAdded, &context, [&]() {
        order.append(QStringLiteral("first-added"));
    });
    if (scenario == 3) {
        connect(first, &Tile::windowRemoved, &context, [&]() {
            if (!intervened) {
                intervened = true;
                QVERIFY(!second->unmanage(window));
            }
        });
    }
    auto *const quickLeft = m_tileManager->quickTile(QuickTileFlag::Left);
    QVERIFY(quickLeft);
    switch (scenario) {
    case 1:
        QVERIFY(!first->manage(window));
        QVERIFY(intervened);
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        break;
    case 2:
        // The automatic owner is revoked before the manual membership is published; the owner
        // that remains (its restoration cannot finalize) sends the step to the legacy remainder.
        QVERIFY(first->manage(window));
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(order, (QStringList{QStringLiteral("leaf-removed"), QStringLiteral("first-added")}));
        break;
    case 3:
        QVERIFY(!first->unmanage(window));
        QVERIFY(intervened);
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        break;
    case 4:
        QVERIFY(!second->unmanage(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        break;
    case 5:
        second->forget(window);
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        break;
    case 6:
        window->setQuickTileMode(QuickTileFlag::Left, QPointF(640, 512));
        QVERIFY(intervened);
        QVERIFY(!window->requestedTile());
        QVERIFY(!quickLeft->windows().contains(window));
        break;
    case 7:
        window->setQuickTileMode(QuickTileFlag::Left, QPointF(640, 512));
        QCOMPARE(window->requestedTile(), quickLeft);
        QVERIFY(quickLeft->windows().contains(window));
        break;
    case 8:
        QVERIFY(!first->manage(window));
        QVERIFY(intervened);
        QVERIFY(!firstGuard);
        QVERIFY(!window->requestedTile());
        break;
    case 9:
        QVERIFY(!first->manage(window));
        QVERIFY(intervened);
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        break;
    case 10: {
        // The window closes while a member: the workspace's removal notification releases the
        // membership through the legacy membership-only removal (nothing can continue from a
        // deleted window, whose qualified cancellation is stale by design), and a deleted window
        // is never added.
        QSignalSpy firstRemoved(first, &Tile::windowRemoved);
        connect(window, &Window::closed, &context, [&]() {
            QVERIFY(window->isDeleted());
            // Not even the existing-member fast path reports a deleted window as managed, and
            // nothing is stamped; a non-member destination adds nothing.
            const auto membership = window->m_tileMembershipChange;
            QVERIFY(first->windows().contains(window));
            QVERIFY(!first->manage(window));
            QCOMPARE(window->m_tileMembershipChange, membership);
            QVERIFY(!second->manage(window));
            QVERIFY(!second->windows().contains(window));
        });
        shell.reset();
        surface.reset();
        QVERIFY(Test::waitForWindowClosed(window));
        QCOMPARE(firstRemoved.count(), 1);
        QVERIFY(first->windows().isEmpty());
        QVERIFY(second->windows().isEmpty());
        break;
    }
    }
}

void TilesTest::setTileCompatibility_data()
{
    QTest::addColumn<int>("scenario");
    QTest::addColumn<bool>("property");
    const struct
    {
        const char *name;
        int scenario;
    } rows[] = {
        {"applied-same-desktop", 0},
        {"applied-inactive-across-desktops", 1},
        {"null-target", 2},
        {"equal-marker-cleared", 3},
        {"absent-original-with-destination", 4},
        {"null-with-absent-original", 5},
        {"context-changed-during-cancellation", 6},
        {"cleanup-headroom-refused-legacy", 7},
        {"original-readded-at-windows-changed", 8},
        {"cleanup-notification-installs-another", 9},
        {"x11-guarded", 10},
        {"interactive-refused-legacy-remainder", 11},
        {"exhaustion-edge-legacy", 12},
        {"superseded-preparation-stops", 13},
        {"target-destroyed-during-cancellation", 14},
        {"cleanup-association-preflight-refused-legacy", 15},
        {"context-changed-destination-installed-during-cancellation", 16},
        {"already-member-destination", 17},
        {"null-target-membership-exhaustion-legacy", 18},
    };
    for (const auto &row : rows) {
        QTest::addRow("%s-direct", row.name) << row.scenario << false;
        QTest::addRow("%s-property", row.name) << row.scenario << true;
    }
}

void TilesTest::setTileCompatibility()
{
    // The compatibility setter consumes the guarded ingress with its own continuation: the
    // destination's context and the original membership captured before the first callback, the
    // complete path preflighted before any guarded mutation, the ingress and the cleanup of the
    // captured original consumed on currentness; legacy only at the initial refusal, unsupported
    // backend and exhaustion boundaries. The property write's boolean means Qt accepted the write.
    QFETCH(int, scenario);
    QFETCH(bool, property);
    using Result = TransientPlacementWriter::Result;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second && first->isActive());
    Tile *inactive = m_tileManager->rootTile(VirtualDesktopManager::self()->desktops()[1])->childTile(0);
    QVERIFY(inactive && !inactive->isActive());
    const auto assign = [&](Window *window, Tile *tile) {
        if (property) {
            QVERIFY(window->setProperty("tile", QVariant::fromValue(tile)));
        } else {
            window->setTileCompatibility(tile);
        }
    };
    if (scenario == 10) {
#if KWIN_BUILD_X11
        Test::XcbConnectionPtr connection = Test::createX11Connection();
        QVERIFY(!xcb_connection_has_error(connection.get()));
        X11Window *window = createWindow(connection.get(), Rect(0, 0, 100, 50));
        QVERIFY(window);
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        const auto membership = window->m_tileMembershipChange;
        // The guarded path for X11 too: the evacuation, the addition, the association committed
        // immediately as the step's own publication, the cleanup an unstamped no-op.
        assign(window, second);
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(window->tile(), second);
        QVERIFY(second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membership + 2);
        xcb_destroy_window(connection.get(), window->window());
        xcb_flush(connection.get());
        QSignalSpy closed(window, &Window::closed);
        QVERIFY(closed.wait());
        return;
#else
        QSKIP("X11 support is not built");
#endif
    }
    std::unique_ptr<KWayland::Client::Surface> surface;
    std::unique_ptr<Test::XdgToplevel> shell;
    Window *window = showSettledXdgWindow(surface, shell, QStringLiteral("org.example.compat"));
    QVERIFY(window);
    QSignalSpy configure(shell->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigure(shell.get(), &Test::XdgToplevel::configureRequested);
    const QRectF original = window->frameGeometry();
    const auto ack = [&](const QColor &color) {
        QVERIFY(!configure.isEmpty() || configure.wait());
        shell->xdgSurface()->ack_configure(configure.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigure.last().at(0).value<QSize>(), color);
        QVERIFY(Test::waylandSync());
        configure.clear();
    };
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    TransientTileController controller;
    Tile *leaf = nullptr;
    const bool owner = scenario == 6 || scenario == 13 || scenario == 14 || scenario == 16;
    if (owner) {
        const auto token = writer.registerProvider(&provider);
        const auto key = QStringLiteral("org.example.compat");
        QVERIFY(writer.trackWindow(window->internalId(), key));
        QCOMPARE(writer.record(token, 1, {window->internalId(), original, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        auto desktop = VirtualDesktopManager::self()->currentDesktop(m_output);
        QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), TransientTileController::Result::Accepted);
        leaf = controller.tile();
        const auto state = writer.snapshot();
        QCOMPARE(controller.admit(controller.generation(), window, &writer, token, key, state.providerRevision, state.mapRevision), TransientTileController::Result::Accepted);
        QVERIFY(window->ownedTransientPlacementGeneration());
        QCOMPARE(window->requestedTile(), leaf);
    }
    const bool allDesktops = scenario == 1 || scenario == 7 || scenario == 9 || scenario == 15 || scenario == 17;
    if (allDesktops) {
        window->setOnAllDesktops(true);
    }
    const bool managedFirst = scenario == 0 || scenario == 1 || scenario == 2 || scenario == 3 || scenario == 7 || scenario == 8 || scenario == 9
        || scenario == 11 || scenario == 12 || scenario == 15 || scenario == 17 || scenario == 18;
    if (managedFirst) {
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        ack(Qt::cyan);
        if (scenario == 17) {
            // The destination already holds the window (an independent membership on another
            // desktop): the ingress is a no-op, the original is still cleaned.
            QVERIFY(inactive->manage(window));
            QVERIFY(inactive->windows().contains(window));
            QCOMPARE(window->requestedTile(), first);
        }
    } else if (scenario == 4 || scenario == 5) {
        // An association without a membership.
        window->requestTile(first);
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(!first->windows().contains(window));
        ack(Qt::cyan);
    }
    switch (scenario) {
    case 3:
    case 15:
        window->m_requestedTransientTileGeneration = 5;
        break;
    case 7:
        window->m_tileMembershipChange = maximum - 2;
        break;
    case 11:
        workspace()->activateWindow(window);
        workspace()->slotWindowMove();
        QVERIFY(window->isInteractiveMove());
        break;
    case 12:
        window->m_placementContinuationRevision = maximum - 1;
        break;
    case 18:
        window->m_tileMembershipChange = maximum - 1;
        break;
    }
    const auto interactiveReset = qScopeGuard([&]() {
        if (scenario == 11 && window->isInteractiveMove()) {
            window->keyPressEvent(Qt::Key_Escape);
        }
    });
    const QPointer<Tile> firstGuard(first);
    const auto membershipBefore = window->m_tileMembershipChange;
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QObject context;
    bool intervened = false;
    const auto once = [&](std::function<void()> action) {
        return [&, action]() {
            if (!intervened) {
                intervened = true;
                action();
            }
        };
    };
    if (leaf) {
        connect(leaf, &Tile::windowRemoved, &context, once([&]() {
            if (scenario == 6) {
                // The destination's geometry changed inside the release, without touching the
                // window's counters: the setter's captured context is stale.
                static_cast<CustomTile *>(first)->setRelativeGeometry(RectF(0, 0, 0.3, 1));
            } else if (scenario == 16) {
                // The release installs the very destination (through the legacy path, the owner
                // still restoring), changes its geometry and leaves a marker: the release's verdict
                // makes the preparation stale, so the setter acts on nothing, not even its equal
                // path. (The explicit context check behind it is not independently observable: a
                // release that leaves the counters alone is stopped by the chained provenance at
                // the ingress's entry instead.)
                QVERIFY(first->manage(window));
                QCOMPARE(window->requestedTile(), first);
                static_cast<CustomTile *>(first)->setRelativeGeometry(RectF(0, 0, 0.3, 1));
                window->m_requestedTransientTileGeneration = 5;
            } else if (scenario == 13) {
                QVERIFY(!second->unmanage(window));
            } else {
                m_rootTile->destroyChild(first);
            }
        }));
    } else if (scenario == 8) {
        // The original membership re-added, property-only with its stamp, inside the destination's
        // final notification: the ingress is stale after it.
        connect(second, &Tile::windowsChanged, &context, once([&]() {
            first->m_windows.append(window);
            window->noteTileMembershipChanged();
        }));
    } else if (scenario == 9) {
        // The cleanup's own notification installs another assignment.
        connect(first, &Tile::windowRemoved, &context, once([&]() {
            QVERIFY(second->manage(window));
        }));
    }
    Tile *destination = second;
    if (scenario == 1 || scenario == 7 || scenario == 9 || scenario == 15 || scenario == 17) {
        destination = inactive;
    } else if (scenario == 2 || scenario == 5 || scenario == 18) {
        destination = nullptr;
    } else if (scenario == 3) {
        destination = first;
    } else if (owner) {
        destination = first;
    }
    configure.clear();
    assign(window, destination);
    QCOMPARE(intervened, scenario == 6 || scenario == 8 || scenario == 9 || scenario == 13 || scenario == 14 || scenario == 16);
    switch (scenario) {
    case 0:
        // The ingress evacuated the original and requested the destination; the cleanup is an
        // operation-owned no-op without a stamp.
        QCOMPARE(window->requestedTile(), second);
        QVERIFY(second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 2);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 1:
        // An inactive destination on another desktop: the membership added without an association,
        // then the captured original cleaned with its association cleared.
        QVERIFY(inactive->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 2);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 2:
        QVERIFY(!window->requestedTile());
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 3:
        // The setter's public equal path: the marker cleared, nothing else.
        QCOMPARE(window->m_requestedTransientTileGeneration, quint64(0));
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 4:
        // No original membership: nothing cleaned, nothing stamped for it.
        QCOMPARE(window->requestedTile(), second);
        QVERIFY(second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        break;
    case 5:
        // An explicit null with no original membership keeps the association (the legacy miss did
        // not clear it either) and stamps nothing.
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_tileMembershipChange, membershipBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 6:
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        break;
    case 7:
        // The ingress alone had headroom; the whole path did not: the legacy remainder completes
        // the move at the exhaustion edge (the membership revision saturates and the association
        // fails closed), and the guarded path never left the window in both tiles.
        QVERIFY(inactive->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, maximum);
        QCOMPARE(window->requestedTile(), first);
        break;
    case 8:
        // The ingress is stale after the re-addition: no cleanup, the write stands, the
        // association untouched.
        QVERIFY(second->windows().contains(window));
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(requestedTileChanged.count(), 0);
        break;
    case 9:
        // The nested assignment installed inside the cleanup stands; the cleanup's null association
        // never runs over it.
        QVERIFY(inactive->windows().contains(window));
        QVERIFY(second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->requestedTile(), second);
        break;
    case 11:
        // The ingress refused without effect: the legacy remainder moves the window.
        QCOMPARE(window->requestedTile(), second);
        QVERIFY(second->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        break;
    case 12:
        // The legacy sequence at the exhaustion edge: its cancellation saturates and returns.
        QCOMPARE(window->m_placementContinuationRevision, maximum);
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(first->windows().contains(window));
        QVERIFY(!second->windows().contains(window));
        break;
    case 13:
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        break;
    case 14:
        QVERIFY(!firstGuard);
        QVERIFY(!window->requestedTile());
        break;
    case 15:
        // The cleanup's null association is refused by the marker: known before any guarded
        // mutation, the legacy remainder moves the window and consumes the marker.
        QVERIFY(inactive->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_requestedTransientTileGeneration, quint64(0));
        break;
    case 16:
        // The nested assignment stands (newer intent inside the release); the setter acted on
        // nothing, not even its equal path.
        QCOMPARE(window->requestedTile(), first);
        QVERIFY(first->windows().contains(window));
        QCOMPARE(window->m_requestedTransientTileGeneration, quint64(5));
        break;
    case 17:
        // The ingress was a no-op on the already-member destination; the captured original was
        // cleaned with its association cleared.
        QVERIFY(inactive->windows().contains(window));
        QVERIFY(!first->windows().contains(window));
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_tileMembershipChange, membershipBefore + 1);
        break;
    case 18:
        // No ingress runs for a null target, so the cleanup's own membership increment is the
        // preflight: known short, the legacy remainder removes the original at the exhaustion edge
        // (the revision saturates and the association fails closed).
        QVERIFY(!first->windows().contains(window));
        QCOMPARE(window->m_tileMembershipChange, maximum);
        QCOMPARE(window->requestedTile(), first);
        break;
    }
}

void TilesTest::x11NormalizeMaximizeForAssignment_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("applied-full") << 0;
    QTest::newRow("applied-vertical-captures-horizontal-axis") << 1;
    QTest::newRow("applied-horizontal-captures-vertical-axis") << 2;
    QTest::newRow("restore-no-op") << 3;
    QTest::newRow("quick-tile-skips-capture") << 4;
    QTest::newRow("invalid-restore-refused") << 5;
    QTest::newRow("stale-fence-superseded") << 6;
    QTest::newRow("lapsed-provenance-superseded") << 7;
    QTest::newRow("refused-unresizable") << 8;
    QTest::newRow("refused-fullscreen") << 9;
    QTest::newRow("refused-rule-forced") << 10;
    QTest::newRow("refused-electric-border") << 11;
    QTest::newRow("refused-headroom-revision") << 12;
    QTest::newRow("refused-placement-headroom") << 13;
    QTest::newRow("applied-full-decorated") << 14;
    QTest::newRow("applied-borderless-recreation") << 15;
    QTest::newRow("superseded-at-about-to-change-mode-write") << 16;
    QTest::newRow("superseded-at-about-to-change-constraints") << 17;
    QTest::newRow("superseded-at-restore-signal-legacy-maximize") << 18;
    QTest::newRow("superseded-nested-geometry-at-buffer-notification") << 19;
    QTest::newRow("closed-at-buffer-notification") << 20;
    QTest::newRow("receipt-fails-after-mode-revision") << 21;
    QTest::newRow("publication-callback-supersedes") << 22;
    QTest::newRow("refused-blocked") << 23;
    QTest::newRow("superseded-constraints-at-restore-signal") << 24;
    QTest::newRow("applied-constrained-to-minimum") << 25;
    QTest::newRow("legacy-maximize-advances-revision") << 26;
    QTest::newRow("superseded-at-about-to-change-provenance") << 27;
    QTest::newRow("superseded-increments-at-restore-signal") << 28;
    QTest::newRow("superseded-aspect-presence-at-restore-signal") << 29;
    QTest::newRow("superseded-base-size-presence-at-restore-signal") << 30;
    QTest::newRow("applied-vertical-decorated") << 31;
    QTest::newRow("applied-client-change-frame-unchanged") << 32;
    QTest::newRow("foreign-move-at-policy-notification") << 33;
    QTest::newRow("foreign-policy-at-decoration-changed") << 34;
    QTest::newRow("closure-at-decoration-changed") << 35;
    QTest::newRow("late-replacement-at-client-notification") << 36;
    QTest::newRow("foreign-policy-write-at-policy-notification") << 37;
    QTest::newRow("foreign-generation-at-policy-notification") << 38;
    QTest::newRow("quick-tile-decorated-refused") << 39;
    QTest::newRow("cross-window-recursion-guard") << 40;
    QTest::newRow("foreign-policy-at-maximized-signal") << 41;
    QTest::newRow("foreign-generation-at-maximized-signal") << 42;
    QTest::newRow("replacement-at-border-radius") << 43;
    QTest::newRow("quick-tile-borderless-refused") << 44;
}

void TilesTest::x11NormalizeMaximizeForAssignment()
{
    // The X11 normalization of a maximized window for an ordinary guarded ingress, the undecorated
    // window only: its own recorded writes and guarded child calls, the legacy restore capture and
    // Restore rectangle, the mode write published as its own obligation in the legacy order, and a
    // receipt pinning the mode revision and the complete geometry constraints.
#if KWIN_BUILD_X11
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second && first->isActive());
    // The decorated rows (Breeze, the default fixture): the transition routed through the operation.
    const bool decorated = scenario == 14 || scenario == 15 || (scenario >= 31 && scenario <= 39) || (scenario >= 41 && scenario <= 44);
    const bool borderless = scenario == 15 || scenario == 33 || scenario == 34 || scenario == 35 || scenario == 37 || scenario == 38 || scenario == 43 || scenario == 44;
    KSharedConfig::Ptr rules;
    if (scenario == 10) {
        rules = KSharedConfig::openConfig(QStringLiteral("kwinrulesrc"), KConfig::SimpleConfig);
        for (const QString &group : rules->groupList()) {
            rules->deleteGroup(group);
        }
        workspace()->rulebook()->setConfig(rules);
        rules->group(QStringLiteral("General")).writeEntry("rules", QStringList{QStringLiteral("x11-normalize-rule")});
        KConfigGroup group = rules->group(QStringLiteral("x11-normalize-rule"));
        group.writeEntry("maximizehoriz", true);
        group.writeEntry("maximizehorizrule", int(Rules::Force));
        group.writeEntry("maximizevert", true);
        group.writeEntry("maximizevertrule", int(Rules::Force));
        group.writeEntry("wmclass", "org.example.x11normalize");
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
    // Breeze recommends no side borders and the automatic border size follows it, so a maximized
    // and a restored decoration would have the same borders; the decorated rows need the real
    // border reaction, hence an explicit normal border size for their duration.
    KConfigGroup decorationConfig = kwinApp()->config()->group(QStringLiteral("org.kde.kdecoration2"));
    if (decorated || scenario == 40) {
        decorationConfig.writeEntry("BorderSizeAuto", false);
        decorationConfig.writeEntry("BorderSize", QStringLiteral("Normal"));
        kwinApp()->config()->sync();
        workspace()->slotReconfigure();
    }
    const auto decorationConfigReset = qScopeGuard([&]() {
        if (decorated || scenario == 40) {
            decorationConfig.deleteEntry("BorderSizeAuto");
            decorationConfig.deleteEntry("BorderSize");
            kwinApp()->config()->sync();
            workspace()->slotReconfigure();
        }
    });
    if (borderless) {
        options->setBorderlessMaximizedWindows(true);
    }
    const auto borderlessReset = qScopeGuard([&]() {
        if (borderless) {
            options->setBorderlessMaximizedWindows(false);
        }
    });
    Test::XcbConnectionPtr connection = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(connection.get()));
    X11Window *window = createWindow(connection.get(), Rect(10, 10, 100, 50), [&](xcb_window_t windowId) {
        if (!decorated) {
            Test::applyMotifHints(connection.get(), windowId, Test::MotifHints{.flags = Test::MWM_HINTS_DECORATIONS, .decorations = 0});
        }
        if (scenario == 10) {
            xcb_icccm_set_wm_class(connection.get(), windowId, 37, "x11normalize\0org.example.x11normalize");
        }
    });
    QVERIFY(window);
    QCOMPARE(window->isDecorated(), decorated);
    const QRectF frame0 = window->frameGeometry();
    const xcb_window_t windowId = window->window();
    const QPointer<Window> guard(window);
    const auto closeWindow = qScopeGuard([&]() {
        xcb_destroy_window(connection.get(), windowId);
        xcb_flush(connection.get());
        if (guard) {
            QSignalSpy closed(guard.data(), &Window::closed);
            closed.wait();
        }
    });
    const auto electricReset = qScopeGuard([&]() {
        if (scenario == 11 && guard && !window->isDeleted()) {
            window->setElectricBorderMaximizing(false);
            window->setElectricBorderMode(std::nullopt);
        }
    });
    const auto blockReset = qScopeGuard([&]() {
        if (scenario == 23 && guard && !window->isDeleted() && window->areGeometryUpdatesBlocked()) {
            window->unblockGeometryUpdates();
        }
    });
    const auto netState = [&]() {
        xcb_flush(kwinApp()->x11Connection());
        NETWinInfo info(connection.get(), windowId, kwinApp()->x11RootWindow(), NET::WMState, NET::Properties2());
        return NET::States(info.state() & NET::Max);
    };
    // The frame extents the client reads back, which must describe the current decoration.
    const auto frameExtentsCurrent = [&]() {
        xcb_flush(kwinApp()->x11Connection());
        NETWinInfo info(connection.get(), windowId, kwinApp()->x11RootWindow(), NET::WMFrameExtents, NET::Properties2());
        const NETStrut extents = info.frameExtents();
        return extents.left == Xcb::toXNative(window->borderLeft()) && extents.right == Xcb::toXNative(window->borderRight())
            && extents.top == Xcb::toXNative(window->borderTop()) && extents.bottom == Xcb::toXNative(window->borderBottom());
    };
    // The client's own view of how often its frame extents were written: property notifications
    // selected on the client connection, drained before the operation and counted after it (a
    // round trip on the compositor's connection first, so its writes reach the server before the
    // client's round trip).
    const auto selectPropertyNotifications = [&]() {
        const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(connection.get(), windowId, XCB_CW_EVENT_MASK, &mask);
        xcb_flush(connection.get());
        free(xcb_get_input_focus_reply(connection.get(), xcb_get_input_focus(connection.get()), nullptr));
        while (xcb_generic_event_t *event = xcb_poll_for_event(connection.get())) {
            free(event);
        }
    };
    const auto frameExtentsWrites = [&]() {
        xcb_flush(kwinApp()->x11Connection());
        free(xcb_get_input_focus_reply(kwinApp()->x11Connection(), xcb_get_input_focus(kwinApp()->x11Connection()), nullptr));
        free(xcb_get_input_focus_reply(connection.get(), xcb_get_input_focus(connection.get()), nullptr));
        int writes = 0;
        while (xcb_generic_event_t *event = xcb_poll_for_event(connection.get())) {
            if ((event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY && reinterpret_cast<xcb_property_notify_event_t *>(event)->atom == atoms->net_frame_extents) {
                ++writes;
            }
            free(event);
        }
        return writes;
    };
    const auto setHints = [&](const QSize &minimum, const QSize &maximum) {
        xcb_size_hints_t hints;
        memset(&hints, 0, sizeof(hints));
        xcb_icccm_size_hints_set_min_size(&hints, minimum.width(), minimum.height());
        if (!maximum.isEmpty()) {
            xcb_icccm_size_hints_set_max_size(&hints, maximum.width(), maximum.height());
        }
        xcb_icccm_set_wm_normal_hints(connection.get(), windowId, &hints);
        xcb_flush(connection.get());
    };
    // A hint change inside a handler, synchronously: the client writes the property and
    // round-trips, then the window re-reads it; no geometry is written.
    const auto rereadHints = [&](const std::function<void(xcb_size_hints_t &)> &fill) {
        xcb_size_hints_t hints;
        memset(&hints, 0, sizeof(hints));
        fill(hints);
        xcb_icccm_set_wm_normal_hints(connection.get(), windowId, &hints);
        free(xcb_get_input_focus_reply(connection.get(), xcb_get_input_focus(connection.get()), nullptr));
        window->m_geometryHints.fetch();
        window->m_geometryHints.read();
    };
    MaximizeMode mode = MaximizeFull;
    if (scenario == 1 || scenario == 4 || scenario == 18 || scenario == 24 || (scenario >= 28 && scenario <= 30) || scenario == 31) {
        mode = MaximizeVertical;
    } else if (scenario == 2) {
        mode = MaximizeHorizontal;
    }
    if (scenario == 10) {
        QCOMPARE(window->maximizeMode(), MaximizeFull);
    } else if (scenario != 3 && scenario != 26) {
        window->maximize(mode);
        QCOMPARE(window->maximizeMode(), mode);
        QCOMPARE(netState(), mode == MaximizeFull ? NET::States(NET::Max) : NET::States(mode == MaximizeVertical ? NET::MaxVert : NET::MaxHoriz));
    }
    const QRectF area = workspace()->clientArea(MaximizeArea, window, window->moveResizeOutput());
    if (scenario == 1 || scenario == 4 || scenario == 18 || scenario == 24 || (scenario >= 28 && scenario <= 30) || scenario == 31) {
        // The horizontal axis moves while the vertical one is maximized: the capture refreshes it
        // (a full maximize's capture would rewrite an equal restore, without a notification).
        window->moveResize(RectF(40, window->y(), scenario == 31 ? window->width() : 100, window->height()));
    } else if (scenario == 2) {
        window->moveResize(RectF(window->x(), 60, window->width(), 50));
    }
    switch (scenario) {
    case 4:
        // Property only: a requested quick tile skips the capture.
        window->m_requestedTile = m_tileManager->quickTile(QuickTileFlag::Left);
        QVERIFY(window->requestedQuickTileMode() != QuickTileMode(QuickTileFlag::None));
        break;
    case 5:
        window->setGeometryRestore(RectF());
        QVERIFY(!window->geometryRestore().isValid());
        break;
    case 8:
        setHints(QSize(area.width(), area.height()), QSize(area.width(), area.height()));
        QTRY_VERIFY(!window->isResizable());
        break;
    case 9:
        window->setFullScreen(true);
        QVERIFY(window->isFullScreen());
        break;
    case 11:
        // The mode first: enabling the maximizing state dereferences it.
        window->setElectricBorderMode(ElectricBorderMode(MaximizeFull));
        window->setElectricBorderMaximizing(true);
        break;
    case 12:
        window->m_maximizeModeRevision = maximum - 1;
        break;
    case 13:
        window->m_placementContinuationRevision = maximum - Window::assignmentPlacementBudget;
        break;
    case 23:
        window->blockGeometryUpdates();
        break;
    case 25:
        setHints(QSize(120, 60), QSize(0, 0));
        QTRY_COMPARE(window->minSize(), QSizeF(120, 60));
        break;
    case 26:
        // At the maximize area with that restore preset: a legacy maximize then changes no
        // geometry; the public move-resize still advances the request generation.
        window->moveResize(area);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(area));
        window->setGeometryRestore(area);
        break;
    case 15:
    case 33:
    case 34:
    case 35:
    case 37:
    case 38:
        // The borderless policy stripped the decoration at the maximize; the transition recreates it.
        QVERIFY(!window->isDecorated());
        break;
    case 43:
        // Recreated likewise; the installation's border-radius notification is the boundary this row
        // hooks, and Breeze reports no radius change on these fixtures, so the undecorated window
        // carries the radius a decoration with rounded corners would have left behind.
        QVERIFY(!window->isDecorated());
        window->setBorderRadius(BorderRadius(8));
        QVERIFY(!window->borderRadius().isNull());
        break;
    case 44:
        // Undecorated at entry under the borderless policy, a quick tile requested: the recreation's
        // workspace check would take the special-state branch, so the preflight refuses.
        QVERIFY(!window->isDecorated());
        window->m_requestedTile = m_tileManager->quickTile(QuickTileFlag::Left);
        break;
    case 32:
        // The restore equals the maximize area: the frame stays, the client changes with the borders.
        window->setGeometryRestore(area);
        break;
    case 39:
        window->m_requestedTile = m_tileManager->quickTile(QuickTileFlag::Left);
        break;
    }
    std::unique_ptr<QObject> otherContext;
    if (scenario == 40) {
        // A second, decorated window whose legacy maximize emits its border reaction under the
        // file-static recursion guard: a normalization of this window from that reaction is refused.
        X11Window *other = createWindow(connection.get(), Rect(300, 300, 120, 60));
        QVERIFY(other && other->isDecorated());
        otherContext = std::make_unique<QObject>();
        bool refusedUnderGuard = false;
        connect(other, &Window::frameGeometryChanged, otherContext.get(), [&]() {
            if (!refusedUnderGuard) {
                refusedUnderGuard = true;
                QVERIFY(!window->canNormalizeMaximizeForAssignment());
                auto innerFence = window->makeTileAssignmentFence();
                QCOMPARE(window->normalizeMaximizeForAssignment(innerFence).outcome, Outcome::Refused);
            }
        });
        other->maximize(MaximizeFull);
        QVERIFY(refusedUnderGuard);
        QVERIFY(window->canNormalizeMaximizeForAssignment());
        xcb_destroy_window(connection.get(), other->window());
        xcb_flush(connection.get());
    }
    const bool expectRefusal = scenario == 5 || (scenario >= 8 && scenario <= 13) || scenario == 23 || scenario == 39 || scenario == 44;
    QCOMPARE(window->canNormalizeMaximizeForAssignment(), !expectRefusal);
    QCOMPARE(window->canRequestTileForAssignment(first), !expectRefusal);
    const MaximizeMode before = window->maximizeMode();
    const auto revisionBefore = window->m_maximizeModeRevision;
    const QRectF restoreBefore = window->geometryRestore();
    const QRectF frameBefore = window->frameGeometry();
    // The frame a foreign public write inside the operation leaves (row 41); the operation writes none after it.
    QRectF foreignFrame = frameBefore;
    const auto publicationBefore = window->m_geometryUpdateGeneration;
    const auto requestBefore = window->m_geometryRequestGeneration;
    QSignalSpy aboutToChange(window, &Window::maximizedAboutToChange);
    QSignalSpy maximizedChanged(window, &Window::maximizedChanged);
    QSignalSpy bufferChanged(window, &Window::bufferGeometryChanged);
    QSignalSpy clientChanged(window, &Window::clientGeometryChanged);
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    QSignalSpy decorationChanged(window, &Window::decorationChanged);
    QSignalSpy policyChanged(window, &Window::decorationPolicyChanged);
    const auto generationBefore = window->m_decorationGeneration;
    const auto policyRevisionBefore = window->m_decorationPolicyRevision;
    // The decorated window's later maximize signals, which a boundary failing after the first one
    // must not emit.
    KDecoration3::DecoratedWindow *const decoratedWindow = window->isDecorated() ? window->decoration()->window() : nullptr;
    std::unique_ptr<QSignalSpy> decoratedHorizontally, decoratedMaximized;
    if (decoratedWindow) {
        decoratedHorizontally = std::make_unique<QSignalSpy>(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedHorizontallyChanged);
        decoratedMaximized = std::make_unique<QSignalSpy>(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedChanged);
    }
    QObject context;
    bool intervened = false;
    bool incoming = true;
    const auto once = [&](std::function<void()> action) {
        return [&, action]() {
            if (!intervened) {
                intervened = true;
                action();
            }
        };
    };
    const RectF nested(300, 300, 120, 60);
    if (scenario == 16) {
        // The revision alone, through the writer: a legacy maximize would also write geometry,
        // which the entry snapshot catches first.
        connect(window, &Window::maximizedAboutToChange, &context, once([&]() {
            window->writeMaximizeMode(MaximizeVertical);
        }));
    } else if (scenario == 17) {
        connect(window, &Window::maximizedAboutToChange, &context, once([&]() {
            window->m_clientFrameExtents = QMarginsF(1, 1, 1, 1);
        }));
    } else if (scenario == 18) {
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, once([&]() {
            window->maximize(MaximizeFull);
        }));
    } else if (scenario == 24) {
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, once([&]() {
            window->m_clientFrameExtents = QMarginsF(1, 1, 1, 1);
        }));
    } else if (scenario == 27) {
        connect(window, &Window::maximizedAboutToChange, &context, once([&]() {
            incoming = false;
        }));
    } else if (scenario == 28) {
        // The limits unchanged, the increments not.
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, once([&]() {
            rereadHints([](xcb_size_hints_t &hints) {
                xcb_icccm_size_hints_set_resize_inc(&hints, 2, 2);
            });
            QCOMPARE(window->resizeIncrements(), QSizeF(2, 2));
        }));
    } else if (scenario == 29) {
        // The aspect bounds at their absent defaults: only the presence flag flips.
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, once([&]() {
            rereadHints([](xcb_size_hints_t &hints) {
                xcb_icccm_size_hints_set_aspect(&hints, 1, INT_MAX, INT_MAX, 1);
            });
            QVERIFY(window->m_geometryHints.hasAspect());
            QCOMPARE(window->m_geometryHints.minAspect(), QSize(1, INT_MAX));
            QCOMPARE(window->m_geometryHints.maxAspect(), QSize(INT_MAX, 1));
        }));
    } else if (scenario == 33) {
        connect(window, &Window::decorationPolicyChanged, &context, once([&]() {
            window->moveResize(nested);
        }));
    } else if (scenario == 34) {
        connect(window, &Window::decorationChanged, &context, once([&]() {
            window->setDecorationPolicy(DecorationPolicy::None);
        }));
    } else if (scenario == 35) {
        connect(window, &Window::decorationChanged, &context, once([&]() {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }));
    } else if (scenario == 36) {
        connect(window, &Window::clientGeometryChanged, &context, once([&]() {
            window->setDecorationPolicy(DecorationPolicy::None);
        }));
    } else if (scenario == 37) {
        // A bare policy write: no geometry, only the policy and its revision.
        connect(window, &Window::decorationPolicyChanged, &context, once([&]() {
            window->writeDecorationPolicy(DecorationPolicy::Server);
        }));
    } else if (scenario == 38) {
        // A bare generation advance: no geometry, no decoration change.
        connect(window, &Window::decorationPolicyChanged, &context, once([&]() {
            window->advanceDecorationGeneration();
        }));
    } else if (scenario == 41) {
        // A foreign policy change on the same incarnation inside the first maximize signal: the
        // decoration is kept and only the policy and its revision change, but since 6.8 the policy
        // write's decoration update runs anyway and requests its frame-margin rectangle publicly
        // (the fence detects it); the frame it leaves is recorded.
        QVERIFY(decoratedWindow);
        connect(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, &context, once([&]() {
            window->setDecorationPolicy(DecorationPolicy::Server);
            foreignFrame = window->frameGeometry();
        }));
    } else if (scenario == 42) {
        // A bare generation advance inside the first maximize signal.
        QVERIFY(decoratedWindow);
        connect(decoratedWindow, &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, &context, once([&]() {
            window->advanceDecorationGeneration();
        }));
    } else if (scenario == 43) {
        // A public forced recreation inside the recreation's first notification (the border
        // radius): the installation withholds its final notification, the obligation never
        // describes the newer decoration, and the nested legacy path wrote the extents once.
        connect(window, &Window::borderRadiusChanged, &context, once([&]() {
            window->updateDecoration(true, true);
        }));
    } else if (scenario == 30) {
        // A zero base size: only the presence flag flips, the minimum stays.
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, once([&]() {
            const QSizeF minimumBefore = window->minSize();
            rereadHints([](xcb_size_hints_t &hints) {
                xcb_icccm_size_hints_set_base_size(&hints, 0, 0);
            });
            QVERIFY(window->m_geometryHints.hasBaseSize());
            QCOMPARE(window->m_geometryHints.baseSize(), QSize(0, 0));
            QCOMPARE(window->minSize(), minimumBefore);
        }));
    } else if (scenario == 19) {
        connect(window, &Window::bufferGeometryChanged, &context, once([&]() {
            window->moveResize(nested);
        }));
    } else if (scenario == 20) {
        connect(window, &Window::bufferGeometryChanged, &context, once([&]() {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }));
    } else if (scenario == 22) {
        connect(window, &Window::maximizedChanged, &context, once([&]() {
            QVERIFY(!second->unmanage(window));
        }));
    }
    const auto extentsReset = qScopeGuard([&]() {
        if ((scenario == 17 || scenario == 24) && guard && !window->isDeleted()) {
            window->m_clientFrameExtents = QMarginsF();
        }
    });
    Window::OwnedProvenance provenance;
    if (scenario == 7 || scenario == 27) {
        provenance = provenance.withConstraint([&incoming]() {
            return incoming;
        });
        incoming = scenario != 7;
    }
    std::unique_ptr<TileAssignmentFence> fencePtr = window->makeTileAssignmentCheckpoint();
    const TileAssignmentFence &fence = *fencePtr;
    if (scenario == 6) {
        window->moveResize(RectF(window->x() + 5, window->y(), window->width(), window->height()));
    }
    if (scenario == 14 || scenario == 15 || scenario == 43) {
        selectPropertyNotifications();
    }
    auto result = window->normalizeMaximizeForAssignment(fence, provenance);
    QCOMPARE(intervened, scenario >= 16 && scenario <= 20 || scenario == 22 || scenario == 24 || (scenario >= 27 && scenario <= 30) || (scenario >= 33 && scenario <= 38) || (scenario >= 41 && scenario <= 43));
    if (expectRefusal) {
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), before);
        QCOMPARE(window->m_maximizeModeRevision, revisionBefore);
        QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
        QCOMPARE(aboutToChange.count(), 0);
        QCOMPARE(maximizedChanged.count(), 0);
        if (scenario == 5) {
            // The legacy remainder still tiles it.
            window->requestTile(first);
            QCOMPARE(window->requestedTile(), first);
            QCOMPARE(window->tile(), first);
            QCOMPARE(window->maximizeMode(), MaximizeRestore);
        }
        return;
    }
    if (scenario == 3 || scenario == 26) {
        // The operation-owned no-op: a fresh checkpoint, nothing recorded, no notification; the
        // receipt detects a later mode change. Row 26 observes the legacy path's own use of the
        // writer: its maximize changes no geometry, yet the revision advances (the public
        // move-resize advances the request generation regardless, so the checkpoint is stale
        // either way and the revision is observed directly).
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(result.owned.kind, Window::OwnedProvenance::Kind::Operation);
        QCOMPARE(aboutToChange.count(), 0);
        QCOMPARE(maximizedChanged.count(), 0);
        QCOMPARE(window->m_geometryUpdateGeneration, publicationBefore);
        window->maximize(MaximizeFull);
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        if (scenario == 26) {
            QCOMPARE(QRectF(window->frameGeometry()), QRectF(area));
            QCOMPARE(window->m_maximizeModeRevision, revisionBefore + 1);
        }
        QVERIFY(!window->assignmentResultIsCurrent(result));
        return;
    }
    if (scenario == 6 || scenario == 7) {
        // Stopped at entry: nothing written, no boundary crossed.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), before);
        QCOMPARE(aboutToChange.count(), 0);
        QCOMPARE(maximizedChanged.count(), 0);
        return;
    }
    // The nested legacy maximize of row 18 announces its own boundary too.
    QCOMPARE(aboutToChange.count(), scenario == 18 ? 2 : 1);
    QCOMPARE(aboutToChange.first().at(0).value<MaximizeMode>(), MaximizeRestore);
    if (scenario == 16) {
        // A mode write inside the first boundary: superseded before this write, the handler's
        // mode stands, nothing published by this step.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), MaximizeVertical);
        QCOMPARE(window->m_maximizeModeRevision, revisionBefore + 1);
        QCOMPARE(maximizedChanged.count(), 0);
        QCOMPARE(netState(), NET::States(NET::Max));
        return;
    }
    if (scenario == 17 || scenario == 27) {
        // The constraints changed, or the incoming provenance lapsed, inside the first boundary:
        // superseded before the mode write.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), before);
        QCOMPARE(window->m_maximizeModeRevision, revisionBefore);
        QCOMPARE(maximizedChanged.count(), 0);
        QCOMPARE(netState(), NET::States(NET::Max));
        return;
    }
    if (scenario == 18) {
        // A legacy maximize inside the restore capture's notification re-wrote the mode after this
        // write: the receipt fails, the step is superseded, and the obsolete publication is
        // skipped (the legacy write published its own state).
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        QCOMPARE(window->m_maximizeModeRevision, revisionBefore + 2);
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States(NET::Max));
        return;
    }
    if (scenario == 24 || (scenario >= 28 && scenario <= 30)) {
        // The constraints changed inside the restore capture's notification (the frame extents, the
        // increments, the aspect presence, the base-size presence): the receipt fails at the
        // request, the step is superseded, and the still-current mode write is published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States());
        return;
    }
    if (scenario == 19) {
        // A newer committed update inside the request's first notification: the request is
        // superseded, the nested geometry is final, and the mode write is still published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(nested))));
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States());
        return;
    }
    if (scenario == 20) {
        // Closed inside the request's first notification: superseded, nothing published, the
        // window released once the caller's own reference goes.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(guard && guard->isDeleted());
        QCOMPARE(bufferChanged.count(), 1);
        QCOMPARE(maximizedChanged.count(), 0);
        fencePtr.reset();
        QTRY_VERIFY(!guard);
        return;
    }
    if (scenario == 33) {
        // A foreign public move inside the policy notification of the recreation: the written
        // fence detects it, the mode is still published, and the recreated decoration's extents are
        // still the obligation's.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(window->isDecorated());
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(nested))));
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States());
        QVERIFY(frameExtentsCurrent());
        return;
    }
    if (scenario == 34) {
        // A foreign public policy change inside the recreation's notification: the public path
        // destroys the decoration again with its legacy writes, which the fence detects.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!window->isDecorated());
        QCOMPARE(window->m_decorationPolicy, DecorationPolicy::None);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(maximizedChanged.count(), 1);
        return;
    }
    if (scenario == 35) {
        // Closed inside the recreation's notification: superseded, nothing published, released once
        // the caller's own reference goes.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(guard && guard->isDeleted());
        QCOMPARE(maximizedChanged.count(), 0);
        fencePtr.reset();
        QTRY_VERIFY(!guard);
        return;
    }
    if (scenario == 36) {
        // A late replacement inside the request's client notification: the receipt pins the
        // incarnation, the request is superseded, the mode is still published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(!window->isDecorated());
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(maximizedChanged.count(), 1);
        return;
    }
    if (scenario == 41 || scenario == 42) {
        // A foreign policy-only change, or a bare generation advance, inside the first maximize
        // signal on the kept decoration: the boundary after that signal ends the operation, so the
        // later maximize signals are never emitted; the mode is still published, the decoration
        // kept, and the frame is the one the foreign write left (row 41's public update; row 42
        // writes none): the operation writes nothing after it.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(window->isDecorated());
        QCOMPARE(decoratedHorizontally->count(), 0);
        QCOMPARE(decoratedMaximized->count(), 0);
        QCOMPARE(foreignFrame != frameBefore, scenario == 41);
        QCOMPARE(QRectF(window->frameGeometry()), foreignFrame);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States());
        if (scenario == 41) {
            QCOMPARE(window->m_decorationPolicy, DecorationPolicy::Server);
            QCOMPARE(window->m_decorationPolicyRevision, policyRevisionBefore + 1);
            QCOMPARE(window->m_decorationGeneration, generationBefore);
            QCOMPARE(policyChanged.count(), 1);
            // The kept incarnation's extents are still the obligation's.
            QVERIFY(frameExtentsCurrent());
        } else {
            QCOMPARE(window->m_decorationGeneration, generationBefore + 1);
            QCOMPARE(window->m_decorationPolicyRevision, policyRevisionBefore);
        }
        return;
    }
    if (scenario == 43) {
        // Replaced inside the recreation's border-radius notification by a public forced
        // recreation: the outer installation announces nothing more (the nested one announced its
        // replacement: since 6.8 a forced recreation replaces in one installation, no removal
        // first), the outer's policy write is still announced once, the outer obligation never
        // describes the newer decoration (the extents were written exactly as often as a legacy
        // forced recreation writes them, measured the same way below), the mode is still published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(window->isDecorated());
        QCOMPARE(window->m_decorationPolicy, DecorationPolicy::ClientPreference);
        QCOMPARE(window->m_decorationGeneration, generationBefore + 2);
        QCOMPARE(decorationChanged.count(), 1);
        QCOMPARE(policyChanged.count(), 1);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States());
        QVERIFY(frameExtentsCurrent());
        const int transitionWrites = frameExtentsWrites();
        selectPropertyNotifications();
        window->updateDecoration(true, true);
        const int legacyWrites = frameExtentsWrites();
        QVERIFY(legacyWrites >= 1);
        QCOMPARE(transitionWrites, legacyWrites);
        QVERIFY(frameExtentsCurrent());
        return;
    }
    if (scenario == 37 || scenario == 38) {
        // A bare policy write or generation advance inside the policy notification, no geometry:
        // the boundary after that notification sees it; superseded before any request, the mode
        // still published, the frame unchanged.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(window->isDecorated());
        QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(netState(), NET::States());
        return;
    }
    if (scenario == 22) {
        // A foreign membership stamp inside the publication's last notification: the geometry and
        // the mode stand, the outcome decided after the publication is a supersession.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(10, 10, 100, 50));
        QCOMPARE(maximizedChanged.count(), 1);
        return;
    }
    // Applied: the synchronous request completed while current, the mode published in the legacy
    // order, the receipt current.
    QCOMPARE(result.outcome, Outcome::AppliedCurrent);
    QVERIFY(result.checkpoint && result.checkpoint->isCurrentFor(*window));
    QVERIFY(window->assignmentResultIsCurrent(result));
    QCOMPARE(result.owned.kind, Window::OwnedProvenance::Kind::Operation);
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    QCOMPARE(window->m_maximizeModeRevision, revisionBefore + 1);
    QCOMPARE(maximizedChanged.count(), 1);
    QCOMPARE(netState(), NET::States());
    QCOMPARE(window->m_geometryUpdateGeneration, publicationBefore + 1);
    QCOMPARE(window->m_geometryRequestGeneration, requestBefore + 1);
    QCOMPARE(frameChanged.count(), scenario == 32 ? 0 : 1);
    QRectF expectedRestore = decorated ? frame0 : QRectF(10, 10, 100, 50);
    QRectF expectedFrame = expectedRestore;
    if (scenario == 31) {
        expectedRestore = QRectF(40, frame0.y(), frame0.width(), frame0.height());
        expectedFrame = expectedRestore;
    } else if (scenario == 32) {
        expectedRestore = QRectF(area);
        expectedFrame = QRectF(area);
    } else if (scenario == 1) {
        expectedRestore = QRectF(40, 10, 100, 50);
        expectedFrame = expectedRestore;
    } else if (scenario == 2) {
        expectedRestore = QRectF(10, 60, 100, 50);
        expectedFrame = expectedRestore;
    } else if (scenario == 4) {
        // No capture: the restore keeps the maximize's capture, the horizontal axis is the current.
        expectedRestore = restoreBefore;
        expectedFrame = QRectF(40, 10, 100, 50);
    } else if (scenario == 25) {
        expectedFrame = QRectF(10, 10, 120, 60);
    }
    QCOMPARE(QRectF(window->geometryRestore()), expectedRestore);
    QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(RectF(expectedFrame)))));
    if (decorated) {
        // The transition's effects: the decoration present (recreated under the borderless policy
        // with the policy restored), its border reaction or recreation folded into the single
        // publication, the client inside the borders, and the frame extents describing it.
        QVERIFY(window->isDecorated());
        QCOMPARE(window->m_decorationPolicy, DecorationPolicy::ClientPreference);
        QCOMPARE(clientChanged.count(), 1);
        QCOMPARE(frameChanged.count(), scenario == 32 ? 0 : 1);
        QCOMPARE(QRectF(window->clientGeometry()).size(), QRectF(window->frameGeometry()).size() - QSizeF(window->borderLeft() + window->borderRight(), window->borderTop() + window->borderBottom()));
        QVERIFY(frameExtentsCurrent());
        if (scenario == 14 || scenario == 15) {
            // The obligation wrote the extents once, as the legacy update would have.
            QCOMPARE(frameExtentsWrites(), 1);
        }
    }
    if (scenario == 21) {
        // A bare mode revision advance (the writer, no geometry): the checkpoint stays current, the
        // receipt does not.
        window->writeMaximizeMode(MaximizeRestore);
        QVERIFY(result.checkpoint->isCurrentFor(*window));
        QVERIFY(!window->assignmentResultIsCurrent(result));
    }
#else
    QSKIP("X11 support is disabled");
#endif
}

void TilesTest::workspacePositionDecision_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("edge-constrained-geometry") << 0;
    QTest::newRow("invalid-old-geometry-explicit-output") << 1;
    QTest::newRow("dock-no-op") << 2;
    QTest::newRow("desktop-no-op") << 3;
    QTest::newRow("special-state-move-then-restore") << 4;
}

void TilesTest::workspacePositionDecision()
{
    // The workspace-position decision is read-only and the wrapper keeps the legacy writes and
    // their order: the off-screen rule gives a known rectangle, an invalid old geometry falls back to the
    // current one, a dock or desktop window gets no request at all, an equal ordinary rectangle is
    // still requested publicly, and the special-state branch moves first and moves the restore
    // fields afterwards.
#if KWIN_BUILD_X11
    QFETCH(int, scenario);
    using Kind = Window::WorkspacePositionDecision::Kind;
    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.size(), 2);
    Test::XcbConnectionPtr connection = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(connection.get()));
    X11Window *window = createWindow(connection.get(), Rect(10, 20, 100, 50), [&](xcb_window_t windowId) {
        Test::applyMotifHints(connection.get(), windowId, Test::MotifHints{.flags = Test::MWM_HINTS_DECORATIONS, .decorations = 0});
        if (scenario == 2 || scenario == 3) {
            NETWinInfo info(connection.get(), windowId, rootWindow(), NET::WMWindowType, NET::Properties2());
            info.setWindowType(scenario == 2 ? NET::Dock : NET::Desktop);
        }
    });
    QVERIFY(window);
    QVERIFY(!window->isDecorated());
    const xcb_window_t windowId = window->window();
    const QPointer<Window> guard(window);
    const auto closeWindow = qScopeGuard([&]() {
        xcb_destroy_window(connection.get(), windowId);
        xcb_flush(connection.get());
        if (guard) {
            QSignalSpy closed(guard.data(), &Window::closed);
            closed.wait();
        }
    });
    QCOMPARE(window->isDock(), scenario == 2);
    QCOMPARE(window->isDesktop(), scenario == 3);
    const RectF screenArea = workspace()->clientArea(ScreenArea, window, outputs[0]);
    const RectF otherArea = workspace()->clientArea(ScreenArea, window, outputs[1]);
    RectF oldGeometry;
    LogicalOutput *oldOutput = nullptr;
    QRectF expectedFrame;
    switch (scenario) {
    case 0:
        // Entirely above the screen: the off-screen rule brings its bottom edge back inside by its
        // own height (less than a quarter of the screen). 6.8 removed the edge-snapping rules the
        // 6.7 row exercised (upstream 2793b44003); the old geometry only selects the old area.
        window->moveResize(RectF(10, -120, 100, 50));
        oldGeometry = RectF(10, 0, 100, 50);
        expectedFrame = QRectF(10, 0, 100, 50);
        break;
    case 1:
        oldOutput = outputs[1];
        expectedFrame = QRectF(window->frameGeometry());
        break;
    case 4:
        window->maximize(MaximizeFull);
        QCOMPARE(window->maximizeMode(), MaximizeFull);
        // The restore lives in the other output's area; the old geometry does too.
        window->setGeometryRestore(RectF(1300, 100, 100, 50));
        oldGeometry = RectF(1300, 100, 100, 50);
        expectedFrame = QRectF(window->frameGeometry());
        break;
    default:
        expectedFrame = QRectF(window->frameGeometry());
        break;
    }
    const QRectF frameBefore = window->frameGeometry();
    const QRectF restoreBefore = window->geometryRestore();
    const auto requestBefore = window->m_geometryRequestGeneration;
    const auto placementBefore = window->m_placementContinuationRevision;
    const auto publicationBefore = window->m_geometryUpdateGeneration;
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    QSignalSpy restoreChanged(window, &Window::maximizeGeometryRestoreChanged);
    // The decision, read-only.
    const auto decision = window->workspacePositionDecision(oldGeometry, oldOutput, window->moveResizeGeometry());
    QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
    QCOMPARE(QRectF(window->geometryRestore()), restoreBefore);
    QCOMPARE(window->m_geometryRequestGeneration, requestBefore);
    QCOMPARE(window->m_placementContinuationRevision, placementBefore);
    QCOMPARE(window->m_geometryUpdateGeneration, publicationBefore);
    QCOMPARE(frameChanged.count(), 0);
    QCOMPARE(restoreChanged.count(), 0);
    if (scenario == 2 || scenario == 3) {
        QCOMPARE(decision.kind, Kind::NoOp);
    } else if (scenario == 4) {
        QCOMPARE(decision.kind, Kind::LegacySpecial);
        QCOMPARE(QRectF(decision.geometry), frameBefore);
        QCOMPARE(QRectF(decision.oldScreenArea), QRectF(otherArea));
        QCOMPARE(QRectF(decision.screenArea), QRectF(screenArea));
    } else {
        QCOMPARE(decision.kind, Kind::Geometry);
        QCOMPARE(QRectF(decision.geometry), expectedFrame);
        QCOMPARE(QRectF(decision.screenArea), QRectF(screenArea));
    }
    // The wrapper.
    QObject context;
    bool restoreObserved = false;
    if (scenario == 4) {
        connect(window, &Window::maximizeGeometryRestoreChanged, &context, [&]() {
            // The special branch moved first: its public request is already recorded.
            restoreObserved = true;
            QCOMPARE(window->m_geometryRequestGeneration, requestBefore + 1);
            QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
        });
    }
    window->checkWorkspacePosition(oldGeometry, oldOutput);
    QCOMPARE(QRectF(window->frameGeometry()), expectedFrame);
    if (scenario == 2 || scenario == 3) {
        // No request at all: no intent recorded, nothing announced.
        QCOMPARE(window->m_geometryRequestGeneration, requestBefore);
        QCOMPARE(window->m_placementContinuationRevision, placementBefore);
        QCOMPARE(frameChanged.count(), 0);
        return;
    }
    // Every other outcome is a public request, an equal rectangle included.
    QCOMPARE(window->m_geometryRequestGeneration, requestBefore + 1);
    QCOMPARE(frameChanged.count(), expectedFrame == frameBefore ? 0 : 1);
    if (scenario == 4) {
        QVERIFY(restoreObserved);
        QCOMPARE(restoreChanged.count(), 1);
        // The same relative position in this output's area: the other area's offset removed.
        QCOMPARE(QRectF(window->geometryRestore()), QRectF(1300 - otherArea.x() + screenArea.x(), 100, 100, 50));
    } else {
        QCOMPARE(QRectF(window->geometryRestore()), restoreBefore);
    }
#else
    QSKIP("X11 support is disabled");
#endif
}

void TilesTest::decorationGeneration_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("clear-ordering") << 0;
    QTest::newRow("recreate-away-and-back") << 1;
    QTest::newRow("no-op-writes") << 2;
    QTest::newRow("saturation") << 3;
    QTest::newRow("policy-ordering") << 4;
    QTest::newRow("nested-replacement") << 5;
    QTest::newRow("closure-inside-notification") << 6;
    QTest::newRow("closure-inside-installation") << 7;
    QTest::newRow("closure-inside-replacement") << 8;
}

void TilesTest::decorationGeneration()
{
    // The decoration generation versions every installation, removal and replacement, installed
    // with the pointer before any observer runs; the X11 policy revision versions every policy
    // change, installed with the value before the decoration update; both saturate, equal writes
    // change nothing, and a nested replacement advances again without the outer call overwriting it.
#if KWIN_BUILD_X11
    QFETCH(int, scenario);
    const auto maximum = std::numeric_limits<quint64>::max();
    Test::XcbConnectionPtr connection = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(connection.get()));
    X11Window *window = createWindow(connection.get(), Rect(10, 20, 100, 50));
    QVERIFY(window);
    QVERIFY(window->isDecorated());
    QCOMPARE(window->m_decorationPolicy, DecorationPolicy::ClientPreference);
    const xcb_window_t windowId = window->window();
    const QPointer<Window> guard(window);
    const auto closeWindow = qScopeGuard([&]() {
        xcb_destroy_window(connection.get(), windowId);
        xcb_flush(connection.get());
        if (guard) {
            QSignalSpy closed(guard.data(), &Window::closed);
            closed.wait();
        }
    });
    if (scenario == 3) {
        window->m_decorationGeneration = maximum - 1;
        window->m_decorationPolicyRevision = maximum - 1;
    }
    if (scenario == 7) {
        // Start cleared: the direct installation below is the non-null case.
        window->setDecorationPolicy(DecorationPolicy::None);
        QVERIFY(!window->isDecorated());
    }
    const auto generationBefore = window->m_decorationGeneration;
    const auto revisionBefore = window->m_decorationPolicyRevision;
    QSignalSpy decorationChanged(window, &Window::decorationChanged);
    QSignalSpy policyChanged(window, &Window::decorationPolicyChanged);
    QObject context;
    bool nested = false;
    int observed = 0;
    if (scenario == 0 || scenario == 4) {
        const auto observe = [&]() {
            // Installed before the observers: the pointer gone, the generation and the policy
            // revision advanced, the policy written.
            ++observed;
            QVERIFY(!window->decoration());
            QCOMPARE(window->m_decorationGeneration, generationBefore + 1);
            QCOMPARE(window->m_decorationPolicy, DecorationPolicy::None);
            QCOMPARE(window->m_decorationPolicyRevision, revisionBefore + 1);
        };
        connect(window, &Window::decorationChanged, &context, observe);
        connect(window, &Window::borderRadiusChanged, &context, observe);
    } else if (scenario == 5) {
        connect(window, &Window::decorationChanged, &context, [&]() {
            if (!nested) {
                nested = true;
                window->setDecorationPolicy(DecorationPolicy::ClientPreference);
            }
        });
    } else if (scenario == 6) {
        // Closed inside the clear's notification with no reference held outside: the window
        // outlives the installation and the displaced decoration it parents is released before the
        // window is; the transition's remaining steps run on the live object. The Memcheck lane
        // qualifies this row.
        connect(window, &Window::decorationChanged, &context, [&]() {
            if (!nested) {
                nested = true;
                window->setSkipCloseAnimation(true);
                window->destroyWindow();
            }
        });
    } else if (scenario == 7 || scenario == 8) {
        // Closed inside an installation's notification, through the X11 helpers directly with no
        // reference held outside: the installer's incoming ownership was moved into the member and
        // the helpers hand their reference over, so the new decoration, which the window parents,
        // is deleted once, by the window. Row 8 closes at the forced replacement's notification:
        // since 6.8 a forced update replaces the decoration in one installation, with no removal
        // first. The Memcheck lane qualifies both rows.
        connect(window, &Window::decorationChanged, &context, [&]() {
            if (!nested) {
                nested = true;
                window->setSkipCloseAnimation(true);
                window->destroyWindow();
            }
        });
    }
    if (scenario == 2) {
        window->setDecorationPolicy(DecorationPolicy::ClientPreference);
        window->setDecoration(window->m_decoration.decoration);
        QCOMPARE(window->m_decorationGeneration, generationBefore);
        QCOMPARE(window->m_decorationPolicyRevision, revisionBefore);
        QCOMPARE(decorationChanged.count(), 0);
        QCOMPARE(policyChanged.count(), 0);
        QVERIFY(window->isDecorated());
        return;
    }
    if (scenario == 7) {
        window->createDecoration(KDecoration3::Style::Titled, true);
        QVERIFY(nested);
        QVERIFY(!guard);
        QCOMPARE(decorationChanged.count(), 1);
        return;
    }
    if (scenario == 8) {
        window->updateDecoration(true, true);
        QVERIFY(nested);
        QVERIFY(!guard);
        QCOMPARE(decorationChanged.count(), 1);
        return;
    }
    window->setDecorationPolicy(DecorationPolicy::None);
    if (scenario == 6) {
        QVERIFY(nested);
        QVERIFY(!guard);
        QCOMPARE(decorationChanged.count(), 1);
        return;
    }
    if (scenario == 5) {
        // The nested recreation advanced again; the outer call did not overwrite it.
        QVERIFY(nested);
        QVERIFY(window->isDecorated());
        QCOMPARE(window->m_decorationPolicy, DecorationPolicy::ClientPreference);
        QCOMPARE(window->m_decorationGeneration, generationBefore + 2);
        QCOMPARE(window->m_decorationPolicyRevision, revisionBefore + 2);
        QCOMPARE(decorationChanged.count(), 2);
        return;
    }
    QVERIFY(!window->isDecorated());
    QCOMPARE(window->m_decorationPolicy, DecorationPolicy::None);
    QCOMPARE(decorationChanged.count(), 1);
    QCOMPARE(policyChanged.count(), 1);
    if (scenario == 0 || scenario == 4) {
        QVERIFY(observed >= 1);
    }
    if (scenario == 3) {
        QCOMPARE(window->m_decorationGeneration, maximum);
        QCOMPARE(window->m_decorationPolicyRevision, maximum);
    } else {
        QCOMPARE(window->m_decorationGeneration, generationBefore + 1);
        QCOMPARE(window->m_decorationPolicyRevision, revisionBefore + 1);
    }
    if (scenario == 1 || scenario == 3) {
        // Back again: a replacement advances once more (or stays saturated); the policy equal at
        // the end does not hide the change.
        window->setDecorationPolicy(DecorationPolicy::ClientPreference);
        QVERIFY(window->isDecorated());
        QCOMPARE(window->m_decorationPolicy, DecorationPolicy::ClientPreference);
        QCOMPARE(decorationChanged.count(), 2);
        QCOMPARE(window->m_decorationGeneration, scenario == 3 ? maximum : generationBefore + 2);
        QCOMPARE(window->m_decorationPolicyRevision, scenario == 3 ? maximum : revisionBefore + 2);
    }
#else
    QSKIP("X11 support is disabled");
#endif
}

void TilesTest::x11GuardedAssignment_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("blocked-refused") << 0;
    QTest::newRow("unmanaged-refused") << 1;
    QTest::newRow("unchanged-geometry-published") << 2;
    QTest::newRow("unchanged-tile-commit") << 3;
    QTest::newRow("blocked-at-boundary-superseded") << 4;
    QTest::newRow("provenance-lapsed-at-tile-changed") << 5;
    QTest::newRow("geometry-superseded-publication-required") << 6;
    QTest::newRow("nested-request-at-tile-changed") << 7;
    QTest::newRow("committed-headroom-refused") << 8;
    QTest::newRow("placement-publication-headroom-refused") << 9;
    QTest::newRow("forget-committed-headroom-refused") << 10;
    QTest::newRow("maximized-decorated-applied") << 11;
    QTest::newRow("fractional-scale-rounding") << 12;
    QTest::newRow("nested-geometry-at-buffer-notification") << 13;
    QTest::newRow("closed-at-buffer-notification") << 14;
    QTest::newRow("forget-nested-request-at-requested-tile-changed") << 15;
    QTest::newRow("unchanged-reentry-then-unchanged-guarded") << 16;
    QTest::newRow("maximized-undecorated-applied") << 17;
    QTest::newRow("maximized-undecorated-setter") << 18;
}

void TilesTest::x11GuardedAssignment()
{
    // The X11 specialization: the guarded request publishes synchronously as its own write, the
    // association write's obligation commits the tile as the step's own publication through a
    // carrier, both committed changes are budgeted before any mutation, and an operation
    // superseded during the request still publishes its still-current association without being
    // resurrected.
#if KWIN_BUILD_X11
    QFETCH(int, scenario);
    using Outcome = Window::AssignmentStepOutcome;
    const auto maximum = std::numeric_limits<quint64>::max();
    Tile *first = m_rootTile->childTile(0);
    Tile *second = m_rootTile->childTile(1);
    QVERIFY(first && second && first->isActive());
    if (scenario == 12) {
        kwinApp()->setXwaylandScale(1.25);
    }
    const auto scaleReset = qScopeGuard([&]() {
        if (scenario == 12) {
            kwinApp()->setXwaylandScale(1);
        }
    });
    Test::XcbConnectionPtr connection = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(connection.get()));
    X11Window *window = createWindow(connection.get(), Rect(10, 10, 100, 50), [&](xcb_window_t windowId) {
        if (scenario == 1) {
            quint32 value = 1;
            xcb_change_window_attributes(connection.get(), windowId, XCB_CW_OVERRIDE_REDIRECT, &value);
        }
        if (scenario == 17 || scenario == 18) {
            Test::applyMotifHints(connection.get(), windowId, Test::MotifHints{.flags = Test::MWM_HINTS_DECORATIONS, .decorations = 0});
        }
    });
    QVERIFY(window);
    QCOMPARE(window->isUnmanaged(), scenario == 1);
    // The default fixture is decorated (row 11 normalizes it, decoration transition included); rows 17
    // and 18 normalize an undecorated window.
    QCOMPARE(window->isDecorated(), scenario != 1 && scenario != 17 && scenario != 18);
    const xcb_window_t windowId = window->window();
    const QPointer<Window> guard(window);
    const auto closeWindow = qScopeGuard([&]() {
        xcb_destroy_window(connection.get(), windowId);
        xcb_flush(connection.get());
        if (guard) {
            QSignalSpy closed(guard.data(), &Window::closed);
            closed.wait();
        }
    });
    const auto blockReset = qScopeGuard([&]() {
        if (guard && !window->isDeleted() && window->areGeometryUpdatesBlocked()) {
            window->unblockGeometryUpdates();
        }
    });
    switch (scenario) {
    case 0:
        window->blockGeometryUpdates();
        break;
    case 2:
        // The legacy move leaves the frame already at the tile's geometry.
        window->moveResize(first->windowGeometry());
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        break;
    case 3:
    case 10:
        // An association without a committed tile, property-only: the forget step's publication
        // finds the committed tile already null.
        window->m_requestedTile = first;
        window->m_requestedTileChange = window->m_requestedTileChange + 1;
        QVERIFY(!window->tile());
        break;
    case 15:
        QVERIFY(first->manage(window));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        break;
    case 8:
        window->m_committedTileChange = maximum - 1;
        break;
    case 9:
        window->m_placementContinuationRevision = maximum - 2 * Window::assignmentPlacementBudget - 1;
        break;
    case 11:
    case 17:
    case 18:
        window->maximize(MaximizeFull);
        QCOMPARE(window->requestedMaximizeMode(), MaximizeFull);
        break;
    }
    if (scenario == 10) {
        window->m_committedTileChange = maximum - 1;
    }
    const auto committedBefore = window->m_committedTileChange;
    const auto publicationBefore = window->m_geometryUpdateGeneration;
    const auto associationBefore = window->m_requestedTileChange;
    const QRectF frameBefore = window->frameGeometry();
    QSignalSpy requestedTileChanged(window, &Window::requestedTileChanged);
    QSignalSpy tileChanged(window, &Window::tileChanged);
    QSignalSpy quickTileModeChanged(window, &Window::quickTileModeChanged);
    QSignalSpy bufferChanged(window, &Window::bufferGeometryChanged);
    QSignalSpy clientChanged(window, &Window::clientGeometryChanged);
    QSignalSpy frameChanged(window, &Window::frameGeometryChanged);
    QObject context;
    bool intervened = false;
    bool incoming = true;
    const auto once = [&](std::function<void()> action) {
        return [&, action]() {
            if (!intervened) {
                intervened = true;
                action();
            }
        };
    };
    const RectF nested(300, 300, 120, 60);
    if (scenario == 4) {
        connect(window, &Window::frameGeometryAboutToChange, &context, once([&]() {
            window->blockGeometryUpdates();
        }));
    } else if (scenario == 13) {
        // A newer committed update inside the first notification of this one.
        connect(window, &Window::bufferGeometryChanged, &context, once([&]() {
            window->moveResize(nested);
        }));
    } else if (scenario == 14) {
        connect(window, &Window::bufferGeometryChanged, &context, once([&]() {
            window->setSkipCloseAnimation(true);
            window->destroyWindow();
        }));
    } else if (scenario == 15) {
        connect(window, &Window::requestedTileChanged, &context, once([&]() {
            window->requestTile(second);
        }));
    } else if (scenario == 16) {
        // A foreign stamp and a legacy request of the unchanged rectangle inside the first
        // notification: this frame is superseded with its client, frame and shape notifications
        // pending, and the unchanged legacy request delivers nothing.
        connect(window, &Window::bufferGeometryChanged, &context, once([&]() {
            QVERIFY(!second->unmanage(window));
            window->moveResize(window->frameGeometry());
        }));
    } else if (scenario == 5) {
        connect(window, &Window::tileChanged, &context, once([&]() {
            incoming = false;
        }));
    } else if (scenario == 6) {
        connect(window, &Window::frameGeometryChanged, &context, once([&]() {
            QVERIFY(!second->unmanage(window));
        }));
    } else if (scenario == 7) {
        connect(window, &Window::tileChanged, &context, once([&]() {
            window->requestTile(second);
        }));
    }
    Window::OwnedProvenance provenance;
    if (scenario == 5) {
        provenance = provenance.withConstraint([&incoming]() {
            return incoming;
        });
    }
    const bool forget = scenario == 3 || scenario == 10 || scenario == 15;
    const bool expectRefusal = scenario == 0 || scenario == 1 || scenario == 8 || scenario == 9 || scenario == 10;
    if (forget) {
        QCOMPARE(window->canForgetTileForAssignment(), !expectRefusal);
    } else {
        QCOMPARE(window->canRequestTileForAssignment(first), !expectRefusal);
    }
    if (scenario == 18) {
        // The compatibility setter selects the guarded ingress for the undecorated maximized
        // window: normalized through the X11 override, then assigned.
        QSignalSpy maximizedChanged(window, &Window::maximizedChanged);
        window->setTileCompatibility(first);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(maximizedChanged.count(), 1);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        return;
    }
    // The caller's fence, owned here: the one reference the closure row releases before awaiting
    // the destruction it observed logically.
    std::unique_ptr<TileAssignmentFence> fencePtr = window->makeTileAssignmentCheckpoint();
    const TileAssignmentFence &fence = *fencePtr;
    const auto result = forget ? window->forgetTileForAssignment(first, fence) : window->requestTileForAssignment(first, fence, provenance);
    QCOMPARE(intervened, (scenario >= 4 && scenario <= 7) || (scenario >= 13 && scenario <= 16));
    if (scenario == 16) {
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(bufferChanged.count(), 1);
        QCOMPARE(clientChanged.count(), 0);
        QCOMPARE(frameChanged.count(), 0);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        // A fresh guarded request of the unchanged rectangle publishes what is pending under its
        // own validated continuation, without a write.
        const auto publicationBeforeDrain = window->m_geometryUpdateGeneration;
        auto drainFence = window->makeTileAssignmentFence();
        auto drain = window->requestMoveResizeGuarded(first->windowGeometry(), Window::MoveResizeMode::MoveResize, drainFence, Window::OwnedProvenance());
        QCOMPARE(drain.result.outcome, Outcome::AppliedCurrent);
        QCOMPARE(drain.dispatch, Window::AssignmentRequestDispatch::Published);
        QCOMPARE(clientChanged.count(), 1);
        QCOMPARE(frameChanged.count(), 1);
        QCOMPARE(frameChanged.first().at(0).value<RectF>(), RectF(frameBefore));
        QCOMPARE(window->m_geometryUpdateGeneration, publicationBeforeDrain + 1);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        // Nothing left pending: a further unchanged guarded request publishes nothing.
        auto againFence = window->makeTileAssignmentFence();
        auto again = window->requestMoveResizeGuarded(first->windowGeometry(), Window::MoveResizeMode::MoveResize, againFence, Window::OwnedProvenance());
        QCOMPARE(again.result.outcome, Outcome::AppliedCurrent);
        QCOMPARE(window->m_geometryUpdateGeneration, publicationBeforeDrain + 1);
        QCOMPARE(frameChanged.count(), 1);
        return;
    }
    if (scenario == 14) {
        // Closed inside the first notification: the frame's announcements end there, nothing is
        // published, the operation is superseded, and the window is released once the caller's
        // own reference goes.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QVERIFY(guard && guard->isDeleted());
        QCOMPARE(bufferChanged.count(), 1);
        QCOMPARE(clientChanged.count(), 0);
        QCOMPARE(frameChanged.count(), 0);
        QCOMPARE(tileChanged.count(), 0);
        QCOMPARE(requestedTileChanged.count(), 0);
        fencePtr.reset();
        QTRY_VERIFY(!guard);
        return;
    }
    if (expectRefusal) {
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->m_requestedTileChange, associationBefore);
        QCOMPARE(window->m_committedTileChange, committedBefore);
        QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
        QCOMPARE(requestedTileChanged.count(), 0);
        if (scenario == 0) {
            window->unblockGeometryUpdates();
            QVERIFY(window->canRequestTileForAssignment(first));
        }
        return;
    }
    if (scenario == 11 || scenario == 17) {
        // The maximized window, decorated (its decoration transition the operation's own) or not,
        // takes the guarded ingress: normalized by the X11 override (the mode published, its restore
        // the entry frame), then assigned and committed.
        QCOMPARE(window->isDecorated(), scenario == 11);
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        if (scenario == 17) {
            QCOMPARE(QRectF(window->geometryRestore()), QRectF(10, 10, 100, 50));
        }
        QCOMPARE(tileChanged.count(), 1);
        QCOMPARE(requestedTileChanged.count(), 1);
        xcb_flush(kwinApp()->x11Connection());
        NETWinInfo state(connection.get(), windowId, kwinApp()->x11RootWindow(), NET::WMState, NET::Properties2());
        QCOMPARE(state.state() & NET::Max, NET::States());
        return;
    }
    switch (scenario) {
    case 2:
        // Published without a write: the request's own checkpoint stands, the commit follows.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->m_geometryUpdateGeneration, publicationBefore);
        QCOMPARE(window->tile(), first);
        QCOMPARE(window->m_committedTileChange, committedBefore + 1);
        QCOMPARE(tileChanged.count(), 1);
        break;
    case 3:
        // The committed tile already equals the publication's target: nothing committed, no fresh
        // checkpoint minted, the step current.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QVERIFY(!window->requestedTile());
        QVERIFY(!window->tile());
        QCOMPARE(window->m_committedTileChange, committedBefore);
        QCOMPARE(tileChanged.count(), 0);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 4:
        // Blocked at the boundary by a callback: the dispatch supersedes instead of reporting a
        // publication; the association stands and its obligation still commits the tile.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(QRectF(window->frameGeometry()), frameBefore);
        QCOMPARE(window->m_geometryUpdateGeneration, publicationBefore);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tileChanged.count(), 1);
        break;
    case 5:
        // The incoming provenance lapsed inside the commit's own callback: committed, the
        // obsolete secondary notification suppressed by the commit writer, the still-current
        // association notified, the operation superseded.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(window->m_committedTileChange, committedBefore + 1);
        QCOMPARE(quickTileModeChanged.count(), 0);
        QCOMPARE(requestedTileChanged.count(), 1);
        break;
    case 6:
        // The synchronous publication's notification stamped a foreign membership change: the
        // request is superseded, the association still publishes, the operation stays superseded.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tileChanged.count(), 1);
        break;
    case 7:
        // A newer association installed inside the commit's callback: the older requested-tile
        // notification is suppressed; only the nested one is published.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(window->tile(), second);
        QCOMPARE(requestedTileChanged.count(), 1);
        QCOMPARE(tileChanged.count(), 2);
        break;
    case 12:
        // The native rounding under a fractional Xwayland scale is the frame's, in both paths.
        QCOMPARE(result.outcome, Outcome::AppliedCurrent);
        QVERIFY(window->assignmentResultIsCurrent(result));
        QCOMPARE(window->tile(), first);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(first->windowGeometry()))));
        break;
    case 13:
        // The nested update delivered this frame's pending client and frame transitions from their
        // oldest unannounced values and this frame stopped: every transition announced once, the
        // newer geometry final, the association still published, the operation superseded.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(bufferChanged.count(), 2);
        QCOMPARE(clientChanged.count(), 1);
        QCOMPARE(frameChanged.count(), 1);
        QCOMPARE(QRectF(window->frameGeometry()), QRectF(Xcb::fromXNative(Xcb::toXNative(nested))));
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        break;
    case 15:
        // The forget's publication promoted its carrier, then a newer association was installed
        // inside its notification: superseded, never a success with an invalid checkpoint.
        QCOMPARE(result.outcome, Outcome::Superseded);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), second);
        QCOMPARE(window->tile(), second);
        break;
    }
#else
    QSKIP("X11 support is not built");
#endif
}

void TilesTest::transientChildDestruction_data()
{
    QTest::addColumn<bool>("transientParent");
    QTest::addColumn<bool>("transientChild");
    QTest::newRow("native-to-transient") << false << true;
    QTest::newRow("transient-to-native") << true << false;
    QTest::newRow("native-to-native") << false << false;
}

void TilesTest::transientChildDestruction()
{
    QFETCH(bool, transientParent);
    QFETCH(bool, transientChild);
    std::unique_ptr<NativeTestTile> nativeRoot;
    std::unique_ptr<TransientTestTile> transientRoot;
    if (transientParent) {
        transientRoot = std::make_unique<TransientTestTile>(m_tileManager);
    } else {
        nativeRoot = std::make_unique<NativeTestTile>(m_tileManager);
    }
    Tile *root = transientParent ? static_cast<Tile *>(transientRoot.get()) : nativeRoot.get();
    QPointer<Tile> child = transientChild ? static_cast<Tile *>(new TransientTestTile(m_tileManager, root)) : new Tile(m_tileManager, root);
    if (transientParent) {
        transientRoot->attachForTest(child);
    } else {
        nativeRoot->attachForTest(child);
    }
    QSignalSpy childrenChanged(root, &Tile::childTilesChanged);
    QSignalSpy layoutChanged(root, &Tile::isLayoutChanged);
    root->destroyChild(child);
    if (transientParent || transientChild) {
        QVERIFY(child);
        QCOMPARE(root->childTiles(), QList<Tile *>{child});
        QCOMPARE(childrenChanged.count(), 0);
        QCOMPARE(layoutChanged.count(), 0);
    } else {
        QVERIFY(!child);
        QCOMPARE(root->childCount(), 0);
        QVERIFY(!childrenChanged.isEmpty());
    }
}

void TilesTest::transientControllerGeometry()
{
    using Result = TransientTileController::Result;
    TransientTileController controller;
    auto desktop = m_rootTile->desktop();
    const RectF original(0.1, 0.1, 0.4, 0.4);
    const auto manualChildren = m_rootTile->childTiles();
    QSignalSpy manualChanged(m_rootTile, &CustomTile::layoutModified);
    QCOMPARE(controller.createTree(m_tileManager, desktop, original), Result::Accepted);
    const quint64 generation = controller.generation();
    QVERIFY(generation != 0);
    auto root = controller.rootTile();
    auto tile = controller.tile();
    QVERIFY(root && tile);
    QVERIFY(root->isTransient() && tile->isTransient());
    QCOMPARE(root->parent(), &controller);
    QCOMPARE(tile->parentTile(), root);
    QCOMPARE(root->childTiles(), QList<Tile *>{tile});
    QCOMPARE(tile->relativeGeometry(), original);
    QVERIFY(root->windows().isEmpty() && tile->windows().isEmpty());
    QCOMPARE(controller.createTree(m_tileManager, desktop, original), Result::Busy);
    const RectF next(0.2, 0.1, 0.5, 0.6);
    QSignalSpy relative(tile, &Tile::relativeGeometryChanged);
    QSignalSpy absolute(tile, &Tile::absoluteGeometryChanged);
    QSignalSpy geometry(tile, &Tile::windowGeometryChanged);
    QCOMPARE(controller.setGeometry(generation + 1, next), Result::StaleGeneration);
    QCOMPARE(controller.setGeometry(generation, next), Result::Accepted);
    QCOMPARE(tile->relativeGeometry(), next);
    QCOMPARE(relative.count(), 1);
    QCOMPARE(absolute.count(), 1);
    QCOMPARE(geometry.count(), 1);
    QVERIFY(tile->setProperty("relativeGeometry", QVariant::fromValue(original)));
    QCOMPARE(tile->relativeGeometry(), next);
    QCOMPARE(m_rootTile->childTiles(), manualChildren);
    QCOMPARE(manualChanged.count(), 0);
    QCOMPARE(controller.clearTree(generation), Result::Accepted);
    QVERIFY(!controller.rootTile() && !controller.tile());
    QCOMPARE(controller.generation(), quint64(0));
    QCOMPARE(controller.createTree(m_tileManager, desktop, original), Result::Accepted);
    QVERIFY(controller.generation() > generation);
    QCOMPARE(controller.setGeometry(generation, next), Result::StaleGeneration);
}

void TilesTest::transientControllerInvalidGeometry_data()
{
    QTest::addColumn<RectF>("geometry");
    QTest::newRow("zero") << RectF(0, 0, 0, 1);
    QTest::newRow("negative-size") << RectF(0, 0, 1, -1);
    QTest::newRow("negative-origin") << RectF(-0.1, 0, 0.5, 1);
    QTest::newRow("outside") << RectF(0.8, 0, 0.5, 1);
    QTest::newRow("nan") << RectF(std::numeric_limits<qreal>::quiet_NaN(), 0, 0.5, 1);
    QTest::newRow("infinity") << RectF(0, 0, 0.5, std::numeric_limits<qreal>::infinity());
}

void TilesTest::transientControllerInvalidGeometry()
{
    QFETCH(RectF, geometry);
    using Result = TransientTileController::Result;
    TransientTileController controller;
    QSignalSpy changed(&controller, &TransientTileController::treeChanged);
    QCOMPARE(controller.createTree(m_tileManager, m_rootTile->desktop(), geometry), Result::InvalidGeometry);
    QVERIFY(controller.children().isEmpty());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(controller.createTree(nullptr, m_rootTile->desktop(), RectF(0, 0, 1, 1)), Result::InvalidContext);
    QCOMPARE(controller.createTree(m_tileManager, nullptr, RectF(0, 0, 1, 1)), Result::InvalidContext);
    QCOMPARE(controller.createTree(m_tileManager, m_rootTile->desktop(), RectF(0, 0, 1, 1)), Result::Accepted);
    QCOMPARE(controller.setGeometry(controller.generation(), geometry), Result::InvalidGeometry);
    QCOMPARE(controller.tile()->relativeGeometry(), RectF(0, 0, 1, 1));
}

void TilesTest::transientControllerRevocation_data()
{
    QTest::addColumn<int>("cause");
    QTest::addColumn<int>("notification");
    const QStringList causes{QStringLiteral("clear"), QStringLiteral("delete-leaf"), QStringLiteral("delete-root"),
                             QStringLiteral("delete-controller"), QStringLiteral("delete-manager"), QStringLiteral("delete-desktop")};
    const QStringList notifications{QStringLiteral("relative"), QStringLiteral("absolute"), QStringLiteral("window")};
    for (int cause = 0; cause < causes.size(); ++cause) {
        for (int notification = 0; notification < notifications.size(); ++notification) {
            QTest::newRow(qPrintable(notifications[notification] + QLatin1Char('-') + causes[cause])) << cause << notification;
        }
    }
}

void TilesTest::transientControllerRevocation()
{
    QFETCH(int, cause);
    QFETCH(int, notification);
    using Result = TransientTileController::Result;
    auto manager = std::make_unique<TileManager>(m_output);
    auto controller = std::make_unique<TransientTileController>();
    auto desktop = VirtualDesktopManager::self()->desktops().constLast();
    QCOMPARE(controller->createTree(manager.get(), desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    auto tile = controller->tile();
    QPointer<Tile> oldRoot = controller->rootTile();
    const auto generation = controller->generation();
    QSignalSpy absolute(tile, &Tile::absoluteGeometryChanged);
    QSignalSpy geometry(tile, &Tile::windowGeometryChanged);
    auto nested = Result::Accepted;
    auto nestedUpdate = Result::Accepted;
    const auto signal = notification == 0 ? &Tile::relativeGeometryChanged
        : notification == 1               ? &Tile::absoluteGeometryChanged
                                          : &Tile::windowGeometryChanged;
    connect(tile, signal, this, [&]() {
        nestedUpdate = controller->setGeometry(generation, RectF(0, 0, 0.5, 1));
        switch (cause) {
        case 0:
            controller->clearTree(generation);
            nested = controller->createTree(manager.get(), desktop, RectF(0, 0, 1, 1));
            break;
        case 1:
            delete tile;
            break;
        case 2:
            delete controller->rootTile();
            break;
        case 3:
            controller.reset();
            break;
        case 4:
            manager.reset();
            break;
        case 5:
            VirtualDesktopManager::self()->removeVirtualDesktop(desktop);
            break;
        }
    });
    QCOMPARE(controller->setGeometry(generation, RectF(0.1, 0.1, 0.5, 0.5)), Result::Superseded);
    QCOMPARE(nestedUpdate, Result::Busy);
    QCOMPARE(absolute.count(), notification >= 1 ? 1 : 0);
    QCOMPARE(geometry.count(), notification >= 2 ? 1 : 0);
    if (cause == 0) {
        QCOMPARE(nested, Result::Busy);
    }
    if (controller) {
        QCOMPARE(controller->generation(), quint64(0));
        QVERIFY(!controller->rootTile() && !controller->tile());
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!oldRoot);
}

void TilesTest::transientControllerShutdown()
{
    using Result = TransientTileController::Result;
    auto controller = std::make_unique<TransientTileController>();
    const RectF geometry(0, 0, 0.5, 1);
    auto desktop = m_rootTile->desktop();
    QCOMPARE(controller->createTree(m_tileManager, desktop, geometry), Result::Accepted);
    auto pointer = controller.get();
    const auto generation = controller->generation();
    auto createResult = Result::Accepted;
    auto updateResult = Result::Accepted;
    connect(controller->rootTile(), &QObject::destroyed, this, [&]() {
        createResult = pointer->createTree(m_tileManager, desktop, geometry);
        updateResult = pointer->setGeometry(generation, geometry);
    });
    controller.reset();
    QCOMPARE(createResult, Result::Busy);
    QCOMPARE(updateResult, Result::Busy);
}

void TilesTest::transientControllerConstructionContext_data()
{
    QTest::addColumn<int>("cause");
    QTest::addColumn<bool>("rootParent");
    QTest::newRow("leaf-manager") << 0 << false;
    QTest::newRow("leaf-controller") << 1 << false;
    QTest::newRow("leaf-desktop") << 2 << false;
    QTest::newRow("root-manager") << 0 << true;
    QTest::newRow("root-controller") << 1 << true;
    QTest::newRow("root-desktop") << 2 << true;
}

void TilesTest::transientControllerConstructionContext()
{
    QFETCH(int, cause);
    QFETCH(bool, rootParent);
    using Result = TransientTileController::Result;
    auto manager = std::make_unique<TileManager>(m_output);
    auto controller = std::make_unique<TransientTileController>();
    auto desktop = VirtualDesktopManager::self()->desktops().constLast();
    bool destroyed = false;
    ChildEventFilter filter;
    filter.childAdded = [&](QObject *object, QObject *) {
        auto tile = qobject_cast<Tile *>(object);
        if ((rootParent ? object == controller.get() : tile && tile->isTransient()) && !destroyed) {
            destroyed = true;
            if (cause == 0) {
                manager.reset();
            } else if (cause == 1) {
                controller.reset();
            } else {
                VirtualDesktopManager::self()->removeVirtualDesktop(desktop);
            }
        }
    };
    qApp->installEventFilter(&filter);
    const auto result = controller->createTree(manager.get(), desktop, RectF(0, 0, 0.5, 1));
    qApp->removeEventFilter(&filter);
    QVERIFY(destroyed);
    QCOMPARE(result, Result::Superseded);
    if (controller) {
        QVERIFY(!controller->rootTile() && !controller->tile());
        QVERIFY(controller->children().isEmpty());
    }
}

void TilesTest::transientControllerLeafThenRootDeletion()
{
    using Result = TransientTileController::Result;
    TransientTileController controller;
    QCOMPARE(controller.createTree(m_tileManager, m_rootTile->desktop(), RectF(0, 0, 0.5, 1)), Result::Accepted);
    QPointer<Tile> root = controller.rootTile();
    bool deletedRoot = false;
    connect(&controller, &TransientTileController::treeChanged, this, [&]() {
        if (root) {
            deletedRoot = true;
            delete root.data();
        }
    });
    delete controller.tile();
    QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
    QVERIFY(deletedRoot);
    QVERIFY(!root);
    QVERIFY(!controller.rootTile() && !controller.tile());
}

void TilesTest::transientControllerCreationReentry()
{
    using Result = TransientTileController::Result;
    TransientTileController controller;
    auto desktop = m_rootTile->desktop();
    auto nested = Result::Accepted;
    bool revoked = false;
    connect(&controller, &TransientTileController::treeChanged, this, [&]() {
        if (!revoked) {
            revoked = true;
            controller.clearTree(controller.generation());
            nested = controller.createTree(m_tileManager, desktop, RectF(0, 0, 1, 1));
        }
    });
    QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), Result::Superseded);
    QCOMPARE(nested, Result::Busy);
    QCOMPARE(controller.generation(), quint64(0));
    QCOMPARE(controller.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
}

void TilesTest::transientControllerUnpublishedRootCleanup()
{
    using Result = TransientTileController::Result;
    auto manager = std::make_unique<TileManager>(m_output);
    auto controller = std::make_unique<TransientTileController>();
    bool invalidated = false;
    ChildEventFilter filter;
    filter.childAdded = [&](QObject *object, QObject *child) {
        if (object == controller.get() && !invalidated) {
            invalidated = true;
            connect(child, &QObject::destroyed, this, [&]() {
                controller.reset();
            });
            manager.reset();
        }
    };
    qApp->installEventFilter(&filter);
    const auto result = controller->createTree(manager.get(), m_rootTile->desktop(), RectF(0, 0, 0.5, 1));
    qApp->removeEventFilter(&filter);
    QVERIFY(invalidated);
    QCOMPARE(result, Result::Superseded);
    QVERIFY(!controller);
}

void TilesTest::transientControllerIgnoresRetiredNotification()
{
    using Result = TransientTileController::Result;
    TransientTileController controller;
    const RectF geometry(0, 0, 0.5, 1);
    QCOMPARE(controller.createTree(m_tileManager, m_rootTile->desktop(), geometry), Result::Accepted);
    const auto oldGeneration = controller.generation();
    delete controller.tile();
    QSignalSpy changed(&controller, &TransientTileController::treeChanged);
    QCOMPARE(controller.createTree(m_tileManager, m_rootTile->desktop(), geometry), Result::Accepted);
    QVERIFY(controller.generation() > oldGeneration);
    QCOMPARE(changed.count(), 1);
    QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
    QCOMPARE(changed.count(), 1);
    QVERIFY(controller.rootTile() && controller.tile());
}

void TilesTest::transientControllerRemovedDesktop_data()
{
    QTest::addColumn<bool>("creation");
    QTest::newRow("create") << true;
    QTest::newRow("update") << false;
}

void TilesTest::transientControllerRemovedDesktop()
{
    QFETCH(bool, creation);
    using Result = TransientTileController::Result;
    TransientTileController existing;
    TransientTileController empty;
    auto desktop = m_rootTile->desktop();
    QCOMPARE(existing.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1)), Result::Accepted);
    const auto generation = existing.generation();
    QSignalSpy changed(existing.tile(), &Tile::relativeGeometryChanged);
    auto result = Result::Accepted;
    bool attempted = false;
    const auto connection = connect(VirtualDesktopManager::self(), &VirtualDesktopManager::currentChanged, this,
                                    [&](VirtualDesktop *old, VirtualDesktop *, LogicalOutput *output) {
        if (old == desktop && output == m_output) {
            attempted = true;
            result = creation ? empty.createTree(m_tileManager, desktop, RectF(0, 0, 0.5, 1))
                              : existing.setGeometry(generation, RectF(0.1, 0.1, 0.5, 0.5));
        }
    });
    VirtualDesktopManager::self()->removeVirtualDesktop(desktop);
    disconnect(connection);
    QVERIFY(attempted);
    QCOMPARE(result, Result::InvalidContext);
    QCOMPARE(changed.count(), 0);
}

void TilesTest::createSimpleLayout()
{
    std::vector<qreal> leftTileWidths = {0.5, 0.45, 0.4, 0.35, 0.3, 0.25};
    int i = 0;
    for (VirtualDesktop *desk : VirtualDesktopManager::self()->desktops()) {
        for (LogicalOutput *out : workspace()->outputs()) {
            qreal leftTileWidth = leftTileWidths[i++];
            CustomTile *rootTile = workspace()->rootTile(out, desk);
            while (rootTile->childCount() > 0) {
                static_cast<CustomTile *>(rootTile->childTile(0))->remove();
            }

            QCOMPARE(rootTile->childCount(), 0);
            rootTile->split(CustomTile::LayoutDirection::Horizontal);
            QCOMPARE(rootTile->childCount(), 2);

            auto leftTile = qobject_cast<CustomTile *>(rootTile->childTiles().first());
            auto rightTile = qobject_cast<CustomTile *>(rootTile->childTiles().last());
            QVERIFY(leftTile);
            QVERIFY(rightTile);

            leftTile->setRelativeGeometry(RectF(0, 0, leftTileWidth, 1));

            QCOMPARE(leftTile->relativeGeometry(), RectF(0, 0, leftTileWidth, 1));
            QCOMPARE(rightTile->relativeGeometry(), RectF(leftTileWidth, 0, 1 - leftTileWidth, 1));
        }
    }
}

void TilesTest::createComplexLayout()
{
    while (m_rootTile->childCount() > 0) {
        static_cast<CustomTile *>(m_rootTile->childTile(0))->remove();
    }

    QCOMPARE(m_rootTile->childCount(), 0);
    m_rootTile->split(CustomTile::LayoutDirection::Horizontal);
    QCOMPARE(m_rootTile->childCount(), 2);

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    auto rightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().last());
    QVERIFY(leftTile);
    QVERIFY(rightTile);

    QCOMPARE(leftTile->relativeGeometry(), RectF(0, 0, 0.5, 1));
    QCOMPARE(rightTile->relativeGeometry(), RectF(0.5, 0, 0.5, 1));

    // Splitting with the same layout direction creates a sibling, not 2 children
    rightTile->split(CustomTile::LayoutDirection::Horizontal);
    auto newRightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().last());

    QCOMPARE(m_rootTile->childCount(), 3);
    QCOMPARE(m_rootTile->relativeGeometry(), RectF(0, 0, 1, 1));
    QCOMPARE(leftTile->relativeGeometry(), RectF(0, 0, 0.5, 1));
    QCOMPARE(rightTile->relativeGeometry(), RectF(0.5, 0, 0.25, 1));
    QCOMPARE(newRightTile->relativeGeometry(), RectF(0.75, 0, 0.25, 1));

    QCOMPARE(m_rootTile->windowGeometry(), RectF(4, 4, 1272, 1016));
    QCOMPARE(leftTile->windowGeometry(), RectF(4, 4, 634, 1016));
    QCOMPARE(rightTile->windowGeometry(), RectF(642, 4, 316, 1016));
    QCOMPARE(newRightTile->windowGeometry(), RectF(962, 4, 314, 1016));

    // Splitting with a different layout direction creates 2 children in the tile
    QVERIFY(!rightTile->isLayout());
    QCOMPARE(rightTile->childCount(), 0);
    rightTile->split(CustomTile::LayoutDirection::Vertical);
    QVERIFY(rightTile->isLayout());
    QCOMPARE(rightTile->childCount(), 2);
    auto verticalTopTile = qobject_cast<CustomTile *>(rightTile->childTiles().first());
    auto verticalBottomTile = qobject_cast<CustomTile *>(rightTile->childTiles().last());

    // geometry of rightTile should be the same
    QCOMPARE(m_rootTile->childCount(), 3);
    QCOMPARE(rightTile->relativeGeometry(), RectF(0.5, 0, 0.25, 1));
    QCOMPARE(rightTile->windowGeometry(), RectF(642, 4, 316, 1016));

    QCOMPARE(verticalTopTile->relativeGeometry(), RectF(0.5, 0, 0.25, 0.5));
    QCOMPARE(verticalBottomTile->relativeGeometry(), RectF(0.5, 0.5, 0.25, 0.5));
    QCOMPARE(verticalTopTile->windowGeometry(), RectF(642, 4, 316, 506));
    QCOMPARE(verticalBottomTile->windowGeometry(), RectF(642, 514, 316, 506));

    // TODO: add tests for the tile flags
}

void TilesTest::testWindowInteraction()
{
    createComplexLayout();
    // Test that resizing a tile resizes the contained window and resizes the neighboring tiles as well
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());

    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));

    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);

    auto rootWindow = Test::renderAndWaitForShown(surface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);
    QSignalSpy frameGeometryChangedSpy(rootWindow, &Window::frameGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 1);

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(leftTile);

    leftTile->manage(rootWindow);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 2);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), RectF(leftTile->windowGeometry().toRect()));

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), leftTile->windowGeometry().toRect().size());

    // Resize owning tile
    leftTile->setRelativeGeometry({0, 0, 0.4, 1});

    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 3);

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), leftTile->windowGeometry().toRect().size());

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), leftTile->windowGeometry().toRect().size());

    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), RectF(leftTile->windowGeometry().toRect()));

    auto middleTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    QVERIFY(middleTile);
    auto rightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[2]);
    QVERIFY(rightTile);
    auto verticalTopTile = qobject_cast<CustomTile *>(middleTile->childTiles().first());
    QVERIFY(verticalTopTile);
    auto verticalBottomTile = qobject_cast<CustomTile *>(middleTile->childTiles().last());
    QVERIFY(verticalBottomTile);

    QCOMPARE(leftTile->relativeGeometry(), RectF(0, 0, 0.4, 1));
    QCOMPARE(middleTile->relativeGeometry(), RectF(0.4, 0, 0.35, 1));
    QCOMPARE(rightTile->relativeGeometry(), RectF(0.75, 0, 0.25, 1));
    QCOMPARE(verticalTopTile->relativeGeometry(), RectF(0.4, 0, 0.35, 0.5));
    QCOMPARE(verticalBottomTile->relativeGeometry(), RectF(0.4, 0.5, 0.35, 0.5));
}

void TilesTest::testAssignedTileDeletion()
{
    createComplexLayout();

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(leftTile);
    leftTile->setRelativeGeometry({0, 0, 0.4, 1});

    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());

    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));

    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);

    auto rootWindow = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);
    QSignalSpy frameGeometryChangedSpy(rootWindow, &Window::frameGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 1);
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    auto middleTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    QVERIFY(middleTile);
    auto middleBottomTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]->childTiles()[1]);
    QVERIFY(middleBottomTile);

    middleBottomTile->manage(rootWindow);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 2);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), RectF(middleBottomTile->windowGeometry().toRect()));

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), middleBottomTile->windowGeometry().toRect().size());

    QCOMPARE(middleBottomTile->windowGeometry().toRect(), Rect(514, 514, 444, 506));

    middleBottomTile->remove();

    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 3);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    // The window has been reassigned to middleTile after deletion of the children
    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), middleTile->windowGeometry().toRect().size());

    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), RectF(middleTile->windowGeometry().toRect()));

    // Both children have been deleted as the system avoids tiles with ha single child
    QCOMPARE(middleTile->isLayout(), false);
    QCOMPARE(middleTile->childCount(), 0);
    QCOMPARE(rootWindow->tile(), middleTile);
}

void TilesTest::resizeTileFromWindow()
{
    createComplexLayout();

    auto middleBottomTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]->childTiles()[1]);
    QVERIFY(middleBottomTile);
    middleBottomTile->remove();

    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());

    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));

    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);

    Test::XdgToplevel::States states;
    auto window = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(window);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QVERIFY(frameGeometryChangedSpy.isValid());
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 1);
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(leftTile);
    leftTile->setRelativeGeometry({0, 0, 0.4, 1});
    QCOMPARE(leftTile->windowGeometry(), RectF(4, 4, 506, 1016));

    auto middleTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    QVERIFY(middleTile);
    QCOMPARE(middleTile->windowGeometry(), RectF(514, 4, 444, 1016));

    leftTile->split(CustomTile::LayoutDirection::Vertical);
    auto topLeftTile = qobject_cast<CustomTile *>(leftTile->childTiles().first());
    QVERIFY(topLeftTile);
    QCOMPARE(topLeftTile->windowGeometry(), RectF(4, 4, 506, 506));
    QSignalSpy tileGeometryChangedSpy(topLeftTile, &Tile::windowGeometryChanged);
    auto bottomLeftTile = qobject_cast<CustomTile *>(leftTile->childTiles().last());
    QVERIFY(bottomLeftTile);
    QCOMPARE(bottomLeftTile->windowGeometry(), RectF(4, 514, 506, 506));

    topLeftTile->manage(window);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 2);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), topLeftTile->windowGeometry().toRect().size());
    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), RectF(4, 4, 506, 506));

    QCOMPARE(workspace()->activeWindow(), window);
    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    QVERIFY(interactiveMoveResizeStartedSpy.isValid());
    QSignalSpy moveResizedChangedSpy(window, &Window::moveResizedChanged);
    QVERIFY(moveResizedChangedSpy.isValid());
    QSignalSpy interactiveMoveResizeFinishedSpy(window, &Window::interactiveMoveResizeFinished);
    QVERIFY(interactiveMoveResizeFinishedSpy.isValid());

    // begin resize
    QCOMPARE(workspace()->moveResizeWindow(), nullptr);
    QCOMPARE(window->isInteractiveMove(), false);
    QCOMPARE(window->isInteractiveResize(), false);
    workspace()->slotWindowResize();
    QCOMPARE(workspace()->moveResizeWindow(), window);
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 1);
    QCOMPARE(moveResizedChangedSpy.count(), 1);
    QCOMPARE(window->isInteractiveResize(), true);
    QCOMPARE(window->geometryRestore(), RectF(0, 0, 100, 100));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 3);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    // Trigger a change.
    QPoint cursorPos = window->frameGeometry().bottomRight().toPoint();
    input()->pointer()->warp(cursorPos + QPoint(8, 0));
    window->updateInteractiveMoveResize(Cursors::self()->mouse()->pos(), Qt::KeyboardModifiers());
    QCOMPARE(Cursors::self()->mouse()->pos(), cursorPos + QPoint(8, 0));

    // The client should receive a configure event with the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 4);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(516, 508));

    // Now render new size.
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), RectF(4, 4, 516, 508));

    QTRY_COMPARE(tileGeometryChangedSpy.count(), 2);
    QCOMPARE(window->tile(), topLeftTile);
    QCOMPARE(topLeftTile->windowGeometry(), Rect(4, 4, 516, 508));
    QCOMPARE(bottomLeftTile->windowGeometry(), Rect(4, 516, 516, 504));
    QCOMPARE(leftTile->windowGeometry(), Rect(4, 4, 516, 1016));
    QCOMPARE(middleTile->windowGeometry(), Rect(524, 4, 434, 1016));

    // Resize vertically
    workspace()->slotWindowResize();
    QCOMPARE(workspace()->moveResizeWindow(), window);
    QCOMPARE(interactiveMoveResizeStartedSpy.count(), 2);
    QCOMPARE(moveResizedChangedSpy.count(), 3);
    QCOMPARE(window->isInteractiveResize(), true);
    QCOMPARE(window->geometryRestore(), RectF(0, 0, 100, 100));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 5);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 5);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));

    // Trigger a change.
    cursorPos = window->frameGeometry().bottomRight().toPoint();
    input()->pointer()->warp(cursorPos + QPoint(0, 8));
    window->updateInteractiveMoveResize(Cursors::self()->mouse()->pos(), Qt::KeyboardModifiers());
    QCOMPARE(Cursors::self()->mouse()->pos(), cursorPos + QPoint(0, 8));

    // The client should receive a configure event with the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 6);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 6);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(518, 518));

    // Now render new size.
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), RectF(4, 4, 518, 518));

    QTRY_COMPARE(tileGeometryChangedSpy.count(), 5);
    QCOMPARE(window->tile(), topLeftTile);
    QCOMPARE(topLeftTile->windowGeometry(), Rect(4, 4, 518, 518));
    QCOMPARE(bottomLeftTile->windowGeometry(), Rect(4, 526, 518, 494));
    QCOMPARE(leftTile->windowGeometry(), Rect(4, 4, 518, 1016));
    QCOMPARE(middleTile->windowGeometry(), Rect(526, 4, 432, 1016));
}

void TilesTest::shortcuts()
{
    createComplexLayout();

    // Our tile layout
    // | | | |
    // | |-| |
    // | | | |
    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[0]);
    auto centerTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    auto rightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[2]);
    auto topCenterTile = qobject_cast<CustomTile *>(centerTile->childTiles()[0]);
    auto bottomCenterTile = qobject_cast<CustomTile *>(centerTile->childTiles()[1]);

    // Create a window, don't tile yet
    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));

    auto window = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(window);

    // Trigger the shortcut, window should be tiled now
    // |w| | |
    // |w|-| |
    // |w| | |
    window->handleCustomQuickTileShortcut(QuickTileFlag::Left);
    QCOMPARE(window->requestedTile(), leftTile);
    QVERIFY(leftTile->windows().contains(window));

    // Make the window move around the grid
    // | |w| |
    // | |-| |
    // | | | |
    window->handleCustomQuickTileShortcut(QuickTileFlag::Right);
    QCOMPARE(window->requestedTile(), topCenterTile);
    QVERIFY(!leftTile->windows().contains(window));
    QVERIFY(topCenterTile->windows().contains(window));

    // | | |w|
    // | |-|w|
    // | | |w|
    window->handleCustomQuickTileShortcut(QuickTileFlag::Right);
    QCOMPARE(window->requestedTile(), rightTile);
    QVERIFY(!topCenterTile->windows().contains(window));
    QVERIFY(rightTile->windows().contains(window));

    // Right doesn't do anything now
    // | | |w|
    // | |-|w|
    // | | |w|
    window->handleCustomQuickTileShortcut(QuickTileFlag::Right);
    QCOMPARE(window->requestedTile(), rightTile);
    QVERIFY(!topCenterTile->windows().contains(window));
    QVERIFY(rightTile->windows().contains(window));

    // | |w| |
    // | |-| |
    // | | | |
    window->handleCustomQuickTileShortcut(QuickTileFlag::Left);
    QCOMPARE(window->requestedTile(), topCenterTile);
    QVERIFY(!rightTile->windows().contains(window));
    QVERIFY(topCenterTile->windows().contains(window));

    // | | | |
    // | |-| |
    // | |w| |
    window->handleCustomQuickTileShortcut(QuickTileFlag::Bottom);
    QCOMPARE(window->requestedTile(), bottomCenterTile);
    QVERIFY(!topCenterTile->windows().contains(window));
    QVERIFY(bottomCenterTile->windows().contains(window));

    // |w| | |
    // |w|-| |
    // |w| | |
    window->handleCustomQuickTileShortcut(QuickTileFlag::Left);
    QCOMPARE(window->requestedTile(), leftTile);
    QVERIFY(!bottomCenterTile->windows().contains(window));
    QVERIFY(leftTile->windows().contains(window));
}

void TilesTest::testPerDesktopTiles()
{
    auto rootTileD1 = m_tileManager->rootTile(VirtualDesktopManager::self()->desktops()[0]);
    auto rightTileD1 = rootTileD1->childTiles()[1];

    auto rootTileD2 = m_tileManager->rootTile(VirtualDesktopManager::self()->desktops()[1]);
    auto leftTileD2 = rootTileD2->childTiles()[0];

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 100), Qt::cyan);
    window->setOnAllDesktops(true);

    QSignalSpy tileChangedSpy(window, &Window::tileChanged);
    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    auto ackConfigure = [&]() {
        QVERIFY(surfaceConfigureRequestedSpy.wait());
        root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    };

    // Add the window to a tile in desktop 1
    {
        rightTileD1->manage(window);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), rightTileD1);
        QCOMPARE(window->frameGeometry(), RectF(rightTileD1->windowGeometry()));
    }

    // Set current Desktop 2
    {
        VirtualDesktopManager::self()->setCurrent(2);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), nullptr);
        QCOMPARE(window->frameGeometry(), RectF(0, 0, 100, 100));
    }

    // Set a new tile for Desktop 2
    {
        leftTileD2->manage(window);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), leftTileD2);
        QCOMPARE(window->frameGeometry(), RectF(leftTileD2->windowGeometry()));
    }

    // Go back to desktop 1, we go back to rightTileD1
    {
        VirtualDesktopManager::self()->setCurrent(1);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), rightTileD1);
        QCOMPARE(window->frameGeometry(), RectF(rightTileD1->windowGeometry()));
    }

    // Switch to desktop 2
    {
        VirtualDesktopManager::self()->setCurrent(2);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), leftTileD2);
        QCOMPARE(window->frameGeometry(), RectF(leftTileD2->windowGeometry()));
    }
}

void TilesTest::sendToOutput()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 100), Qt::cyan);
    window->setOnAllDesktops(true);

    QSignalSpy tileChangedSpy(window, &Window::tileChanged);
    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    auto ackConfigure = [&]() {
        QVERIFY(surfaceConfigureRequestedSpy.wait());
        root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    };

    const auto desktops = VirtualDesktopManager::self()->desktops();
    const auto outputs = workspace()->outputs();
    Tile *firstTile = workspace()->rootTile(outputs[0], desktops[0])->childTiles()[0];
    Tile *secondTile = workspace()->rootTile(outputs[0], desktops[1])->childTiles()[1];

    // Tile window on desktop 1
    {
        VirtualDesktopManager::self()->setCurrent(desktops[0]);
        firstTile->manage(window);
        QCOMPARE(window->tile(), nullptr);
        QCOMPARE(window->requestedTile(), firstTile);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), firstTile);
        QCOMPARE(window->requestedTile(), firstTile);
        QCOMPARE(window->frameGeometry(), RectF(firstTile->windowGeometry()));
    }

    // Tile window on desktop 2
    {
        VirtualDesktopManager::self()->setCurrent(desktops[1]);
        secondTile->manage(window);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), secondTile);
        QCOMPARE(window->requestedTile(), secondTile);
        QCOMPARE(window->frameGeometry(), RectF(secondTile->windowGeometry()));
    }

    // Send window to the second output
    {
        window->sendToOutput(outputs[1]);
        QVERIFY(!firstTile->windows().contains(window));
        QVERIFY(!secondTile->windows().contains(window));
        QCOMPARE(window->tile(), secondTile);
        QCOMPARE(window->requestedTile(), nullptr);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), nullptr);
        QCOMPARE(window->requestedTile(), nullptr);
    }
}

void TilesTest::sendToOutputX11()
{
#if KWIN_BUILD_X11
    Test::XcbConnectionPtr c = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(c.get()));
    X11Window *window = createWindow(c.get(), Rect(0, 0, 100, 200));
    window->setOnAllDesktops(true);

    const auto desktops = VirtualDesktopManager::self()->desktops();
    const auto outputs = workspace()->outputs();
    Tile *firstTile = workspace()->rootTile(outputs[0], desktops[0])->childTiles()[0];
    Tile *secondTile = workspace()->rootTile(outputs[0], desktops[1])->childTiles()[1];

    // Tile window on desktop 1
    {
        VirtualDesktopManager::self()->setCurrent(desktops[0]);
        firstTile->manage(window);
        QCOMPARE(window->tile(), firstTile);
        QCOMPARE(window->requestedTile(), firstTile);
        QCOMPARE(window->frameGeometry(), RectF(firstTile->windowGeometry()));
    }

    // Tile window on desktop 2
    {
        VirtualDesktopManager::self()->setCurrent(desktops[1]);
        secondTile->manage(window);
        QCOMPARE(window->tile(), secondTile);
        QCOMPARE(window->requestedTile(), secondTile);
        QCOMPARE(window->frameGeometry(), RectF(secondTile->windowGeometry()));
    }

    // Send window to the second output
    {
        window->sendToOutput(outputs[1]);
        QVERIFY(!firstTile->windows().contains(window));
        QVERIFY(!secondTile->windows().contains(window));
        QCOMPARE(window->tile(), nullptr);
        QCOMPARE(window->requestedTile(), nullptr);
    }
#endif
}

void TilesTest::tileAndMaximize()
{
    // This tests the interaction between the tiled and maximized states
    auto rootTileD2 = m_tileManager->rootTile(VirtualDesktopManager::self()->desktops()[1]);
    auto rightTileD2 = qobject_cast<CustomTile *>(rootTileD2->childTiles()[1]);

    auto leftQuickTileD1 = m_tileManager->quickTile(QuickTileFlag::Left);
    QVERIFY(!leftQuickTileD1->isTransient());

    const QList<LogicalOutput *> outputs = workspace()->outputs();
    TileManager *out2TileMan = workspace()->tileManager(outputs[1]);
    auto rootTileD3O2 = out2TileMan->rootTile(VirtualDesktopManager::self()->desktops()[2]);
    auto leftTileD3O2 = qobject_cast<CustomTile *>(rootTileD3O2->childTiles()[0]);

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 100), Qt::cyan);
    window->setOnAllDesktops(true);

    QSignalSpy tileChangedSpy(window, &Window::tileChanged);
    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    auto ackConfigure = [&]() {
        QVERIFY(surfaceConfigureRequestedSpy.wait());
        root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    };

    // Add the window to a quick tile in desktop 1 and a custom tile on desktop 2
    {
        leftQuickTileD1->manage(window);
        rightTileD2->manage(window);
        QCOMPARE(window->requestedTile(), leftQuickTileD1);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), leftQuickTileD1);
        QCOMPARE(window->frameGeometry(), RectF(leftQuickTileD1->windowGeometry()));
    }

    // Set current Desktop 2
    {
        VirtualDesktopManager::self()->setCurrent(2);
        // Tile becomes rightTileD2
        QCOMPARE(window->requestedTile(), rightTileD2);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), rightTileD2);
        QCOMPARE(window->frameGeometry(), RectF(rightTileD2->windowGeometry()));
    }

    // Add the window also on a tile of another output
    leftTileD3O2->manage(window);

    // Maximize the window, it should lose its tile
    {
        window->maximize(MaximizeFull);
        // No requestedTile anymore
        QCOMPARE(window->requestedTile(), nullptr);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), nullptr);
        QCOMPARE(window->frameGeometry(), RectF(0, 0, 1280, 1024));

        // Both tiles have an empty window list now
        QVERIFY(leftQuickTileD1->windows().isEmpty());
        QVERIFY(rightTileD2->windows().isEmpty());
        // Also the tile on the other output llost the window
        QVERIFY(leftTileD3O2->windows().isEmpty());
    }

    // Set a tile again, it should unmaximize
    {
        rightTileD2->manage(window);
        QCOMPARE(window->requestedTile(), rightTileD2);
        ackConfigure();
        QVERIFY(tileChangedSpy.wait());
        QCOMPARE(window->tile(), rightTileD2);
        QCOMPARE(window->maximizeMode(), MaximizeRestore);
        QCOMPARE(window->frameGeometry(), RectF(rightTileD2->windowGeometry()));
    }
}

void TilesTest::evacuateFromRemovedDesktop()
{
    const auto desktops = VirtualDesktopManager::self()->desktops();
    auto rightTileD2 = m_tileManager->rootTile(desktops[1])->childTiles()[1];
    auto leftTileD3 = m_tileManager->rootTile(desktops[2])->childTiles()[0];

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 100), Qt::cyan);
    window->setOnAllDesktops(true);

    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());

    auto ackConfigure = [&]() {
        QVERIFY(surfaceConfigureRequestedSpy.wait());
        root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
        QVERIFY(Test::waylandSync());
    };

    // Set current Desktop 2
    // Add the window to a tile in desktop 2 and desktop 3
    {
        VirtualDesktopManager::self()->setCurrent(desktops[1]);
        rightTileD2->manage(window);
        leftTileD3->manage(window);
        QCOMPARE(window->requestedTile(), rightTileD2);
        ackConfigure();
        QCOMPARE(window->tile(), rightTileD2);
        QCOMPARE(window->frameGeometry(), RectF(rightTileD2->windowGeometry()));
    }

    // Set current Desktop 3
    {
        VirtualDesktopManager::self()->setCurrent(desktops[2]);
        // Tile becomes leftTileD3
        QCOMPARE(window->requestedTile(), leftTileD3);
        ackConfigure();
        QCOMPARE(window->tile(), leftTileD3);
        QCOMPARE(window->frameGeometry(), RectF(leftTileD3->windowGeometry()));
    }

    // Remove the current desktop 3, the window will be tiled again to rightTileD2
    {
        VirtualDesktopManager::self()->removeVirtualDesktop(desktops[2]);
        QCOMPARE(window->requestedTile(), rightTileD2);
        ackConfigure();
        QCOMPARE(window->tile(), rightTileD2);
        QCOMPARE(window->frameGeometry(), RectF(rightTileD2->windowGeometry()));
    }

    // Remove the current desktop 2, the window is now untiles
    {
        VirtualDesktopManager::self()->removeVirtualDesktop(desktops[1]);
        QCOMPARE(window->requestedTile(), nullptr);
        ackConfigure();
        QCOMPARE(window->tile(), nullptr);
        QCOMPARE(window->frameGeometry(), RectF(0, 0, 100, 100));
    }
}

void TilesTest::splitFromSingleTileBelowRoot()
{
    // This test verifies that split works correctly when starting from a single tile below root
    // and splitting first in different direction than root and then in the same direction.

    while (m_rootTile->childCount() > 0) {
        static_cast<CustomTile *>(m_rootTile->childTile(0))->remove();
    }

    m_rootTile->setLayoutDirection(Tile::LayoutDirection::Horizontal);
    auto topLevelTile = m_rootTile->createChildAt(m_rootTile->relativeGeometry(), m_rootTile->layoutDirection(), 0);
    auto topLevelTiles = topLevelTile->split(Tile::LayoutDirection::Vertical);
    QCOMPARE(topLevelTiles.count(), 2);
    QCOMPARE(m_rootTile->childCount(), 2);
    QVERIFY(topLevelTiles.contains(topLevelTile));
    // The root tile has to have a correct layout, otherwise it won't be saved properly.
    // Horizontal layout saves width, which is 1 for both tiles = incorrect layout.
    QCOMPARE(m_rootTile->layoutDirection(), Tile::LayoutDirection::Vertical);

    // Split the top-half tile. It should be the parent instead of the root tile due to different direction.
    auto leafTiles = topLevelTiles[0]->split(Tile::LayoutDirection::Horizontal);
    QCOMPARE(leafTiles.count(), 2);
    QCOMPARE(m_rootTile->childCount(), 2);
    QCOMPARE(topLevelTiles[0]->childCount(), 2);
}

void TilesTest::requestedGeometryAfterUntileBeforeCommit()
{
    // A client may acknowledge a tile configure and commit the matching buffer only after the
    // window has been untiled. The committed frame then has the tile's size while the floating
    // geometry has already been requested again. requestedGeometry tells the two apart until
    // the client has answered the restore configure.
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);

    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(window);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QSignalSpy requestedGeometryChangedSpy(window, &Window::requestedGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(Test::waylandSync());

    const RectF floatingGeometry = window->frameGeometry();
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), floatingGeometry);
    requestedGeometryChangedSpy.clear();

    // Tile the window: the tile's geometry is requested at once, the frame follows on commit.
    auto tile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(tile);
    const RectF tileGeometry = tile->windowGeometry();
    QVERIFY(tileGeometry.size() != floatingGeometry.size());
    QVERIFY(tile->manage(window));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    const quint32 tileSerial = surfaceConfigureRequestedSpy.last().at(0).value<quint32>();
    const QSize tileSize = toplevelConfigureRequestedSpy.last().first().value<QSize>();
    QCOMPARE(tileSize, tileGeometry.size().toSize());
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), tileGeometry);
    QCOMPARE(requestedGeometryChangedSpy.count(), 1);
    QCOMPARE(window->frameGeometry(), floatingGeometry);

    // The client acknowledges the tile configure, but the window is untiled before it commits.
    shellSurface->xdgSurface()->ack_configure(tileSerial);
    requestedGeometryChangedSpy.clear();
    QVERIFY(tile->unmanage(window));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    const quint32 restoreSerial = surfaceConfigureRequestedSpy.last().at(0).value<quint32>();
    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), floatingGeometry.size().toSize());
    QCOMPARE(window->requestedTile(), nullptr);
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), floatingGeometry);
    QCOMPARE(requestedGeometryChangedSpy.count(), 1);

    // The late commit of the acknowledged tile configure gives the frame the tile's size, but
    // the requested geometry stays the floating one.
    frameGeometryChangedSpy.clear();
    Test::render(surface.get(), tileSize, Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().size(), tileGeometry.size());
    QCOMPARE(window->tile(), tile);
    QCOMPARE(window->requestedTile(), nullptr);
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), floatingGeometry);
    QVERIFY(window->frameGeometry() != window->property("requestedGeometry").value<RectF>());

    // Once the client answers the restore configure, the two are equal again, and the
    // requested geometry has not changed on the way.
    requestedGeometryChangedSpy.clear();
    shellSurface->xdgSurface()->ack_configure(restoreSerial);
    Test::render(surface.get(), floatingGeometry.size().toSize(), Qt::cyan);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), floatingGeometry);
    QCOMPARE(window->property("requestedGeometry").value<RectF>(), floatingGeometry);
    QVERIFY(requestedGeometryChangedSpy.isEmpty());
}

void TilesTest::requestedGeometryChangedAfterX11Tile()
{
#if KWIN_BUILD_X11
    // An X11 window's frame follows a tile at once. requestedGeometryChanged comes after the
    // change has been applied, so a handler already sees the new frame geometry.
    Test::XcbConnectionPtr c = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(c.get()));
    X11Window *window = createWindow(c.get(), Rect(0, 0, 100, 200));
    QVERIFY(window);
    const RectF originalGeometry = window->frameGeometry();

    auto tile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(tile);
    int notifications = 0;
    RectF frameGeometryAtNotification;
    RectF requestedGeometryAtNotification;
    QObject context;
    connect(window, &Window::requestedGeometryChanged, &context, [&] {
        ++notifications;
        frameGeometryAtNotification = window->frameGeometry();
        requestedGeometryAtNotification = window->property("requestedGeometry").value<RectF>();
    });

    QVERIFY(tile->manage(window));
    QCOMPARE(notifications, 1);
    QCOMPARE(window->frameGeometry(), tile->windowGeometry());
    QVERIFY(window->frameGeometry() != originalGeometry);
    QCOMPARE(frameGeometryAtNotification, window->frameGeometry());
    QCOMPARE(requestedGeometryAtNotification, tile->windowGeometry());
#else
    QSKIP("KWin was built without X11 support");
#endif
}

}

WAYLANDTEST_MAIN(KWin::TilesTest)
#include "tiles_test.moc"
