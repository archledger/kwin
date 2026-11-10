/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../libinput/mock_libinput.h"
#include "backends/libinput/device.h"
#include "backends/libinput/libinputbackend.h"
#include "backends/virtual/virtual_backend.h"
#include "core/backendoutput.h"
#include "core/output.h"
#include "core/outputconfiguration.h"
#include "input.h"
#include "kwin_wayland_test.h"
#include "outputconfigurationstore.h"
#include "tablet_input.h"
#include "tabletmodemanager.h"
#include "utils/orientationsensor.h"
#include "wayland_server.h"
#include "workspace.h"

#include <KConfigGroup>
#include <QSignalSpy>

using namespace KWin;

class StartupTabletSwitch : public Test::VirtualInputDevice
{
public:
    bool isTabletModeSwitch() const override
    {
        return true;
    }
};

class NativeTabletStorage : public QObject
{
public:
    NativeTabletStorage(std::shared_ptr<libinput_device> native, QObject *parent)
        : QObject(parent)
        , device(std::move(native))
    {
    }

    const std::shared_ptr<libinput_device> device;
};

class TabletModeStartupTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testInitiallyAvailable();
    void testStartupDeviceConfiguration();
    void testRelativeStartupPosition();
    void testSwitchState_data();
    void testSwitchState();

private:
    StartupTabletSwitch m_switch;
    bool m_registered = false;
    bool m_beforeManager = false;
    std::shared_ptr<libinput_device> m_nativeTablet;
    libinput *m_nativeContext = nullptr;
    LibinputBackend *m_backend = nullptr;
    QPointer<LibInput::Device> m_device;
    QString m_outputUuid;
    bool m_relativeAtAnnouncement = false;
    bool m_unmappedAtAnnouncement = false;
};

void TabletModeStartupTest::initTestCase()
{
    auto app = qobject_cast<WaylandTestApplication *>(kwinApp());
    QVERIFY(app);
    qputenv("KWIN_LIBINPUT_NO_PLUGINS", "1");
    app->setInputConfig(KSharedConfig::openConfig(QStringLiteral("startup-inputrc"), KConfig::SimpleConfig));
    KConfigGroup(kwinApp()->config(), QStringLiteral("Input")).writeEntry("TabletMode", "auto");
    KConfigGroup(kwinApp()->inputConfig(), QStringLiteral("Tablet")).writeEntry("SyncWithMouse", false);
    connect(app, &WaylandTestApplication::inputDevicesCreated, this, [this]() {
        m_beforeManager = input() && !kwinApp()->tabletModeManager();
        input()->addInputDevice(&m_switch);
        m_registered = true;

        auto virtualBackend = qobject_cast<VirtualBackend *>(kwinApp()->outputBackend());
        QVERIFY(virtualBackend);
        virtualBackend->setVirtualOutputs({VirtualBackend::OutputInfo{.size = QSize(1280, 1024)}});
        const auto outputs = virtualBackend->outputs();
        QVERIFY(!outputs.isEmpty());
        // Model a saved output binding with a real, persisted UUID while the
        // workspace is still absent; a newly created virtual output has none.
        {
            OutputConfigurationStore store;
            const auto config = store.queryConfig(outputs, false, AccelerometerOrientation::Undefined, false);
            QVERIFY(config.has_value());
            outputs.constFirst()->applyChanges(config->first);
        }
        m_outputUuid = outputs.constFirst()->uuid();
        QVERIFY(!m_outputUuid.isEmpty());

        m_nativeTablet = std::make_shared<libinput_device>();
        m_nativeTablet->tabletTool = true;
        m_nativeTablet->name = QByteArrayLiteral("Startup relative tablet");
        m_nativeTablet->sysName = QByteArrayLiteral("eventTabletStartup");
        auto config = kwinApp()->inputConfig()->group(QStringLiteral("Libinput")).group(QStringLiteral("0")).group(QStringLiteral("0")).group(QString::fromUtf8(m_nativeTablet->name));
        config.writeEntry("TabletToolRelativeMode", true);
        config.writeEntry("OutputUuid", m_outputUuid);
        config.writeEntry("OutputName", outputs.constFirst()->name());

        auto context = std::make_unique<libinput>();
        QVERIFY(context->fd >= 0);
        m_nativeContext = context.get();
        mockLibinputSetNextContext(std::move(context));
        auto backend = std::make_unique<LibinputBackend>(kwinApp()->session());
        m_backend = backend.get();
        // QObject children outlive the backend's derived destructor, which joins
        // its worker and destroys the Connection and its Device wrappers.
        new NativeTabletStorage(m_nativeTablet, m_backend);
        connect(m_backend, &InputBackend::deviceAdded, this, [this](InputDevice *device) {
            m_device = qobject_cast<LibInput::Device *>(device);
            m_relativeAtAnnouncement = device->tabletToolIsRelative();
            m_unmappedAtAnnouncement = m_device && !m_device->output();
        });
        auto event = std::make_unique<libinput_event>();
        event->type = LIBINPUT_EVENT_DEVICE_ADDED;
        event->device = m_nativeTablet.get();
        mockLibinputEnqueueEvent(m_nativeContext, std::move(event));
        input()->addInputBackend(std::move(backend));
    });
    QVERIFY(waylandServer()->init(qAppName()));
    app->start();
    QVERIFY(m_registered);
    QVERIFY(m_beforeManager);
    QVERIFY(input()->hasTabletModeSwitch());
    QVERIFY(!input()->syncTabletWithMouse());
    QVERIFY(m_device);
    QCOMPARE(m_device->device(), m_nativeTablet.get());
    QCoreApplication::sendPostedEvents(m_backend, QEvent::MetaCall);
}

