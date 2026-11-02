/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tiles/transientplacementwriter.h"

#include <KConfig>
#include <KConfigGroup>

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace KWin
{

static TransientPlacementWriter::ReservationRequest reservation(TransientPlacementWriter &writer, const QUuid &window, const QRectF &frame)
{
    const auto snapshot = writer.snapshot();
    return {window, QStringLiteral("generation-1"), frame, snapshot.providerRevision, snapshot.mapRevision, QStringLiteral("app")};
}

class TransientPlacementWriterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void writesExistingPlacementFormat();
    void retiredProviderCannotWrite();
    void staleRevisionDoesNotMutate();
    void failedSaveDoesNotBlockReplacementSnapshot();
    void reservationRequiresExactPersistedFrame();
    void deferredManualWinnerSurvivesCompletion();
    void failedFlushIsNotRetriedByDestructor();
    void failedReservationDoesNotHoldKey();
    void duplicateGlobalKeyRefusesReservation();
    void deferredManualWinnerSurvivesOwnerDestruction();
    void staleReservationAndGenerationAreRejected();
    void latestDeferredIntentWinsWithoutBlockingOtherKeys();
    void providerReplacementDuringDestructionIsRejected();
    void invalidOriginDoesNotConsumeRevision();
    void futureFileVersionIsNotOverwritten();
    void providerRetirementKeepsReservation();
    void delayedOldIntentAfterReservation_data();
    void delayedOldIntentAfterReservation();
    void immutableMatchingEntryCannotGrantReservation();
    void largeCoordinatesRemainExact_data();
    void largeCoordinatesRemainExact();
    void preparedReservationCanBeCancelled();
    void ownedReservationCannotBeCancelled();
    void activationRevalidates_data();
    void activationRevalidates();
    void cancellationPreservesDeferredIntent_data();
    void cancellationPreservesDeferredIntent();
    void cancelledGenerationCannotAffectReplacement();
    void reservationRejectsWrongKey();
    void cancellationKeepsLatestCompetingIntent();
    void preparedPendingSurvivesTermination_data();
    void preparedPendingSurvivesTermination();
};

void TransientPlacementWriterTest::writesExistingPlacementFormat()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    QVERIFY(!token.isNull());
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF frame(73, 91, 430, 310);
    const TransientPlacementWriter::Update update{window, frame, 1, TransientPlacementWriter::Origin::Manual};
    QCOMPARE(writer.record(token, 1, update), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.snapshot().dirty);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), frame);
    QVERIFY(writer.flush());
    const auto saved = writer.snapshot();
    QVERIFY(!saved.dirty);
    QCOMPARE(saved.mapRevision, saved.persistedRevision);

    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), frame);
}

void TransientPlacementWriterTest::retiredProviderCannotWrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto oldToken = writer.registerProvider(provider.get());
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    provider.reset();
    QObject replacement;
    const auto newToken = writer.registerProvider(&replacement);
    QVERIFY(!newToken.isNull());
    QVERIFY(newToken != oldToken);
    const TransientPlacementWriter::Update update{window, QRectF(20, 30, 600, 400), 1, TransientPlacementWriter::Origin::Manual};
    QCOMPARE(writer.record(oldToken, 1, update), TransientPlacementWriter::Result::StaleProvider);
    QVERIFY(writer.snapshot().entries.isEmpty());
    QCOMPARE(writer.record(newToken, 1, update), TransientPlacementWriter::Result::Accepted);
}

