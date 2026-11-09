/*
    SPDX-FileCopyrightText: 2015 Martin Flöser <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "waylandwindow.h"
#include "core/pixelgrid.h"
#include "scene/windowitem.h"
#include "wayland/clientconnection.h"
#include "wayland/display.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "workspace.h"

#include <QFileInfo>
#include <QScopeGuard>

#include <csignal>
#include <limits>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

namespace KWin
{

enum WaylandGeometryType {
    WaylandGeometryClient = 0x1,
    WaylandGeometryFrame = 0x2,
    WaylandGeometryBuffer = 0x4,
};
Q_DECLARE_FLAGS(WaylandGeometryTypes, WaylandGeometryType)

WaylandWindow::WaylandWindow(SurfaceInterface *surface)
    : m_isScreenLocker(surface->client() == waylandServer()->screenLockerClientConnection())
    , m_pid(surface->client()->processId())
{
    setSurface(surface);

    connect(surface, &SurfaceInterface::shadowChanged,
            this, &WaylandWindow::updateShadow);
    connect(this, &WaylandWindow::frameGeometryChanged,
            this, &WaylandWindow::updateClientOutputs);
    connect(workspace(), &Workspace::outputsChanged, this, &WaylandWindow::updateClientOutputs);

    updateResourceName();
    updateShadow();
}

std::unique_ptr<WindowItem> WaylandWindow::createItem(Item *parentItem)
{
    return std::make_unique<WindowItemWayland>(this, parentItem);
}

QString WaylandWindow::captionNormal() const
{
    return m_captionNormal;
}

QString WaylandWindow::captionSuffix() const
{
    return m_captionSuffix;
}

pid_t WaylandWindow::pid() const
{
    return m_pid;
}

bool WaylandWindow::isClient() const
{
    return true;
}

bool WaylandWindow::isLockScreen() const
{
    return m_isScreenLocker;
}

bool WaylandWindow::isLocalhost() const
{
    return true;
}

RectF WaylandWindow::resizeWithChecks(const RectF &geometry, const QSizeF &size) const
{
    const RectF area = workspace()->clientArea(WorkArea, this, geometry.center());

    qreal width = size.width();
    qreal height = size.height();

    // don't allow growing larger than workarea
    if (width > area.width()) {
        width = area.width();
    }
    if (height > area.height()) {
        height = area.height();
    }
    return RectF(geometry.topLeft(), QSizeF(width, height));
}

void WaylandWindow::killWindow()
{
    if (!surface()) {
        return;
    }
    auto c = surface()->client();
    if (c->processId() == getpid() || c->processId() == 0) {
        c->destroy();
        return;
    }
    ::kill(c->processId(), SIGTERM);
    // give it time to terminate and only if terminate fails, try destroy Wayland connection
    QTimer::singleShot(5000, c, &ClientConnection::destroy);
}

QString WaylandWindow::windowRole() const
{
    return QString();
}

bool WaylandWindow::belongsToSameApplication(const Window *other, SameApplicationChecks checks) const
{
    if (checks.testFlag(SameApplicationCheck::AllowCrossProcesses)) {
        if (other->desktopFileName() == desktopFileName()) {
            return true;
        }
    }
    if (auto s = other->surface()) {
        return s->client() == surface()->client();
    }
    return false;
}

bool WaylandWindow::belongsToDesktop() const
{
    const auto clients = waylandServer()->windows();

    return std::any_of(clients.constBegin(), clients.constEnd(), [this](const Window *client) {
        if (belongsToSameApplication(client, SameApplicationChecks())) {
            return client->isDesktop();
        }
        return false;
    });
}

void WaylandWindow::updateClientOutputs()
{
    if (isDeleted()) {
        return;
    }
    const auto rect = frameGeometry().toAlignedRect();
    if (rect.isEmpty()) {
        return;
    }

    surface()->setOutputs(waylandServer()->display()->outputsIntersecting(rect));
}

void WaylandWindow::updateResourceName()
{
    const QFileInfo fileInfo(surface()->client()->executablePath());
    if (fileInfo.exists()) {
        const QByteArray executableFileName = fileInfo.fileName().toUtf8();
        setResourceClass(executableFileName, executableFileName);
    }
}

void WaylandWindow::updateCaption()
{
    const QString suffix = shortcutCaptionSuffix();
    if (m_captionSuffix != suffix) {
        m_captionSuffix = suffix;
        Q_EMIT captionChanged();
    }
}

void WaylandWindow::setCaption(const QString &caption)
{
    const QString simplified = caption.simplified();
    if (m_captionNormal != simplified) {
        m_captionNormal = simplified;
        Q_EMIT captionNormalChanged();
        Q_EMIT captionChanged();
    }
}

void WaylandWindow::cleanGrouping()
{
    // We want to break parent-child relationships, but preserve stacking
    // order constraints at the same time for window closing animations.

    if (transientFor()) {
        transientFor()->removeTransientFromList(this);
        setTransientFor(nullptr);
    }

    const auto children = transients();
    for (Window *transient : children) {
        removeTransientFromList(transient);
        transient->setTransientFor(nullptr);
    }
}

RectF WaylandWindow::frameRectToBufferRect(const RectF &rect) const
{
    return RectF(rect.topLeft(), snapToPixels(surface()->size(), targetScale()));
}

void WaylandWindow::updateGeometry(const RectF &rect)
{
    updateGeometryInternal(rect);
}

Window::AssignmentStepResult WaylandWindow::updateGeometryForAssignment(RectF rect, const TileAssignmentFence &before)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    AssignmentStepResult result;
    result.outcome = AssignmentStepOutcome::Superseded;
    if (!before.isCurrentFor(*this)) {
        return result;
    }
    // Preparation may call a virtual conversion; never borrow the caller's fence afterward.
    result.checkpoint.reset(new TileAssignmentFence(*this));
    updateGeometryInternal(rect, &result);
    return result;
}

void WaylandWindow::updateGeometryInternal(const RectF &rect, AssignmentStepResult *guardedResult)
{
    if (!guardedResult && (isDeleted() || m_geometryUpdateGeneration == std::numeric_limits<quint64>::max())) {
        return;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const RectF oldClientGeometry = m_clientGeometry;
    const RectF oldFrameGeometry = m_frameGeometry;
    const RectF oldBufferGeometry = m_bufferGeometry;
    LogicalOutput *oldOutput = m_output;
    const QPointer<LogicalOutput> entryOutput(oldOutput);
    const auto entryGeneration = m_geometryUpdateGeneration;
    const RectF clientGeometry = frameRectToClientRect(rect);
    const RectF bufferGeometry = frameRectToBufferRect(rect);
    auto nextOutput = workspace()->outputAt(rect.center());
    if (guardedResult && (!assignmentResultIsCurrent(*guardedResult) || entryGeneration != m_geometryUpdateGeneration || (oldOutput && !entryOutput) || m_output != entryOutput.data())) {
        guardedResult->checkpoint.reset();
        return;
    }

    WaylandGeometryTypes changedGeometries;

    if (clientGeometry != oldClientGeometry) {
        changedGeometries |= WaylandGeometryClient;
    }
    if (rect != oldFrameGeometry) {
        changedGeometries |= WaylandGeometryFrame;
    }
    if (bufferGeometry != oldBufferGeometry) {
        changedGeometries |= WaylandGeometryBuffer;
    }

    auto &pending = m_pendingGeometryNotifications;
    const bool outputReplaced = oldOutput != nextOutput;
    const bool changed = changedGeometries || (guardedResult && outputReplaced);
    const bool hasPending = pending.buffer || pending.client || pending.frame || pending.outputPending;
    if (!changed && (!guardedResult || !hasPending)) {
        if (guardedResult) {
            guardedResult->outcome = AssignmentStepOutcome::AppliedCurrent;
        }
        return;
    }
    const auto exhausted = std::numeric_limits<quint64>::max();
    if (guardedResult && (m_geometryUpdateGeneration >= exhausted - 1 || (changed && m_placementContinuationRevision >= exhausted - 1))) {
        guardedResult->outcome = AssignmentStepOutcome::Refused;
        guardedResult->checkpoint.reset();
        return;
    }

    const auto generation = ++m_geometryUpdateGeneration;
    if ((changedGeometries & WaylandGeometryBuffer) && !pending.buffer) {
        pending.buffer = oldBufferGeometry;
    }
    if ((changedGeometries & WaylandGeometryClient) && !pending.client) {
        pending.client = oldClientGeometry;
    }
    if ((changedGeometries & WaylandGeometryFrame) && !pending.frame) {
        pending.frame = oldFrameGeometry;
    }
    if (outputReplaced && !pending.outputPending) {
        pending.outputPending = true;
        pending.outputWasNull = !oldOutput;
        pending.output = oldOutput;
    }
    if (changed) {
        m_clientGeometry = clientGeometry;
        m_frameGeometry = rect;
        m_bufferGeometry = bufferGeometry;
        m_output = nextOutput;
        if (m_placementContinuationRevision != exhausted) {
            ++m_placementContinuationRevision;
        }
    }
    const QPointer<LogicalOutput> expectedOutput(m_output);
    const bool hadExpectedOutput = m_output;
    if (guardedResult) {
        guardedResult->checkpoint.reset(new TileAssignmentFence(*this));
    }

    const auto current = [&]() {
        if (isDeleted()) {
            pending = {};
            return false;
        }
        return generation == m_geometryUpdateGeneration
            && (!guardedResult || (assignmentResultIsCurrent(*guardedResult) && (!hadExpectedOutput || expectedOutput) && m_output == expectedOutput.data()));
    };
    const auto finish = qScopeGuard([&]() {
        if (guardedResult) {
            if (current()) {
                guardedResult->outcome = AssignmentStepOutcome::AppliedCurrent;
            } else {
                guardedResult->checkpoint.reset();
            }
        }
    });
    if (changed) {
        updateWindowRules(Rules::Position | Rules::Size);
    }
    if (!current()) {
        return;
    }
    // A newer committed update inherits fields that have not yet been announced.
    if (auto old = std::exchange(pending.buffer, std::nullopt); old && *old != m_bufferGeometry) {
        Q_EMIT bufferGeometryChanged(*old);
        if (!current()) {
            return;
        }
    }
    if (auto old = std::exchange(pending.client, std::nullopt); old && *old != m_clientGeometry) {
        Q_EMIT clientGeometryChanged(*old);
        if (!current()) {
            return;
        }
    }
    if (auto old = std::exchange(pending.frame, std::nullopt); old && *old != m_frameGeometry) {
        Q_EMIT frameGeometryChanged(*old);
        if (!current()) {
            return;
        }
    }
    if (pending.outputPending) {
        const auto output = pending.output;
        const bool wasNull = pending.outputWasNull;
        pending.outputPending = false;
        pending.output.clear();
        // A destroyed old output is not evidence of a net-zero transition.
        if ((!wasNull && !output) || output.data() != m_output) {
            Q_EMIT outputChanged(output.data());
        }
    }
}

void WaylandWindow::markAsMapped()
{
    if (Q_UNLIKELY(!ready_for_painting)) {
        setupItem();
        setReadyForPainting();
    }
}

} // namespace KWin

#include "moc_waylandwindow.cpp"
