/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2019 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xdgshellwindow.h"
#include "core/output.h"
#include "effect/globals.h"
#include "utils/common.h"
#if KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "core/pixelgrid.h"
#include "decorations/decorationbridge.h"
#include "input.h"
#include "killprompt.h"
#include "placement.h"
#include "tiles/tilemanager.h"
#include "utils/subsurfacemonitor.h"
#include "virtualdesktops.h"
#include "wayland/appmenu.h"
#include "wayland/output.h"
#include "wayland/plasmashell.h"
#include "wayland/seat.h"
#include "wayland/server_decoration.h"
#include "wayland/server_decoration_palette.h"
#include "wayland/surface.h"
#include "wayland/tablet_v2.h"
#include "wayland/xdgdecoration_v1.h"
#include "wayland/xdgdialog_v1.h"
#include "wayland/xdgsession_v1.h"
#include "wayland_server.h"
#include "workspace.h"

#include <KDecoration3/DecoratedWindow>
#include <KDecoration3/Decoration>

#include <QPointer>
#include <QScopeGuard>

#include <cmath>
#include <limits>

namespace KWin
{

XdgSurfaceWindow::XdgSurfaceWindow(XdgSurfaceInterface *shellSurface)
    : WaylandWindow(shellSurface->surface())
    , m_shellSurface(shellSurface)
    , m_configureTimer(new QTimer(this))
{
    connect(shellSurface, &XdgSurfaceInterface::configureAcknowledged,
            this, &XdgSurfaceWindow::handleConfigureAcknowledged);
    connect(shellSurface, &XdgSurfaceInterface::resetOccurred,
            this, &XdgSurfaceWindow::destroyWindow);
    connect(shellSurface->surface(), &SurfaceInterface::committed,
            this, &XdgSurfaceWindow::handleCommit);
    connect(shellSurface, &XdgSurfaceInterface::aboutToBeDestroyed,
            this, &XdgSurfaceWindow::destroyWindow);
    connect(shellSurface->surface(), &SurfaceInterface::aboutToBeDestroyed,
            this, &XdgSurfaceWindow::destroyWindow);

    // The effective window geometry is determined by two things: (a) the rectangle that bounds
    // the main surface and all of its sub-surfaces, (b) the client-specified window geometry, if
    // any. If the client hasn't provided the window geometry, we fallback to the bounding sub-
    // surface rectangle. If the client has provided the window geometry, we intersect it with
    // the bounding rectangle and that will be the effective window geometry. It's worth to point
    // out that geometry updates do not occur that frequently, so we don't need to recompute the
    // bounding geometry every time the client commits the surface.

    SubSurfaceMonitor *treeMonitor = new SubSurfaceMonitor(surface(), this);

    connect(treeMonitor, &SubSurfaceMonitor::subSurfaceAdded,
            this, &XdgSurfaceWindow::setHaveNextWindowGeometry);
    connect(treeMonitor, &SubSurfaceMonitor::subSurfaceRemoved,
            this, &XdgSurfaceWindow::setHaveNextWindowGeometry);
    connect(treeMonitor, &SubSurfaceMonitor::subSurfaceMoved,
            this, &XdgSurfaceWindow::setHaveNextWindowGeometry);
    connect(treeMonitor, &SubSurfaceMonitor::subSurfaceResized,
            this, &XdgSurfaceWindow::setHaveNextWindowGeometry);
    connect(shellSurface, &XdgSurfaceInterface::windowGeometryChanged,
            this, &XdgSurfaceWindow::setHaveNextWindowGeometry);
    connect(surface(), &SurfaceInterface::sizeChanged,
            this, &XdgSurfaceWindow::setHaveNextWindowGeometry);

    // Configure events are not sent immediately, but rather scheduled to be sent when the event
    // loop is about to be idle. By doing this, we can avoid sending configure events that do
    // nothing, and implementation-wise, it's simpler.

    m_configureTimer->setSingleShot(true);
    connect(m_configureTimer, &QTimer::timeout, this, &XdgSurfaceWindow::sendConfigure);
}

XdgSurfaceWindow::~XdgSurfaceWindow()
{
}

WindowType XdgSurfaceWindow::windowType() const
{
    return m_windowType;
}

XdgSurfaceConfigure *XdgSurfaceWindow::lastAcknowledgedConfigure() const
{
    return m_lastAcknowledgedConfigure.get();
}

bool XdgSurfaceWindow::hasPendingNativeGeometry() const
{
    return m_configureTimer->isActive() || !m_configureEvents.isEmpty() || m_lastAcknowledgedConfigure
        || m_lastAcknowledgedConfigureSerial.has_value() || m_haveNextWindowGeometry || m_configureFlags;
}

void XdgSurfaceWindow::scheduleConfigure()
{
    if (!isDeleted()) {
        m_configureTimer->start();
    }
}

void XdgSurfaceWindow::sendConfigure()
{
    XdgSurfaceConfigure *configureEvent = sendRoleConfigure();

    // The configure event inherits configure flags from the previous event.
    if (!m_configureEvents.isEmpty()) {
        const XdgSurfaceConfigure *previousEvent = m_configureEvents.constLast();
        configureEvent->flags = previousEvent->flags;
    }

    configureEvent->gravity = m_nextGravity;
    configureEvent->flags |= m_configureFlags;
    configureEvent->scale = m_nextTargetScale;
    m_configureFlags = {};
    if (!isInteractiveMoveResize()) {
        m_nextGravity = Gravity::None;
    }

    m_configureEvents.append(configureEvent);
}

void XdgSurfaceWindow::handleConfigureAcknowledged(quint32 serial)
{
    m_lastAcknowledgedConfigureSerial = serial;
}

void XdgSurfaceWindow::handleCommit()
{
    if (isDeleted()) {
        return;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    if (!m_shellSurface->isConfigured()) {
        return;
    }

    if (!surface()->buffer()) {
        return;
    }

    if (m_lastAcknowledgedConfigureSerial.has_value()) {
        const quint32 serial = m_lastAcknowledgedConfigureSerial.value();
        while (!m_configureEvents.isEmpty()) {
            if (serial < m_configureEvents.constFirst()->serial) {
                break;
            }
            m_lastAcknowledgedConfigure.reset(m_configureEvents.takeFirst());
        }
    }

    handleRolePrecommit();
    if (isDeleted()) {
        return;
    }
    if (haveNextWindowGeometry()) {
        handleNextWindowGeometry();
        if (isDeleted()) {
            return;
        }
        resetHaveNextWindowGeometry();
    }

    handleRoleCommit();
    if (isDeleted()) {
        return;
    }
    m_lastAcknowledgedConfigure.reset();
    m_lastAcknowledgedConfigureSerial.reset();

    QPointer<XdgSurfaceWindow> guard(this);
    markAsMapped();
    if (guard && !guard->isDeleted()) {
        guard->notifyRequestedGeometryChanged();
    }
}

void XdgSurfaceWindow::handleRolePrecommit()
{
}

void XdgSurfaceWindow::handleRoleCommit()
{
}

void XdgSurfaceWindow::maybeUpdateMoveResizeGeometry(const RectF &rect)
{
    // We are about to send a configure event, ignore the committed window geometry.
    if (m_configureTimer->isActive()) {
        return;
    }

    // If there are unacknowledged configure events that change the geometry, don't sync
    // the move resize geometry in order to avoid rolling back to old state. When the last
    // configure event is acknowledged, the move resize geometry will be synchronized.
    for (int i = m_configureEvents.count() - 1; i >= 0; --i) {
        if (m_configureEvents[i]->flags & XdgSurfaceConfigure::ConfigurePosition) {
            return;
        }
    }

    setMoveResizeGeometry(rect);
}

void XdgSurfaceWindow::handleNextWindowGeometry()
{
    if (const XdgSurfaceConfigure *configureEvent = lastAcknowledgedConfigure()) {
        setTargetScale(configureEvent->scale);
        if (isDeleted()) {
            return;
        }
    }
    const RectF boundingGeometry = surface()->boundingRect();

    // The effective window geometry is defined as the intersection of the window geometry
    // and the rectangle that bounds the main surface and all of its sub-surfaces. If the
    // client hasn't specified the window geometry, we must fallback to the bounding geometry.
    // Note that the xdg-shell spec is not clear about when exactly we have to clamp the
    // window geometry.

    m_windowGeometry = m_shellSurface->windowGeometry();
    if (m_windowGeometry.isValid()) {
        m_windowGeometry &= boundingGeometry;
    } else {
        m_windowGeometry = boundingGeometry;
    }

    if (m_windowGeometry.isEmpty()) {
        qCWarning(KWIN_CORE) << "Committed empty window geometry, dealing with a buggy client!";
    } else {
        m_windowGeometry = snapToPixels(m_windowGeometry, targetScale());
    }

    RectF frameGeometry(pos(), clientSizeToFrameSize(m_windowGeometry.size()));
    if (const XdgSurfaceConfigure *configureEvent = lastAcknowledgedConfigure()) {
        if (configureEvent->flags & XdgSurfaceConfigure::ConfigurePosition) {
            frameGeometry = configureEvent->gravity.apply(frameGeometry, configureEvent->bounds);
        }
    }

    if (isInteractiveMove()) {
        bool fullscreen = isFullScreen();
        if (const auto configureEvent = static_cast<XdgToplevelConfigure *>(lastAcknowledgedConfigure())) {
            fullscreen = configureEvent->states & XdgToplevelInterface::State::FullScreen;
        }
        if (!fullscreen) {
            frameGeometry = nextInteractiveMoveGeometry(frameGeometry);
        }
    }

    maybeUpdateMoveResizeGeometry(frameGeometry);
    if (isDeleted()) {
        return;
    }
    updateGeometry(frameGeometry);
}

bool XdgSurfaceWindow::haveNextWindowGeometry() const
{
    return m_haveNextWindowGeometry || m_lastAcknowledgedConfigure;
}

void XdgSurfaceWindow::setHaveNextWindowGeometry()
{
    m_haveNextWindowGeometry = true;
}

void XdgSurfaceWindow::resetHaveNextWindowGeometry()
{
    m_haveNextWindowGeometry = false;
}

void XdgSurfaceWindow::moveResizeInternal(const RectF &rect, MoveResizeMode mode)
{
    dispatchMoveResize(rect, mode, nullptr);
}

void XdgSurfaceWindow::dispatchAssignmentGeometryRequest(const RectF &rect, MoveResizeMode mode, AssignmentGeometryRequest &request)
{
    dispatchMoveResize(rect, mode, &request);
}

void XdgSurfaceWindow::dispatchMoveResize(const RectF &rect, MoveResizeMode mode, AssignmentGeometryRequest *guarded)
{
    const auto generation = geometryRequestGeneration();
    const auto current = [&]() {
        if (!geometryRequestIsCurrent(generation) || !transientGeometryRequestIsCurrent(generation)) {
            return false;
        }
        return !guarded || assignmentResultIsCurrent(guarded->result);
    };
    Q_EMIT frameGeometryAboutToChange();
    if (!current()) {
        if (guarded) {
            guarded->result.checkpoint.reset();
        }
        return;
    }

    if (mode != MoveResizeMode::Move) {
        // xdg-shell doesn't support fractional sizes, so the requested
        // and current client sizes have to be rounded to integers
        const QSizeF requestedFrameSize = snapToPixels(rect.size(), nextTargetScale());
        const QSizeF requestedClientSize = nextFrameSizeToClientSize(requestedFrameSize);
        const QSizeF roundedRequestedClientSize = surface()->snappedSize(requestedClientSize);

        const QSizeF roundedClientSize = surface()->snappedSize(clientSize());
        geometryResizeSubmitted(generation);
        if (roundedRequestedClientSize == roundedClientSize) {
            const RectF snappedRect = RectF(rect.topLeft(), nextClientSizeToFrameSize(snapToPixels(roundedClientSize, nextTargetScale())));
            const RectF target = m_nextGravity.apply(snappedRect, rect);
            if (guarded) {
                publishForAssignment(target, *guarded);
            } else {
                updateGeometry(target);
            }
        } else {
            m_configureFlags |= XdgSurfaceConfigure::ConfigurePosition;
            scheduleConfigure();
            if (guarded) {
                // Staged for the coalesced configure, neither sent nor completed. Staging runs no
                // callbacks, so the checkpoint taken before it is validated, not replaced.
                guarded->dispatch = AssignmentRequestDispatch::Queued;
                if (current()) {
                    guarded->result.outcome = AssignmentStepOutcome::AppliedCurrent;
                } else {
                    guarded->result.checkpoint.reset();
                }
            }
        }
    } else {
        // If the window is moved, cancel any queued window position updates.
        for (XdgSurfaceConfigure *configureEvent : std::as_const(m_configureEvents)) {
            configureEvent->flags.setFlag(XdgSurfaceConfigure::ConfigurePosition, false);
        }
        m_configureFlags.setFlag(XdgSurfaceConfigure::ConfigurePosition, false);
        const RectF target(rect.topLeft(), size());
        if (guarded) {
            publishForAssignment(target, *guarded);
        } else {
            updateGeometry(target);
        }
    }
}

void XdgSurfaceWindow::publishForAssignment(const RectF &rect, AssignmentGeometryRequest &request)
{
    // Fences do not observe the publication generation, so a nested drain during earlier callbacks
    // can have consumed the headroom reserved at preflight. Detect that here rather than letting
    // the publication helper refuse after this request has already mutated state.
    if (m_geometryUpdateGeneration >= std::numeric_limits<quint64>::max() - 1) {
        request.result.checkpoint.reset();
        return;
    }
    request.dispatch = AssignmentRequestDispatch::Published;
    updateGeometryInternal(rect, &request.result);
}

RectF XdgSurfaceWindow::frameRectToBufferRect(const RectF &rect) const
{
    const qreal left = rect.left() + borderLeft() - m_windowGeometry.left();
    const qreal top = rect.top() + borderTop() - m_windowGeometry.top();
    return RectF(QPointF(left, top), snapToPixels(surface()->size(), m_targetScale));
}

void XdgSurfaceWindow::handleRoleDestroyed()
{
    if (m_plasmaShellSurface) {
        m_plasmaShellSurface->disconnect(this);
    }
    m_shellSurface->disconnect(this);
    m_shellSurface->surface()->disconnect(this);
}

void XdgSurfaceWindow::destroyWindow()
{
    handleRoleDestroyed();
    markAsDeleted();
    Q_EMIT closed();

    stopDelayedInteractiveMoveResize();
    if (isInteractiveMoveResize()) {
        leaveInteractiveMoveResize();
        Q_EMIT interactiveMoveResizeFinished();
    }
    m_configureTimer->stop();
    qDeleteAll(m_configureEvents);
    m_configureEvents.clear();
    cleanTabBox();
    StackingUpdatesBlocker blocker(workspace());
    workspace()->rulebook()->discardUsed(this, true);
    waylandServer()->removeWindow(this);
    cleanGrouping();

    unref();
}

/**
 * \todo This whole plasma shell surface thing doesn't seem right. It turns xdg-toplevel into
 * something completely different! Perhaps plasmashell surfaces need to be implemented via a
 * proprietary protocol that doesn't piggyback on existing shell surface protocols. It'll lead
 * to cleaner code and will be technically correct, but I'm not sure whether this is do-able.
 */
void XdgSurfaceWindow::installPlasmaShellSurface(PlasmaShellSurfaceInterface *shellSurface)
{
    m_plasmaShellSurface = shellSurface;

    auto updatePosition = [this, shellSurface] {
        if (!isInteractiveMoveResize()) {
            place(shellSurface->position());
        }
    };
    auto showUnderCursor = [this] {
        // Wait for the first commit
        auto moveUnderCursor = [this] {
            if (input()->hasPointer()) {
                place(input()->globalPointer());
                moveResize(keepInArea(moveResizeGeometry(), workspace()->clientArea(PlacementArea, this, workspace()->outputAt(pos()))));
            }
        };
        connect(this, &Window::readyForPaintingChanged, this, moveUnderCursor, Qt::SingleShotConnection);
    };
    auto updateRole = [this, shellSurface] {
        WindowType type = WindowType::Unknown;
        switch (shellSurface->role()) {
        case PlasmaShellSurfaceInterface::Role::Desktop:
            type = WindowType::Desktop;
            break;
        case PlasmaShellSurfaceInterface::Role::Panel:
            type = WindowType::Dock;
            break;
        case PlasmaShellSurfaceInterface::Role::OnScreenDisplay:
            type = WindowType::OnScreenDisplay;
            break;
        case PlasmaShellSurfaceInterface::Role::Notification:
            type = WindowType::Notification;
            break;
        case PlasmaShellSurfaceInterface::Role::ToolTip:
            type = WindowType::Tooltip;
            break;
        case PlasmaShellSurfaceInterface::Role::CriticalNotification:
            type = WindowType::CriticalNotification;
            break;
        case PlasmaShellSurfaceInterface::Role::AppletPopup:
            type = WindowType::AppletPopup;
            break;
        case PlasmaShellSurfaceInterface::Role::Normal:
        default:
            type = WindowType::Normal;
            break;
        }
        if (m_windowType == type) {
            return;
        }
        m_windowType = type;
        switch (m_windowType) {
        case WindowType::Desktop:
        case WindowType::Dock:
        case WindowType::OnScreenDisplay:
        case WindowType::Notification:
        case WindowType::CriticalNotification:
        case WindowType::Tooltip:
        case WindowType::AppletPopup:
            setOnAllDesktops(true);
#if KWIN_BUILD_ACTIVITIES
            setOnAllActivities(true);
#endif
            break;
        default:
            break;
        }
    };
    connect(shellSurface, &PlasmaShellSurfaceInterface::positionChanged, this, updatePosition);
    connect(shellSurface, &PlasmaShellSurfaceInterface::openUnderCursorRequested, this, showUnderCursor);
    connect(shellSurface, &PlasmaShellSurfaceInterface::roleChanged, this, updateRole);
    connect(shellSurface, &PlasmaShellSurfaceInterface::panelTakesFocusChanged, this, [this] {
        if (m_plasmaShellSurface->panelTakesFocus()) {
            workspace()->activateWindow(this);
        }
    });
    if (shellSurface->isPositionSet()) {
        updatePosition();
    }
    if (shellSurface->wantsOpenUnderCursor()) {
        showUnderCursor();
    }
    updateRole();

    setSkipTaskbar(shellSurface->skipTaskbar());
    connect(shellSurface, &PlasmaShellSurfaceInterface::skipTaskbarChanged, this, [this] {
        setSkipTaskbar(m_plasmaShellSurface->skipTaskbar());
    });

    setSkipSwitcher(shellSurface->skipSwitcher());
    connect(shellSurface, &PlasmaShellSurfaceInterface::skipSwitcherChanged, this, [this] {
        setSkipSwitcher(m_plasmaShellSurface->skipSwitcher());
    });
}

std::optional<XdgToplevelSessionData> XdgToplevelSessionData::parse(const QVariant &variant)
{
    if (variant.isNull()) {
        return std::nullopt;
    }

    XdgToplevelSessionData data;

    const QVariantHash vardict = variant.toHash();
    for (const auto &[key, value] : vardict.asKeyValueRange()) {
        if (key == QStringLiteral("position")) {
            data.position = value.toPointF();
        } else if (key == QStringLiteral("outputLayoutId")) {
            data.outputLayoutId = value.toString();
        } else if (key == QStringLiteral("size")) {
            data.size = value.toSizeF();
        } else if (key == QStringLiteral("keepAbove")) {
            data.keepAbove = value.toBool();
        } else if (key == QStringLiteral("keepBelow")) {
            data.keepBelow = value.toBool();
        } else if (key == QStringLiteral("skipSwitcher")) {
            data.skipSwitcher = value.toBool();
        } else if (key == QStringLiteral("skipPager")) {
            data.skipPager = value.toBool();
        } else if (key == QStringLiteral("skipTaskbar")) {
            data.skipTaskbar = value.toBool();
        } else if (key == QStringLiteral("maximizeMode")) {
            data.maximizeMode = MaximizeMode(value.toUInt());
        } else if (key == QStringLiteral("fullscreenMode")) {
            data.fullscreen = value.toBool();
        } else if (key == QStringLiteral("minimizeMode")) {
            data.minimized = value.toBool();
        } else if (key == QStringLiteral("desktops")) {
            data.desktops = value.toStringList();
        } else if (key == QStringLiteral("activities")) {
            data.activities = value.toStringList();
        } else if (key == QStringLiteral("decorationPolicy")) {
            data.decorationPolicy = DecorationPolicy(value.toUInt());
        } else if (key == QStringLiteral("shortcut")) {
            data.shortcut = value.toString();
        } else if (key == QStringLiteral("excludeFromCapture")) {
            data.excludeFromCapture = value.toBool();
        }
    }

    return data;
}

QVariant XdgToplevelSessionData::save(const Window *window)
{
    return QVariantHash{
        {QStringLiteral("position"), window->moveResizeGeometry().topLeft()},
        {QStringLiteral("outputLayoutId"), workspace()->outputLayoutId()},
        {QStringLiteral("size"), window->moveResizeGeometry().size()},
        {QStringLiteral("keepAbove"), window->keepAbove()},
        {QStringLiteral("keepBelow"), window->keepBelow()},
        {QStringLiteral("skipSwitcher"), window->skipSwitcher()},
        {QStringLiteral("skipPager"), window->skipPager()},
        {QStringLiteral("skipTaskbar"), window->skipTaskbar()},
        {QStringLiteral("maximizeMode"), uint(window->requestedMaximizeMode())},
        {QStringLiteral("fullscreenMode"), window->isRequestedFullScreen()},
        {QStringLiteral("minimizeMode"), window->isMinimized()},
        {QStringLiteral("desktops"), window->desktopIds()},
        {QStringLiteral("activities"), window->activities()},
        {QStringLiteral("decorationPolicy"), uint(window->decorationPolicy())},
        {QStringLiteral("shortcut"), window->shortcut().toString()},
        {QStringLiteral("excludeFromCapture"), window->excludeFromCapture()},
    };
}

XdgToplevelWindow::XdgToplevelWindow(XdgToplevelInterface *shellSurface)
    : XdgSurfaceWindow(shellSurface->xdgSurface())
    , m_shellSurface(shellSurface)
{
    setOutput(workspace()->activeOutput());
    setMoveResizeOutput(workspace()->activeOutput());
    setDesktops({VirtualDesktopManager::self()->currentDesktop()});
#if KWIN_BUILD_ACTIVITIES
    if (auto a = Workspace::self()->activities()) {
        setOnActivities({a->current()});
    }
#endif
    move(workspace()->activeOutput()->geometry().center());

    connect(shellSurface, &XdgToplevelInterface::titleChanged,
            this, &XdgToplevelWindow::handleWindowTitleChanged);
    connect(shellSurface, &XdgToplevelInterface::appIdChanged,
            this, &XdgToplevelWindow::handleAppIdChanged);
    connect(shellSurface, &XdgToplevelInterface::windowMenuRequested,
            this, &XdgToplevelWindow::handleWindowMenuRequested);
    connect(shellSurface, &XdgToplevelInterface::moveRequested,
            this, &XdgToplevelWindow::handleMoveRequested);
    connect(shellSurface, &XdgToplevelInterface::resizeRequested,
            this, &XdgToplevelWindow::handleResizeRequested);
    connect(shellSurface, &XdgToplevelInterface::maximizeRequested,
            this, &XdgToplevelWindow::handleMaximizeRequested);
    connect(shellSurface, &XdgToplevelInterface::unmaximizeRequested,
            this, &XdgToplevelWindow::handleUnmaximizeRequested);
    connect(shellSurface, &XdgToplevelInterface::fullscreenRequested,
            this, &XdgToplevelWindow::handleFullscreenRequested);
    connect(shellSurface, &XdgToplevelInterface::unfullscreenRequested,
            this, &XdgToplevelWindow::handleUnfullscreenRequested);
    connect(shellSurface, &XdgToplevelInterface::minimizeRequested,
            this, &XdgToplevelWindow::handleMinimizeRequested);
    connect(shellSurface, &XdgToplevelInterface::parentXdgToplevelChanged,
            this, &XdgToplevelWindow::handleTransientForChanged);
    connect(shellSurface, &XdgToplevelInterface::initializeRequested,
            this, &XdgToplevelWindow::initialize);
    connect(shellSurface, &XdgToplevelInterface::aboutToBeDestroyed,
            this, &XdgToplevelWindow::destroyWindow);
    connect(shellSurface, &XdgToplevelInterface::maximumSizeChanged,
            this, &XdgToplevelWindow::handleMaximumSizeChanged);
    connect(shellSurface, &XdgToplevelInterface::minimumSizeChanged,
            this, &XdgToplevelWindow::handleMinimumSizeChanged);
    connect(shellSurface->shell(), &XdgShellInterface::pingTimeout,
            this, &XdgToplevelWindow::handlePingTimeout);
    connect(shellSurface->shell(), &XdgShellInterface::pingDelayed,
            this, &XdgToplevelWindow::handlePingDelayed);
    connect(shellSurface->shell(), &XdgShellInterface::pongReceived,
            this, &XdgToplevelWindow::handlePongReceived);

    connect(waylandServer(), &WaylandServer::foreignTransientChanged,
            this, &XdgToplevelWindow::handleForeignTransientForChanged);

    connect(shellSurface, &XdgToplevelInterface::customIconChanged, this, &XdgToplevelWindow::updateIcon);
    connect(this, &XdgToplevelWindow::desktopFileNameChanged, this, &XdgToplevelWindow::updateIcon);

    connect(shellSurface, &XdgToplevelInterface::tagChanged, this, [this]() {
        if (m_tag == m_shellSurface->tag()) {
            return;
        }
        m_tag = m_shellSurface->tag();
        Q_EMIT tagChanged();
        if (m_isInitialized) {
            evaluateWindowRules();
        }
    });
    connect(shellSurface, &XdgToplevelInterface::descriptionChanged, this, [this]() {
        setDescription(m_shellSurface->description());
    });
}

XdgToplevelWindow::~XdgToplevelWindow()
{
    if (m_killPrompt) {
        m_killPrompt->quit();
    }
}

void XdgToplevelWindow::handleRoleDestroyed()
{
    if (XdgToplevelSessionV1Interface *session = m_shellSurface->session()) {
        session->write(XdgToplevelSessionData::save(this));
    }

    destroyWindowManagementInterface();

    if (m_appMenuInterface) {
        m_appMenuInterface->disconnect(this);
    }
    if (m_paletteInterface) {
        m_paletteInterface->disconnect(this);
    }
    if (m_xdgDecoration) {
        m_xdgDecoration->disconnect(this);
    }
    if (m_serverDecoration) {
        m_serverDecoration->disconnect(this);
    }
    if (m_xdgDialog) {
        m_xdgDialog->disconnect(this);
    }

    m_shellSurface->disconnect(this);

    disconnect(waylandServer(), &WaylandServer::foreignTransientChanged,
               this, &XdgToplevelWindow::handleForeignTransientForChanged);

    XdgSurfaceWindow::handleRoleDestroyed();
}

XdgToplevelInterface *XdgToplevelWindow::shellSurface() const
{
    return m_shellSurface;
}

MaximizeMode XdgToplevelWindow::maximizeMode() const
{
    return m_maximizeMode;
}

MaximizeMode XdgToplevelWindow::requestedMaximizeMode() const
{
    return m_requestedMaximizeMode;
}

QSizeF XdgToplevelWindow::ruleCheckedMinimumSize() const
{
    return rules()->checkMinSize(m_minimumSize);
}

QSizeF XdgToplevelWindow::ruleCheckedMaximumSize() const
{
    return rules()->checkMaxSize(QSizeF(m_maximumSize.width() > 0 ? m_maximumSize.width() : INT_MAX,
                                        m_maximumSize.height() > 0 ? m_maximumSize.height() : INT_MAX));
}

QSizeF XdgToplevelWindow::minSize() const
{
    const int enforcedMinimum = m_nextDecoration ? 150 : 20;
    return ruleCheckedMinimumSize().expandedTo(QSizeF(enforcedMinimum, enforcedMinimum));
}

QSizeF XdgToplevelWindow::maxSize() const
{
    return ruleCheckedMaximumSize().expandedTo(minSize());
}

bool XdgToplevelWindow::isFullScreen() const
{
    return m_isFullScreen;
}

bool XdgToplevelWindow::isRequestedFullScreen() const
{
    return m_isRequestedFullScreen;
}

bool XdgToplevelWindow::isMovable() const
{
    if (isRequestedFullScreen()) {
        return false;
    }
    if (isSpecialWindow() && !isSplash() && !isToolbar()) {
        return false;
    }
    if (rules()->checkPosition(invalidPoint) != invalidPoint) {
        return false;
    }
    if (!options->interactiveWindowMoveEnabled()) {
        return false;
    }
    return true;
}

bool XdgToplevelWindow::isMovableAcrossScreens() const
{
    if (isSpecialWindow() && !isSplash() && !isToolbar()) {
        return false;
    }
    if (rules()->checkPosition(invalidPoint) != invalidPoint) {
        return false;
    }
    if (!options->interactiveWindowMoveEnabled()) {
        return false;
    }
    return true;
}

bool XdgToplevelWindow::isResizable() const
{
    if (isRequestedFullScreen()) {
        return false;
    }
    if (isSpecialWindow() && !isAppletPopup()) {
        return false;
    }
    if (rules()->checkSize(QSize()).isValid()) {
        return false;
    }
    const QSizeF min = minSize();
    const QSizeF max = maxSize();
    return min.width() < max.width() || min.height() < max.height();
}

bool XdgToplevelWindow::isCloseable() const
{
    return rules()->checkCloseable(!isDesktop() && !isDock());
}

bool XdgToplevelWindow::isFullScreenable() const
{
    if (!rules()->checkFullScreen(true)) {
        return false;
    }
    return !isSpecialWindow();
}

bool XdgToplevelWindow::isMaximizable() const
{
    if (!isResizable() || isAppletPopup()) {
        return false;
    }
    if (rules()->checkMaximize(MaximizeRestore) != MaximizeRestore || rules()->checkMaximize(MaximizeFull) != MaximizeFull) {
        return false;
    }
    return true;
}

bool XdgToplevelWindow::isMinimizable() const
{
    if ((isSpecialWindow() && !isTransient()) || isAppletPopup()) {
        return false;
    }
    if (!rules()->checkMinimize(true)) {
        return false;
    }
    return true;
}

bool XdgToplevelWindow::isPlaceable() const
{
    if (m_plasmaShellSurface) {
        return !m_plasmaShellSurface->isPositionSet() && !m_plasmaShellSurface->wantsOpenUnderCursor();
    }
    return true;
}

bool XdgToplevelWindow::isTransient() const
{
    return m_isTransient;
}

DecorationPolicy XdgToplevelWindow::decorationPolicy() const
{
    return m_decorationPolicy;
}

void XdgToplevelWindow::setDecorationPolicy(DecorationPolicy policy)
{
    const auto effectivePolicy = rules()->checkDecorationPolicy(policy);
    if (m_decorationPolicy == effectivePolicy) {
        return;
    }
    m_decorationPolicy = effectivePolicy;
    configureDecoration();
    updateWindowRules(Rules::NoBorder);
    Q_EMIT decorationPolicyChanged();
}

KDecoration3::Decoration *XdgToplevelWindow::nextDecoration() const
{
    return m_nextDecoration.get();
}

void XdgToplevelWindow::invalidateDecoration()
{
    clearDecoration();
    configureDecoration();
}

bool XdgToplevelWindow::supportsWindowRules() const
{
    return true;
}

void XdgToplevelWindow::applyWindowRules()
{
    WaylandWindow::applyWindowRules();
    updateCapabilities();
}

void XdgToplevelWindow::closeWindow()
{
    if (isDeleted()) {
        return;
    }
    if (isCloseable()) {
        sendPing(PingReason::CloseWindow);
        m_shellSurface->sendClose();
    }
}

XdgSurfaceConfigure *XdgToplevelWindow::sendRoleConfigure()
{
    surface()->setPreferredBufferScale(nextTargetScale());
    surface()->setPreferredBufferTransform(preferredBufferTransform());
    surface()->setPreferredColorDescription(preferredColorDescription());

    QSizeF framePadding(0, 0);
    if (m_nextDecoration) {
        const auto borders = m_nextDecorationState->borders();
        framePadding.setWidth(borders.left() + borders.right());
        framePadding.setHeight(borders.top() + borders.bottom());
    }

    QSizeF nextClientSize = snapToPixels(moveResizeGeometry().size(), nextTargetScale());
    if (!nextClientSize.isEmpty()) {
        nextClientSize.setWidth(std::max(1.0, nextClientSize.width() - framePadding.width()));
        nextClientSize.setHeight(std::max(1.0, nextClientSize.height() - framePadding.height()));
    }

    if (nextClientSize.isEmpty()) {
        QSizeF bounds = workspace()->clientArea(PlacementArea, this, moveResizeOutput()).size();
        bounds.setWidth(std::max(1.0, bounds.width() - framePadding.width()));
        bounds.setHeight(std::max(1.0, bounds.height() - framePadding.height()));
        m_shellSurface->sendConfigureBounds(bounds);
    }

    const quint32 serial = m_shellSurface->sendConfigure(nextClientSize, m_nextStates);

    XdgToplevelConfigure *configureEvent = new XdgToplevelConfigure();
    configureEvent->bounds = moveResizeGeometry();
    configureEvent->states = m_nextStates;
    configureEvent->decoration = m_nextDecoration;
    configureEvent->decorationState = m_nextDecorationState;
    configureEvent->serial = serial;
    configureEvent->tile = m_requestedTile;
    configureEvent->transientOwnershipGeneration = requestedTransientTileGeneration();

    return configureEvent;
}

void XdgToplevelWindow::handleRolePrecommit()
{
    if (auto configureEvent = static_cast<XdgToplevelConfigure *>(lastAcknowledgedConfigure())) {
        const auto nextDecoration = configureEvent->decoration;
        const auto nextState = configureEvent->decorationState;
        if (nextDecoration) {
            nextDecoration->apply(nextState);
            if (isDeleted()) {
                return;
            }
        }

        if (decoration() != nextDecoration.get()) {
            setDecoration(nextDecoration);
            if (isDeleted()) {
                return;
            }
            updateShadow();
        }
    }
}

void XdgToplevelWindow::handleRoleCommit()
{
    auto configureEvent = static_cast<XdgToplevelConfigure *>(lastAcknowledgedConfigure());
    if (configureEvent) {
        const auto states = configureEvent->states;
        const QPointer<Tile> tile = configureEvent->tile;
        const quint64 transientGeneration = configureEvent->transientOwnershipGeneration;
        handleStatesAcknowledged(states);
        if (isDeleted()) {
            return;
        }
        commitTileFromConfigure(tile, transientGeneration);
    }
}

bool XdgToplevelWindow::supportsAssignmentGeometryRequest() const
{
    // Popups keep their own placement rules; only toplevels take guarded geometry requests.
    return true;
}

bool XdgToplevelWindow::canStartTransientTileRequest(const RectF &target) const
{
    if (!m_isInitialized || !readyForPainting() || !frameGeometry().isValid() || !moveResizeGeometry().isValid()) {
        return false;
    }
    for (qreal value : {frameGeometry().x(), frameGeometry().y(), frameGeometry().width(), frameGeometry().height()}) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return !hasPendingNativeGeometry() && !requestedTile() && !tile() && isTransientTileTargetFeasible(target);
}

bool XdgToplevelWindow::isTransientTileTargetFeasible(const RectF &target) const
{
    if (isDeleted() || !isNormalWindow() || !isMovable() || !isResizable() || isInteractiveMoveResize()
        || isFullScreen() || isRequestedFullScreen() || maximizeMode() != MaximizeRestore || requestedMaximizeMode() != MaximizeRestore) {
        return false;
    }
    for (qreal value : {target.x(), target.y(), target.width(), target.height()}) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    if (target.width() <= 0 || target.height() <= 0) {
        return false;
    }
    const auto constrained = constrainFrameSize(target.size());
    if (constrained.width() != target.width() || constrained.height() != target.height()) {
        return false;
    }
    const auto snappedFrame = snapToPixels(target.size(), nextTargetScale());
    if (snappedFrame.width() != target.width() || snappedFrame.height() != target.height()) {
        return false;
    }
    const auto client = nextFrameSizeToClientSize(snappedFrame);
    const auto snappedClient = surface()->snappedSize(client);
    const auto roundTrip = nextClientSizeToFrameSize(snapToPixels(snappedClient, nextTargetScale()));
    return client.width() == snappedClient.width() && client.height() == snappedClient.height()
        && roundTrip.width() == target.width() && roundTrip.height() == target.height();
}

void XdgToplevelWindow::doMinimize()
{
    if (m_isInitialized) {
        if (isMinimized()) {
            workspace()->activateNextWindow(this);
        }
    }
    workspace()->updateMinimizedOfTransients(this);
}

void XdgToplevelWindow::doSetActive()
{
    if (isActive()) {
        m_nextStates |= XdgToplevelInterface::State::Activated;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::Activated;
    }

    scheduleConfigure();
}

void XdgToplevelWindow::doSetFullScreen()
{
    if (isRequestedFullScreen()) {
        m_nextStates |= XdgToplevelInterface::State::FullScreen;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::FullScreen;
    }

    scheduleConfigure();
}

void XdgToplevelWindow::doSetMaximized()
{
    if (requestedMaximizeMode() & MaximizeHorizontal) {
        m_nextStates |= XdgToplevelInterface::State::MaximizedHorizontal;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::MaximizedHorizontal;
    }

    if (requestedMaximizeMode() & MaximizeVertical) {
        m_nextStates |= XdgToplevelInterface::State::MaximizedVertical;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::MaximizedVertical;
    }

    scheduleConfigure();
}

void XdgToplevelWindow::doSetQuickTileMode()
{
    const Qt::Edges anchors = requestedTile() ? requestedTile()->anchors() : Qt::Edges();

    if (anchors & Qt::LeftEdge) {
        m_nextStates |= XdgToplevelInterface::State::TiledLeft;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::TiledLeft;
    }

    if (anchors & Qt::RightEdge) {
        m_nextStates |= XdgToplevelInterface::State::TiledRight;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::TiledRight;
    }

    if (anchors & Qt::TopEdge) {
        m_nextStates |= XdgToplevelInterface::State::TiledTop;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::TiledTop;
    }

    if (anchors & Qt::BottomEdge) {
        m_nextStates |= XdgToplevelInterface::State::TiledBottom;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::TiledBottom;
    }

    scheduleConfigure();
}

bool XdgToplevelWindow::doStartInteractiveMoveResize()
{
    if (interactiveMoveResizeGravity() != Gravity::None) {
        m_nextGravity = interactiveMoveResizeGravity();
        m_nextStates |= XdgToplevelInterface::State::Resizing;
        scheduleConfigure();
    }
    return true;
}

void XdgToplevelWindow::doFinishInteractiveMoveResize()
{
    if (m_nextStates & XdgToplevelInterface::State::Resizing) {
        m_nextStates &= ~XdgToplevelInterface::State::Resizing;
        scheduleConfigure();
    }
}

void XdgToplevelWindow::doSetSuspended()
{
    if (isSuspended()) {
        m_nextStates |= XdgToplevelInterface::State::Suspended;
    } else {
        m_nextStates &= ~XdgToplevelInterface::State::Suspended;
    }

    scheduleConfigure();
}

void XdgToplevelWindow::doSetNextTargetScale()
{
    if (isDeleted()) {
        return;
    }
    if (m_isInitialized) {
        scheduleConfigure();
    }
}

void XdgToplevelWindow::doSetPreferredBufferTransform()
{
    if (isDeleted()) {
        return;
    }
    if (m_isInitialized) {
        scheduleConfigure();
    }
}

void XdgToplevelWindow::doSetPreferredColorDescription()
{
    if (isDeleted()) {
        return;
    }
    if (m_isInitialized) {
        scheduleConfigure();
    }
}

void XdgToplevelWindow::takeFocus()
{
    sendPing(PingReason::FocusWindow);
}

bool XdgToplevelWindow::wantsInput() const
{
    return rules()->checkAcceptFocus(acceptsFocus());
}

bool XdgToplevelWindow::acceptsFocus() const
{
    if (m_plasmaShellSurface) {
        if (m_plasmaShellSurface->role() == PlasmaShellSurfaceInterface::Role::OnScreenDisplay || m_plasmaShellSurface->role() == PlasmaShellSurfaceInterface::Role::ToolTip) {
            return false;
        }
        switch (m_plasmaShellSurface->role()) {
        case PlasmaShellSurfaceInterface::Role::Notification:
        case PlasmaShellSurfaceInterface::Role::CriticalNotification:
        case PlasmaShellSurfaceInterface::Role::Panel:
            return m_plasmaShellSurface->panelTakesFocus();
        default:
            break;
        }
    }
    return !isDeleted() && readyForPainting();
}

void XdgToplevelWindow::handleWindowTitleChanged()
{
    setCaption(m_shellSurface->title());
}

void XdgToplevelWindow::handleAppIdChanged()
{
    const QString applicationId = m_shellSurface->appId();
    setResourceClass(resourceName(), applicationId);
    if (shellSurface()->isConfigured()) {
        evaluateWindowRules();
    }
    setDesktopFileName(applicationId);
}

void XdgToplevelWindow::handleWindowMenuRequested(SeatInterface *seat, const QPointF &surfacePos, quint32 serial)
{
    performMousePressCommand(Options::MouseOperationsMenu, mapFromLocal(surfacePos));
}

void XdgToplevelWindow::handleMoveRequested(SeatInterface *seat, quint32 serial)
{
    if (isMovable()) {
        if (const auto anchor = input()->implicitGrabPositionBySerial(seat, serial)) {
            performMousePressCommand(Options::MouseMove, *anchor);
        }
    } else {
        qCDebug(KWIN_CORE) << this << "is immovable, ignoring the move request";
    }
}

void XdgToplevelWindow::handleResizeRequested(SeatInterface *seat, Gravity gravity, quint32 serial)
{
    const auto anchor = input()->implicitGrabPositionBySerial(seat, serial);
    if (!anchor) {
        return;
    }
    if (!isResizable()) {
        return;
    }
    if (isInteractiveMoveResize()) {
        finishInteractiveMoveResize(false);
    }
    setInteractiveMoveResizePointerButtonDown(true);
    setInteractiveMoveResizeAnchor(*anchor);
    setInteractiveMoveResizeModifiers(Qt::KeyboardModifiers());
    setInteractiveMoveOffset(QPointF((anchor->x() - x()) / width(), (anchor->y() - y()) / height())); // map from global
    setUnrestrictedInteractiveMoveResize(false);
    setInteractiveMoveResizeGravity(gravity);
    if (!startInteractiveMoveResize()) {
        setInteractiveMoveResizePointerButtonDown(false);
    }
    updateCursor();
}

void XdgToplevelWindow::handleStatesAcknowledged(const XdgToplevelInterface::States &states)
{
    const XdgToplevelInterface::States delta = m_acknowledgedStates ^ states;

    if (delta & XdgToplevelInterface::State::Maximized) {
        MaximizeMode maximizeMode = MaximizeRestore;
        if (states & XdgToplevelInterface::State::MaximizedHorizontal) {
            maximizeMode = MaximizeMode(maximizeMode | MaximizeHorizontal);
        }
        if (states & XdgToplevelInterface::State::MaximizedVertical) {
            maximizeMode = MaximizeMode(maximizeMode | MaximizeVertical);
        }
        updateMaximizeMode(maximizeMode);
        if (isDeleted()) {
            return;
        }
    }
    if (delta & XdgToplevelInterface::State::FullScreen) {
        updateFullScreenMode(states & XdgToplevelInterface::State::FullScreen);
        if (isDeleted()) {
            return;
        }
    }

    m_acknowledgedStates = states;
}

void XdgToplevelWindow::handleMaximizeRequested()
{
    if (m_isInitialized) {
        maximize(MaximizeFull);
        scheduleConfigure();
    } else {
        m_initialStates |= XdgToplevelInterface::State::Maximized;
    }
}

void XdgToplevelWindow::handleUnmaximizeRequested()
{
    if (m_isInitialized) {
        maximize(MaximizeRestore);
        scheduleConfigure();
    } else {
        m_initialStates &= ~XdgToplevelInterface::State::Maximized;
    }
}

void XdgToplevelWindow::handleFullscreenRequested(OutputInterface *output)
{
    writeFullScreenRequestedOutput(output ? output->handle() : nullptr);
    if (m_isInitialized) {
        setFullScreen(true);
        scheduleConfigure();
    } else {
        m_initialStates |= XdgToplevelInterface::State::FullScreen;
    }
}

void XdgToplevelWindow::handleUnfullscreenRequested()
{
    writeFullScreenRequestedOutput(nullptr);
    if (m_isInitialized) {
        setFullScreen(false);
        scheduleConfigure();
    } else {
        m_initialStates &= ~XdgToplevelInterface::State::FullScreen;
    }
}

void XdgToplevelWindow::handleMinimizeRequested()
{
    setMinimized(true);
}

void XdgToplevelWindow::handleTransientForChanged()
{
    SurfaceInterface *transientForSurface = nullptr;
    if (XdgToplevelInterface *parentToplevel = m_shellSurface->parentXdgToplevel()) {
        transientForSurface = parentToplevel->surface();
    }
    if (!transientForSurface) {
        transientForSurface = waylandServer()->findForeignTransientForSurface(surface());
    }
    Window *transientForWindow = waylandServer()->findWindow(transientForSurface);
    if (transientForWindow != transientFor()) {
        if (transientFor()) {
            transientFor()->removeTransient(this);
        }
        if (transientForWindow) {
            transientForWindow->addTransient(this);
        }
        setTransientFor(transientForWindow);
    }
    m_isTransient = transientForWindow;
}

void XdgToplevelWindow::handleForeignTransientForChanged(SurfaceInterface *child)
{
    if (surface() == child) {
        handleTransientForChanged();
    }
}

void XdgToplevelWindow::handlePingTimeout(quint32 serial)
{
    auto pingIt = m_pings.find(serial);
    if (pingIt == m_pings.end()) {
        return;
    }
    if (pingIt.value() == PingReason::CloseWindow) {
        qCDebug(KWIN_CORE) << "Final ping timeout on a close attempt, asking to kill:" << caption();

        if (!m_killPrompt) {
            m_killPrompt = std::make_unique<KillPrompt>(this);
        }
        if (!m_killPrompt->isRunning()) {
            m_killPrompt->start();
        }
    }
}

void XdgToplevelWindow::handlePingDelayed(quint32 serial)
{
    auto it = m_pings.find(serial);
    if (it != m_pings.end()) {
        qCDebug(KWIN_CORE) << "First ping timeout:" << caption();
        setUnresponsive(true);
    }
}

void XdgToplevelWindow::handlePongReceived(quint32 serial)
{
    if (m_pings.remove(serial)) {
        setUnresponsive(false);
        if (m_killPrompt) {
            m_killPrompt->quit();
        }
    }
}

void XdgToplevelWindow::handleMaximumSizeChanged()
{
    m_maximumSize = m_shellSurface->maximumSize();
    updateCapabilities();
    Q_EMIT maximizeableChanged(isMaximizable());
}

void XdgToplevelWindow::handleMinimumSizeChanged()
{
    m_minimumSize = m_shellSurface->minimumSize();
    updateCapabilities();
    Q_EMIT maximizeableChanged(isMaximizable());
}

void XdgToplevelWindow::sendPing(PingReason reason)
{
    XdgShellInterface *shell = m_shellSurface->shell();
    XdgSurfaceInterface *surface = m_shellSurface->xdgSurface();

    const quint32 serial = shell->ping(surface);
    m_pings.insert(serial, reason);
}

std::optional<QPointF> XdgToplevelWindow::initialPosition(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (session->position && workspace()->outputLayoutId() == session->outputLayoutId) {
            return session->position.value();
        }
    }
    return std::nullopt;
}

QSizeF XdgToplevelWindow::initialSize(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto size = session->size) {
            return size.value();
        }
    }
    return QSizeF();
}

