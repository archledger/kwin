/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "mock_libinput.h"

#include "backends/libinput/connection.h"
#include "backends/libinput/device.h"
#include "backends/libinput/libinputbackend.h"
#include "core/session.h"

#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <poll.h>
#include <vector>

using namespace KWin;
using namespace KWin::LibInput;
using namespace std::chrono_literals;

class TestSession : public Session
{
public:
    bool isActive() const override
    {
        return true;
    }
    Capabilities capabilities() const override
    {
        return {};
    }
    QString seat() const override
    {
        return QStringLiteral("seat0");
    }
    uint terminal() const override
    {
        return 0;
    }
    std::expected<int, Error> openRestricted(const QString &) override
    {
        ++openRequests;
        return std::unexpected(Error::Other);
    }
    void closeRestricted(int) override
    {
    }
    void switchTo(uint) override
    {
    }
    std::shared_ptr<FileDescriptor> delaySleep(const QString &) override
    {
        return {};
    }

    std::atomic_int openRequests = 0;
};

class TestLibinputConnection : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void testStartupDefersInputUntilActivation();
    void testDeviceAddIsMotionBarrier();
    void testRemovalFollowsBufferedInput();
    void testNotifierAfterActivation();
    void testBackendQueuesActivationOnMainThread();
    void testActivationDuringDeviceAnnouncement();

private:
    libinput_device *addNativeDevice();
    void enqueueDeviceEvent(libinput_device *device, libinput_event_type type);
    void enqueueMotion(libinput_device *device, const QPointF &delta, std::chrono::microseconds time);
    bool nativeQueueDrained() const;

    struct Motion
    {
        InputDevice *device;
        QPointF delta;
        QPointF unaccelerated;
        std::chrono::microseconds time;
        bool leftHanded;
    };
    struct Toggle
    {
        InputDevice *device;
        SwitchState state;
        std::chrono::microseconds time;
    };

    TestSession m_session;
    QThread m_worker;
    QPointer<Connection> m_connection;
    libinput *m_context = nullptr;
    std::unique_ptr<QTemporaryDir> m_configDirectory;
    KSharedConfigPtr m_config;
    std::vector<std::unique_ptr<libinput_device>> m_nativeDevices;
    QList<Device *> m_added;
    QList<QPointer<Device>> m_deviceGuards;
    QList<Device *> m_removed;
    QList<Motion> m_motions;
    QList<Toggle> m_toggles;
    QStringList m_order;
    int m_frames = 0;
    int m_eventsRead = 0;
    std::atomic_int m_destroyedDevices = 0;
};

void TestLibinputConnection::initTestCase()
{
    // Context is real, but plugin loading must never receive a mock native handle.
    qputenv("KWIN_LIBINPUT_NO_PLUGINS", "1");
}

void TestLibinputConnection::init()
{
    m_configDirectory = std::make_unique<QTemporaryDir>();
    QVERIFY(m_configDirectory->isValid());
    m_config = KSharedConfig::openConfig(m_configDirectory->filePath(QStringLiteral("connectionrc")), KConfig::SimpleConfig);
    m_config->group(QStringLiteral("Libinput")).group(QStringLiteral("Defaults")).group(QStringLiteral("Pointer")).writeEntry("LeftHanded", true);

    auto context = std::make_unique<libinput>();
    QVERIFY(context->fd >= 0);
    m_context = context.get();
    mockLibinputSetNextContext(std::move(context));
    m_connection = Connection::create(&m_session);
    QVERIFY(m_connection);
    QCOMPARE(m_context->seat, QByteArrayLiteral("seat0"));
    m_connection->setInputConfig(m_config);

    // Match the backend: read on the worker, process on the main thread.
    connect(m_connection, &Connection::eventsRead, this, [this] {
        ++m_eventsRead;
        m_connection->processEvents();
    }, Qt::QueuedConnection);
    connect(m_connection, &Connection::deviceAdded, this, [this](Device *device) {
        m_added.append(device);
        m_deviceGuards.append(device);
        m_order.append(QStringLiteral("add:") + device->sysName());
        connect(device, &Device::pointerMotion, this, [this, device](const QPointF &delta, const QPointF &unaccelerated, std::chrono::microseconds time, InputDevice *source) {
            m_motions.append({source, delta, unaccelerated, time, device->isLeftHanded()});
            m_order.append(QStringLiteral("motion:") + device->sysName());
        });
        connect(device, &Device::pointerFrame, this, [this, device] {
            ++m_frames;
            m_order.append(QStringLiteral("frame:") + device->sysName());
        });
        connect(device, &Device::switchToggle, this, [this, device](SwitchState state, std::chrono::microseconds time, InputDevice *source) {
            m_toggles.append({source, state, time});
            m_order.append(QStringLiteral("switch:") + device->sysName());
        });
        connect(device, &QObject::destroyed, m_connection, [this] {
            ++m_destroyedDevices;
        }, Qt::DirectConnection);
    });
    connect(m_connection, &Connection::deviceRemoved, this, [this](Device *device) {
        m_removed.append(device);
        // Access the device during removal, before its deferred deletion.
        m_order.append(QStringLiteral("remove:") + device->sysName());
    });

    connect(&m_worker, &QThread::finished, m_connection, &QObject::deleteLater);
    QVERIFY(m_connection->moveToThread(&m_worker));
    m_worker.start();
    QVERIFY(m_connection->thread() != QThread::currentThread());
}