void TransientPlacementWriterTest::staleRevisionDoesNotMutate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF frame(73, 91, 430, 310);
    QCOMPARE(writer.record(token, 2, {window, frame, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.record(token, 1, {window, QRectF(20, 30, 600, 400), 3, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::StaleRevision);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), frame);
    QCOMPARE(writer.record(token, 3, {window, QRectF(20.5, 30, 600, 400), 3, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::InvalidRecord);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), frame);
}

void TransientPlacementWriterTest::failedSaveDoesNotBlockReplacementSnapshot()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF oldFrame(73, 91, 430, 310);
    QCOMPARE(writer.record(token, 1, {window, oldFrame, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.flush());
    const QRectF newFrame(200, 100, 600, 400);
    QCOMPARE(writer.record(token, 2, {window, newFrame, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    // A directory cannot be replaced by the config writer, even if tests run as root.
    QVERIFY(QFile::rename(path, path + QStringLiteral(".saved")));
    QVERIFY(QDir().mkdir(path));
    QVERIFY(!writer.flush());
    provider.reset();
    QObject replacement;
    QVERIFY(!writer.registerProvider(&replacement).isNull());
    const auto available = writer.snapshot();
    QCOMPARE(available.entries.value(QStringLiteral("app")), newFrame);
    QVERIFY(available.dirty);
    QVERIFY(available.persistedRevision < available.mapRevision);
    QVERIFY(QDir().rmdir(path));
    QVERIFY(QFile::rename(path + QStringLiteral(".saved"), path));
    QVERIFY(writer.flush());
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), newFrame);
}

void TransientPlacementWriterTest::reservationRequiresExactPersistedFrame()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF frame(73, 91, 430, 310);
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, window, QRectF(20, 30, 600, 400))), TransientPlacementWriter::Result::InvalidRecord);
    QVERIFY(!QFile::exists(path));
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.isReserved(window));
    QVERIFY(!writer.snapshot().dirty);
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), frame);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), TransientPlacementWriter::Result::Busy);
}

void TransientPlacementWriterTest::deferredManualWinnerSurvivesCompletion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    const auto other = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    const QRectF original(73, 91, 430, 310);
    const QRectF manual(200, 100, 600, 400);
    const QRectF later(230, 160, 620, 420);
    QCOMPARE(writer.record(token, 1, {owner, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, original)), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.activateReservation(token, owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.beginRestore(owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.trackWindow(other, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 2, {other, manual, 20, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Deferred);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
    QCOMPARE(writer.completeRestore(owner, QStringLiteral("generation-1"), 30), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.record(token, 3, {owner, original, 30, TransientPlacementWriter::Origin::Restoration}), TransientPlacementWriter::Result::Suppressed);
    QCOMPARE(writer.record(token, 4, {owner, original, 31, TransientPlacementWriter::Origin::Close}), TransientPlacementWriter::Result::Suppressed);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), manual);
    QVERIFY(writer.flush());
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), manual);
    QCOMPARE(writer.record(token, 5, {owner, later, 32, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), later);
}

void TransientPlacementWriterTest::failedFlushIsNotRetriedByDestructor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    auto writer = std::make_unique<TransientPlacementWriter>(path);
    QObject provider;
    const auto token = writer->registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer->trackWindow(window, QStringLiteral("app")));
    const QRectF saved(73, 91, 430, 310);
    QCOMPARE(writer->record(token, 1, {window, saved, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer->flush());
    const QRectF pending(200, 100, 600, 400);
    QCOMPARE(writer->record(token, 2, {window, pending, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(QFile::rename(path, path + QStringLiteral(".saved")));
    QVERIFY(QDir().mkdir(path));
    QVERIFY(!writer->flush());
    QVERIFY(QDir().rmdir(path));
    QVERIFY(QFile::rename(path + QStringLiteral(".saved"), path));
    writer.reset();
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), saved);
}

void TransientPlacementWriterTest::failedReservationDoesNotHoldKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF frame(73, 91, 430, 310);
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(QDir().mkdir(path));
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), TransientPlacementWriter::Result::PersistenceFailed);
    QVERIFY(!writer.isReserved(window));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), frame);
    QVERIFY(writer.snapshot().dirty);
    QVERIFY(QDir().rmdir(path));
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), TransientPlacementWriter::Result::Accepted);
}

void TransientPlacementWriterTest::duplicateGlobalKeyRefusesReservation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QVERIFY(writer.trackWindow(QUuid::createUuid(), QStringLiteral("app")));
    const QRectF frame(73, 91, 430, 310);
    QCOMPARE(writer.record(token, 1, {owner, frame, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, frame)), TransientPlacementWriter::Result::Conflict);
    QVERIFY(!writer.isReserved(owner));
    QVERIFY(!QFile::exists(path));
}