bool XdgToplevelWindow::initialKeepAbove(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto keepAbove = session->keepAbove) {
            return keepAbove.value();
        }
    }
    return keepAbove();
}

bool XdgToplevelWindow::initialKeepBelow(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto keepBelow = session->keepBelow) {
            return keepBelow.value();
        }
    }
    return keepBelow();
}

bool XdgToplevelWindow::initialSkipSwitcher(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto skipSwitcher = session->skipSwitcher) {
            return skipSwitcher.value();
        }
    }
    return skipSwitcher();
}

bool XdgToplevelWindow::initialSkipPager(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto skipPager = session->skipPager) {
            return skipPager.value();
        }
    }
    return skipPager();
}

bool XdgToplevelWindow::initialSkipTaskbar(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto skipTaskbar = session->skipTaskbar) {
            return skipTaskbar.value();
        }
    }
    return skipTaskbar();
}

MaximizeMode XdgToplevelWindow::initialMaximizeMode(const std::optional<XdgToplevelSessionData> &session) const
{
    // We prefer set_maximized() requests over maximized state stored in the session.
    if (m_initialStates & XdgToplevelInterface::State::Maximized) {
        MaximizeMode maximizeMode = MaximizeRestore;
        if (m_initialStates & XdgToplevelInterface::State::MaximizedHorizontal) {
            maximizeMode = MaximizeMode(maximizeMode | MaximizeHorizontal);
        }
        if (m_initialStates & XdgToplevelInterface::State::MaximizedVertical) {
            maximizeMode = MaximizeMode(maximizeMode | MaximizeVertical);
        }
        return maximizeMode;
    }
    if (session) {
        if (const auto maximizeMode = session->maximizeMode) {
            return maximizeMode.value();
        }
    }
    if (isPlaceable()) {
        const RectF area = workspace()->clientArea(PlacementArea, this, workspace()->activeOutput());
        if (const auto placement = workspace()->placement()->place(this, area)) {
            if (const auto maximizeMode = std::get_if<MaximizeMode>(&*placement)) {
                return *maximizeMode;
            }
        }
    }
    return MaximizeRestore;
}