void TestLibinputConnection::cleanup()
{
    m_worker.quit();
    m_worker.wait();
    QVERIFY(m_connection.isNull());
    // A failed assertion may leave a queued eventsRead callback. Its connection
    // has now been destroyed, so discard it before the next fixture starts.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    m_context = nullptr;
    QCOMPARE(m_session.openRequests.load(), 0);
    m_nativeDevices.clear();
    m_config.reset();
    m_configDirectory.reset();
    m_added.clear();
    m_deviceGuards.clear();
    m_removed.clear();
    m_motions.clear();
    m_toggles.clear();
    m_order.clear();
    m_frames = 0;
    m_eventsRead = 0;
    m_destroyedDevices = 0;
}

libinput_device *TestLibinputConnection::addNativeDevice()
{
    auto device = std::make_unique<libinput_device>();
    device->sysName = QByteArrayLiteral("event") + QByteArray::number(static_cast<qulonglong>(m_nativeDevices.size()));
    device->name = QByteArrayLiteral("Startup pointer ") + device->sysName;
    device->pointer = true;
    device->supportsLeftHanded = true;
    const auto result = device.get();
    m_nativeDevices.push_back(std::move(device));
    return result;
}

void TestLibinputConnection::enqueueDeviceEvent(libinput_device *device, libinput_event_type type)
{
    auto event = std::make_unique<libinput_event>();
    event->type = type;
    event->device = device;
    mockLibinputEnqueueEvent(m_context, std::move(event));
}

void TestLibinputConnection::enqueueMotion(libinput_device *device, const QPointF &delta, std::chrono::microseconds time)
{
    auto event = std::make_unique<libinput_event_pointer>();
    event->type = LIBINPUT_EVENT_POINTER_MOTION;
    event->device = device;
    event->delta = delta;
    event->time = time;
    mockLibinputEnqueueEvent(m_context, std::move(event));
}

bool TestLibinputConnection::nativeQueueDrained() const
{
    pollfd fd{libinput_get_fd(m_context), POLLIN, 0};
    return mockLibinputPendingEventCount(m_context) == 0 && poll(&fd, 1, 0) == 0;
}

void TestLibinputConnection::testStartupDefersInputUntilActivation()
{
    auto native = addNativeDevice();
    native->switchDevice = true;
    native->tabletModeSwitch = true;
    enqueueDeviceEvent(native, LIBINPUT_EVENT_DEVICE_ADDED);
    enqueueMotion(native, QPointF(3, -2), 10us);
    auto toggle = std::make_unique<libinput_event_switch>();
    toggle->type = LIBINPUT_EVENT_SWITCH_TOGGLE;
    toggle->device = native;
    toggle->state = libinput_event_switch::State::On;
    toggle->time = 20us;
    mockLibinputEnqueueEvent(m_context, std::move(toggle));

    // Deliberately call setup on the main thread after moving to the worker.
    m_connection->setup();
    QCOMPARE(m_added.size(), 1);
    auto device = m_added.constFirst();
    QCOMPARE(device->device(), native);
    QCOMPARE(m_connection->devices(), QList<Device *>{device});
    QVERIFY(device->isTabletModeSwitch());
    QCOMPARE(m_motions.size(), 0);
    QCOMPARE(m_toggles.size(), 0);
    QCOMPARE(m_frames, 0);
    QVERIFY(device->isLeftHanded()); // Workspace-independent input settings are needed during integration.
    QVERIFY(nativeQueueDrained());

    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    QCOMPARE(m_eventsRead, 1);
    QCOMPARE(m_motions.size(), 0);
    QCOMPARE(m_toggles.size(), 0);
    QCOMPARE(m_frames, 0);
    QVERIFY(device->isLeftHanded());

    // No further native event or fd wakeup is supplied to activate the buffer.
    m_connection->startProcessingEvents();
    QCOMPARE(m_motions.size(), 1);
    QCOMPARE(m_motions[0].device, device);
    QCOMPARE(m_motions[0].delta, QPointF(3, -2));
    QCOMPARE(m_motions[0].unaccelerated, QPointF(3, -2));
    QCOMPARE(m_motions[0].time, 10us);
    QVERIFY(m_motions[0].leftHanded); // Config must precede replayed input.
    QCOMPARE(m_toggles.size(), 1);
    QCOMPARE(m_toggles[0].device, device);
    QCOMPARE(m_toggles[0].state, SwitchState::On);
    QCOMPARE(m_toggles[0].time, 20us);
    QCOMPARE(m_frames, 1);
    QCOMPARE(m_order, (QStringList{QStringLiteral("add:event0"), QStringLiteral("motion:event0"), QStringLiteral("frame:event0"), QStringLiteral("switch:event0")}));

    m_connection->startProcessingEvents();
    m_connection->startProcessingEvents();
    m_connection->processEvents();
    QCOMPARE(m_added.size(), 1);
    QCOMPARE(m_motions.size(), 1);
    QCOMPARE(m_toggles.size(), 1);
    QCOMPARE(m_frames, 1);
    QCOMPARE(m_removed.size(), 0);
    QVERIFY(nativeQueueDrained());
}

