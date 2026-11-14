/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "transientplacementwriter.h"

#include <KConfig>
#include <KConfigGroup>

#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedValueRollback>

#include <cmath>
#include <limits>
#include <utility>

namespace KWin
{
namespace
{

bool sameFrame(const QRectF &left, const QRectF &right)
{
    // QRectF equality is fuzzy, but a persisted floating frame is an exact contract.
    return left.x() == right.x() && left.y() == right.y() && left.width() == right.width() && left.height() == right.height();
}

bool sameEntries(const QMap<QString, QRectF> &left, const QMap<QString, QRectF> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    auto other = right.cbegin();
    for (auto entry = left.cbegin(); entry != left.cend(); ++entry, ++other) {
        if (entry.key() != other.key() || !sameFrame(entry.value(), other.value())) {
            return false;
        }
    }
    return true;
}

bool entryIsMutable(KConfig &config)
{
    const KConfigGroup group(&config, QStringLiteral("Windows"));
    return !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable("windows");
}

bool validFrame(const QRectF &frame)
{
    for (qreal value : {frame.x(), frame.y(), frame.width(), frame.height()}) {
        if (!std::isfinite(value) || std::trunc(value) != value) {
            return false;
        }
    }
    return frame.width() >= 50 && frame.height() >= 50 && frame.width() <= 20000 && frame.height() <= 20000;
}

QMap<QString, QRectF> readEntries(KConfig &config, bool *valid)
{
    const QString text = KConfigGroup(&config, QStringLiteral("Windows")).readEntry("windows", QString());
    if (text.isEmpty()) {
        *valid = true;
        return {};
    }
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    const auto object = document.object();
    const auto apps = object.value(QStringLiteral("apps"));
    const auto version = object.value(QStringLiteral("v"));
    *valid = document.isObject() && apps.isObject() && (version.isUndefined() || (version.isDouble() && version.toDouble() == 1));
    QMap<QString, QRectF> entries;
    const QJsonObject appEntries = apps.toObject();
    for (auto it = appEntries.constBegin(); it != appEntries.constEnd(); ++it) {
        const auto object = it.value().toObject();
        const auto x = object.value(QStringLiteral("x"));
        const auto y = object.value(QStringLiteral("y"));
        const auto width = object.value(QStringLiteral("width"));
        const auto height = object.value(QStringLiteral("height"));
        const QRectF frame(x.toDouble(), y.toDouble(), width.toDouble(), height.toDouble());
        if (it.key().isEmpty() || !x.isDouble() || !y.isDouble() || !width.isDouble() || !height.isDouble() || !validFrame(frame)) {
            *valid = false;
            continue;
        }
        entries.insert(it.key(), frame);
    }
    return entries;
}

QString serialize(const QMap<QString, QRectF> &entries)
{
    QJsonObject apps;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const auto &frame = it.value();
        apps.insert(it.key(), QJsonObject{{QStringLiteral("x"), frame.x()}, {QStringLiteral("y"), frame.y()}, {QStringLiteral("width"), frame.width()}, {QStringLiteral("height"), frame.height()}});
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("apps"), apps}}).toJson(QJsonDocument::Compact));
}

} // namespace

TransientPlacementWriter::TransientPlacementWriter(const QString &path, QObject *parent)
    : QObject(parent)
    , m_path(path)
    , m_config(std::make_unique<KConfig>(path, KConfig::SimpleConfig))
{
    m_entries = readEntries(*m_config, &m_readable);
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(2000);
    connect(&m_flushTimer, &QTimer::timeout, this, [this]() {
        flush();
    });
}

TransientPlacementWriter::~TransientPlacementWriter()
{
    m_flushTimer.stop();
    // An orderly compositor teardown is the last chance a record inside the flush coalescing
    // window has: the legacy transport this writer replaced wrote its newest state on the
    // script's own teardown, so the pending map is persisted once here too. Only a change no
    // attempt has covered yet may be written - a flush that already failed must not be retried
    // from a destructor, and KConfig's own destructor never retries either.
    if (!m_busy && !m_flushAttempted && m_mapRevision != m_persistedRevision) {
        flushImpl();
    }
    m_config->markAsClean();
}