bool XdgToplevelWindow::initialFullScreenMode(const std::optional<XdgToplevelSessionData> &session) const
{
    // We prefer set_fullscreen() requests over fullscreen state stored in the session.
    if (m_initialStates & XdgToplevelInterface::State::FullScreen) {
        return m_initialStates & XdgToplevelInterface::State::FullScreen;
    }
    if (session) {
        if (const auto fullscreen = session->fullscreen) {
            return fullscreen.value();
        }
    }
    return false;
}

bool XdgToplevelWindow::initialMinimizeMode(const std::optional<XdgToplevelSessionData> &session) const
{
    if (isMinimized()) {
        return true;
    }
    if (session) {
        if (const auto minimized = session->minimized) {
            return minimized.value();
        }
    }
    return false;
}

QVector<VirtualDesktop *> XdgToplevelWindow::initialDesktops(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto desktopIds = session->desktops) {
            QVector<VirtualDesktop *> desktops;
            desktops.reserve(desktopIds->size());
            for (const QString &desktopId : *desktopIds) {
                if (VirtualDesktop *desktop = VirtualDesktopManager::self()->desktopForId(desktopId)) {
                    desktops.append(desktop);
                }
            }
            if (desktopIds->isEmpty() == desktops.isEmpty()) {
                return desktops;
            }
        }
    }

    // XdgToplevelWindow is first assigned to the current virtual desktop on the active output by the constructor.
    // However, the application can request that the window is fullscreened on a particular output, which may have a different
    // desktop. This caused the desktop to change on that output. This happened for Spectacle.
    // On the other hand there's also installPlasmaShellSurface(), which can reassign it to all desktops before initialize().
    if (moveResizeOutput() != output() && !desktops().isEmpty()) {
        return {VirtualDesktopManager::self()->currentDesktop(moveResizeOutput())};
    }

    return desktops();
}