void TestLibinputConnection::testDeviceAddIsMotionBarrier()
{
    auto first = addNativeDevice();
    auto second = addNativeDevice();
    enqueueDeviceEvent(first, LIBINPUT_EVENT_DEVICE_ADDED);
    enqueueMotion(first, QPointF(2, 3), 10us);
    enqueueDeviceEvent(second, LIBINPUT_EVENT_DEVICE_ADDED);
    enqueueMotion(second, QPointF(17, -4), 20us);

    m_connection->setup();
    QCOMPARE(m_added.size(), 2);
    QCOMPARE(m_connection->devices(), m_added);
    QCOMPARE(m_added[0]->device(), first);
    QCOMPARE(m_added[1]->device(), second);
    QVERIFY(m_added[0] != m_added[1]);
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    QCOMPARE(m_eventsRead, 1);
    QCOMPARE(m_motions.size(), 0);
    QCOMPARE(m_frames, 0);
    QVERIFY(nativeQueueDrained());

    m_connection->startProcessingEvents();
    QCOMPARE(m_motions.size(), 2);
    QCOMPARE(m_motions[0].device, m_added[0]);
    QCOMPARE(m_motions[0].delta, QPointF(2, 3));
    QCOMPARE(m_motions[0].unaccelerated, QPointF(2, 3));
    QCOMPARE(m_motions[0].time, 10us);
    QCOMPARE(m_motions[1].device, m_added[1]);
    QCOMPARE(m_motions[1].delta, QPointF(17, -4));
    QCOMPARE(m_motions[1].unaccelerated, QPointF(17, -4));
    QCOMPARE(m_motions[1].time, 20us);
    QCOMPARE(m_order, (QStringList{QStringLiteral("add:event0"), QStringLiteral("add:event1"), QStringLiteral("motion:event0"), QStringLiteral("frame:event0"), QStringLiteral("motion:event1"), QStringLiteral("frame:event1")}));
    m_connection->startProcessingEvents();
    QCOMPARE(m_added.size(), 2);
    QCOMPARE(m_connection->devices(), m_added);
    QCOMPARE(m_motions.size(), 2);
    QCOMPARE(m_frames, 2);
}

void TestLibinputConnection::testRemovalFollowsBufferedInput()
{
    auto native = addNativeDevice();
    enqueueDeviceEvent(native, LIBINPUT_EVENT_DEVICE_ADDED);
    enqueueMotion(native, QPointF(5, 8), 10us);
    enqueueDeviceEvent(native, LIBINPUT_EVENT_DEVICE_REMOVED);

    m_connection->setup();
    QCOMPARE(m_added.size(), 1);
    QCOMPARE(m_connection->devices(), m_added);
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    QCOMPARE(m_eventsRead, 1);
    QCOMPARE(m_motions.size(), 0);
    QCOMPARE(m_removed.size(), 0);
    QCOMPARE(m_destroyedDevices.load(), 0);
    QVERIFY(nativeQueueDrained());

    m_connection->startProcessingEvents();
    QCOMPARE(m_motions.size(), 1);
    QCOMPARE(m_motions[0].device, m_added.constFirst());
    QCOMPARE(m_motions[0].delta, QPointF(5, 8));
    QCOMPARE(m_removed, m_added);
    QVERIFY(m_connection->devices().isEmpty());
    QCOMPARE(m_order, (QStringList{QStringLiteral("add:event0"), QStringLiteral("motion:event0"), QStringLiteral("frame:event0"), QStringLiteral("remove:event0")}));

    // Synchronize with deletion on the device's real worker before inspecting
    // its QPointer. Native device storage outlives the entire connection.
    QVERIFY(QMetaObject::invokeMethod(m_connection, [] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }, Qt::BlockingQueuedConnection));
    QVERIFY(m_deviceGuards.constFirst().isNull());
    QCOMPARE(m_destroyedDevices.load(), 1);
    m_connection->startProcessingEvents();
    QCOMPARE(m_added.size(), 1);
    QCOMPARE(m_removed.size(), 1);
    QCOMPARE(m_motions.size(), 1);
}

