/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QUuid>

#include <memory>
#include <optional>

class KConfig;

namespace KWin
{

// Compositor-thread-only. Event ordering/origin comes from the native adapter, never QML claims.
// This writer is not instantiated by the desktop until its native integration is ready.
class TransientPlacementWriter : public QObject
{
    Q_OBJECT

public:
    enum class Origin {
        Manual,
        Ordinary,
        Restoration,
        Close,
    };

    enum class Result {
        Accepted,
        StaleProvider,
        StaleRevision,
        UnknownWindow,
        InvalidRecord,
        OutdatedEvent,
        Busy,
        Conflict,
        StaleGeneration,
        PersistenceFailed,
        Deferred,
        Suppressed,
    };

    struct Update
    {
        QUuid window;
        QRectF frame;
        quint64 eventSequence;
        Origin origin;
    };

    struct Snapshot
    {
        QMap<QString, QRectF> entries;
        quint64 mapRevision;
        quint64 persistedRevision;
        bool dirty;
        quint64 providerRevision;
    };

    struct ReservationRequest
    {
        QUuid window;
        QString generation;
        QRectF frame;
        quint64 providerRevision;
        quint64 mapRevision;
    };

    explicit TransientPlacementWriter(const QString &path, QObject *parent = nullptr);
    ~TransientPlacementWriter() override;

    QUuid registerProvider(QObject *provider);
    bool trackWindow(const QUuid &window, const QString &key);
    // Called at QObject destruction, not before the window's close callbacks.
    bool forgetWindow(const QUuid &window);
    Result record(const QUuid &provider, quint64 revision, const Update &update);
    Result reserve(const QUuid &provider, const ReservationRequest &request);
    Result beginRestore(const QUuid &window, const QString &generation);
    Result completeRestore(const QUuid &window, const QString &generation, quint64 eventSequence);
    bool isReserved(const QUuid &window) const;
    Snapshot snapshot() const;
    bool flush();

Q_SIGNALS:
    void keyConflict(const QString &key, const QUuid &owner, const QString &generation);
    void providerRetired(const QUuid &token);

private:
    struct WindowRecord
    {
        QString key;
        quint64 eventSequence = 0;
        QString lastGeneration;
        std::optional<quint64> restoreWatermark;
    };

    struct Reservation
    {
        QUuid owner;
        QString generation;
        QRectF original;
        bool restoring = false;
        std::optional<Update> deferred;
    };

    bool applyRecord(const QString &key, const QRectF &frame, quint64 eventSequence);
    bool flushImpl();

    QString m_path;
    std::unique_ptr<KConfig> m_config;
    QMap<QString, QRectF> m_entries;
    QHash<QString, quint64> m_keyEventSequences;
    QHash<QUuid, WindowRecord> m_windows;
    QHash<QString, Reservation> m_reservations;
    QPointer<QObject> m_provider;
    QUuid m_providerToken;
    quint64 m_providerRevision = 0;
    quint64 m_mapRevision = 0;
    quint64 m_persistedRevision = 0;
    bool m_readable = true;
    bool m_busy = false;
    QTimer m_flushTimer;
};

} // namespace KWin