QStringList XdgToplevelWindow::initialActivities(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto activities = session->activities) {
            return activities.value();
        }
    }
    return activities();
}

DecorationPolicy XdgToplevelWindow::initialDecorationPolicy(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto decorationPolicy = session->decorationPolicy) {
            return decorationPolicy.value();
        }
    }
    return decorationPolicy();
}

QString XdgToplevelWindow::initialShortcut(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto shortcut = session->shortcut) {
            return shortcut.value();
        }
    }
    return shortcut().toString();
}

bool XdgToplevelWindow::initialExcludeFromCapture(const std::optional<XdgToplevelSessionData> &session) const
{
    if (session) {
        if (const auto exclude = session->excludeFromCapture) {
            return exclude.value();
        }
    }
    return excludeFromCapture();
}

void XdgToplevelWindow::initialize()
{
    setupWindowRules();

    std::optional<XdgToplevelSessionData> sessionData;
    if (m_shellSurface->session()) {
        sessionData = XdgToplevelSessionData::parse(m_shellSurface->session()->read());
    }

    // Move or resize the window only if enforced by a window rule.
    auto position = rules()->checkPositionSafe(true).or_else([&]() {
        return initialPosition(sessionData);
    });
    if (position) {
        place(*position);
    }
    const QSizeF forcedSize = rules()->checkSize(initialSize(sessionData), true);
    if (forcedSize.isValid()) {
        resize(forcedSize);
    }

    setOpacity(rules()->checkOpacityInactive(opacity()));
    maximize(rules()->checkMaximize(initialMaximizeMode(sessionData), true));
    setFullScreen(rules()->checkFullScreen(initialFullScreenMode(sessionData), true));
    setOnActivities(rules()->checkActivity(initialActivities(sessionData), true));
    setDesktops(rules()->checkDesktops(initialDesktops(sessionData), true));
    setDesktopFileName(rules()->checkDesktopFile(desktopFileName(), true).toUtf8());
    setMinimized(rules()->checkMinimize(initialMinimizeMode(sessionData), true));
    setSkipTaskbar(rules()->checkSkipTaskbar(initialSkipTaskbar(sessionData), true));
    setSkipPager(rules()->checkSkipPager(initialSkipPager(sessionData), true));
    setSkipSwitcher(rules()->checkSkipSwitcher(initialSkipSwitcher(sessionData), true));
    setKeepAbove(rules()->checkKeepAbove(initialKeepAbove(sessionData), true));
    setKeepBelow(rules()->checkKeepBelow(initialKeepBelow(sessionData), true));
    setShortcut(rules()->checkShortcut(initialShortcut(sessionData), true));
    setDecorationPolicy(rules()->checkDecorationPolicy(initialDecorationPolicy(sessionData), true));
    setExcludeFromCapture(rules()->checkExcludeFromCapture(initialExcludeFromCapture(sessionData), true));

    workspace()->rulebook()->discardUsed(this, false); // Remove Apply Now rules.
    updateWindowRules(Rules::All);

    if (sessionData) {
        m_shellSurface->session()->sendRestored();
    }

    configureDecoration();
    scheduleConfigure();
    updateColorScheme();
    updateCapabilities();
    updateIcon();
    setupWindowManagementInterface();

    m_isInitialized = true;
}

