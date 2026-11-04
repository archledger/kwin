/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

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
#include "x11window.h"
#include <netwm.h>
#include <xcb/xcb_icccm.h>
#endif

#include <KConfigGroup>
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
    QCOMPARE(window->requestedTile(), target.data());
    QCOMPARE(target->windows(), QList<Window *>{window});
    QCOMPARE(window->moveResizeGeometry(), target->windowGeometry());
    if (x11) {
        QCOMPARE(window->tile(), target.data());
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
        // The membership revision alone changed (or nothing did): the other counters are untouched.
        QCOMPARE(window->m_requestedTileChange, requested);
        QCOMPARE(window->m_committedTileChange, committed);
        QCOMPARE(window->m_placementContinuationRevision, placement);
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
    QTest::newRow("x11-refused") << 0;
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
        const auto placement = window->m_placementContinuationRevision;
        const auto association = window->m_requestedTileChange;
        QVERIFY(!window->canRequestTileForAssignment(first));
        auto fence = window->makeTileAssignmentFence();
        const auto result = window->requestTileForAssignment(first, fence);
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QVERIFY(!window->requestedTile());
        QCOMPARE(window->m_placementContinuationRevision, placement);
        QCOMPARE(window->m_requestedTileChange, association);
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
    QTest::newRow("x11-refused") << 2;
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
        const auto association = window->m_requestedTileChange;
        auto fence = window->makeTileAssignmentFence();
        const auto result = window->forgetTileForAssignment(first, fence);
        QCOMPARE(result.outcome, Outcome::Refused);
        QVERIFY(!result.checkpoint);
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->m_requestedTileChange, association);
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
    QTest::newRow("x11-legacy") << 0;
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
        // The legacy sequence: the association, the immediate commit, one notification.
        QCOMPARE(window->requestedTile(), first);
        QCOMPARE(window->tile(), first);
        QCOMPARE(requestedTileChanged.count(), 1);
        // The legacy body's own moveResize cancels again: the stamp count is not its invariant.
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