void TransientPlacementWriterTest::deferredManualWinnerSurvivesOwnerDestruction()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    const auto other = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    const QRectF original(73, 91, 430, 310);
    const QRectF manual(200, 100, 600, 400);
    QCOMPARE(writer.record(token, 1, {owner, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, original)), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.activateReservation(token, owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.beginRestore(owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QSignalSpy conflict(&writer, &TransientPlacementWriter::keyConflict);
    QVERIFY(writer.trackWindow(other, QStringLiteral("app")));
    QCOMPARE(conflict.count(), 1);
    QCOMPARE(writer.record(token, 2, {other, manual, 20, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Deferred);
    QCOMPARE(writer.record(token, 3, {owner, original, 21, TransientPlacementWriter::Origin::Close}), TransientPlacementWriter::Result::Suppressed);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
    QVERIFY(writer.forgetWindow(owner));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), manual);
    QCOMPARE(writer.record(token, 4, {owner, original, 22, TransientPlacementWriter::Origin::Close}), TransientPlacementWriter::Result::UnknownWindow);
}

void TransientPlacementWriterTest::staleReservationAndGenerationAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF frame(73, 91, 430, 310);
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    const auto stale = reservation(writer, window, frame);
    QCOMPARE(writer.record(token, 2, {window, frame, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, stale), TransientPlacementWriter::Result::StaleRevision);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.activateReservation(token, window, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.beginRestore(window, QStringLiteral("wrong")), TransientPlacementWriter::Result::StaleGeneration);
    QCOMPARE(writer.completeRestore(window, QStringLiteral("generation-1"), 3), TransientPlacementWriter::Result::Busy);
    QCOMPARE(writer.beginRestore(window, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.completeRestore(window, QStringLiteral("wrong"), 3), TransientPlacementWriter::Result::StaleGeneration);
    QCOMPARE(writer.completeRestore(window, QStringLiteral("generation-1"), 3), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), TransientPlacementWriter::Result::StaleGeneration);
}

void TransientPlacementWriterTest::latestDeferredIntentWinsWithoutBlockingOtherKeys()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    const auto newer = QUuid::createUuid();
    const auto older = QUuid::createUuid();
    const auto unrelated = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QVERIFY(writer.trackWindow(unrelated, QStringLiteral("other")));
    const QRectF original(73, 91, 430, 310);
    const QRectF manual(200, 100, 600, 400);
    const QRectF otherFrame(10, 20, 300, 200);
    QCOMPARE(writer.record(token, 1, {owner, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, original)), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.activateReservation(token, owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.beginRestore(owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.trackWindow(newer, QStringLiteral("app")));
    QVERIFY(writer.trackWindow(older, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 2, {newer, manual, 20, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Deferred);
    QCOMPARE(writer.record(token, 3, {unrelated, otherFrame, 25, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.record(token, 4, {older, original, 10, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::OutdatedEvent);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("other")), otherFrame);
    QCOMPARE(writer.completeRestore(owner, QStringLiteral("generation-1"), 30), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), manual);
}

void TransientPlacementWriterTest::providerReplacementDuringDestructionIsRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    QObject replacement;
    QUuid earlyToken;
    connect(provider.get(), &QObject::destroyed, &writer, [&]() {
        earlyToken = writer.registerProvider(&replacement);
    });
    const auto oldToken = writer.registerProvider(provider.get());
    QVERIFY(!oldToken.isNull());
    provider.reset();
    QVERIFY(earlyToken.isNull());
    const auto token = writer.registerProvider(&replacement);
    QVERIFY(!token.isNull());
    QCoreApplication::sendPostedEvents(&writer, QEvent::MetaCall);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, QRectF(10, 20, 300, 200), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
}

void TransientPlacementWriterTest::invalidOriginDoesNotConsumeRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    const QRectF frame(10, 20, 300, 200);
    QCOMPARE(writer.record(token, 1, {window, frame, 1, static_cast<TransientPlacementWriter::Origin>(999)}), TransientPlacementWriter::Result::InvalidRecord);
    QVERIFY(writer.snapshot().entries.isEmpty());
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
}

void TransientPlacementWriterTest::futureFileVersionIsNotOverwritten()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    const QByteArray content("[Windows]\nwindows={\"v\":2,\"apps\":{\"app\":{\"x\":10,\"y\":20,\"width\":300,\"height\":200}},\"future\":\"keep\"}\n");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, QRectF(20, 30, 400, 300), 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(!writer.flush());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), content);
}

void TransientPlacementWriterTest::providerRetirementKeepsReservation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    const auto oldToken = writer.registerProvider(provider.get());
    const auto owner = QUuid::createUuid();
    const auto other = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QVERIFY(writer.trackWindow(other, QStringLiteral("other")));
    const QRectF original(73, 91, 430, 310);
    QCOMPARE(writer.record(oldToken, 1, {owner, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(oldToken, reservation(writer, owner, original)), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.activateReservation(oldToken, owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.beginRestore(owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    provider.reset();
    QObject replacement;
    const auto token = writer.registerProvider(&replacement);
    QVERIFY(!token.isNull());
    QVERIFY(writer.isReserved(owner));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), original);
    QCOMPARE(writer.record(oldToken, 2, {other, original, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::StaleProvider);
    QCOMPARE(writer.record(token, 1, {other, original, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, original)), TransientPlacementWriter::Result::Busy);
    QVERIFY(writer.forgetWindow(owner));
    QVERIFY(!writer.isReserved(owner));
}

void TransientPlacementWriterTest::delayedOldIntentAfterReservation_data()
{
    QTest::addColumn<bool>("destroyOwner");
    QTest::newRow("completion") << false;
    QTest::newRow("destruction") << true;
}

void TransientPlacementWriterTest::delayedOldIntentAfterReservation()
{
    QFETCH(bool, destroyOwner);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    const auto newer = QUuid::createUuid();
    const auto older = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    const QRectF original(73, 91, 430, 310);
    const QRectF manual(200, 100, 600, 400);
    QCOMPARE(writer.record(token, 1, {owner, original, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, original)), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.activateReservation(token, owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QCOMPARE(writer.beginRestore(owner, QStringLiteral("generation-1")), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.trackWindow(newer, QStringLiteral("app")));
    QVERIFY(writer.trackWindow(older, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 2, {newer, manual, 20, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Deferred);
    if (destroyOwner) {
        QVERIFY(writer.forgetWindow(owner));
    } else {
        QCOMPARE(writer.completeRestore(owner, QStringLiteral("generation-1"), 30), TransientPlacementWriter::Result::Accepted);
    }
    QCOMPARE(writer.record(token, 3, {older, original, 10, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::OutdatedEvent);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), manual);
}

void TransientPlacementWriterTest::immutableMatchingEntryCannotGrantReservation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray content("[Windows]\nwindows[$i]={\"v\":1,\"apps\":{\"app\":{\"x\":73,\"y\":91,\"width\":430,\"height\":310}}}\n");
    QCOMPARE(file.write(content), content.size());
    file.close();
    KConfig configuration(path, KConfig::SimpleConfig);
    QVERIFY(KConfigGroup(&configuration, QStringLiteral("Windows")).isEntryImmutable("windows"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QCOMPARE(writer.reserve(token, reservation(writer, owner, QRectF(73, 91, 430, 310))), TransientPlacementWriter::Result::PersistenceFailed);
    QVERIFY(!writer.isReserved(owner));
}

void TransientPlacementWriterTest::largeCoordinatesRemainExact_data()
{
    QTest::addColumn<bool>("admission");
    QTest::newRow("admission") << true;
    QTest::newRow("record-and-readback") << false;
}

void TransientPlacementWriterTest::largeCoordinatesRemainExact()
{
    QFETCH(bool, admission);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    const QRectF previous(1000000000000.0, 91, 430, 310);
    const QRectF current(1000000000001.0, 91, 430, 310);
    QCOMPARE(writer.record(token, 1, {owner, previous, 1, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
    QVERIFY(writer.flush());
    if (admission) {
        QCOMPARE(writer.reserve(token, reservation(writer, owner, current)), TransientPlacementWriter::Result::InvalidRecord);
        QVERIFY(!writer.isReserved(owner));
    } else {
        QCOMPARE(writer.record(token, 2, {owner, current, 2, TransientPlacementWriter::Origin::Manual}), TransientPlacementWriter::Result::Accepted);
        QVERIFY(writer.snapshot().entries.value(QStringLiteral("app")).x() == current.x());
        QVERIFY(writer.flush());
        TransientPlacementWriter reopened(path);
        QVERIFY(reopened.snapshot().entries.value(QStringLiteral("app")).x() == current.x());
    }
}

void TransientPlacementWriterTest::preparedReservationCanBeCancelled()
{
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    const auto generation = QStringLiteral("generation-1");
    const QRectF frame(73, 91, 430, 310);
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), Result::Accepted);
    QVERIFY(writer.isReserved(window));
    QVERIFY(!writer.ownsReservation(window, generation));
    QCOMPARE(writer.beginRestore(window, generation), Result::Busy);
    QCOMPARE(writer.cancelReservation(window, QStringLiteral("stale")), Result::StaleGeneration);
    QVERIFY(writer.isReserved(window));
    QCOMPARE(writer.cancelReservation(window, generation), Result::Accepted);
    QVERIFY(!writer.isReserved(window));
    QCOMPARE(writer.cancelReservation(window, generation), Result::StaleGeneration);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), frame);
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), frame);
}

void TransientPlacementWriterTest::ownedReservationCannotBeCancelled()
{
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    const auto generation = QStringLiteral("generation-1");
    const QRectF frame(73, 91, 430, 310);
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), Result::Accepted);
    QCOMPARE(writer.activateReservation(token, window, generation), Result::Accepted);
    QVERIFY(writer.ownsReservation(window, generation));
    QVERIFY(!writer.ownsReservation(window, QStringLiteral("stale")));
    QCOMPARE(writer.activateReservation(token, window, generation), Result::Busy);
    QCOMPARE(writer.cancelReservation(window, generation), Result::Busy);
    QCOMPARE(writer.beginRestore(window, generation), Result::Accepted);
    QVERIFY(writer.isReserved(window));
    QVERIFY(!writer.ownsReservation(window, generation));
    QCOMPARE(writer.cancelReservation(window, generation), Result::Busy);
}

void TransientPlacementWriterTest::activationRevalidates_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("provider") << 0;
    QTest::newRow("revision") << 1;
    QTest::newRow("same-key-window") << 2;
}

void TransientPlacementWriterTest::activationRevalidates()
{
    QFETCH(int, change);
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    auto provider = std::make_unique<QObject>();
    auto token = writer.registerProvider(provider.get());
    const auto window = QUuid::createUuid();
    const auto generation = QStringLiteral("generation-1");
    const QRectF frame(73, 91, 430, 310);
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), Result::Accepted);
    auto expected = Result::StaleProvider;
    QObject replacement;
    if (change == 0) {
        provider.reset();
        token = writer.registerProvider(&replacement);
    } else if (change == 1) {
        const auto other = QUuid::createUuid();
        QVERIFY(writer.trackWindow(other, QStringLiteral("other")));
        QCOMPARE(writer.record(token, 2, {other, frame, 2, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
        expected = Result::StaleRevision;
    } else {
        QVERIFY(writer.trackWindow(QUuid::createUuid(), QStringLiteral("app")));
        expected = Result::Conflict;
    }
    QCOMPARE(writer.activateReservation(token, window, generation), expected);
    QVERIFY(!writer.ownsReservation(window, generation));
    QCOMPARE(writer.cancelReservation(window, generation), Result::Accepted);
    QVERIFY(!writer.isReserved(window));
}

void TransientPlacementWriterTest::cancellationPreservesDeferredIntent_data()
{
    QTest::addColumn<bool>("sibling");
    QTest::newRow("owner") << false;
    QTest::newRow("sibling") << true;
}

void TransientPlacementWriterTest::cancellationPreservesDeferredIntent()
{
    QFETCH(bool, sibling);
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    const auto generation = QStringLiteral("generation-1");
    const QRectF frame(73, 91, 430, 310);
    const QRectF manual(200, 100, 600, 400);
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {owner, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, frame)), Result::Accepted);
    const auto source = sibling ? QUuid::createUuid() : owner;
    if (sibling) {
        QVERIFY(writer.trackWindow(source, QStringLiteral("app")));
    }
    QCOMPARE(writer.record(token, 2, {source, manual, 20, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
    QCOMPARE(writer.activateReservation(token, owner, generation), Result::StaleRevision);
    QCOMPARE(writer.cancelReservation(owner, generation), Result::Accepted);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), manual);
    QCOMPARE(writer.record(token, 3, {source, manual, 21, TransientPlacementWriter::Origin::Close}), Result::Accepted);
    QVERIFY(writer.flush());
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), manual);
}

void TransientPlacementWriterTest::cancelledGenerationCannotAffectReplacement()
{
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    const QRectF frame(73, 91, 430, 310);
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, window, frame)), Result::Accepted);
    QCOMPARE(writer.cancelReservation(window, QStringLiteral("generation-1")), Result::Accepted);
    auto next = reservation(writer, window, frame);
    next.generation = QStringLiteral("generation-2");
    QCOMPARE(writer.reserve(token, next), Result::Accepted);
    QCOMPARE(writer.activateReservation(token, window, next.generation), Result::Accepted);
    QCOMPARE(writer.cancelReservation(window, QStringLiteral("generation-1")), Result::StaleGeneration);
    QCOMPARE(writer.beginRestore(window, QStringLiteral("generation-1")), Result::StaleGeneration);
    QVERIFY(writer.ownsReservation(window, next.generation));
}