void XdgToplevelWindow::updateMaximizeMode(MaximizeMode maximizeMode)
{
    if (m_maximizeMode == maximizeMode) {
        return;
    }
    m_maximizeMode = maximizeMode;
    updateWindowRules(Rules::MaximizeVert | Rules::MaximizeHoriz);
    Q_EMIT maximizedChanged();
}

void XdgToplevelWindow::updateFullScreenMode(bool set)
{
    if (m_isFullScreen == set) {
        return;
    }
    StackingUpdatesBlocker blocker1(workspace());
    m_isFullScreen = set;
    updateLayer();
    updateWindowRules(Rules::Fullscreen);
    Q_EMIT fullScreenChanged();
}

void XdgToplevelWindow::updateCapabilities()
{
    XdgToplevelInterface::Capabilities caps = XdgToplevelInterface::Capability::WindowMenu;

    if (isMaximizable()) {
        caps.setFlag(XdgToplevelInterface::Capability::Maximize);
    }
    if (isFullScreenable()) {
        caps.setFlag(XdgToplevelInterface::Capability::FullScreen);
    }
    if (isMinimizable()) {
        caps.setFlag(XdgToplevelInterface::Capability::Minimize);
    }

    if (m_capabilities != caps) {
        m_capabilities = caps;
        m_shellSurface->sendWmCapabilities(caps);
    }
}

void XdgToplevelWindow::updateIcon()
{
    if (!m_shellSurface->customIcon().isNull()) {
        setIcon(m_shellSurface->customIcon());
        return;
    }
    const QString waylandIconName = QStringLiteral("wayland");
    const QString dfIconName = iconFromDesktopFile();
    const QString iconName = dfIconName.isEmpty() ? waylandIconName : dfIconName;
    if (iconName == icon().name()) {
        return;
    }
    setIcon(QIcon::fromTheme(iconName));
}

QString XdgToplevelWindow::preferredColorScheme() const
{
    if (m_paletteInterface) {
        return rules()->checkDecoColor(m_paletteInterface->palette());
    }
    return rules()->checkDecoColor(QString());
}

void XdgToplevelWindow::installAppMenu(AppMenuInterface *appMenu)
{
    m_appMenuInterface = appMenu;

    auto updateMenu = [this](const AppMenuInterface::InterfaceAddress &address) {
        updateApplicationMenuServiceName(address.serviceName);
        updateApplicationMenuObjectPath(address.objectPath);
    };
    connect(m_appMenuInterface, &AppMenuInterface::addressChanged, this, updateMenu);
    updateMenu(appMenu->address());
}

DecorationMode XdgToplevelWindow::preferredDecorationMode() const
{
    return preferredDecorationMode(isRequestedFullScreen());
}

DecorationMode XdgToplevelWindow::preferredDecorationMode(bool requestedFullScreen) const
{
    if (!Decoration::DecorationBridge::hasPlugin()) {
        return DecorationMode::Client;
    } else if (requestedFullScreen) {
        return DecorationMode::None;
    }

    switch (m_decorationPolicy) {
    case DecorationPolicy::None:
        return DecorationMode::None;
    case DecorationPolicy::Server:
        return DecorationMode::Server;
    case DecorationPolicy::PreferredByClient:
        if (m_xdgDecoration) {
            switch (m_xdgDecoration->preferredMode()) {
            case XdgToplevelDecorationV1Interface::Mode::Undefined:
                return DecorationMode::Server;
            case XdgToplevelDecorationV1Interface::Mode::None:
                return DecorationMode::None;
            case XdgToplevelDecorationV1Interface::Mode::Client:
                return DecorationMode::Client;
            case XdgToplevelDecorationV1Interface::Mode::Server:
                return DecorationMode::Server;
            }
        }

        if (m_serverDecoration) {
            switch (m_serverDecoration->preferredMode()) {
            case ServerSideDecorationManagerInterface::Mode::None:
                return DecorationMode::None;
            case ServerSideDecorationManagerInterface::Mode::Client:
                return DecorationMode::Client;
            case ServerSideDecorationManagerInterface::Mode::Server:
                return DecorationMode::Server;
            }
        }

        break;
    }

    return DecorationMode::Client;
}

void XdgToplevelWindow::clearDecoration()
{
    if (m_nextDecoration) {
        disconnect(m_nextDecoration.get(), &KDecoration3::Decoration::nextStateChanged, this, &XdgToplevelWindow::processDecorationState);
    }

    m_nextDecoration = nullptr;
    m_nextDecorationState = nullptr;
}

void XdgToplevelWindow::configureDecoration()
{
    const DecorationMode decorationMode = preferredDecorationMode();
    switch (decorationMode) {
    case DecorationMode::None:
    case DecorationMode::Client:
        clearDecoration();
        break;
    case DecorationMode::Server:
        if (!m_nextDecoration) {
            m_nextDecoration.reset(Workspace::self()->decorationBridge()->createDecoration(this));
            if (m_nextDecoration) {
                connect(m_nextDecoration.get(), &KDecoration3::Decoration::nextStateChanged, this, &XdgToplevelWindow::processDecorationState);
                m_nextDecorationState = m_nextDecoration->nextState()->clone();
            }
        }
        break;
    }

    // All decoration updates are synchronized to toplevel configure events.
    if (m_xdgDecoration) {
        configureXdgDecoration(decorationMode);
    } else if (m_serverDecoration) {
        configureServerDecoration(decorationMode);
    }
    scheduleConfigure();
}

void XdgToplevelWindow::processDecorationState(std::shared_ptr<KDecoration3::DecorationState> state)
{
    if (isDeleted()) {
        return;
    }

    m_nextDecorationState = state->clone();
    if (m_shellSurface->isConfigured()) {
        scheduleConfigure();
    }
}

void XdgToplevelWindow::configureXdgDecoration(DecorationMode decorationMode)
{
    switch (decorationMode) {
    case DecorationMode::None: // Faked as server side mode under the hood.
        m_xdgDecoration->sendConfigure(XdgToplevelDecorationV1Interface::Mode::None);
        break;
    case DecorationMode::Client:
        m_xdgDecoration->sendConfigure(XdgToplevelDecorationV1Interface::Mode::Client);
        break;
    case DecorationMode::Server:
        m_xdgDecoration->sendConfigure(XdgToplevelDecorationV1Interface::Mode::Server);
        break;
    }
}

void XdgToplevelWindow::configureServerDecoration(DecorationMode decorationMode)
{
    switch (decorationMode) {
    case DecorationMode::None:
        m_serverDecoration->setMode(ServerSideDecorationManagerInterface::Mode::None);
        break;
    case DecorationMode::Client:
        m_serverDecoration->setMode(ServerSideDecorationManagerInterface::Mode::Client);
        break;
    case DecorationMode::Server:
        m_serverDecoration->setMode(ServerSideDecorationManagerInterface::Mode::Server);
        break;
    }
}

void XdgToplevelWindow::installXdgDecoration(XdgToplevelDecorationV1Interface *decoration)
{
    m_xdgDecoration = decoration;

    connect(m_xdgDecoration, &XdgToplevelDecorationV1Interface::destroyed,
            this, &XdgToplevelWindow::clearDecoration);
    connect(m_xdgDecoration, &XdgToplevelDecorationV1Interface::preferredModeChanged, this, [this] {
        if (m_isInitialized) {
            configureDecoration();
        }
    });
}

void XdgToplevelWindow::installServerDecoration(ServerSideDecorationInterface *decoration)
{
    m_serverDecoration = decoration;
    if (m_isInitialized) {
        configureDecoration();
    }

    connect(m_serverDecoration, &ServerSideDecorationInterface::destroyed,
            this, &XdgToplevelWindow::clearDecoration);
    connect(m_serverDecoration, &ServerSideDecorationInterface::preferredModeChanged, this, [this]() {
        if (m_isInitialized) {
            configureDecoration();
        }
    });
}

void XdgToplevelWindow::installPalette(ServerSideDecorationPaletteInterface *palette)
{
    m_paletteInterface = palette;

    connect(m_paletteInterface, &ServerSideDecorationPaletteInterface::paletteChanged,
            this, &XdgToplevelWindow::updateColorScheme);
    connect(m_paletteInterface, &QObject::destroyed,
            this, &XdgToplevelWindow::updateColorScheme);
    updateColorScheme();
}