QUuid TransientPlacementWriter::registerProvider(QObject *provider)
{
    if (!provider || !m_providerToken.isNull() || m_busy) {
        return {};
    }
    m_provider = provider;
    m_providerToken = QUuid::createUuid();
    m_providerRevision = 0;
    connect(provider, &QObject::destroyed, this, [this, token = m_providerToken]() {
        if (m_providerToken == token) {
            m_providerToken = {};
            m_providerRevision = 0;
            QMetaObject::invokeMethod(this, [this, token]() {
                Q_EMIT providerRetired(token);
            }, Qt::QueuedConnection);
        }
    });
    return m_providerToken;
}

bool TransientPlacementWriter::trackWindow(const QUuid &window, const QString &key)
{
    if (window.isNull() || key.isEmpty() || m_windows.contains(window) || m_busy) {
        return false;
    }
    m_windows.insert(window, WindowRecord{key, 0, {}, std::nullopt});
    const auto reservation = m_reservations.constFind(key);
    if (reservation != m_reservations.cend()) {
        // Copy before notifying; the controller may release or destroy either window.
        const auto owner = reservation->owner;
        const auto generation = reservation->generation;
        Q_EMIT keyConflict(key, owner, generation);
    }
    return true;
}

QString TransientPlacementWriter::windowBinding(const QUuid &window) const
{
    const auto tracked = m_windows.constFind(window);
    return tracked == m_windows.constEnd() ? QString() : tracked->key;
}

bool TransientPlacementWriter::forgetWindow(const QUuid &window)
{
    if (m_busy) {
        return false;
    }
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return false;
    }
    const QString key = tracked->key;
    const auto reservation = m_reservations.find(key);
    if (reservation != m_reservations.end() && reservation->owner == window) {
        const auto deferred = reservation->deferred;
        if (deferred && !applyRecord(key, deferred->frame, deferred->eventSequence)) {
            return false;
        }
        m_reservations.erase(reservation);
    }
    m_windows.remove(window);
    return true;
}

QRectF TransientPlacementWriter::memoryFrame(const QRectF &frame)
{
    // The fractional part of a double is exact, so a half is decided on it: value + 0.5 would
    // carry the largest double below a half up to one.
    const auto round = [](qreal value) {
        const qreal whole = std::floor(value);
        return value - whole >= 0.5 ? whole + 1 : whole;
    };
    return QRectF(round(frame.x()), round(frame.y()), round(frame.width()), round(frame.height()));
}

