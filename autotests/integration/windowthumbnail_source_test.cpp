/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "scripting/windowthumbnailitem.h"
#include "wayland_server.h"
#include "window.h"

#include <KWayland/Client/surface.h>
#include <QQuickWindow>

namespace KWin
{

// Access QObject's diagnostic through its member pointer, without casting the sender
// to a derived type or depending on private Qt connection structures.
class ConnectionInspector : public QObject
{
public:
    static auto receiverCounter()
    {
        return &ConnectionInspector::receivers;
    }
};

static int destructionConnections(const QObject *sender)
{
    return (sender->*ConnectionInspector::receiverCounter())(SIGNAL(destroyed(QObject *)));
}

class WindowThumbnailSourceTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void testLifecycleCapabilityVersion();
    void testReuseAndRecreation();
    void testIndependentViews();
    void testIndependentClients();
    void testViewDestroyedFirst();
    void testClientDestroyedFirst();

private:
    std::unique_ptr<KWayland::Client::Surface> m_surface;
    std::unique_ptr<Test::XdgToplevel> m_toplevel;
    QPointer<Window> m_window;
};

void WindowThumbnailSourceTest::initTestCase()
{
    QVERIFY(waylandServer()->init(qAppName()));
    kwinApp()->start();
    Test::setOutputConfig({Rect(0, 0, 1280, 1024)});
}

void WindowThumbnailSourceTest::init()
{
    QVERIFY(Test::setupWaylandConnection());
    m_surface = Test::createSurface();
    m_toplevel = Test::createXdgToplevelSurface(m_surface.get());
    m_window = Test::renderAndWaitForShown(m_surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(m_window);
}

void WindowThumbnailSourceTest::cleanup()
{
    m_toplevel.reset();
    m_surface.reset();
    Test::destroyWaylandConnection();
}

void WindowThumbnailSourceTest::testLifecycleCapabilityVersion()
{
    // The downstream capability is visible before a client is assigned and
    // describes the lifecycle behavior that the tests below verify.
    const QMetaObject &metaObject = WindowThumbnailItem::staticMetaObject;
    const int index = metaObject.indexOfProperty("plasmaFusionThumbnailSourceLifecycleVersion");
    QVERIFY(index >= 0);
    const QMetaProperty property = metaObject.property(index);
    QVERIFY(property.isValid());
    QVERIFY(property.isReadable());
    QVERIFY(!property.isWritable());
    QVERIFY(property.isConstant());
    QCOMPARE(property.typeId(), QMetaType::Int);

    WindowThumbnailItem item;
    QVERIFY(!item.client());
    QCOMPARE(item.property("plasmaFusionThumbnailSourceLifecycleVersion").toInt(), 1);
    QCOMPARE(item.plasmaFusionThumbnailSourceLifecycleVersion(), 1);
}

void WindowThumbnailSourceTest::testReuseAndRecreation()
{
    QQuickWindow view;
    const int viewBaseline = destructionConnections(&view);
    const int clientBaseline = destructionConnections(m_window);

    auto source = WindowThumbnailSource::getOrCreate(&view, m_window);
    auto reused = WindowThumbnailSource::getOrCreate(&view, m_window);
    QCOMPARE(source, reused);
    std::weak_ptr<WindowThumbnailSource> weak = source;
    source.reset();
    QVERIFY(!weak.expired());
    reused.reset();
    QVERIFY(weak.expired());

    for (int i = 0; i < 20; ++i) {
        auto recreated = WindowThumbnailSource::getOrCreate(&view, m_window);
        QCOMPARE(destructionConnections(&view), viewBaseline + 1);
        QCOMPARE(destructionConnections(m_window), clientBaseline + 1);
        recreated.reset();
    }
}

void WindowThumbnailSourceTest::testIndependentViews()
{
    QQuickWindow firstView;
    QQuickWindow secondView;
    const int clientBaseline = destructionConnections(m_window);
    auto first = WindowThumbnailSource::getOrCreate(&firstView, m_window);
    auto second = WindowThumbnailSource::getOrCreate(&secondView, m_window);
    QVERIFY(first != second);
    QCOMPARE(destructionConnections(m_window), clientBaseline + 2);
    first.reset();
    auto recreated = WindowThumbnailSource::getOrCreate(&firstView, m_window);
    QCOMPARE(destructionConnections(m_window), clientBaseline + 2);
    QCOMPARE(WindowThumbnailSource::getOrCreate(&secondView, m_window), second);
}

void WindowThumbnailSourceTest::testIndependentClients()
{
    QQuickWindow view;
    const int baseline = destructionConnections(&view);
    auto surface = Test::createSurface();
    auto toplevel = Test::createXdgToplevelSurface(surface.get());
    Window *otherWindow = Test::renderAndWaitForShown(surface.get(), QSize(90, 50), Qt::red);
    QVERIFY(otherWindow);

    auto first = WindowThumbnailSource::getOrCreate(&view, m_window);
    auto second = WindowThumbnailSource::getOrCreate(&view, otherWindow);
    QVERIFY(first != second);
    QCOMPARE(destructionConnections(&view), baseline + 2);
    first.reset();
    first = WindowThumbnailSource::getOrCreate(&view, m_window);
    QCOMPARE(destructionConnections(&view), baseline + 2);
    QCOMPARE(WindowThumbnailSource::getOrCreate(&view, otherWindow), second);

    first.reset();
    second.reset();
    toplevel.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowClosed(otherWindow));
    QCOMPARE(destructionConnections(&view), baseline + 1);
}

void WindowThumbnailSourceTest::testViewDestroyedFirst()
{
    const int clientBaseline = destructionConnections(m_window);
    auto view = std::make_unique<QQuickWindow>();
    auto source = WindowThumbnailSource::getOrCreate(view.get(), m_window);
    source.reset();
    view.reset();
    QCOMPARE(destructionConnections(m_window), clientBaseline);

    view = std::make_unique<QQuickWindow>();
    const int viewBaseline = destructionConnections(view.get());
    source = WindowThumbnailSource::getOrCreate(view.get(), m_window);
    QCOMPARE(destructionConnections(view.get()), viewBaseline + 1);
    QCOMPARE(destructionConnections(m_window), clientBaseline + 1);
}

void WindowThumbnailSourceTest::testClientDestroyedFirst()
{
    QQuickWindow view;
    const int baseline = destructionConnections(&view);
    auto source = WindowThumbnailSource::getOrCreate(&view, m_window);
    source.reset();
    m_toplevel.reset();
    m_surface.reset();
    QVERIFY(Test::waitForWindowClosed(m_window));
    QCOMPARE(destructionConnections(&view), baseline);
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::WindowThumbnailSourceTest)
#include "windowthumbnail_source_test.moc"