void XdgToplevelWindow::installXdgDialogV1(XdgDialogV1Interface *dialog)
{
    m_xdgDialog = dialog;

    connect(dialog, &XdgDialogV1Interface::modalChanged, this, &Window::setModal);
    connect(dialog, &QObject::destroyed, this, [this] {
        setModal(false);
    });
    setModal(dialog->isModal());
}

static bool isFiniteExtent(const RectF &rect)
{
    for (qreal value : {rect.x(), rect.y(), rect.width(), rect.height()}) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return rect.width() > 0 && rect.height() > 0;
}

static void advanceRevision(quint64 &revision)
{
    if (revision != std::numeric_limits<quint64>::max()) {
        ++revision;
    }
}

void XdgToplevelWindow::writeRequestedFullScreen(bool set)
{
    m_isRequestedFullScreen = set;
    advanceRevision(m_requestedFullScreenRevision);
    advanceRevision(m_fullScreenIntentRevision);
}

void XdgToplevelWindow::writeFullScreenRequestedOutput(LogicalOutput *output)
{
    m_fullScreenRequestedOutput = output;
    advanceRevision(m_fullScreenIntentRevision);
}

RectF XdgToplevelWindow::ownedFullScreenExitGeometry(const RectF &managed, MaximizeMode maximizeMode) const
{
    RectF geometry = managed;
    if (maximizeMode != MaximizeRestore) {
        const RectF area = workspace()->clientArea(MaximizeArea, this, moveResizeOutput());
        if (maximizeMode & MaximizeHorizontal) {
            geometry.setLeft(area.left());
            geometry.setRight(area.right());
        }
        if (maximizeMode & MaximizeVertical) {
            geometry.setTop(area.top());
            geometry.setBottom(area.bottom());
        }
    }
    return geometry;
}

bool XdgToplevelWindow::ownedFullScreenExitFeasible(const RectF &managed, MaximizeMode maximizeMode) const
{
    // Every axis that is not maximized returns to the managed extent exactly, under the decoration
    // the exit restores: the same check an owned unmaximize makes for its released axes.
    return ownedMaximizeFeasible(managed, maximizeMode, MaximizeFull);
}

std::optional<XdgToplevelWindow::OwnedFullScreen> XdgToplevelWindow::ownedFullScreenPlan(bool set) const
{
    // Only an ordinary toggle qualifies: no interactive operation, a live current ownership record,
    // and, when entering, no client-requested output other than the one the ownership lives on (a
    // different output leaves that context, so the legacy path moves and releases as before). An
    // exit must be able to return to the managed extents; the decoration it restores does not
    // exist while fullscreen, so this is exact when no server decoration will return and only a
    // necessary condition otherwise, settled exactly once the decoration is configured.
    const auto exhausted = std::numeric_limits<quint64>::max();
    if (isInteractiveMoveResize() || !m_transientOwnership || m_requestedFullScreenRevision >= exhausted - 1
        || m_fullScreenIntentRevision >= exhausted - 2) {
        return std::nullopt;
    }
    const auto generation = m_transientOwnership->generation;
    if (!transientOwnershipIsCurrent(generation)) {
        return std::nullopt;
    }
    const RectF managed = m_transientOwnership->tile->windowGeometry();
    if (!isFiniteExtent(managed)) {
        return std::nullopt;
    }
    const MaximizeMode maximizeMode = m_requestedMaximizeMode;
    if (set) {
        if (m_fullScreenRequestedOutput && m_fullScreenRequestedOutput.data() != moveResizeOutput()) {
            return std::nullopt;
        }
    } else {
        if (!ownedFullScreenExitFeasible(managed, maximizeMode)) {
            return std::nullopt;
        }
        if (preferredDecorationMode(false) == DecorationMode::Server && !m_nextDecoration) {
            // A returning server decoration raises the enforced client minimum to 150 on each
            // returning axis; the frame cannot be smaller than that client.
            const qreal enforcedMinimum = 150;
            if ((!(maximizeMode & MaximizeHorizontal) && managed.width() < enforcedMinimum)
                || (!(maximizeMode & MaximizeVertical) && managed.height() < enforcedMinimum)) {
                return std::nullopt;
            }
        }
    }
    return OwnedFullScreen{generation, managed, ownedFullScreenExitGeometry(managed, maximizeMode), maximizeMode, m_requestedMaximizeRevision,
                           m_fullScreenRequestedOutput, !m_fullScreenRequestedOutput.isNull(), m_minimumSize, m_maximumSize};
}

XdgToplevelWindow::OwnedFullScreenOutcome XdgToplevelWindow::setFullScreenOwned(const OwnedFullScreen &plan, bool set)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const auto before = makeTileAssignmentFence();
    writeRequestedFullScreen(set);
    const auto revision = m_requestedFullScreenRevision;
    bool legacy = false;
    // Publishing the requested state to the client is this write's own obligation: it is discharged
    // as long as the write is still current, whatever happened to the geometry, and reports no
    // success. Only an exit the legacy path takes over below, before anything owned has happened,
    // publishes through that path instead.
    const auto publish = qScopeGuard([this, revision, &legacy]() {
        if (!legacy && !isDeleted() && m_requestedFullScreenRevision == revision) {
            doSetFullScreen();
        }
    });
    QPointer<LogicalOutput> requestedOutput = plan.requestedOutput;
    bool hadRequestedOutput = plan.hadRequestedOutput;
    quint64 intentRevision = m_fullScreenIntentRevision;
    const auto intentCurrent = [&]() {
        return m_fullScreenIntentRevision == intentRevision && m_requestedMaximizeRevision == plan.maximizeRevision
            && (hadRequestedOutput ? (!requestedOutput.isNull() && m_fullScreenRequestedOutput.data() == requestedOutput.data())
                                   : m_fullScreenRequestedOutput.isNull());
    };
    const auto current = [&]() {
        return !isDeleted() && before.isCurrentFor(*this) && transientOwnershipIsCurrent(plan.generation) && intentCurrent();
    };
    // The decoration is configured through plugin code: a callback boundary of its own. A handler
    // that revoked ownership, changed fenced state, wrote the fullscreen or maximize intent (even
    // back to the same value) or changed the requested output holds the newer intent: nothing but
    // the state write happened, and only that is published.
    configureDecoration();
    if (!current()) {
        return OwnedFullScreenOutcome::Stopped;
    }
    if (!set) {
        // The restored decoration's borders and enforced minimum are known only now, so the exit's
        // dispatch is validated here, after a prefix (the state write and the decoration
        // configuration) the legacy path shares. If the managed extents cannot be returned to under
        // that decoration while the client's own size hints are those of the plan, that is the
        // exit's predictable effect: the legacy exit takes the operation over and releases as
        // before. Changed hints are a handler's newer intent and end the operation here. The hint
        // comparison is deliberately narrow: rule limits and the decoration's state are separate
        // inputs the exact check already reflects.
        if (!ownedFullScreenExitFeasible(plan.managed, plan.maximizeMode)) {
            legacy = m_minimumSize == plan.minimumHint && m_maximumSize == plan.maximumHint;
            return legacy ? OwnedFullScreenOutcome::Legacy : OwnedFullScreenOutcome::Stopped;
        }
        writeFullScreenRequestedOutput(nullptr);
        requestedOutput.clear();
        hadRequestedOutput = false;
        intentRevision = m_fullScreenIntentRevision;
    }
    // Neither the fence nor the ownership record sees the fullscreen, output or maximize intent or
    // the client's size limits, so the owned writes carry them as their own constraint for every
    // child boundary; captured by value, the constraint outlives nothing of this frame.
    const auto constraint = [this, managed = plan.managed, maximizeMode = plan.maximizeMode, maximizeRevision = plan.maximizeRevision,
                             intentRevision, hadRequestedOutput, requestedOutput, set]() {
        return m_fullScreenIntentRevision == intentRevision && m_requestedMaximizeRevision == maximizeRevision
            && (hadRequestedOutput ? (!requestedOutput.isNull() && m_fullScreenRequestedOutput.data() == requestedOutput.data())
                                   : m_fullScreenRequestedOutput.isNull())
            && (set || ownedFullScreenExitFeasible(managed, maximizeMode));
    };
    const auto configured = makeTileAssignmentFence();
    if (set) {
        // Legacy consumers (the placement tracker, a legacy exit after a release) read the restore
        // field: it holds the rectangle an owned exit returns to. The write is a callback boundary;
        // a handler's replacement stands and ends the operation.
        const auto restore = setFullscreenGeometryRestoreForAssignment(plan.target, configured, OwnedProvenance::forOwnership(plan.generation, constraint));
        if (restore.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(restore)) {
            return OwnedFullScreenOutcome::Stopped;
        }
        // The ownership pins the move-resize output to the recorded one, and a requested output
        // was admitted only when it is that same output.
        const RectF area = workspace()->clientArea(FullScreenArea, this, moveResizeOutput());
        const auto request = requestMoveResizeForOwnership(area, MoveResizeMode::MoveResize, *restore.checkpoint, plan.generation, constraint);
        if (request.result.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(request.result)) {
            return OwnedFullScreenOutcome::Stopped;
        }
    } else {
        // The restore field is left as it is, as on the legacy exit; the geometry returns to the
        // rectangle frozen before the first callback, never to that field.
        const auto request = requestMoveResizeForOwnership(plan.target, MoveResizeMode::MoveResize, configured, plan.generation, constraint);
        if (request.result.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(request.result)) {
            return OwnedFullScreenOutcome::Stopped;
        }
    }
    markAsPlaced();
    return OwnedFullScreenOutcome::Applied;
}

void XdgToplevelWindow::setFullScreen(bool set)
{
    if (!isFullScreenable()) {
        return;
    }

    set = rules()->checkFullScreen(set);
    if (m_isRequestedFullScreen == set) {
        return;
    }

    applyFullScreen(set);
}

XdgToplevelWindow::OwnedFullScreenOutcome XdgToplevelWindow::applyFullScreen(bool set)
{
    // An owner keeps its placement authority through an ordinary toggle. Eligibility is decided
    // before the first callback; only an exit's exact feasibility may still be open then, because
    // the decoration it restores does not exist while fullscreen. That dispatch is validated right
    // after the decoration is configured, a prefix both paths share; a superseded owned operation
    // never reaches the legacy tail.
    if (const auto plan = ownedFullScreenPlan(set)) {
        const auto outcome = setFullScreenOwned(*plan, set);
        if (outcome != OwnedFullScreenOutcome::Legacy) {
            return outcome;
        }
    } else {
        writeRequestedFullScreen(set);
        configureDecoration();
    }
    finishFullScreenLegacy(set);
    return OwnedFullScreenOutcome::Legacy;
}