TransientPlacementWriter::Result TransientPlacementWriter::record(const QUuid &provider, quint64 revision, const Update &update)
{
    if (m_busy) {
        return Result::Busy;
    }
    if (!m_provider || provider.isNull() || provider != m_providerToken) {
        return Result::StaleProvider;
    }
    if (revision <= m_providerRevision) {
        return Result::StaleRevision;
    }
    auto window = m_windows.find(update.window);
    if (window == m_windows.end()) {
        return Result::UnknownWindow;
    }
    if (!validFrame(update.frame) || m_mapRevision == std::numeric_limits<quint64>::max()) {
        return Result::InvalidRecord;
    }
    switch (update.origin) {
    case Origin::Manual:
    case Origin::Ordinary:
    case Origin::Restoration:
    case Origin::Close:
        break;
    default:
        return Result::InvalidRecord;
    }
    if (update.eventSequence <= window->eventSequence) {
        return Result::OutdatedEvent;
    }
    if (update.eventSequence <= m_keyEventSequences.value(window->key)) {
        return Result::OutdatedEvent;
    }
    m_providerRevision = revision;
    window->eventSequence = update.eventSequence;
    if (update.origin == Origin::Restoration) {
        return Result::Suppressed;
    }
    if (window->restoreWatermark) {
        if (update.origin == Origin::Close || update.eventSequence <= *window->restoreWatermark) {
            return Result::Suppressed;
        }
        window->restoreWatermark.reset();
    }
    auto reservation = m_reservations.find(window->key);
    if (reservation != m_reservations.end()) {
        if (reservation->owner == update.window && reservation->phase != Phase::Prepared
            && (reservation->phase != Phase::Restoring || update.origin == Origin::Close)) {
            return Result::Suppressed;
        }
        if (reservation->deferred && update.eventSequence <= reservation->deferred->eventSequence) {
            return Result::OutdatedEvent;
        }
        if (reservation->phase == Phase::Restoring && reservation->owner == update.window
            && reservation->deferred && reservation->deferred->window != update.window
            && sameFrame(update.frame, reservation->original)) {
            // The owner's record of the frame it is restored to (the reservation's own frame,
            // re-recorded when it lands) is not a newer placement: a sibling's manual update
            // keeps the one deferred slot whatever the sequences say. A frame the released
            // owner was moved to by hand differs from it and takes the slot as the latest.
            // An owner moved by hand back onto exactly that frame is not told apart from the
            // landing and is suppressed too: the sibling's update is the newer placement
            // either way. After a transfer to another output the frame the owner is restored
            // to is no longer the reservation's, and the latest record wins as before.
            return Result::Suppressed;
        }
        reservation->deferred = update;
        return Result::Deferred;
    }
    if (!applyRecord(window->key, update.frame, update.eventSequence)) {
        return Result::InvalidRecord;
    }
    return Result::Accepted;
}

TransientPlacementWriter::Result TransientPlacementWriter::reserve(const QUuid &provider, const ReservationRequest &request)
{
    if (m_busy) {
        return Result::Busy;
    }
    if (!m_provider || provider.isNull() || provider != m_providerToken) {
        return Result::StaleProvider;
    }
    if (request.mapRevision != m_mapRevision || request.providerRevision != m_providerRevision) {
        return Result::StaleRevision;
    }
    auto window = m_windows.find(request.window);
    if (window == m_windows.end()) {
        return Result::UnknownWindow;
    }
    if (request.generation.isEmpty() || request.key.isEmpty() || request.key != window->key
        || !validFrame(request.frame) || !sameFrame(m_entries.value(window->key), request.frame)) {
        return Result::InvalidRecord;
    }
    if (m_reservations.contains(window->key)) {
        return Result::Busy;
    }
    if (window->lastGeneration == request.generation) {
        return Result::StaleGeneration;
    }
    for (auto other = m_windows.cbegin(); other != m_windows.cend(); ++other) {
        if (other.key() != request.window && other->key == window->key) {
            return Result::Conflict;
        }
    }
    const QString key = window->key;
    QScopedValueRollback<bool> transaction(m_busy, true);
    m_reservations.insert(key, Reservation{request.window, request.generation, request.frame, Phase::Prepared, provider, m_providerRevision, std::nullopt});
    if (!flushImpl()) {
        m_reservations.remove(key);
        return Result::PersistenceFailed;
    }
    if (!m_provider || provider != m_providerToken) {
        m_reservations.remove(key);
        return Result::StaleProvider;
    }
    window->lastGeneration = request.generation;
    return Result::Accepted;
}

TransientPlacementWriter::Result TransientPlacementWriter::activateReservation(const QUuid &provider, const QUuid &window, const QString &generation)
{
    if (m_busy) {
        return Result::Busy;
    }
    if (!m_provider || provider.isNull() || provider != m_providerToken) {
        return Result::StaleProvider;
    }
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return Result::UnknownWindow;
    }
    auto reservation = m_reservations.find(tracked->key);
    if (reservation == m_reservations.end() || reservation->owner != window || reservation->generation != generation) {
        return Result::StaleGeneration;
    }
    if (reservation->phase != Phase::Prepared) {
        return Result::Busy;
    }
    if (reservation->provider != provider) {
        return Result::StaleProvider;
    }
    if (reservation->providerRevision != m_providerRevision) {
        return Result::StaleRevision;
    }
    for (auto other = m_windows.cbegin(); other != m_windows.cend(); ++other) {
        if (other.key() != window && other->key == tracked->key) {
            return Result::Conflict;
        }
    }
    reservation->phase = Phase::Owned;
    return Result::Accepted;
}