void TabletModeStartupTest::cleanupTestCase()
{
    if (m_registered) {
        input()->removeInputDevice(&m_switch);
        m_registered = false;
    }
    if (m_device) {
        auto event = std::make_unique<libinput_event>();
        event->type = LIBINPUT_EVENT_DEVICE_REMOVED;
        event->device = m_nativeTablet.get();
        mockLibinputEnqueueEvent(m_nativeContext, std::move(event));
        QTRY_VERIFY(m_device.isNull());
    }
}

void TabletModeStartupTest::testInitiallyAvailable()
{
    auto manager = kwinApp()->tabletModeManager();
    QVERIFY(manager);
    QCOMPARE(manager->configuredMode(), TabletModeManager::ConfiguredMode::Auto);
    QVERIFY(manager->isTabletModeAvailable());
    QVERIFY(manager->property("tabletModeAvailable").toBool());
}

void TabletModeStartupTest::testStartupDeviceConfiguration()
{
    QVERIFY(m_relativeAtAnnouncement);
    QVERIFY(m_unmappedAtAnnouncement);
    QVERIFY(m_device->tabletToolIsRelative());
    QVERIFY(m_device->output());
    QCOMPARE(m_device->output()->uuid(), m_outputUuid);
}

void TabletModeStartupTest::testRelativeStartupPosition()
{
    const QPointF center = workspace()->activeOutput()->geometry().center();
    QVERIFY(center != QPointF());
    auto app = qobject_cast<WaylandTestApplication *>(kwinApp());
    auto tool = app->virtualTabletTool();
    Q_EMIT m_device->tabletToolProximityEvent(QPointF(13, 17), 0, 0, 0, 0, true, 0, tool, std::chrono::microseconds(1), m_device.data());
    QCOMPARE(input()->tablet()->position(), center);
    const QPointF delta(7, 11);
    Q_EMIT m_device->tabletToolAxisEventRelative(delta, 0.5, 0, 0, 0, 0, false, 0, tool, std::chrono::microseconds(2), m_device.data());
    QCOMPARE(input()->tablet()->position(), center + delta);
    Q_EMIT m_device->tabletToolProximityEvent(QPointF(), 0, 0, 0, 0, false, 0, tool, std::chrono::microseconds(3), m_device.data());
}

void TabletModeStartupTest::testSwitchState_data()
{
    QTest::addColumn<bool>("tablet");
    QTest::newRow("on") << true;
    QTest::newRow("off") << false;
}

void TabletModeStartupTest::testSwitchState()
{
    QFETCH(bool, tablet);
    auto manager = kwinApp()->tabletModeManager();
    Q_EMIT m_switch.switchToggle(tablet ? SwitchState::Off : SwitchState::On, std::chrono::microseconds(1), &m_switch);
    QSignalSpy changed(manager, &TabletModeManager::tabletModeChanged);
    Q_EMIT m_switch.switchToggle(tablet ? SwitchState::On : SwitchState::Off, std::chrono::microseconds(2), &m_switch);
    QCOMPARE(manager->isTablet(), tablet);
    QCOMPARE(manager->effectiveTabletMode(), tablet);
    QCOMPARE(changed.count(), 1);
    QVERIFY(manager->isTabletModeAvailable());
}

WAYLANDTEST_MAIN(TabletModeStartupTest)
#include "tabletmode_startup_test.moc"
