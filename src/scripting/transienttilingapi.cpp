/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "transienttilingapi.h"

#include "tiles/tile.h"
#include "tiles/tilemanager.h"
#include "utils/common.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

namespace KWin
{

using Origin = TransientPlacementWriter::Origin;

static QString placementKeyForWindow(Window *window)
{
    // The controller's canonical key (transienttilecontroller.cpp:305): the desktop
    // file's basename minus .desktop, else the resource class, lowercased.
    QString name = window->desktopFileName();
    if (name.isEmpty()) {
        name = window->resourceClass();
    }
    if (name.endsWith(QLatin1String(".desktop"))) {
        name.chop(8);
    }
    return name.toLower();
}

TransientTilingApi::TransientTilingApi(Workspace *workspace, QObject *parent)
    : QObject(parent)
    , m_workspace(workspace)
{
    auto controller = workspace->transientTileController();
    if (controller) {
        connect(controller, &TransientTileController::treeChanged, this, &TransientTilingApi::treeChanged);
    }
}

TransientTilingApi::~TransientTilingApi() = default;

// ------------------------------------------------------------------ the writer

QString TransientTilingApi::registerProvider(QObject *provider)
{
    auto writer = m_workspace ? m_workspace->placementWriter() : nullptr;
    if (!writer || !provider) {
        return QString();
    }
    const auto token = writer->registerProvider(provider);
    return token.toString();
}

bool TransientTilingApi::trackWindow(Window *window, const QString &key)
{
    auto writer = m_workspace ? m_workspace->placementWriter() : nullptr;
    if (!writer || !window) {
        return false;
    }
    return writer->trackWindow(window->internalId(), key.isEmpty() ? placementKeyForWindow(window) : key);
}

int TransientTilingApi::record(const QString &token, int revision, Window *window,
                                const QRectF &frame, int eventSequence, int origin)
{
    auto writer = m_workspace ? m_workspace->placementWriter() : nullptr;
    if (!writer || !window) {
        return -1;
    }
    const auto provider = QUuid(token);
    if (provider.isNull()) {
        return -1;
    }
    TransientPlacementWriter::Update update;
    update.window = window->internalId();
    update.frame = frame;
    update.eventSequence = eventSequence;
    update.origin = static_cast<Origin>(origin);
    return static_cast<int>(writer->record(provider, revision, update));
}

QVariantMap TransientTilingApi::snapshot()
{
    auto writer = m_workspace ? m_workspace->placementWriter() : nullptr;
    if (!writer) {
        return {};
    }
    const auto snap = writer->snapshot();
    QVariantMap out;
    QVariantMap entries;
    for (auto it = snap.entries.constBegin(); it != snap.entries.constEnd(); ++it) {
        const auto &r = it.value();
        entries.insert(it.key(), QVariantMap{
            {QStringLiteral("x"), r.x()},
            {QStringLiteral("y"), r.y()},
            {QStringLiteral("width"), r.width()},
            {QStringLiteral("height"), r.height()},
        });
    }
    out.insert(QStringLiteral("entries"), entries);
    out.insert(QStringLiteral("mapRevision"), static_cast<qlonglong>(snap.mapRevision));
    out.insert(QStringLiteral("providerRevision"), static_cast<qlonglong>(snap.providerRevision));
    return out;
}

// ------------------------------------------------------------------ the controller

QVariantMap TransientTilingApi::createTile(LogicalOutput *output, VirtualDesktop *desktop,
                                             const QRectF &relativeRect)
{
    auto controller = m_workspace ? m_workspace->transientTileController() : nullptr;
    QVariantMap out;
    if (!controller || !output || !desktop) {
        out.insert(QStringLiteral("result"), -1);
        out.insert(QStringLiteral("generation"), 0);
        return out;
    }
    auto manager = m_workspace->tileManager(output);
    if (!manager) {
        out.insert(QStringLiteral("result"), -1);
        out.insert(QStringLiteral("generation"), 0);
        return out;
    }
    const auto result = controller->createTree(manager, desktop, relativeRect);
    out.insert(QStringLiteral("result"), static_cast<int>(result));
    out.insert(QStringLiteral("generation"), static_cast<int>(controller->generation()));
    return out;
}

int TransientTilingApi::admitTile(int generation, Window *window, const QString &key,
                                    const QString &providerToken)
{
    auto controller = m_workspace ? m_workspace->transientTileController() : nullptr;
    auto writer = m_workspace ? m_workspace->placementWriter() : nullptr;
    if (!controller || !writer || !window) {
        return -1;
    }
    const auto snap = writer->snapshot();
    const auto result = controller->admit(
        generation,
        window,
        writer,
        QUuid(providerToken),
        key.isEmpty() ? placementKeyForWindow(window) : key,
        snap.providerRevision,
        snap.mapRevision);
    return static_cast<int>(result);
}

int TransientTilingApi::releaseTile(int generation)
{
    auto controller = m_workspace ? m_workspace->transientTileController() : nullptr;
    if (!controller) {
        return -1;
    }
    return static_cast<int>(controller->clearTree(generation));
}

QRectF TransientTilingApi::masterWindowRect() const
{
    auto controller = m_workspace ? m_workspace->transientTileController() : nullptr;
    if (!controller || !controller->tile()) {
        return {};
    }
    return controller->tile()->windowGeometry();
}

bool TransientTilingApi::hasTree() const
{
    auto controller = m_workspace ? m_workspace->transientTileController() : nullptr;
    return controller && controller->rootTile();
}

int TransientTilingApi::generation() const
{
    auto controller = m_workspace ? m_workspace->transientTileController() : nullptr;
    return controller ? static_cast<int>(controller->generation()) : 0;
}

} // namespace KWin

#include "moc_transienttilingapi.cpp"
