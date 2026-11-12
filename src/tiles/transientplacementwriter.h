/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <kwin_export.h>

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

class KConfig;

namespace KWin
{

class Window;
class TransientPlacementWriter;

// Only Window can mint evidence that an exact native ownership attempt issued no geometry.
class TransientNoRequestProof final
{
public:
    ~TransientNoRequestProof() = default;

private:
    friend class Window;
    friend class TransientPlacementWriter;
    Q_DISABLE_COPY_MOVE(TransientNoRequestProof)

    enum class Validity {
        Current,
        Closing,
        Superseded,
    };
    TransientNoRequestProof(const QUuid &window, quint64 generation, quint64 sequence, std::function<Validity()> validity)
        : m_window(window)
        , m_generation(generation)
        , m_sequence(sequence)
        , m_validity(std::move(validity))
    {
    }

    const QUuid m_window;
    const quint64 m_generation;
    const quint64 m_sequence;
    const std::function<Validity()> m_validity;
    mutable bool m_consumed = false;
};

// Compositor-thread-only. Event ordering/origin comes from the native adapter, never QML claims.
// This writer is not instantiated by the desktop until its native integration is ready.
class KWIN_EXPORT TransientPlacementWriter : public QObject
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
        QString key;
    };

    explicit TransientPlacementWriter(const QString &path, QObject *parent = nullptr);
    ~TransientPlacementWriter() override;

    QUuid registerProvider(QObject *provider);
    bool isProviderCurrent(const QUuid &provider, quint64 revision) const
    {
        return m_provider && !provider.isNull() && provider == m_providerToken && revision == m_providerRevision;
    }
    bool trackWindow(const QUuid &window, const QString &key);
    // The key a window is bound under, or empty when it is not bound: read-only, so a
    // reloaded provider incarnation can ask which key an earlier one left a window under
    // instead of assuming one (a record files under the binding's key, not the caller's).
    QString windowBinding(const QUuid &window) const;
    // Called at QObject destruction, not before the window's close callbacks.
    bool forgetWindow(const QUuid &window);
    Result record(const QUuid &provider, quint64 revision, const Update &update);
    Result reserve(const QUuid &provider, const ReservationRequest &request);
    Result activateReservation(const QUuid &provider, const QUuid &window, const QString &generation);
    Result cancelReservation(const QUuid &window, const QString &generation);
    Result finalizeNoRequest(const std::shared_ptr<const TransientNoRequestProof> &proof);
    bool ownsReservation(const QUuid &window, const QString &generation) const;
    // Read-only exact owner/generation/phase state, not readiness or permission to restore.
    // It does not validate provider freshness or any native restore attempt.
    bool ownsRestoringReservation(const QUuid &window, const QString &generation) const;
    Result beginRestore(const QUuid &window, const QString &generation);
    Result completeRestore(const QUuid &window, const QString &generation, quint64 eventSequence);
    // The completion's watermark is the newest sequence the writer has seen for the window (any
    // deferred record included); callers cannot know it, so the arithmetic stays here.
    Result completeRestore(const QUuid &window, const QString &generation);
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

    enum class Phase {
        Prepared,
        Owned,
        Restoring,
    };

    struct Reservation
    {
        QUuid owner;
        QString generation;
        QRectF original;
        Phase phase = Phase::Prepared;
        QUuid provider;
        quint64 providerRevision = 0;
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