bool XdgToplevelWindow::releaseOwnedPlacement()
{
    const quint64 owned = ownedTransientPlacementGeneration();
    if (!owned) {
        return false;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    if (m_isRequestedFullScreen) {
        // A requested fullscreen state, acknowledged or still pending, is left as the owner first;
        // the release follows only if that exit completed as the current intent, so a handler's
        // newer state write, even one ending not fullscreen, is not cancelled. A legacy exit
        // releases on its own; a rule keeping the window fullscreen leaves nothing to restore.
        if (!isFullScreenable() || rules()->checkFullScreen(false)) {
            return false;
        }
        switch (applyFullScreen(false)) {
        case OwnedFullScreenOutcome::Legacy:
            return !isDeleted() && !ownedTransientPlacementGeneration();
        case OwnedFullScreenOutcome::Stopped:
            return false;
        case OwnedFullScreenOutcome::Applied:
            break;
        }
        if (isDeleted() || ownedTransientPlacementGeneration() != owned) {
            return false;
        }
    }
    setQuickTileModeAtCurrentPosition(QuickTileFlag::None);
    return !isDeleted() && !ownedTransientPlacementGeneration();
}

void XdgToplevelWindow::finishFullScreenLegacy(bool set)
{
    if (set) {
        const LogicalOutput *output = m_fullScreenRequestedOutput ? m_fullScreenRequestedOutput.data() : moveResizeOutput();
        setFullscreenGeometryRestore(moveResizeGeometry());
        moveResize(workspace()->clientArea(FullScreenArea, this, output));
    } else {
        writeFullScreenRequestedOutput(nullptr);
        if (fullscreenGeometryRestore().isValid()) {
            moveResize(RectF(fullscreenGeometryRestore().topLeft(),
                             constrainFrameSize(fullscreenGeometryRestore().size())));
        } else {
            // this can happen when the window was first shown already fullscreen,
            // so let the client set the size by itself
            moveResize(RectF(workspace()->clientArea(PlacementArea, this).topLeft(), QSize(0, 0)));
        }
    }

    markAsPlaced();

    doSetFullScreen();
}

static bool changeMaximizeRecursion = false;
void XdgToplevelWindow::maximize(MaximizeMode mode, const RectF &restore)
{
    if (changeMaximizeRecursion) {
        return;
    }

    if (!isResizable() || isAppletPopup()) {
        return;
    }

    const RectF clientArea = isElectricBorderMaximizing() ? workspace()->clientArea(MaximizeArea, this, interactiveMoveResizeAnchor()) : workspace()->clientArea(MaximizeArea, this, moveResizeOutput());

    const MaximizeMode oldMode = m_requestedMaximizeMode;
    const RectF oldGeometry = moveResizeGeometry();

    mode = rules()->checkMaximize(mode);
    if (m_requestedMaximizeMode == mode) {
        return;
    }

    // An owner keeps its placement authority through an ordinary toggle. The choice is made before
    // the first callback; afterwards only that operation's own checks decide, never this path.
    if (const auto plan = ownedMaximizePlan(mode, oldMode, restore)) {
        maximizeOwned(*plan, mode, oldMode, oldGeometry, clientArea);
        return;
    }

    Q_EMIT maximizedAboutToChange(mode);
    writeRequestedMaximizeMode(mode);

    // call into decoration update borders
    if (m_nextDecoration && !(options->borderlessMaximizedWindows() && m_requestedMaximizeMode == MaximizeFull)) {
        changeMaximizeRecursion = true;
        const auto c = m_nextDecoration->window();
        if ((m_requestedMaximizeMode & MaximizeVertical) != (oldMode & MaximizeVertical)) {
            Q_EMIT c->maximizedVerticallyChanged(m_requestedMaximizeMode & MaximizeVertical);
        }
        if ((m_requestedMaximizeMode & MaximizeHorizontal) != (oldMode & MaximizeHorizontal)) {
            Q_EMIT c->maximizedHorizontallyChanged(m_requestedMaximizeMode & MaximizeHorizontal);
        }
        if ((m_requestedMaximizeMode == MaximizeFull) != (oldMode == MaximizeFull)) {
            Q_EMIT c->maximizedChanged(m_requestedMaximizeMode == MaximizeFull);
        }
        changeMaximizeRecursion = false;
    }

    if (options->borderlessMaximizedWindows()) {
        setNoBorder(m_requestedMaximizeMode == MaximizeFull);
    }

    if (!restore.isNull()) {
        setGeometryRestore(restore);
    } else {
        if (requestedQuickTileMode() == QuickTileMode(QuickTileFlag::None)) {
            RectF savedGeometry = geometryRestore();
            if (!(oldMode & MaximizeVertical)) {
                savedGeometry.setTop(oldGeometry.top());
                savedGeometry.setBottom(oldGeometry.bottom());
            }
            if (!(oldMode & MaximizeHorizontal)) {
                savedGeometry.setLeft(oldGeometry.left());
                savedGeometry.setRight(oldGeometry.right());
            }
            setGeometryRestore(savedGeometry);
        }
    }

    if (m_requestedMaximizeMode != MaximizeRestore) {
        exitQuickTileMode();
    }

    RectF geometry = oldGeometry;

    if (m_requestedMaximizeMode & MaximizeHorizontal) {
        // Stretch the window vertically to fit the size of the maximize area.
        geometry.setLeft(clientArea.left());
        geometry.setRight(clientArea.right());
    } else if (oldMode & MaximizeHorizontal) {
        if (geometryRestore().isValid()) {
            // The window is no longer maximized horizontally and the saved geometry is valid.
            geometry.setWidth(std::max(geometryRestore().width(), minSize().width()));
            geometry.moveHorizontalCenter(geometryRestore().horizontalCenter());
        } else {
            // The window is no longer maximized horizontally and the saved geometry is
            // invalid. This would happen if the window had been mapped in the maximized state.
            // We ask the client to resize the window horizontally to its preferred size.
            geometry.setX(clientArea.x());
            geometry.setWidth(0);
        }
    }

    if (m_requestedMaximizeMode & MaximizeVertical) {
        // Stretch the window horizontally to fit the size of the maximize area.
        geometry.setTop(clientArea.top());
        geometry.setBottom(clientArea.bottom());
    } else if (oldMode & MaximizeVertical) {
        if (geometryRestore().isValid()) {
            // The window is no longer maximized vertically and the saved geometry is valid.
            geometry.setHeight(std::max(geometryRestore().height(), minSize().height()));
            geometry.moveVerticalCenter(geometryRestore().verticalCenter());
        } else {
            // The window is no longer maximized vertically and the saved geometry is
            // invalid. This would happen if the window had been mapped in the maximized state.
            // We ask the client to resize the window vertically to its preferred size.
            geometry.setY(clientArea.y());
            geometry.setHeight(0);
        }
    }

    moveResize(geometry);
    markAsPlaced();

    doSetMaximized();
}

void XdgToplevelWindow::writeRequestedMaximizeMode(MaximizeMode mode)
{
    m_requestedMaximizeMode = mode;
    if (m_requestedMaximizeRevision != std::numeric_limits<quint64>::max()) {
        ++m_requestedMaximizeRevision;
    }
}

bool XdgToplevelWindow::ownedMaximizeFeasible(const RectF &managed, MaximizeMode mode, MaximizeMode oldMode) const
{
    if (!isFiniteExtent(managed)) {
        return false;
    }
    // A released axis returns to the managed extent exactly, so the client's size limits must
    // allow that extent; an owner is never silently enlarged or recentred.
    const MaximizeMode released = MaximizeMode(oldMode & ~mode);
    if (released == MaximizeRestore) {
        return true;
    }
    const QSizeF constrained = constrainFrameSize(managed.size());
    return (!(released & MaximizeHorizontal) || constrained.width() == managed.width())
        && (!(released & MaximizeVertical) || constrained.height() == managed.height());
}

std::optional<XdgToplevelWindow::OwnedMaximize> XdgToplevelWindow::ownedMaximizePlan(MaximizeMode mode, MaximizeMode oldMode, const RectF &restore) const
{
    // Only an ordinary toggle qualifies: no caller-supplied restore rectangle, no interactive or
    // electric-border operation, and a live current ownership record whose managed rectangle the
    // released axes can actually return to. Anything else, including a record whose tile or context
    // is no longer usable, takes the legacy path and releases as before; the recorded adoption
    // rectangle is never a substitute for current placement authority.
    if (!restore.isNull() || isInteractiveMoveResize() || isElectricBorderMaximizing() || !m_transientOwnership
        || m_requestedMaximizeRevision >= std::numeric_limits<quint64>::max() - 1) {
        return std::nullopt;
    }
    const auto generation = m_transientOwnership->generation;
    if (!transientOwnershipIsCurrent(generation)) {
        return std::nullopt;
    }
    const RectF managed = m_transientOwnership->tile->windowGeometry();
    if (!ownedMaximizeFeasible(managed, mode, oldMode)) {
        return std::nullopt;
    }
    return OwnedMaximize{generation, managed};
}

void XdgToplevelWindow::maximizeOwned(const OwnedMaximize &plan, MaximizeMode mode, MaximizeMode oldMode, const RectF &oldGeometry, const RectF &clientArea)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const auto before = makeTileAssignmentFence();
    const auto revisionBefore = m_requestedMaximizeRevision;
    Q_EMIT maximizedAboutToChange(mode);
    // A handler that revoked ownership, changed fenced state, wrote the requested mode (even back
    // to the same value) or changed the client's size limits holds the newer intent. Nothing was
    // written, so nothing is published.
    if (isDeleted() || !before.isCurrentFor(*this) || m_requestedMaximizeRevision != revisionBefore
        || !transientOwnershipIsCurrent(plan.generation) || !ownedMaximizeFeasible(plan.managed, mode, oldMode)) {
        return;
    }
    writeRequestedMaximizeMode(mode);
    const auto revision = m_requestedMaximizeRevision;
    // Publishing the requested mode to the client is this write's own obligation: it is discharged
    // as long as the write is still current, whatever happened to the geometry, and reports no success.
    const auto publish = qScopeGuard([this, revision]() {
        if (!isDeleted() && m_requestedMaximizeRevision == revision) {
            doSetMaximized();
        }
    });
    const auto written = makeTileAssignmentFence();
    const auto current = [&]() {
        return !isDeleted() && m_requestedMaximizeRevision == revision && written.isCurrentFor(*this)
            && transientOwnershipIsCurrent(plan.generation) && ownedMaximizeFeasible(plan.managed, mode, oldMode);
    };
    if (m_nextDecoration && !(options->borderlessMaximizedWindows() && mode == MaximizeFull)) {
        // Every signal is a callback boundary of its own, and a handler may replace the decoration.
        const QPointer<KDecoration3::DecoratedWindow> decorated(m_nextDecoration->window());
        changeMaximizeRecursion = true;
        const auto recursion = qScopeGuard([this]() {
            changeMaximizeRecursion = false;
        });
        const auto decorationCurrent = [&]() {
            return current() && decorated && m_nextDecoration && m_nextDecoration->window() == decorated.data();
        };
        if ((mode & MaximizeVertical) != (oldMode & MaximizeVertical)) {
            Q_EMIT decorated->maximizedVerticallyChanged(mode & MaximizeVertical);
            if (!decorationCurrent()) {
                return;
            }
        }
        if ((mode & MaximizeHorizontal) != (oldMode & MaximizeHorizontal)) {
            Q_EMIT decorated->maximizedHorizontallyChanged(mode & MaximizeHorizontal);
            if (!decorationCurrent()) {
                return;
            }
        }
        if ((mode == MaximizeFull) != (oldMode == MaximizeFull)) {
            Q_EMIT decorated->maximizedChanged(mode == MaximizeFull);
            if (!decorationCurrent()) {
                return;
            }
        }
    }
    if (options->borderlessMaximizedWindows()) {
        setNoBorder(mode == MaximizeFull);
        if (!current()) {
            return;
        }
    }
    // The restore field keeps floating A and the association stays. Maximized axes span the area;
    // released axes return to the managed rectangle frozen before the first callback.
    RectF geometry = oldGeometry;
    if (mode & MaximizeHorizontal) {
        geometry.setLeft(clientArea.left());
        geometry.setRight(clientArea.right());
    } else if (oldMode & MaximizeHorizontal) {
        geometry.setLeft(plan.managed.left());
        geometry.setRight(plan.managed.right());
    }
    if (mode & MaximizeVertical) {
        geometry.setTop(clientArea.top());
        geometry.setBottom(clientArea.bottom());
    } else if (oldMode & MaximizeVertical) {
        geometry.setTop(plan.managed.top());
        geometry.setBottom(plan.managed.bottom());
    }
    // The client's size limits are visible to neither the fence nor the ownership record, so the
    // request carries this operation's feasibility as its own constraint for every child boundary.
    const auto request = requestMoveResizeForOwnership(geometry, MoveResizeMode::MoveResize, written, plan.generation, [this, &plan, mode, oldMode]() {
        return ownedMaximizeFeasible(plan.managed, mode, oldMode);
    });
    // The request allocated a newer generation, so its own result, not the earlier fence, says
    // whether this operation is still the current one.
    if (request.result.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(request.result)
        || m_requestedMaximizeRevision != revision) {
        return;
    }
    markAsPlaced();
}