TransientPlacementWriter::Result TransientPlacementWriter::cancelReservation(const QUuid &window, const QString &generation)
{
    if (m_busy) {
        return Result::Busy;
    }
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return Result::UnknownWindow;
    }
    auto reservation = m_reservations.find(tracked->key);
    if (reservation == m_reservations.end() || reservation->owner != window || reservation->generation != generation) {
        return Result::StaleGeneration;
    }
    if (reservation->phase != Phase::Prepared) {
        return Result::Busy;
    }
    const auto deferred = reservation->deferred;
    if (deferred && !applyRecord(tracked->key, deferred->frame, deferred->eventSequence)) {
        return Result::InvalidRecord;
    }
    m_reservations.erase(reservation);
    return Result::Accepted;
}

TransientPlacementWriter::Result TransientPlacementWriter::finalizeNoRequest(const std::shared_ptr<const TransientNoRequestProof> &proof)
{
    if (m_busy) {
        return Result::Busy;
    }
    if (!proof || proof->m_consumed || proof->m_window.isNull() || !proof->m_generation || !proof->m_sequence || !proof->m_validity) {
        return Result::InvalidRecord;
    }
    const auto tracked = m_windows.constFind(proof->m_window);
    if (tracked == m_windows.cend()) {
        return Result::UnknownWindow;
    }
    auto reservation = m_reservations.find(tracked->key);
    if (reservation == m_reservations.end() || reservation->owner != proof->m_window
        || reservation->generation != QString::number(proof->m_generation)) {
        return Result::StaleGeneration;
    }
    if (reservation->phase == Phase::Prepared) {
        return Result::Busy;
    }
    QScopedValueRollback<bool> transaction(m_busy, true);
    switch (proof->m_validity()) {
    case TransientNoRequestProof::Validity::Closing:
        return Result::Busy;
    case TransientNoRequestProof::Validity::Superseded:
        return Result::StaleGeneration;
    case TransientNoRequestProof::Validity::Current:
        break;
    }
    // This path has no restoration event. Preserve later native intent without installing a watermark.
    const auto deferred = reservation->deferred;
    if (deferred && !applyRecord(tracked->key, deferred->frame, deferred->eventSequence)) {
        return Result::InvalidRecord;
    }
    proof->m_consumed = true;
    m_reservations.erase(reservation);
    return Result::Accepted;
}

bool TransientPlacementWriter::ownsReservation(const QUuid &window, const QString &generation) const
{
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return false;
    }
    const auto reservation = m_reservations.constFind(tracked->key);
    return reservation != m_reservations.cend() && reservation->owner == window
        && reservation->generation == generation && reservation->phase == Phase::Owned;
}

bool TransientPlacementWriter::ownsRestoringReservation(const QUuid &window, const QString &generation) const
{
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return false;
    }
    const auto reservation = m_reservations.constFind(tracked->key);
    return reservation != m_reservations.cend() && reservation->owner == window
        && reservation->generation == generation && reservation->phase == Phase::Restoring;
}

TransientPlacementWriter::Result TransientPlacementWriter::beginRestore(const QUuid &window, const QString &generation)
{
    if (m_busy) {
        return Result::Busy;
    }
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return Result::UnknownWindow;
    }
    auto reservation = m_reservations.find(tracked->key);
    if (reservation == m_reservations.end() || reservation->owner != window || reservation->generation != generation) {
        return Result::StaleGeneration;
    }
    if (reservation->phase == Phase::Prepared) {
        return Result::Busy;
    }
    reservation->phase = Phase::Restoring;
    return Result::Accepted;
}