void TestLibinputConnection::testNotifierAfterActivation()
{
    auto native = addNativeDevice();
    enqueueDeviceEvent(native, LIBINPUT_EVENT_DEVICE_ADDED);
    m_connection->setup();
    QCOMPARE(m_added.size(), 1);
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    QCOMPARE(m_eventsRead, 1);
    m_connection->startProcessingEvents();
    QVERIFY(nativeQueueDrained());

    // Only the eventfd notifier can move this event into Connection's queue.
    enqueueMotion(native, QPointF(7, 11), 30us);
    QTRY_COMPARE(m_motions.size(), 1);
    QCOMPARE(m_motions[0].device, m_added.constFirst());
    QCOMPARE(m_motions[0].delta, QPointF(7, 11));
    QCOMPARE(m_motions[0].time, 30us);
    QCOMPARE(m_eventsRead, 2);
    QCOMPARE(m_frames, 1);
    QCOMPARE(m_added.size(), 1);
    QVERIFY(nativeQueueDrained());
}

void TestLibinputConnection::testActivationDuringDeviceAnnouncement()
{
    auto first = addNativeDevice();
    auto second = addNativeDevice();
    enqueueDeviceEvent(first, LIBINPUT_EVENT_DEVICE_ADDED);
    enqueueMotion(first, QPointF(2, 3), 10us);
    enqueueDeviceEvent(second, LIBINPUT_EVENT_DEVICE_ADDED);
    enqueueMotion(second, QPointF(5, 7), 20us);
    bool deferred = false;
    connect(m_connection, &Connection::deviceAdded, this, [&](Device *) {
        if (m_added.size() == 1) {
            m_connection->startProcessingEvents();
            deferred = m_motions.isEmpty();
        }
    });

    m_connection->setup();
    QVERIFY(deferred);
    QCOMPARE(m_added.size(), 2);
    QCOMPARE(m_motions.size(), 2);
    QCOMPARE(m_motions[0].device, m_added[0]);
    QCOMPARE(m_motions[1].device, m_added[1]);
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    QCOMPARE(m_added.size(), 2);
    QCOMPARE(m_motions.size(), 2);
}

void TestLibinputConnection::testBackendQueuesActivationOnMainThread()
{
    auto native = addNativeDevice();
    auto context = std::make_unique<libinput>();
    auto nativeContext = context.get();
    QVERIFY(context->fd >= 0);
    mockLibinputSetNextContext(std::move(context));
    int added = 0;
    int motions = 0;
    QThread *dispatchThread = nullptr;
    auto backend = std::make_unique<LibinputBackend>(&m_session);
    backend->setConfig(m_config);
    connect(backend.get(), &InputBackend::deviceAdded, this, [&](InputDevice *device) {
        ++added;
        connect(device, &InputDevice::pointerMotion, this, [&]() {
            ++motions;
            dispatchThread = QThread::currentThread();
        }, Qt::DirectConnection);
    });
    auto add = std::make_unique<libinput_event>();
    add->type = LIBINPUT_EVENT_DEVICE_ADDED;
    add->device = native;
    mockLibinputEnqueueEvent(nativeContext, std::move(add));
    auto motion = std::make_unique<libinput_event_pointer>();
    motion->type = LIBINPUT_EVENT_POINTER_MOTION;
    motion->device = native;
    motion->delta = QPointF(1, 2);
    mockLibinputEnqueueEvent(nativeContext, std::move(motion));

    backend->initialize();
    QCOMPARE(added, 1);
    QCOMPARE(motions, 0);
    QCoreApplication::sendPostedEvents(backend.get(), QEvent::MetaCall);
    QCOMPARE(motions, 0);
    backend->updateScreens();
    QCOMPARE(motions, 0);
    QCoreApplication::sendPostedEvents(backend.get(), QEvent::MetaCall);
    QCOMPARE(motions, 1);
    QCOMPARE(dispatchThread, QThread::currentThread());
    backend->updateScreens();
    QCoreApplication::sendPostedEvents(backend.get(), QEvent::MetaCall);
    QCOMPARE(added, 1);
    QCOMPARE(motions, 1);
}

QTEST_GUILESS_MAIN(TestLibinputConnection)
#include "connection_test.moc"