Window::AssignmentStepResult XdgToplevelWindow::normalizeMaximizeForAssignment(const TileAssignmentFence &before, OwnedProvenance owned)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    AssignmentStepResult result;
    if (!before.isCurrentFor(*this) || !ownedProvenanceIsCurrent(owned)) {
        result.outcome = AssignmentStepOutcome::Superseded;
        return result;
    }
    // Mutation-free refusals. This is an ordinary normalization: an owner or an active request
    // keeps its own path (an ingress revokes at its public cancellation boundary first), a rule or
    // a running operation keeps the mode, the decoration's recursion guard is honoured, and the
    // whole operation must have headroom before anything is written: the restore capture takes
    // one placement continuation increment and the guarded request reserves its synchronization
    // writes (at most seven) plus one committed change, so a shortage known at entry refuses here
    // rather than superseding after the mode write.
    const auto exhausted = std::numeric_limits<quint64>::max();
    constexpr quint64 placementBudget = 1 + 7 + 1;
    if (changeMaximizeRecursion || m_transientOwnership || m_activeTransientRequest || !isResizable() || isAppletPopup()
        || isInteractiveMoveResize() || isElectricBorderMaximizing() || rules()->checkMaximize(MaximizeRestore) != MaximizeRestore
        || !supportsAssignmentGeometryRequest() || m_requestedMaximizeRevision >= exhausted - 1
        || m_placementContinuationRevision >= exhausted - placementBudget || m_geometryRequestGeneration >= exhausted - 1
        || m_geometryUpdateGeneration >= exhausted - 1) {
        return result;
    }
    // The size limits frozen before the first callback: the effective ones, which every boundary
    // up to the operation's own decoration transition must find unchanged, and the client's
    // rule-checked ones, which that transition itself does not touch. The receipt pins the
    // effective limits the geometry is computed under; its constraint is chained onto the incoming
    // provenance and captured by value.
    const QSizeF entryMinimum = minSize();
    const QSizeF entryMaximum = maxSize();
    const QSizeF clientMinimum = ruleCheckedMinimumSize();
    const QSizeF clientMaximum = ruleCheckedMaximumSize();
    const auto entryLimitsHold = [&]() {
        return minSize() == entryMinimum && maxSize() == entryMaximum;
    };
    const auto receiptFor = [this, owned](quint64 revision, const QSizeF &minimum, const QSizeF &maximum) {
        return owned.withConstraint([this, revision, minimum, maximum]() {
            return !isDeleted() && m_requestedMaximizeRevision == revision && minSize() == minimum && maxSize() == maximum;
        });
    };
    const MaximizeMode oldMode = m_requestedMaximizeMode;
    if (oldMode == MaximizeRestore) {
        // An operation-owned no-op: nothing recorded, a fresh checkpoint, the receipt still detects
        // a later requested-mode change.
        result.owned = receiptFor(m_requestedMaximizeRevision, entryMinimum, entryMaximum);
        result.checkpoint = makeTileAssignmentCheckpoint();
        result.outcome = assignmentResultIsCurrent(result) ? AssignmentStepOutcome::AppliedCurrent : AssignmentStepOutcome::Superseded;
        if (result.outcome != AssignmentStepOutcome::AppliedCurrent) {
            result.checkpoint.reset();
        }
        return result;
    }
    // The validated entry state as an owned snapshot: the caller's fence is borrowed and a handler
    // may destroy it, so it is never read again after the first callback. The other inputs are
    // frozen as the legacy path reads them.
    const TileAssignmentFence entry = makeTileAssignmentFence();
    const RectF oldGeometry = moveResizeGeometry();
    const RectF clientArea = workspace()->clientArea(MaximizeArea, this, moveResizeOutput());
    const auto revisionBefore = m_requestedMaximizeRevision;
    Q_EMIT maximizedAboutToChange(MaximizeRestore);
    if (isDeleted() || !entry.isCurrentFor(*this) || !ownedProvenanceIsCurrent(owned) || m_requestedMaximizeRevision != revisionBefore || !entryLimitsHold()) {
        result.outcome = AssignmentStepOutcome::Superseded;
        return result;
    }
    writeRequestedMaximizeMode(MaximizeRestore);
    const auto revision = m_requestedMaximizeRevision;
    // From here every stop is a supersession: the mode is written and its publication is this
    // write's own obligation, discharged while the write is current.
    result.outcome = AssignmentStepOutcome::Superseded;
    const auto publish = qScopeGuard([this, revision]() {
        if (!isDeleted() && m_requestedMaximizeRevision == revision) {
            doSetMaximized();
        }
    });
    const auto written = makeTileAssignmentFence();
    const auto current = [&]() {
        return !isDeleted() && written.isCurrentFor(*this) && ownedProvenanceIsCurrent(owned) && m_requestedMaximizeRevision == revision && entryLimitsHold();
    };
    if (m_nextDecoration) {
        // Every signal is a callback boundary of its own, and a handler may replace the decoration.
        const QPointer<KDecoration3::DecoratedWindow> decorated(m_nextDecoration->window());
        changeMaximizeRecursion = true;
        const auto recursion = qScopeGuard([this]() {
            changeMaximizeRecursion = false;
        });
        const auto decorationCurrent = [&]() {
            return current() && decorated && m_nextDecoration && m_nextDecoration->window() == decorated.data();
        };
        if (oldMode & MaximizeVertical) {
            Q_EMIT decorated->maximizedVerticallyChanged(false);
            if (!decorationCurrent()) {
                return result;
            }
        }
        if (oldMode & MaximizeHorizontal) {
            Q_EMIT decorated->maximizedHorizontallyChanged(false);
            if (!decorationCurrent()) {
                return result;
            }
        }
        if (oldMode == MaximizeFull) {
            Q_EMIT decorated->maximizedChanged(false);
            if (!decorationCurrent()) {
                return result;
            }
        }
    }
    QSizeF minimum = entryMinimum;
    QSizeF maximum = entryMaximum;
    if (options->borderlessMaximizedWindows()) {
        // The operation's own decoration transition changes the enforced minimum, so the receipt
        // is established against the limits after it. What must not change across it: the policy
        // this operation set, the client's rule-checked limits, and the fenced state.
        setNoBorder(false);
        if (isDeleted() || !written.isCurrentFor(*this) || !ownedProvenanceIsCurrent(owned) || m_requestedMaximizeRevision != revision
            || decorationPolicy() != rules()->checkDecorationPolicy(DecorationPolicy::PreferredByClient)
            || ruleCheckedMinimumSize() != clientMinimum || ruleCheckedMaximumSize() != clientMaximum) {
            return result;
        }
        minimum = minSize();
        maximum = maxSize();
    }
    const OwnedProvenance receipt = receiptFor(revision, minimum, maximum);
    // The legacy restore capture, exactly: only the axes that were not maximized are refreshed
    // from the old geometry, and nothing is captured while a quick tile is already requested.
    std::unique_ptr<TileAssignmentFence> afterRestore;
    if (requestedQuickTileMode() == QuickTileMode(QuickTileFlag::None)) {
        RectF savedGeometry = geometryRestore();
        if (!(oldMode & MaximizeVertical)) {
            savedGeometry.setTop(oldGeometry.top());
            savedGeometry.setBottom(oldGeometry.bottom());
        }
        if (!(oldMode & MaximizeHorizontal)) {
            savedGeometry.setLeft(oldGeometry.left());
            savedGeometry.setRight(oldGeometry.right());
        }
        auto restore = setGeometryRestoreForAssignment(savedGeometry, written, receipt);
        if (restore.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(restore)) {
            return result;
        }
        afterRestore = std::move(restore.checkpoint);
    }
    const TileAssignmentFence &checkpoint = afterRestore ? *afterRestore : written;
    // The legacy geometry: a valid restore is returned to, clamped to the minimum and centred per
    // axis; an invalid one lets the client choose its size from the maximize area's origin.
    RectF geometry = oldGeometry;
    if (oldMode & MaximizeHorizontal) {
        if (geometryRestore().isValid()) {
            geometry.setWidth(std::max(geometryRestore().width(), minimum.width()));
            geometry.moveHorizontalCenter(geometryRestore().horizontalCenter());
        } else {
            geometry.setX(clientArea.x());
            geometry.setWidth(0);
        }
    }
    if (oldMode & MaximizeVertical) {
        if (geometryRestore().isValid()) {
            geometry.setHeight(std::max(geometryRestore().height(), minimum.height()));
            geometry.moveVerticalCenter(geometryRestore().verticalCenter());
        } else {
            geometry.setY(clientArea.y());
            geometry.setHeight(0);
        }
    }
    auto request = requestMoveResizeGuarded(geometry, MoveResizeMode::MoveResize, checkpoint, receipt);
    if (request.result.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(request.result)) {
        return result;
    }
    markAsPlaced();
    // AppliedCurrent is this synchronous request, completed while current; not the client's
    // acknowledgement. The carrier keeps the request's checkpoint and the receipt's provenance.
    return std::move(request.result);
}

XdgPopupWindow::XdgPopupWindow(XdgPopupInterface *shellSurface)
    : XdgSurfaceWindow(shellSurface->xdgSurface())
    , m_shellSurface(shellSurface)
{
    setOutput(workspace()->activeOutput());
    setMoveResizeOutput(workspace()->activeOutput());

    m_windowType = WindowType::Unknown;

    connect(shellSurface, &XdgPopupInterface::grabRequested,
            this, &XdgPopupWindow::handleGrabRequested);
    connect(shellSurface, &XdgPopupInterface::initializeRequested,
            this, &XdgPopupWindow::initialize);
    connect(shellSurface, &XdgPopupInterface::repositionRequested,
            this, &XdgPopupWindow::handleRepositionRequested);
    connect(shellSurface, &XdgPopupInterface::aboutToBeDestroyed,
            this, &XdgPopupWindow::destroyWindow);
}

void XdgPopupWindow::handleRoleDestroyed()
{
    if (transientFor()) {
        disconnect(transientFor(), &Window::frameGeometryChanged,
                   this, &XdgPopupWindow::relayout);
        disconnect(transientFor(), &Window::closed,
                   this, &XdgPopupWindow::popupDone);
    }

    m_shellSurface->disconnect(this);

    XdgSurfaceWindow::handleRoleDestroyed();
}

void XdgPopupWindow::handleRepositionRequested(quint32 token)
{
    m_repositionToken = token;

    if (m_isInitialized) {
        updateRelativePlacement();
        relayout();
    }
}

void XdgPopupWindow::updateRelativePlacement()
{
    const QPointF parentPosition = transientFor()->nextFramePosToClientPos(transientFor()->pos());
    const RectF bounds = workspace()->clientArea(transientFor()->isFullScreen() ? FullScreenArea : PlacementArea, transientFor()).translated(-parentPosition);
    const XdgPositioner positioner = m_shellSurface->positioner();

    if (m_plasmaShellSurface && m_plasmaShellSurface->isPositionSet()) {
        m_relativePlacement = RectF(m_plasmaShellSurface->position(), positioner.size()).translated(-parentPosition);
    } else {
        m_relativePlacement = positioner.placement(bounds);
    }
}

void XdgPopupWindow::relayout()
{
    if (m_shellSurface->positioner().isReactive()) {
        updateRelativePlacement();
    }
    place(transientPlacement());
    scheduleConfigure();
}

XdgPopupWindow::~XdgPopupWindow()
{
}

bool XdgPopupWindow::hasPopupGrab() const
{
    return m_haveExplicitGrab;
}

void XdgPopupWindow::popupDone()
{
    m_shellSurface->sendPopupDone();
    destroyWindow();
}

bool XdgPopupWindow::isPopupWindow() const
{
    return true;
}

bool XdgPopupWindow::isTransient() const
{
    return true;
}

bool XdgPopupWindow::isPlaceable() const
{
    return false;
}

bool XdgPopupWindow::isResizable() const
{
    return false;
}

bool XdgPopupWindow::isMovable() const
{
    return false;
}

bool XdgPopupWindow::isMovableAcrossScreens() const
{
    return false;
}

RectF XdgPopupWindow::transientPlacement() const
{
    const QPointF parentPosition = transientFor()->nextFramePosToClientPos(transientFor()->pos());
    return m_relativePlacement.translated(parentPosition);
}

bool XdgPopupWindow::isCloseable() const
{
    return false;
}

void XdgPopupWindow::closeWindow()
{
}

bool XdgPopupWindow::wantsInput() const
{
    return false;
}

bool XdgPopupWindow::acceptsFocus() const
{
    return false;
}

XdgSurfaceConfigure *XdgPopupWindow::sendRoleConfigure()
{
    surface()->setPreferredBufferScale(nextTargetScale());
    surface()->setPreferredBufferTransform(preferredBufferTransform());
    surface()->setPreferredColorDescription(preferredColorDescription());

    if (m_repositionToken) {
        m_shellSurface->sendRepositioned(*m_repositionToken);
        m_repositionToken.reset();
    }

    const quint32 serial = m_shellSurface->sendConfigure(m_relativePlacement);

    XdgSurfaceConfigure *configureEvent = new XdgSurfaceConfigure();
    configureEvent->bounds = moveResizeGeometry();
    configureEvent->serial = serial;

    return configureEvent;
}

void XdgPopupWindow::handleGrabRequested(SeatInterface *seat, quint32 serial)
{
    m_haveExplicitGrab = true;
}

void XdgPopupWindow::initialize()
{
    Window *parent = waylandServer()->findWindow(m_shellSurface->parentSurface());
    if (!parent) {
        popupDone();
        return;
    }

    parent->addTransient(this);
    setTransientFor(parent);
    setDesktops(parent->desktops());
#if KWIN_BUILD_ACTIVITIES
    setOnActivities(parent->activities());
#endif

    updateRelativePlacement();
    connect(parent, &Window::frameGeometryChanged, this, &XdgPopupWindow::relayout);
    connect(parent, &Window::closed, this, &XdgPopupWindow::popupDone);

    place(transientPlacement());
    scheduleConfigure();

    m_isInitialized = true;
}

void XdgPopupWindow::doSetNextTargetScale()
{
    if (isDeleted()) {
        return;
    }
    if (m_shellSurface->isConfigured()) {
        scheduleConfigure();
    }
}

void XdgPopupWindow::doSetPreferredBufferTransform()
{
    if (isDeleted()) {
        return;
    }
    if (m_shellSurface->isConfigured()) {
        scheduleConfigure();
    }
}

void XdgPopupWindow::doSetPreferredColorDescription()
{
    if (isDeleted()) {
        return;
    }
    if (m_shellSurface->isConfigured()) {
        scheduleConfigure();
    }
}

} // namespace KWin

#include "moc_xdgshellwindow.cpp"