TransientPlacementWriter::Result TransientPlacementWriter::completeRestore(const QUuid &window, const QString &generation)
{
    if (m_busy) {
        return Result::Busy;
    }
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.constEnd()) {
        return Result::UnknownWindow;
    }
    quint64 eventSequence = tracked->eventSequence;
    const auto reservation = m_reservations.constFind(tracked->key);
    if (reservation != m_reservations.constEnd() && reservation->deferred
        && reservation->deferred->eventSequence > eventSequence) {
        eventSequence = reservation->deferred->eventSequence;
    }
    if (eventSequence == 0) {
        // No tracked event yet: the watermark starts at the first sequence a later record can
        // exceed, not at zero (which completeRestore refuses).
        eventSequence = 1;
    }
    return completeRestore(window, generation, eventSequence);
}

TransientPlacementWriter::Result TransientPlacementWriter::completeRestore(const QUuid &window, const QString &generation, quint64 eventSequence)
{
    if (m_busy) {
        return Result::Busy;
    }
    auto tracked = m_windows.find(window);
    if (tracked == m_windows.end()) {
        return Result::UnknownWindow;
    }
    auto reservation = m_reservations.find(tracked->key);
    if (reservation == m_reservations.end() || reservation->owner != window || reservation->generation != generation) {
        return Result::StaleGeneration;
    }
    if (reservation->phase != Phase::Restoring) {
        return Result::Busy;
    }
    if (eventSequence == 0 || eventSequence < tracked->eventSequence) {
        return Result::OutdatedEvent;
    }
    if (reservation->deferred && eventSequence < reservation->deferred->eventSequence) {
        return Result::OutdatedEvent;
    }
    const auto deferred = reservation->deferred;
    const auto key = tracked->key;
    if (deferred && !applyRecord(key, deferred->frame, deferred->eventSequence)) {
        return Result::InvalidRecord;
    }
    tracked->restoreWatermark = eventSequence;
    m_reservations.erase(reservation);
    return Result::Accepted;
}

bool TransientPlacementWriter::isReserved(const QUuid &window) const
{
    const auto tracked = m_windows.constFind(window);
    if (tracked == m_windows.cend()) {
        return false;
    }
    const auto reservation = m_reservations.constFind(tracked->key);
    return reservation != m_reservations.cend() && reservation->owner == window;
}

bool TransientPlacementWriter::applyRecord(const QString &key, const QRectF &frame, quint64 eventSequence)
{
    if (eventSequence <= m_keyEventSequences.value(key)) {
        return false;
    }
    if (!sameFrame(m_entries.value(key), frame)) {
        if (m_mapRevision == std::numeric_limits<quint64>::max()) {
            return false;
        }
        m_entries.insert(key, frame);
        ++m_mapRevision;
        m_flushAttempted = false;
        m_flushTimer.start();
    }
    m_keyEventSequences.insert(key, eventSequence);
    return true;
}

TransientPlacementWriter::Snapshot TransientPlacementWriter::snapshot() const
{
    return {m_entries, m_mapRevision, m_persistedRevision, m_mapRevision != m_persistedRevision, m_providerRevision};
}

bool TransientPlacementWriter::flush()
{
    if (m_busy) {
        return false;
    }
    QScopedValueRollback<bool> transaction(m_busy, true);
    return flushImpl();
}

bool TransientPlacementWriter::flushImpl()
{
    // Any attempt - a failure included - covers the map as it stands: the destructor's
    // best-effort write may carry a change no attempt covered, never a retry of a failed one.
    m_flushAttempted = true;
    if (!m_readable || !entryIsMutable(*m_config)) {
        return false;
    }
    KConfigGroup(m_config.get(), QStringLiteral("Windows")).writeEntry("windows", serialize(m_entries));
    if (!m_config->sync()) {
        return false;
    }
    KConfig reader(m_path, KConfig::SimpleConfig);
    bool readable = false;
    const auto saved = readEntries(reader, &readable);
    if (!readable || !entryIsMutable(reader) || !sameEntries(saved, m_entries)) {
        return false;
    }
    m_persistedRevision = m_mapRevision;
    m_flushTimer.stop();
    return true;
}

} // namespace KWin