void TransientPlacementWriterTest::reservationRejectsWrongKey()
{
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto window = QUuid::createUuid();
    const QRectF frame(73, 91, 430, 310);
    QVERIFY(writer.trackWindow(window, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {window, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    auto request = reservation(writer, window, frame);
    request.key = QStringLiteral("wrong-app");
    QCOMPARE(writer.reserve(token, request), Result::InvalidRecord);
    QVERIFY(!writer.isReserved(window));
    QVERIFY(!QFile::exists(path));
}

void TransientPlacementWriterTest::cancellationKeepsLatestCompetingIntent()
{
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TransientPlacementWriter writer(directory.filePath(QStringLiteral("placementrc")));
    QObject provider;
    const auto token = writer.registerProvider(&provider);
    const auto owner = QUuid::createUuid();
    const auto sibling = QUuid::createUuid();
    const QRectF frame(73, 91, 430, 310);
    const QRectF newer(200, 100, 600, 400);
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {owner, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, frame)), Result::Accepted);
    QVERIFY(writer.trackWindow(sibling, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 2, {owner, frame, 20, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
    QCOMPARE(writer.record(token, 3, {sibling, newer, 30, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
    QCOMPARE(writer.record(token, 4, {owner, frame, 25, TransientPlacementWriter::Origin::Manual}), Result::OutdatedEvent);
    QCOMPARE(writer.cancelReservation(owner, QStringLiteral("generation-1")), Result::Accepted);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), newer);
    QCOMPARE(writer.record(token, 5, {owner, frame, 29, TransientPlacementWriter::Origin::Manual}), Result::OutdatedEvent);
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), newer);
}

void TransientPlacementWriterTest::preparedPendingSurvivesTermination_data()
{
    QTest::addColumn<int>("termination");
    QTest::newRow("provider-retired") << 0;
    QTest::newRow("owner-destroyed") << 1;
    QTest::newRow("owner-close") << 2;
}

void TransientPlacementWriterTest::preparedPendingSurvivesTermination()
{
    QFETCH(int, termination);
    using Result = TransientPlacementWriter::Result;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("placementrc"));
    TransientPlacementWriter writer(path);
    auto provider = std::make_unique<QObject>();
    const auto token = writer.registerProvider(provider.get());
    const auto owner = QUuid::createUuid();
    const QRectF frame(73, 91, 430, 310);
    const QRectF manual(200, 100, 600, 400);
    QVERIFY(writer.trackWindow(owner, QStringLiteral("app")));
    QCOMPARE(writer.record(token, 1, {owner, frame, 1, TransientPlacementWriter::Origin::Manual}), Result::Accepted);
    QCOMPARE(writer.reserve(token, reservation(writer, owner, frame)), Result::Accepted);
    QCOMPARE(writer.record(token, 2, {owner, manual, 20, TransientPlacementWriter::Origin::Manual}), Result::Deferred);
    if (termination == 0) {
        provider.reset();
    } else if (termination == 1) {
        QVERIFY(writer.forgetWindow(owner));
    } else {
        QCOMPARE(writer.record(token, 3, {owner, manual, 21, TransientPlacementWriter::Origin::Close}), Result::Deferred);
    }
    if (termination != 1) {
        QCOMPARE(writer.cancelReservation(owner, QStringLiteral("generation-1")), Result::Accepted);
    }
    QVERIFY(!writer.isReserved(owner));
    QCOMPARE(writer.snapshot().entries.value(QStringLiteral("app")), manual);
    QVERIFY(writer.flush());
    TransientPlacementWriter reopened(path);
    QCOMPARE(reopened.snapshot().entries.value(QStringLiteral("app")), manual);
}

} // namespace KWin

QTEST_GUILESS_MAIN(KWin::TransientPlacementWriterTest)

#include "test_transientplacementwriter.moc"
