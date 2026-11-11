/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2019 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "window.h"
#include "effect/globals.h"
#include "utils/common.h"

#if KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "appmenu.h"
#include "client_machine.h"
#include "compositor.h"
#include "core/output.h"
#include "decorations/decoratedwindow.h"
#include "decorations/decorationpalette.h"
#include "focuschain.h"
#include "input.h"
#include "outline.h"
#include "placement.h"
#include "pointer_input.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "screenedge.h"
#include "shadow.h"
#include "tiles/tilemanager.h"
#include "tiles/transientplacementwriter.h"
#include "tiles/transienttilecontroller.h"
#include "useractions.h"
#include "virtualdesktops.h"
#include "wayland/output.h"
#include "wayland/plasmawindowmanagement.h"
#include "wayland/surface.h"
#include "wayland_server.h"
#include "workspace.h"

#include <KDecoration3/DecoratedWindow>
#include <KDecoration3/Decoration>
#include <KDesktopFile>

#include <QDebug>
#include <QDir>
#include <QHash>
#include <QJSEngine>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QStyleHints>

#include <limits>

namespace KWin
{

namespace
{

// Keep the same-version backport ABI-compatible with consumers built against stock headers.
QHash<const Window *, RectF> s_notifiedRequestedGeometry;

bool exactFrameGeometry(const RectF &left, const RectF &right)
{
    return left.x() == right.x() && left.y() == right.y() && left.width() == right.width() && left.height() == right.height();
}

quint64 nextAssociationChange(quint64 current)
{
    // Saturate rather than allowing an old pending notification to match a wrapped identity.
    return current == std::numeric_limits<quint64>::max() ? current : current + 1;
}

}

QHash<QString, std::weak_ptr<Decoration::DecorationPalette>> Window::s_palettes;
std::shared_ptr<Decoration::DecorationPalette> Window::s_defaultPalette;

Window::Window()
    : ready_for_painting(false)
    , m_internalId(QUuid::createUuid())
    , m_clientMachine(new ClientMachine(this))
    , m_skipCloseAnimation(false)
    , m_colorScheme(QStringLiteral("kdeglobals"))
{
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);

    connect(this, &Window::bufferGeometryChanged, this, &Window::inputTransformationChanged);

    connect(this, &Window::interactiveMoveResizeStarted, this, &Window::moveResizedChanged);
    connect(this, &Window::interactiveMoveResizeFinished, this, &Window::moveResizedChanged);

    connect(this, &Window::paletteChanged, this, &Window::triggerDecorationRepaint);

    // If the user manually moved the window, don't restore it after the keyboard closes
    connect(this, &Window::interactiveMoveResizeFinished, this, [this]() {
        m_keyboardGeometryRestore = RectF();
    });
    connect(this, &Window::maximizedChanged, this, [this]() {
        m_keyboardGeometryRestore = RectF();
    });
    connect(this, &Window::fullScreenChanged, this, [this]() {
        m_keyboardGeometryRestore = RectF();
    });

    // replace on-screen-display on size changes
    connect(this, &Window::frameGeometryChanged, this, [this](const RectF &old) {
        if (isOnScreenDisplay() && !frameGeometry().isEmpty() && old.size() != frameGeometry().size() && isPlaceable()) {
            if (const auto placement = workspace()->placement()->place(this, workspace()->clientArea(PlacementArea, this, workspace()->activeOutput()))) {
                place(*placement);
            }
        }
    });

    connect(Workspace::self()->applicationMenu(), &ApplicationMenu::applicationMenuEnabledChanged, this, [this] {
        Q_EMIT hasApplicationMenuChanged(hasApplicationMenu());
    });
    connect(&m_offscreenFramecallbackTimer, &QTimer::timeout, this, &Window::maybeSendFrameCallback);
}

Window::~Window()
{
    s_notifiedRequestedGeometry.remove(this);
}

void Window::ref()
{
    ++m_refCount;
}

void Window::unref()
{
    Q_ASSERT(m_refCount > 0);
    --m_refCount;
    if (m_refCount) {
        return;
    }
    if (m_deleted) {
        workspace()->removeDeleted(this);
    }
    delete this;
}

QDebug operator<<(QDebug debug, const Window *window)
{
    QDebugStateSaver saver(debug);
    debug.nospace();
    if (window) {
        debug << window->metaObject()->className() << '(' << static_cast<const void *>(window);
        if (const SurfaceInterface *surface = window->surface()) {
            debug << ", surface=" << surface;
        }
        if (window->isClient()) {
            if (!window->isPopupWindow()) {
                debug << ", caption=" << window->caption();
            }
            if (window->transientFor()) {
                debug << ", transientFor=" << window->transientFor();
            }
        }
        if (debug.verbosity() > 2) {
            debug << ", frameGeometry=" << window->frameGeometry();
            debug << ", resourceName=" << window->resourceName();
            debug << ", resourceClass=" << window->resourceClass();
        }
        debug << ')';
    } else {
        debug << "Window(0x0)";
    }
    return debug;
}

RectF Window::visibleGeometry() const
{
    if (const WindowItem *item = windowItem()) {
        return item->mapToScene(item->boundingRect());
    }
    return RectF();
}

/**
 * Returns client machine for this window,
 * taken either from its window or from the leader window.
 */
QString Window::wmClientMachine(bool use_localhost) const
{
    if (!m_clientMachine) {
        // this should never happen
        return QString();
    }
    if (use_localhost && m_clientMachine->isLocal()) {
        // special name for the local machine (localhost)
        return ClientMachine::localhost();
    }
    return m_clientMachine->hostName();
}

void Window::setResourceClass(const QString &name, const QString &className)
{
    resource_name = name;
    resource_class = className;
    Q_EMIT windowClassChanged();
}

qreal Window::opacity() const
{
    return m_opacity;
}

void Window::setOpacity(qreal opacity)
{
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (m_opacity == opacity) {
        return;
    }
    const qreal oldOpacity = m_opacity;
    m_opacity = opacity;
    Q_EMIT opacityChanged(this, oldOpacity);
}

void Window::setupItem()
{
    m_windowItem = createItem(kwinApp()->scene()->containerItem());
}

void Window::setReadyForPainting()
{
    if (!ready_for_painting) {
        ready_for_painting = true;
        Q_EMIT readyForPaintingChanged();
    }
}

LogicalOutput *Window::output() const
{
    return m_output;
}

void Window::setOutput(LogicalOutput *output)
{
    if (m_output != output) {
        invalidateTransientRestoreAttempt();
        LogicalOutput *oldOutput = m_output;
        m_output = output;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        Q_EMIT outputChanged(oldOutput);
    }
}

bool Window::isOnActiveOutput() const
{
    return isOnOutput(workspace()->activeOutput());
}

bool Window::isOnOutput(LogicalOutput *output) const
{
    return output->geometry().intersects(frameGeometry().toRect());
}

Shadow *Window::shadow() const
{
    return m_shadow.get();
}

void Window::updateShadow()
{
    if (m_shadow) {
        if (!m_shadow->updateShadow()) {
            m_shadow.reset();
        }
        Q_EMIT shadowChanged();
    } else {
        m_shadow = Shadow::createShadow(this);
        if (m_shadow) {
            Q_EMIT shadowChanged();
        }
    }
}

EffectWindow *Window::effectWindow()
{
    return m_windowItem ? m_windowItem->effectWindow() : nullptr;
}

const EffectWindow *Window::effectWindow() const
{
    return m_windowItem ? m_windowItem->effectWindow() : nullptr;
}

SurfaceItem *Window::surfaceItem() const
{
    if (m_windowItem) {
        return m_windowItem->surfaceItem();
    }
    return nullptr;
}

bool Window::wantsShadowToBeRendered() const
{
    return !isFullScreen() && maximizeMode() != MaximizeFull;
}

bool Window::isClient() const
{
    return false;
}

bool Window::isUnmanaged() const
{
    return false;
}

pid_t Window::pid() const
{
    return -1;
}

bool Window::skipsCloseAnimation() const
{
    return m_skipCloseAnimation;
}

void Window::setSkipCloseAnimation(bool set)
{
    if (set == m_skipCloseAnimation) {
        return;
    }
    m_skipCloseAnimation = set;
    Q_EMIT skipCloseAnimationChanged();
}

SurfaceInterface *Window::surface() const
{
    return m_surface;
}

void Window::setSurface(SurfaceInterface *surface)
{
    if (m_surface == surface) {
        return;
    }
    m_surface = surface;
    Q_EMIT surfaceChanged();
}

int Window::stackingOrder() const
{
    return m_stackingOrder;
}

void Window::setStackingOrder(int order)
{
    if (m_stackingOrder != order) {
        m_stackingOrder = order;
        Q_EMIT stackingOrderChanged();
    }
}

QString Window::windowRole() const
{
    return QString();
}

QMatrix4x4 Window::inputTransformation() const
{
    QMatrix4x4 m;
    m.translate(-m_bufferGeometry.x(), -m_bufferGeometry.y());
    return m;
}

bool Window::hitTest(const QPointF &point) const
{
    if (isDecorated()) {
        if (m_decoration.inputRegion.contains(mapToFrame(point))) {
            return true;
        }
    }
    if (m_surface && m_surface->isMapped()) {
        return m_surface->inputSurfaceAt(mapToLocal(point));
    }
    return m_bufferGeometry.contains(point);
}

QPointF Window::mapToFrame(const QPointF &point) const
{
    return point - frameGeometry().topLeft();
}

QPointF Window::mapToLocal(const QPointF &point) const
{
    return point - bufferGeometry().topLeft();
}

QPointF Window::mapFromLocal(const QPointF &point) const
{
    return point + bufferGeometry().topLeft();
}

bool Window::isLocalhost() const
{
    if (!m_clientMachine) {
        return true;
    }
    return m_clientMachine->isLocal();
}

QMargins Window::frameMargins() const
{
    return QMargins(borderLeft(), borderTop(), borderRight(), borderBottom());
}

BorderRadius Window::borderRadius() const
{
    return m_borderRadius;
}

void Window::setBorderRadius(const BorderRadius &radius)
{
    if (m_borderRadius != radius) {
        m_borderRadius = radius;
        Q_EMIT borderRadiusChanged();
    }
}

bool Window::belongToSameApplication(const Window *c1, const Window *c2, SameApplicationChecks checks)
{
    return c1->belongsToSameApplication(c2, checks);
}

#if KWIN_BUILD_X11
xcb_timestamp_t Window::userTime() const
{
    return XCB_TIME_CURRENT_TIME;
}
#endif

void Window::setSkipSwitcher(bool set)
{
    set = rules()->checkSkipSwitcher(set);
    if (set == skipSwitcher()) {
        return;
    }
    m_skipSwitcher = set;
    doSetSkipSwitcher();
    updateWindowRules(Rules::SkipSwitcher);
    Q_EMIT skipSwitcherChanged();
}

void Window::setSkipPager(bool b)
{
    b = rules()->checkSkipPager(b);
    if (b == skipPager()) {
        return;
    }
    m_skipPager = b;
    doSetSkipPager();
    updateWindowRules(Rules::SkipPager);
    Q_EMIT skipPagerChanged();
}

void Window::doSetSkipPager()
{
}

void Window::setSkipTaskbar(bool b)
{
    if (b == skipTaskbar()) {
        return;
    }
    m_skipTaskbar = b;
    doSetSkipTaskbar();
    updateWindowRules(Rules::SkipTaskbar);
    Q_EMIT skipTaskbarChanged();
}

void Window::setOriginalSkipTaskbar(bool b)
{
    m_originalSkipTaskbar = rules()->checkSkipTaskbar(b);
    setSkipTaskbar(m_originalSkipTaskbar);
}

void Window::doSetSkipTaskbar()
{
}

void Window::doSetSkipSwitcher()
{
}

void Window::setIcon(const QIcon &icon)
{
    m_icon = icon;
    Q_EMIT iconChanged();
}

void Window::setActive(bool act)
{
    if (isDeleted()) {
        return;
    }
    if (m_active == act) {
        return;
    }
    m_active = act;

    setOpacity(m_active ? rules()->checkOpacityActive(opacity()) : rules()->checkOpacityInactive(opacity()));

    if (!m_active) {
        cancelAutoRaise();
    }

    StackingUpdatesBlocker blocker(workspace());
    updateLayer(); // active windows may get different layer
    auto mainwindows = mainWindows();
    for (auto it = mainwindows.constBegin(); it != mainwindows.constEnd(); ++it) {
        if ((*it)->isFullScreen()) { // fullscreens go high even if their transient is active
            (*it)->updateLayer();
        }
    }

    doSetActive();
    Q_EMIT activeChanged();
}

void Window::doSetActive()
{
}

bool Window::isDeleted() const
{
    return m_deleted;
}

void Window::markAsDeleted()
{
    Q_ASSERT(!m_deleted);
    invalidateTransientRestoreAttempt();
    m_deleted = true;
    workspace()->addDeleted(this);
}

Layer Window::layer() const
{
    if (m_layer == UnknownLayer) {
        const_cast<Window *>(this)->m_layer = rules()->checkLayer(belongsToLayer());
    }
    return m_layer;
}

void Window::updateLayer()
{
    if (isDeleted()) {
        return;
    }
    if (layer() == rules()->checkLayer(belongsToLayer())) {
        return;
    }
    StackingUpdatesBlocker blocker(workspace());
    m_layer = UnknownLayer; // invalidate, will be updated when doing restacking
}

Layer Window::belongsToLayer() const
{
    if (isOutline()) {
        return NormalLayer;
    }
    if (isUnmanaged() || isInternal()) {
        return OverlayLayer;
    }
    if (isPictureInPicture()) {
        return PopupLayer;
    }
    if (isInputMethod()) {
        return OverlayLayer;
    }
    if (isLockScreenOverlay() && waylandServer()->isScreenLocked()) {
        return OverlayLayer;
    }
    if (isDesktop()) {
        return DesktopLayer;
    }
    if (isSplash()) { // no damn annoying splashscreens
        return NormalLayer; // getting in the way of everything else
    }
    if (isDock() || isAppletPopup()) {
        return AboveLayer;
    }
    if (isPopupWindow()) {
        return PopupLayer;
    }
    if (isOnScreenDisplay()) {
        return OnScreenDisplayLayer;
    }
    if (isNotification()) {
        return NotificationLayer;
    }
    if (isCriticalNotification()) {
        return CriticalNotificationLayer;
    }
    if (keepBelow()) {
        return BelowLayer;
    }
    if (isActiveFullScreen()) {
        return ActiveLayer;
    }
    if (keepAbove()) {
        return AboveLayer;
    }

    return NormalLayer;
}

bool Window::belongsToDesktop() const
{
    return false;
}

void Window::setKeepAbove(bool b)
{
    b = rules()->checkKeepAbove(b);
    if (b && !rules()->checkKeepBelow(false)) {
        setKeepBelow(false);
    }
    if (b == keepAbove()) {
        return;
    }
    m_keepAbove = b;
    doSetKeepAbove();
    updateLayer();
    updateWindowRules(Rules::Above);

    Q_EMIT keepAboveChanged(m_keepAbove);
}

void Window::doSetKeepAbove()
{
}

void Window::setKeepBelow(bool b)
{
    b = rules()->checkKeepBelow(b);
    if (b && !rules()->checkKeepAbove(false)) {
        setKeepAbove(false);
    }
    if (b == keepBelow()) {
        return;
    }
    m_keepBelow = b;
    doSetKeepBelow();
    updateLayer();
    updateWindowRules(Rules::Below);

    Q_EMIT keepBelowChanged(m_keepBelow);
}

void Window::doSetKeepBelow()
{
}

void Window::startAutoRaise()
{
    delete m_autoRaiseTimer;
    m_autoRaiseTimer = new QTimer(this);
    connect(m_autoRaiseTimer, &QTimer::timeout, this, &Window::autoRaise);
    m_autoRaiseTimer->setSingleShot(true);
    m_autoRaiseTimer->start(options->autoRaiseInterval());
}

void Window::cancelAutoRaise()
{
    delete m_autoRaiseTimer;
    m_autoRaiseTimer = nullptr;
}

void Window::autoRaise()
{
    workspace()->raiseWindow(this);
    cancelAutoRaise();
}

bool Window::isMostRecentlyRaised() const
{
    // The last window in the unconstrained stacking order is the most recently raised one.
    return workspace()->topWindowOnDesktop(VirtualDesktopManager::self()->currentDesktop(output()), nullptr, true, false) == this;
}

bool Window::wantsTabFocus() const
{
    return (isNormalWindow() || isDialog() || isAppletPopup()) && wantsInput();
}

void Window::takeFocus()
{
}

bool Window::isSpecialWindow() const
{
    // TODO
    return isDesktop() || isDock() || isSplash() || isToolbar() || isNotification() || isOnScreenDisplay() || isCriticalNotification() || isAppletPopup() || isTooltip();
}

void Window::demandAttention(bool set)
{
    if (isActive()) {
        set = false;
    }
    if (m_demandsAttention == set) {
        return;
    }
    m_demandsAttention = set;
    doSetDemandsAttention();
    workspace()->windowAttentionChanged(this, set);
    Q_EMIT demandsAttentionChanged();
}

void Window::doSetDemandsAttention()
{
}

void Window::setDesktops(QList<VirtualDesktop *> desktops)
{
    desktops = rules()->checkDesktops(desktops);
    if (desktops == m_desktops) {
        return;
    }

    invalidateTransientRestoreAttempt();
    m_desktops = desktops;

    if (windowManagementInterface()) {
        auto currentDesktops = windowManagementInterface()->plasmaVirtualDesktops();
        for (auto desktop : std::as_const(m_desktops)) {
            if (!currentDesktops.contains(desktop->id())) {
                windowManagementInterface()->addPlasmaVirtualDesktop(desktop->id());
            } else {
                currentDesktops.removeOne(desktop->id());
            }
        }
        for (const auto &desktopId : std::as_const(currentDesktops)) {
            windowManagementInterface()->removePlasmaVirtualDesktop(desktopId);
        }
    }

    auto transients_stacking_order = workspace()->ensureStackingOrder(transients());
    for (auto it = transients_stacking_order.constBegin(); it != transients_stacking_order.constEnd(); ++it) {
        (*it)->setDesktops(desktops);
    }

    if (isModal()) // if a modal dialog is moved, move the mainwindow with it as otherwise
                   // the (just moved) modal dialog will confusingly return to the mainwindow with
                   // the next desktop change
    {
        const auto windows = mainWindows();
        for (Window *other : windows) {
            other->setDesktops(desktops);
        }
    }

    doSetDesktop();

    Workspace::self()->focusChain()->update(this, FocusChain::MakeFirst);
    updateWindowRules(Rules::Desktops);

    Q_EMIT desktopsChanged();
}

void Window::doSetDesktop()
{
}

void Window::enterDesktop(VirtualDesktop *virtualDesktop)
{
    if (m_desktops.contains(virtualDesktop)) {
        return;
    }
    auto desktops = m_desktops;
    desktops.append(virtualDesktop);
    setDesktops(desktops);
}

void Window::leaveDesktop(VirtualDesktop *virtualDesktop)
{
    QList<VirtualDesktop *> currentDesktops;
    if (m_desktops.isEmpty()) {
        currentDesktops = VirtualDesktopManager::self()->desktops();
    } else {
        currentDesktops = m_desktops;
    }

    if (!currentDesktops.contains(virtualDesktop)) {
        return;
    }
    auto desktops = currentDesktops;
    desktops.removeOne(virtualDesktop);
    setDesktops(desktops);
}

void Window::setOnAllDesktops(bool b)
{
    if (b == isOnAllDesktops()) {
        return;
    }
    if (b) {
        setDesktops({});
    } else {
        setDesktops({VirtualDesktopManager::self()->currentDesktop(output())});
    }
}

QList<VirtualDesktop *> Window::desktops() const
{
    return m_desktops;
}

QStringList Window::desktopIds() const
{
    const auto desks = desktops();
    QStringList ids;
    ids.reserve(desks.count());
    std::transform(desks.constBegin(), desks.constEnd(), std::back_inserter(ids), [](const VirtualDesktop *vd) {
        return vd->id();
    });
    return ids;
}

bool Window::isOnDesktop(VirtualDesktop *desktop) const
{
    return isOnAllDesktops() || desktops().contains(desktop);
}

bool Window::isOnCurrentDesktop() const
{
    return isOnDesktop(VirtualDesktopManager::self()->currentDesktop(output()));
}

Qt::Edge Window::titlebarPosition() const
{
    // TODO: still needed, remove?
    return Qt::TopEdge;
}

bool Window::titlebarPositionUnderMouse() const
{
    if (!isDecorated()) {
        return false;
    }
    const auto sectionUnderMouse = decoration()->sectionUnderMouse();
    if (sectionUnderMouse == Qt::TitleBarArea) {
        return true;
    }
    // check other sections based on titlebarPosition
    switch (titlebarPosition()) {
    case Qt::TopEdge:
        return (sectionUnderMouse == Qt::TopLeftSection || sectionUnderMouse == Qt::TopSection || sectionUnderMouse == Qt::TopRightSection);
    case Qt::LeftEdge:
        return (sectionUnderMouse == Qt::TopLeftSection || sectionUnderMouse == Qt::LeftSection || sectionUnderMouse == Qt::BottomLeftSection);
    case Qt::RightEdge:
        return (sectionUnderMouse == Qt::BottomRightSection || sectionUnderMouse == Qt::RightSection || sectionUnderMouse == Qt::TopRightSection);
    case Qt::BottomEdge:
        return (sectionUnderMouse == Qt::BottomLeftSection || sectionUnderMouse == Qt::BottomSection || sectionUnderMouse == Qt::BottomRightSection);
    default:
        // nothing
        return false;
    }
}

void Window::setMinimized(bool set)
{
    const bool effectiveSet = rules()->checkMinimize(set);
    if (m_minimized == effectiveSet) {
        return;
    }

    if (effectiveSet && !isMinimizable()) {
        return;
    }

    m_minimized = effectiveSet;
    doMinimize();

    updateWindowRules(Rules::Minimize);
    Q_EMIT minimizedChanged();
}

void Window::doMinimize()
{
}

QPalette Window::palette()
{
    ensurePalette();
    return m_palette->palette();
}

const Decoration::DecorationPalette *Window::decorationPalette()
{
    ensurePalette();
    return m_palette.get();
}

QString Window::preferredColorScheme() const
{
    return rules()->checkDecoColor(QString());
}

QString Window::colorScheme() const
{
    return m_colorScheme;
}

void Window::setColorScheme(const QString &colorScheme)
{
    QString requestedColorScheme = colorScheme;
    if (requestedColorScheme.isEmpty()) {
        requestedColorScheme = QStringLiteral("kdeglobals");
    }

    if (m_colorScheme == requestedColorScheme) {
        return;
    }

    m_colorScheme = requestedColorScheme;

    if (m_palette) {
        disconnect(m_palette.get(), &Decoration::DecorationPalette::changed, this, &Window::handlePaletteChange);
        m_palette.reset();

        // If there already was a palette, re-create it right away
        // so the signals for repainting the decoration are emitted.
        ensurePalette();
    }

    Q_EMIT colorSchemeChanged();
}

void Window::updateColorScheme()
{
    setColorScheme(preferredColorScheme());
}

void Window::ensurePalette()
{
    if (m_palette) {
        return;
    }

    auto it = s_palettes.find(m_colorScheme);

    if (it == s_palettes.end() || it->expired()) {
        m_palette = std::make_shared<Decoration::DecorationPalette>(m_colorScheme);
        if (m_palette->isValid()) {
            s_palettes[m_colorScheme] = m_palette;
        } else {
            if (!s_defaultPalette) {
                s_defaultPalette = std::make_shared<Decoration::DecorationPalette>(QStringLiteral("kdeglobals"));
                s_palettes[QStringLiteral("kdeglobals")] = s_defaultPalette;
            }

            m_palette = s_defaultPalette;
        }

        if (m_colorScheme == QLatin1StringView("kdeglobals")) {
            s_defaultPalette = m_palette;
        }
    } else {
        m_palette = it->lock();
    }

    connect(m_palette.get(), &Decoration::DecorationPalette::changed, this, &Window::handlePaletteChange);

    handlePaletteChange();
}

void Window::handlePaletteChange()
{
    Q_EMIT paletteChanged(palette());
}

RectF Window::keepInArea(RectF geometry, RectF area, bool partial) const
{
    if (partial) {
        // increase the area so that can have only 100 pixels in the area
        const RectF geometry = moveResizeGeometry();
        area.setLeft(std::min(area.left() - geometry.width() + 100, area.left()));
        area.setTop(std::min(area.top() - geometry.height() + 100, area.top()));
        area.setRight(std::max(area.right() + geometry.width() - 100, area.right()));
        area.setBottom(std::max(area.bottom() + geometry.height() - 100, area.bottom()));
    }
    if (!partial) {
        // resize to fit into area
        if (area.width() < geometry.width() || area.height() < geometry.height()) {
            geometry = resizeWithChecks(geometry, geometry.size().boundedTo(area.size()));
        }
    }

    if (geometry.right() > area.right() && geometry.width() <= area.width()) {
        geometry.moveRight(area.right());
    }
    if (geometry.bottom() > area.bottom() && geometry.height() <= area.height()) {
        geometry.moveBottom(area.bottom());
    }

    if (geometry.left() < area.left()) {
        geometry.moveLeft(area.left());
    }
    if (geometry.top() < area.top()) {
        geometry.moveTop(area.top());
    }
    return geometry;
}

/**
 * Returns the maximum client size, not the maximum frame size.
 */
QSizeF Window::maxSize() const
{
    return rules()->checkMaxSize(QSize(INT_MAX, INT_MAX));
}

/**
 * Returns the minimum client size, not the minimum frame size.
 */
QSizeF Window::minSize() const
{
    return rules()->checkMinSize(QSize(0, 0));
}

void Window::maximize(MaximizeMode mode, const RectF &restore)
{
    qCWarning(KWIN_CORE, "%s doesn't support setting maximized state", metaObject()->className());
}

void Window::setMaximize(bool vertically, bool horizontally, const RectF &restore)
{
    MaximizeMode mode = MaximizeRestore;
    if (vertically) {
        mode = MaximizeMode(mode | MaximizeVertical);
    }
    if (horizontally) {
        mode = MaximizeMode(mode | MaximizeHorizontal);
    }

    maximize(mode, restore);
}

bool Window::startInteractiveMoveResize()
{
    if (isDeleted()) {
        return false;
    }

    Q_ASSERT(!isInteractiveMoveResize());
    stopDelayedInteractiveMoveResize();

    if (isRequestedFullScreen() && (workspace()->outputs().count() < 2 || !isMovableAcrossScreens())) {
        return false;
    }
    if ((interactiveMoveResizeGravity() == Gravity::Center && !isMovableAcrossScreens())
        || (interactiveMoveResizeGravity() != Gravity::Center && !isResizable())) {
        return false;
    }
    if (!doStartInteractiveMoveResize()) {
        return false;
    }

    invalidateDecorationDoubleClickTimer();

    setInteractiveMoveResize(true);
    workspace()->setMoveResizeWindow(this);
    connect(this, &Window::outputChanged, this, &Window::finishInteractiveOutputChange);

    m_interactiveMoveResize.initialGeometry = moveResizeGeometry();
    m_interactiveMoveResize.initialOutputId = moveResizeOutput()->uuid();
    m_interactiveMoveResize.initialScreenArea = moveResizeOutput()->geometryF();
    m_interactiveMoveResize.initialMaximizeMode = requestedMaximizeMode();
    m_interactiveMoveResize.initialQuickTileMode = requestedQuickTileMode();
    m_interactiveMoveResize.initialGeometryRestore = geometryRestore();

    updateElectricGeometryRestore();
    performDelayedRaise();
    Q_EMIT interactiveMoveResizeStarted();
    if (workspace()->screenEdges()->isDesktopSwitchingMovingClients()) {
        workspace()->screenEdges()->reserveDesktopSwitching(true, Qt::Vertical | Qt::Horizontal);
    }
    return true;
}

void Window::finishInteractiveMoveResize(bool cancel)
{
    LogicalOutput *oldOutput = m_output;
    const bool wasMove = isInteractiveMove();
    leaveInteractiveMoveResize();

    doFinishInteractiveMoveResize();

    if (cancel) {
        moveResize(initialInteractiveMoveResizeGeometry());
        if (m_interactiveMoveResize.initialMaximizeMode != MaximizeMode::MaximizeRestore) {
            setMaximize(m_interactiveMoveResize.initialMaximizeMode & MaximizeMode::MaximizeVertical, m_interactiveMoveResize.initialMaximizeMode & MaximizeMode::MaximizeHorizontal, m_interactiveMoveResize.initialGeometryRestore);
        } else if (m_interactiveMoveResize.initialQuickTileMode) {
            setQuickTileMode(m_interactiveMoveResize.initialQuickTileMode, m_interactiveMoveResize.initialGeometry.center());
            setGeometryRestore(m_interactiveMoveResize.initialGeometryRestore);
        }
        finishInteractiveOutputChange(oldOutput);
    } else if (moveResizeOutput()->uuid() != m_interactiveMoveResize.initialOutputId) {
        sendToOutput(moveResizeOutput()); // checks rule validity
        const RectF oldScreenArea = m_interactiveMoveResize.initialScreenArea;
        const RectF screenArea = workspace()->clientArea(MaximizeArea, this, moveResizeOutput());
        m_electricGeometryRestore = moveToArea(m_electricGeometryRestore, oldScreenArea, screenArea);
        if (isRequestedFullScreen() || requestedMaximizeMode() != MaximizeRestore) {
            checkWorkspacePosition();
        }
    }

    if (isElectricBorderMaximizing()) {
        if (auto quickTileMode = std::get_if<QuickTileMode>(&m_electricMode.value())) {
            setQuickTileMode(*quickTileMode, m_interactiveMoveResize.anchor);
        } else if (auto maximizeMode = std::get_if<MaximizeMode>(&m_electricMode.value())) {
            maximize(*maximizeMode, m_electricGeometryRestore);
        }
        setElectricBorderMaximizing(false);
    } else if (wasMove && (m_interactiveMoveResize.modifiers & Qt::ShiftModifier)) {
        setQuickTileMode(QuickTileFlag::Custom, m_interactiveMoveResize.anchor);
    }
    setElectricBorderMode(std::nullopt);
    workspace()->outline()->hide();

    m_interactiveMoveResize.counter++;
    Q_EMIT interactiveMoveResizeFinished();
}

// When the user pressed mouse on the titlebar, don't activate move immediately,
// since it may be just a click. Activate instead after a delay. Move used to be
// activated only after moving by several pixels, but that looks bad.
void Window::startDelayedInteractiveMoveResize()
{
    if (isDeleted()) {
        return;
    }
    Q_ASSERT(!m_interactiveMoveResize.delayedTimer);
    m_interactiveMoveResize.delayedTimer = new QTimer(this);
    m_interactiveMoveResize.delayedTimer->setSingleShot(true);
    connect(m_interactiveMoveResize.delayedTimer, &QTimer::timeout, this, [this]() {
        Q_ASSERT(isInteractiveMoveResizePointerButtonDown());
        if (!startInteractiveMoveResize()) {
            setInteractiveMoveResizePointerButtonDown(false);
        }
        updateInteractiveMoveResizeCursor();
        stopDelayedInteractiveMoveResize();
    });
    m_interactiveMoveResize.delayedTimer->start(QApplication::startDragTime());
}

void Window::stopDelayedInteractiveMoveResize()
{
    delete m_interactiveMoveResize.delayedTimer;
    m_interactiveMoveResize.delayedTimer = nullptr;
}

void Window::updateInteractiveMoveResize(const QPointF &global, Qt::KeyboardModifiers modifiers)
{
    setInteractiveMoveResizeAnchor(global);
    setInteractiveMoveResizeModifiers(modifiers);

    const Gravity gravity = interactiveMoveResizeGravity();
    const RectF currentMoveResizeGeom = moveResizeGeometry();
    RectF nextMoveResizeGeom = currentMoveResizeGeom;

    if (isInteractiveResize()) {
        if (isWaitingForInteractiveResizeSync()) {
            return; // we're still waiting for the client or the timeout
        }

        if (m_tile && m_tile->supportsResizeGravity(gravity)) {
            m_tile->resizeFromGravity(gravity, global.x(), global.y());
            return;
        }

        nextMoveResizeGeom = nextInteractiveResizeGeometry(global);
        if (nextMoveResizeGeom != currentMoveResizeGeom) {
            if (m_tile && !m_tile->supportsResizeGravity(gravity)) {
                setGeometryRestore(nextMoveResizeGeom);
                setQuickTileModeAtCurrentPosition(QuickTileFlag::None);
                return;
            }

            if (requestedMaximizeMode() != MaximizeRestore) {
                switch (interactiveMoveResizeGravity()) {
                case Gravity::Left:
                case Gravity::Right:
                    // Quit maximized horizontally state if the window is resized horizontally.
                    if (requestedMaximizeMode() & MaximizeHorizontal) {
                        RectF originalGeometry = geometryRestore();
                        originalGeometry.setLeft(nextMoveResizeGeom.left());
                        originalGeometry.setRight(nextMoveResizeGeom.right());
                        maximize(requestedMaximizeMode() ^ MaximizeHorizontal, originalGeometry);
                        return;
                    }
                    break;
                case Gravity::Top:
                case Gravity::Bottom:
                    // Quit maximized vertically state if the window is resized vertically.
                    if (requestedMaximizeMode() & MaximizeVertical) {
                        RectF originalGeometry = geometryRestore();
                        originalGeometry.setTop(nextMoveResizeGeom.top());
                        originalGeometry.setBottom(nextMoveResizeGeom.bottom());
                        maximize(requestedMaximizeMode() ^ MaximizeVertical, originalGeometry);
                        return;
                    }
                    break;
                case Gravity::TopLeft:
                case Gravity::BottomLeft:
                case Gravity::TopRight:
                case Gravity::BottomRight:
                    // Quit the maximized mode if the window is resized by dragging one of its corners.
                    maximize(MaximizeRestore, nextMoveResizeGeom);
                    return;
                default:
                    Q_UNREACHABLE();
                }
            }

            doInteractiveResizeSync(nextMoveResizeGeom);
            Q_EMIT interactiveMoveResizeStepped(nextMoveResizeGeom);
        }
    } else if (isInteractiveMove()) {
        if (isRequestedFullScreen()) {
            nextMoveResizeGeom = workspace()->clientArea(FullScreenArea, this, global);
        } else {
            nextMoveResizeGeom = nextInteractiveMoveGeometry(frameGeometry());
        }

        if (nextMoveResizeGeom != currentMoveResizeGeom) {
            if (!isRequestedFullScreen()) {
                if (maximizeMode() != MaximizeRestore) {
                    if (maximizeMode() & MaximizeHorizontal) {
                        if (nextMoveResizeGeom.x() != currentMoveResizeGeom.x() || nextMoveResizeGeom.width() != currentMoveResizeGeom.width()) {
                            maximize(MaximizeRestore);
                            return;
                        }
                    }
                    if (maximizeMode() & MaximizeVertical) {
                        if (nextMoveResizeGeom.y() != currentMoveResizeGeom.y() || nextMoveResizeGeom.height() != currentMoveResizeGeom.height()) {
                            maximize(MaximizeRestore);
                            return;
                        }
                    }
                } else if (quickTileMode() != QuickTileMode(QuickTileFlag::None)) {
                    setQuickTileModeAtCurrentPosition(QuickTileFlag::None);
                    return;
                }
            }

            move(nextMoveResizeGeom.topLeft());
            Q_EMIT interactiveMoveResizeStepped(nextMoveResizeGeom);
        }

        if (!isRequestedFullScreen()) {
            if (modifiers & Qt::ShiftModifier) {
                resetQuickTilingMaximizationZones();
                const auto &r = quickTileGeometry(QuickTileFlag::Custom, global);
                if (r.isEmpty()) {
                    workspace()->outline()->hide();
                } else {
                    if (!workspace()->outline()->isActive() || workspace()->outline()->geometry() != r.toRect()) {
                        workspace()->outline()->show(r.toRect(), moveResizeGeometry().toRect());
                    }
                }
            } else {
                if (quickTileMode() == QuickTileMode(QuickTileFlag::None) && isResizable()) {
                    checkQuickTilingMaximizationZones(global.x(), global.y());
                }
                if (!m_electricMaximizing) {
                    // Only if we are in an electric maximizing gesture we should keep the outline,
                    // otherwise we must make sure it's hidden
                    workspace()->outline()->hide();
                }
            }
        }
    }
}

QSizeF Window::minVisibleArea() const
{
    const qreal minVisibleWidth = 100;

    qreal minVisibleHeight;
    switch (titlebarPosition()) {
    case Qt::LeftEdge:
        minVisibleHeight = borderLeft();
        break;
    case Qt::BottomEdge:
        minVisibleHeight = borderBottom();
        break;
    case Qt::RightEdge:
        minVisibleHeight = borderRight();
        break;
    case Qt::TopEdge:
    default:
        minVisibleHeight = borderTop();
        break;
    }

    if (!minVisibleHeight) {
        minVisibleHeight = 50;
    }

    return QSizeF(minVisibleWidth, minVisibleHeight);
}

/**
 * Computes possible locations for the visible titlebar subrect of height at least @p minHeight
 * and width at least @p minWidth. @p geometry is the geometry of the window,
 * and @p gravity specifies the move/resize mode.
 *
 * When @p gravity is None, TopLeft, Left, BottomLeft, or Top, returns the *top left corner* of the visible subrect.
 * When @p gravity is Right, TopRight, or BottomRight, returns the *top right corner* of the visible subrect.
 * @p gravity being Bottom is not supported, since in this case the titlebar does not move
 *
 * See doc/moveresizerestriction for more details on algorithm.
 */
static RegionF interactiveMoveResizeVisibleSubrectRegion(const RectF &geometry, Gravity gravity, const QSizeF &minVisibleArea)
{
    const auto outputs = workspace()->outputs();
    const auto struts = workspace()->restrictedMoveArea();
    const auto availableArea = workspace()->clientArea(FullArea, workspace()->activeOutput());

    RegionF offscreenArea(availableArea);
    for (const LogicalOutput *output : outputs) {
        offscreenArea -= output->geometry();
    }

    RectF initialRect = availableArea;
    switch (gravity) {
    case Gravity::Center:
    case Gravity::Top:
        // resizing from the top is handled like moving the window to avoid zero width rectangles when window width is equal to minWidth
        initialRect.adjust(0, 0, -minVisibleArea.width(), -minVisibleArea.height());
        break;
    case Gravity::Left:
    case Gravity::TopLeft:
    case Gravity::BottomLeft:
        initialRect.setRight(std::min(initialRect.right(), geometry.right()));
        initialRect.setBottom(std::min(initialRect.bottom(), geometry.bottom()));
        initialRect.adjust(0, 0, -minVisibleArea.width(), -minVisibleArea.height());
        break;
    case Gravity::Right:
    case Gravity::TopRight:
    case Gravity::BottomRight:
        initialRect.setLeft(std::max(initialRect.left(), geometry.left()));
        initialRect.setBottom(std::min(initialRect.bottom(), geometry.bottom()));
        initialRect.adjust(minVisibleArea.width(), 0, 0, -minVisibleArea.height());
        break;
    default:
        Q_UNREACHABLE();
    }

    RegionF availableRegion(initialRect);

    switch (gravity) {
    case Gravity::Center:
    case Gravity::Top:
    case Gravity::Left:
    case Gravity::TopLeft:
    case Gravity::BottomLeft:
        for (const RectF &rect : struts) {
            availableRegion -= rect.adjusted(-minVisibleArea.width(), -minVisibleArea.height(), 0, 0);
        }
        for (const RectF &rect : offscreenArea.rects()) {
            availableRegion -= rect.adjusted(-minVisibleArea.width(), -minVisibleArea.height(), 0, 0);
        }
        break;
    case Gravity::Right:
    case Gravity::TopRight:
    case Gravity::BottomRight:
        for (const RectF &rect : struts) {
            availableRegion -= rect.adjusted(0, -minVisibleArea.height(), minVisibleArea.width(), 0);
        }
        for (const RectF &rect : offscreenArea.rects()) {
            availableRegion -= rect.adjusted(0, -minVisibleArea.height(), minVisibleArea.width(), 0);
        }
        break;
    default:
        Q_UNREACHABLE();
    }

    return availableRegion;
}

/**
 * Find closest point to move top left corner of window such that a titlebar subrect of size at least @p minVisibleWidth by @p minVisibleHeight is visible.
 *
 * See doc/moveresizerestriction for more details on algorithm.
 */
std::optional<QPointF> Window::confineInteractiveMove(const RectF &geometry) const
{
    std::optional<QPointF> candidate;
    qreal bestScore;

    const QSizeF effectiveMinVisibleArea = minVisibleArea().boundedTo(geometry.size());
    const RegionF visibleSubrectRegion = interactiveMoveResizeVisibleSubrectRegion(geometry, Gravity::Center, effectiveMinVisibleArea);
    const QPointF anchor = geometry.topLeft();
    for (RectF rect : visibleSubrectRegion.rects()) {
        // Extend the left edge of the rect so the right edge of the window can be
        // effectiveMinVisibleArea.width() away from the left edge of the work area.
        rect.setLeft(rect.left() - geometry.width() + effectiveMinVisibleArea.width());

        const QPointF closest(std::clamp(anchor.x(), rect.left(), rect.right()),
                              std::clamp(anchor.y(), rect.top(), rect.bottom()));
        const qreal score = QLineF(anchor, closest).length();

        if (!candidate || score < bestScore) {
            candidate = closest;
            bestScore = score;
        }
    }

    return candidate;
}

/**
 * Find closest point to move anchor corner of window such that a titlebar subrect of size at least @p minVisibleWidth by @p minVisibleHeight is visible.
 *
 * When @p gravity is None, TopLeft, Left, BottomLeft, or Top, anchor is *top left corner*
 * When @p gravity is Right, TopRight, or BottomRight, anchor is *top right corner*
 * When @p gravity is Bottom, anchor is *bottom left corner*
 *
 * See doc/moveresizerestriction for more details on algorithm.
 */
std::optional<QPointF> Window::confineInteractiveResize(const RectF &geometry, Gravity gravity) const
{
    QSizeF effectiveMinVisibleArea = minVisibleArea();

    if (gravity == Gravity::Bottom) {
        QPointF candidate = geometry.bottomLeft();
        if (geometry.height() < effectiveMinVisibleArea.height()) {
            candidate.setY(geometry.top() + effectiveMinVisibleArea.height());
        }
        return candidate;
    }

    if (gravity == Gravity::Top) {
        // only in this case is the width of the window fixed during resize
        effectiveMinVisibleArea.rwidth() = std::min(geometry.width(), effectiveMinVisibleArea.width());
    }

    const RegionF visibleSubrectRegion = interactiveMoveResizeVisibleSubrectRegion(geometry, gravity, effectiveMinVisibleArea);
    QPointF anchor;
    switch (gravity) {
    case Gravity::Top:
    case Gravity::Left:
    case Gravity::TopLeft:
    case Gravity::BottomLeft:
        anchor = geometry.topLeft();
        break;
    case Gravity::Right:
    case Gravity::TopRight:
    case Gravity::BottomRight:
        anchor = geometry.topRight();
        break;
    default:
        Q_UNREACHABLE();
    }

    const auto availableArea = workspace()->clientArea(FullArea, workspace()->activeOutput());
    const QSizeF minFrameSize = nextClientSizeToFrameSize(minSize());
    const QSizeF maxFrameSize = nextClientSizeToFrameSize(maxSize());

    const auto closestAllowedPoint = [&](const RectF &rect, const QPointF &point) -> std::optional<QPointF> {
        // The input rect specifies an area where the anchor point can possibly be. Geometry
        // constraints are implemented by applying additional restrictions to the rect.
        RectF constrainedRect = rect;

        switch (gravity) {
        case Gravity::Top:
            // Resizing from the top is handled like moving the window to avoid zero width
            // rectangles when window width is equal to minVisibleWidth.
            constrainedRect.setLeft(rect.left() - geometry.width() + effectiveMinVisibleArea.width());
            break;

        case Gravity::Left:
        case Gravity::TopLeft:
        case Gravity::BottomLeft:
            // Convert top-left of visible titlebar subrect to top-left of the window.
            constrainedRect.setLeft(availableArea.left());
            break;

        case Gravity::Right:
        case Gravity::TopRight:
        case Gravity::BottomRight:
            // Convert top-right of visible titlebar subrect to top-right of the window.
            constrainedRect.setRight(availableArea.right());
            break;

        default:
            Q_UNREACHABLE();
        }

        // The minimum and the maximum height constraints are applied by moving the bottom and the top
        // edges of the constrainedRect so there is enough space reserved for the window and that the
        // window height doesn't exceed the maximum size.
        //
        // Note that after applying the height constraint, the top and the bottom edge of the constrainedRect
        // can swap, i.e. constrainedRect.top() > constrainedRect.bottom(), which means that the anchor
        // cannot lie in this rect. However, constrainedRect.top() == constrainedRect.bottom() is a valid
        // case, it can arise if the min and the max sizes are the same.
        switch (gravity) {
        case Gravity::TopLeft:
        case Gravity::Top:
        case Gravity::TopRight:
        case Gravity::BottomLeft:
        case Gravity::Bottom:
        case Gravity::BottomRight:
            constrainedRect.setBottom(std::min(constrainedRect.bottom(), geometry.bottom() - minFrameSize.height()));
            constrainedRect.setTop(std::max(constrainedRect.top(), geometry.bottom() - maxFrameSize.height()));
            break;

        case Gravity::Left:
        case Gravity::Right:
            break;

        case Gravity::Center:
            Q_UNREACHABLE();
        }

        if (constrainedRect.top() > constrainedRect.bottom()) {
            return std::nullopt;
        }

        // The minimum and the maximum width constraints are applied by moving the left and the right
        // edges of the constrainedRect so there is enough space reserved for the window and that the
        // window width doesn't exceed the maximum size.
        //
        // Note that after applying the width constraint, the left and the right edge of the constrainedRect
        // can swap, i.e. constrainedRect.left() > constrainedRect.right(), which means that the anchor
        // cannot lie in this rect. However, constrainedRect.left() == constrainedRect.right() is a valid
        // case, it can arise if the min and the max sizes are the same.
        switch (gravity) {
        case Gravity::TopLeft:
        case Gravity::Left:
        case Gravity::BottomLeft:
            constrainedRect.setRight(std::min(constrainedRect.right(), geometry.right() - minFrameSize.width()));
            constrainedRect.setLeft(std::max(constrainedRect.left(), geometry.right() - maxFrameSize.width()));
            break;

        case Gravity::Top:
        case Gravity::Bottom:
            break;

        case Gravity::TopRight:
        case Gravity::Right:
        case Gravity::BottomRight:
            constrainedRect.setLeft(std::max(constrainedRect.left(), geometry.left() + minFrameSize.width()));
            constrainedRect.setRight(std::min(constrainedRect.right(), geometry.left() + maxFrameSize.width()));
            break;

        case Gravity::Center:
            Q_UNREACHABLE();
        }

        if (constrainedRect.left() > constrainedRect.right()) {
            return std::nullopt;
        }

        return QPointF(std::clamp(point.x(), constrainedRect.left(), constrainedRect.right()),
                       std::clamp(point.y(), constrainedRect.top(), constrainedRect.bottom()));
    };

    // calculate distance, considering movement constraints in the different modes of resizing
    const auto calcDistance = [gravity](const QPointF &source, const QPointF &dest) {
        switch (gravity) {
        case Gravity::Top:
            // when resizing top edge, no horizontal movement is allowed
            if (!qFuzzyCompare(dest.x(), source.x())) {
                return std::numeric_limits<qreal>::max();
            }
            break;
        case Gravity::Left:
        case Gravity::Right:
        case Gravity::BottomLeft:
        case Gravity::BottomRight:
            // for these cases, no vertical movement is allowed
            if (!qFuzzyCompare(dest.y(), source.y())) {
                return std::numeric_limits<qreal>::max();
            }
            break;
        default:
            break;
        }
        return QLineF(source, dest).length();
    };

    std::optional<QPointF> candidate;
    qreal bestScore;
    for (const RectF &rect : visibleSubrectRegion.rects()) {
        const std::optional<QPointF> closest = closestAllowedPoint(rect, anchor);
        if (!closest) {
            continue;
        }

        const qreal score = calcDistance(anchor, *closest);
        if (!candidate || score < bestScore) {
            candidate = closest;
            bestScore = score;
        }
    }

    return candidate;
}

RectF Window::nextInteractiveResizeGeometry(const QPointF &global) const
{
    RectF nextMoveResizeGeom = moveResizeGeometry();

    const Gravity gravity = interactiveMoveResizeGravity();
    if (gravity == Gravity::Center || !isResizable()) {
        return nextMoveResizeGeom;
    }

    // these two points limit the geometry rectangle, i.e. if bottomleft resizing is done,
    // the bottomleft corner should be at is at (topleft.x(), bottomright().y())
    const RectF orig = initialInteractiveMoveResizeGeometry();
    const QPointF topleft = global - QPointF(interactiveMoveOffset().x() * orig.width(), interactiveMoveOffset().y() * orig.height());
    const QPointF bottomright = global + QPointF((1.0 - interactiveMoveOffset().x()) * orig.width(), (1.0 - interactiveMoveOffset().y()) * orig.height());

    switch (gravity) {
    case Gravity::TopLeft:
        nextMoveResizeGeom = RectF(topleft, orig.bottomRight());
        break;
    case Gravity::BottomRight:
        nextMoveResizeGeom = RectF(orig.topLeft(), bottomright);
        break;
    case Gravity::BottomLeft:
        nextMoveResizeGeom = RectF(QPointF(topleft.x(), orig.y()), QPointF(orig.right(), bottomright.y()));
        break;
    case Gravity::TopRight:
        nextMoveResizeGeom = RectF(QPointF(orig.x(), topleft.y()), QPointF(bottomright.x(), orig.bottom()));
        break;
    case Gravity::Top:
        nextMoveResizeGeom = RectF(QPointF(orig.left(), topleft.y()), orig.bottomRight());
        break;
    case Gravity::Bottom:
        nextMoveResizeGeom = RectF(orig.topLeft(), QPointF(orig.right(), bottomright.y()));
        break;
    case Gravity::Left:
        nextMoveResizeGeom = RectF(QPointF(topleft.x(), orig.top()), orig.bottomRight());
        break;
    case Gravity::Right:
        nextMoveResizeGeom = RectF(orig.topLeft(), QPointF(bottomright.x(), orig.bottom()));
        break;
    case Gravity::Center:
        Q_UNREACHABLE();
        break;
    }

    nextMoveResizeGeom = workspace()->adjustWindowSize(this, nextMoveResizeGeom, gravity);

    const QSizeF constrainedSize = constrainFrameSize(nextMoveResizeGeom.size());
    switch (gravity) {
    case Gravity::TopLeft:
        nextMoveResizeGeom.setLeft(nextMoveResizeGeom.right() - constrainedSize.width());
        nextMoveResizeGeom.setTop(nextMoveResizeGeom.bottom() - constrainedSize.height());
        break;
    case Gravity::Top:
        nextMoveResizeGeom.setTop(nextMoveResizeGeom.bottom() - constrainedSize.height());
        break;
    case Gravity::TopRight:
        nextMoveResizeGeom.setRight(nextMoveResizeGeom.left() + constrainedSize.width());
        nextMoveResizeGeom.setTop(nextMoveResizeGeom.bottom() - constrainedSize.height());
        break;
    case Gravity::Right:
        nextMoveResizeGeom.setRight(nextMoveResizeGeom.left() + constrainedSize.width());
        break;
    case Gravity::BottomRight:
        nextMoveResizeGeom.setRight(nextMoveResizeGeom.left() + constrainedSize.width());
        nextMoveResizeGeom.setBottom(nextMoveResizeGeom.top() + constrainedSize.height());
        break;
    case Gravity::Bottom:
        nextMoveResizeGeom.setBottom(nextMoveResizeGeom.top() + constrainedSize.height());
        break;
    case Gravity::BottomLeft:
        nextMoveResizeGeom.setLeft(nextMoveResizeGeom.right() - constrainedSize.width());
        nextMoveResizeGeom.setBottom(nextMoveResizeGeom.top() + constrainedSize.height());
        break;
    case Gravity::Left:
        nextMoveResizeGeom.setLeft(nextMoveResizeGeom.right() - constrainedSize.width());
        break;
    case Gravity::Center:
        Q_UNREACHABLE();
    }

    if (!isUnrestrictedInteractiveMoveResize()) {
        if (const auto anchor = confineInteractiveResize(nextMoveResizeGeom, gravity)) {
            switch (gravity) {
            case Gravity::TopLeft:
                nextMoveResizeGeom.setTopLeft(*anchor);
                break;
            case Gravity::Top:
                nextMoveResizeGeom.setTop(anchor->y());
                break;
            case Gravity::TopRight:
                nextMoveResizeGeom.setTopRight(*anchor);
                break;
            case Gravity::Right:
            case Gravity::BottomRight:
                nextMoveResizeGeom.setRight(anchor->x());
                break;
            case Gravity::Left:
            case Gravity::BottomLeft:
                nextMoveResizeGeom.setLeft(anchor->x());
                break;
            case Gravity::Bottom:
                nextMoveResizeGeom.setBottom(anchor->y());
                break;
            default:
                Q_UNREACHABLE();
            }
        }
    }

    return nextMoveResizeGeom;
}

RectF Window::nextInteractiveMoveGeometry(const RectF &rect) const
{
    if (!isMovable()) {
        return rect;
    }

    RectF nextMoveResizeGeom = rect;
    nextMoveResizeGeom.moveTopLeft(QPointF(interactiveMoveResizeAnchor().x() - interactiveMoveOffset().x() * rect.width(),
                                           interactiveMoveResizeAnchor().y() - interactiveMoveOffset().y() * rect.height()));
    nextMoveResizeGeom.moveTopLeft(workspace()->adjustWindowPosition(this, nextMoveResizeGeom.topLeft(), isUnrestrictedInteractiveMoveResize()));

    if (!isUnrestrictedInteractiveMoveResize()) {
        if (const auto anchor = confineInteractiveMove(nextMoveResizeGeom)) {
            nextMoveResizeGeom.moveTopLeft(anchor.value());
        }
    }

    return nextMoveResizeGeom;
}

StrutRect Window::strutRect(StrutArea area) const
{
    return StrutRect();
}

StrutRects Window::strutRects() const
{
    StrutRects region;
    if (const StrutRect strut = strutRect(StrutAreaTop); strut.isValid()) {
        region += strut;
    }
    if (const StrutRect strut = strutRect(StrutAreaRight); strut.isValid()) {
        region += strut;
    }
    if (const StrutRect strut = strutRect(StrutAreaBottom); strut.isValid()) {
        region += strut;
    }
    if (const StrutRect strut = strutRect(StrutAreaLeft); strut.isValid()) {
        region += strut;
    }
    return region;
}

bool Window::hasStrut() const
{
    return false;
}

void Window::setupWindowManagementInterface()
{
    if (m_windowManagementInterface) {
        // already setup
        return;
    }
    if (!waylandServer()->windowManagement()) {
        return;
    }
    auto w = waylandServer()->windowManagement()->createWindow(this, internalId());
    w->setTitle(caption());
    w->setActive(isActive());
    w->setFullscreen(isFullScreen());
    w->setKeepAbove(keepAbove());
    w->setKeepBelow(keepBelow());
    w->setMaximized(maximizeMode() == KWin::MaximizeFull);
    w->setMinimized(isMinimized());
    w->setDemandsAttention(isDemandingAttention());
    w->setCloseable(isCloseable());
    w->setMaximizeable(isMaximizable());
    w->setMinimizeable(isMinimizable());
    w->setFullscreenable(isFullScreenable());
    w->setApplicationMenuPaths(applicationMenuServiceName(), applicationMenuObjectPath());
    w->setIcon(icon());
    auto updateAppId = [this, w] {
        w->setResourceName(resourceName());
        w->setAppId(m_desktopFileName.isEmpty() ? resourceClass() : m_desktopFileName);
    };
    updateAppId();
    w->setSkipTaskbar(skipTaskbar());
    w->setSkipSwitcher(skipSwitcher());
    w->setPid(pid());
    w->setResizable(isResizable());
    w->setMovable(isMovable());
    w->setVirtualDesktopChangeable(true); // FIXME Matches X11Window::actionSupported(), but both should be implemented.
    w->setNoBorder(noBorder());
    w->setCanSetNoBorder(userCanSetNoBorder());
    w->setExcludeFromCapture(excludeFromCapture());
    w->setParentWindow(transientFor() ? transientFor()->windowManagementInterface() : nullptr);
    w->setGeometry(frameGeometry().toRect());
    w->setClientGeometry(clientGeometry().toRect());
    connect(this, &Window::skipTaskbarChanged, w, [w, this]() {
        w->setSkipTaskbar(skipTaskbar());
    });
    connect(this, &Window::skipSwitcherChanged, w, [w, this]() {
        w->setSkipSwitcher(skipSwitcher());
    });
    connect(this, &Window::captionChanged, w, [w, this] {
        w->setTitle(caption());
    });

    connect(this, &Window::activeChanged, w, [w, this] {
        w->setActive(isActive());
    });
    connect(this, &Window::fullScreenChanged, w, [w, this] {
        w->setFullscreen(isFullScreen());
    });
    connect(this, &Window::keepAboveChanged, w, &PlasmaWindowInterface::setKeepAbove);
    connect(this, &Window::keepBelowChanged, w, &PlasmaWindowInterface::setKeepBelow);
    connect(this, &Window::minimizedChanged, w, [w, this] {
        w->setMinimized(isMinimized());
    });
    connect(this, &Window::maximizedChanged, w, [w, this]() {
        w->setMaximized(maximizeMode() == MaximizeFull);
    });
    connect(this, &Window::demandsAttentionChanged, w, [w, this] {
        w->setDemandsAttention(isDemandingAttention());
    });
    connect(this, &Window::iconChanged, w, [w, this]() {
        w->setIcon(icon());
    });
    connect(this, &Window::windowClassChanged, w, updateAppId);
    connect(this, &Window::desktopFileNameChanged, w, updateAppId);
    connect(this, &Window::decorationPolicyChanged, w, [w, this] {
        w->setNoBorder(noBorder());
    });
    connect(this, &Window::excludeFromCaptureChanged, w, [w, this] {
        w->setExcludeFromCapture(excludeFromCapture());
    });
    connect(this, &Window::transientChanged, w, [w, this]() {
        w->setParentWindow(transientFor() ? transientFor()->windowManagementInterface() : nullptr);
    });
    connect(this, &Window::frameGeometryChanged, w, [w, this]() {
        w->setGeometry(frameGeometry().toRect());
    });
    connect(this, &Window::clientGeometryChanged, w, [w, this]() {
        w->setClientGeometry(clientGeometry().toRect());
    });
    connect(this, &Window::applicationMenuChanged, w, [w, this]() {
        w->setApplicationMenuPaths(applicationMenuServiceName(), applicationMenuObjectPath());
    });
    connect(w, &PlasmaWindowInterface::closeRequested, this, [this] {
        closeWindow();
    });
    connect(w, &PlasmaWindowInterface::moveRequested, this, [this]() {
        input()->pointer()->warp(frameGeometry().center());
        performMousePressCommand(Options::MouseMove, Cursors::self()->mouse()->pos());
    });
    connect(w, &PlasmaWindowInterface::resizeRequested, this, [this]() {
        input()->pointer()->warp(frameGeometry().bottomRight());
        performMousePressCommand(Options::MouseResize, Cursors::self()->mouse()->pos());
    });
    connect(w, &PlasmaWindowInterface::fullscreenRequested, this, [this](bool set) {
        setFullScreen(set);
    });
    connect(w, &PlasmaWindowInterface::minimizedRequested, this, [this](bool set) {
        setMinimized(set);
    });
    connect(w, &PlasmaWindowInterface::maximizedRequested, this, [this](bool set) {
        maximize(set ? MaximizeFull : MaximizeRestore);
    });
    connect(w, &PlasmaWindowInterface::keepAboveRequested, this, [this](bool set) {
        setKeepAbove(set);
    });
    connect(w, &PlasmaWindowInterface::keepBelowRequested, this, [this](bool set) {
        setKeepBelow(set);
    });
    connect(w, &PlasmaWindowInterface::demandsAttentionRequested, this, [this](bool set) {
        demandAttention(set);
    });
    connect(w, &PlasmaWindowInterface::activeRequested, this, [this](bool set) {
        if (set) {
            workspace()->activateWindow(this, true);
        }
    });
    connect(w, &PlasmaWindowInterface::noBorderRequested, this, [this](bool set) {
        setNoBorder(set);
    });
    connect(w, &PlasmaWindowInterface::excludeFromCaptureRequested, this, [this](bool set) {
        setExcludeFromCapture(set);
    });

    for (const auto vd : std::as_const(m_desktops)) {
        w->addPlasmaVirtualDesktop(vd->id());
    }
    // We need to set `OnAllDesktops` after the actual VD list has been added.
    // Otherwise it will unconditionally add the current desktop to the interface
    // which may not be the case, for example, when using rules
    w->setOnAllDesktops(isOnAllDesktops());

    // Plasma Virtual desktop management
    // show/hide when the window enters/exits from desktop
    connect(w, &PlasmaWindowInterface::enterPlasmaVirtualDesktopRequested, this, [this](const QString &desktopId) {
        VirtualDesktop *vd = VirtualDesktopManager::self()->desktopForId(desktopId);
        if (vd) {
            Workspace::self()->addWindowToDesktop(this, vd);
        }
    });
    connect(w, &PlasmaWindowInterface::enterNewPlasmaVirtualDesktopRequested, this, [this]() {
        VirtualDesktopManager::self()->setCount(VirtualDesktopManager::self()->count() + 1);
        auto vd = VirtualDesktopManager::self()->desktops().last();
        Workspace::self()->addWindowToDesktop(this, vd);
    });
    connect(w, &PlasmaWindowInterface::leavePlasmaVirtualDesktopRequested, this, [this](const QString &desktopId) {
        VirtualDesktop *vd = VirtualDesktopManager::self()->desktopForId(desktopId);
        if (vd) {
            Workspace::self()->removeWindowFromDesktop(this, vd);
        }
    });

    for (const auto &activity : std::as_const(m_activityList)) {
        w->addPlasmaActivity(activity);
    }

    connect(this, &Window::activitiesChanged, w, [w, this] {
        const auto newActivities = QSet<QString>(m_activityList.begin(), m_activityList.end());
        const auto oldActivitiesList = w->plasmaActivities();
        const auto oldActivities = QSet<QString>(oldActivitiesList.begin(), oldActivitiesList.end());

        const auto activitiesToAdd = newActivities - oldActivities;
        for (const auto &activity : activitiesToAdd) {
            w->addPlasmaActivity(activity);
        }

        const auto activitiesToRemove = oldActivities - newActivities;
        for (const auto &activity : activitiesToRemove) {
            w->removePlasmaActivity(activity);
        }
    });

    // Plasma Activities management
    // show/hide when the window enters/exits activity
    connect(w, &PlasmaWindowInterface::enterPlasmaActivityRequested, this, [this](const QString &activityId) {
        setOnActivity(activityId, true);
    });
    connect(w, &PlasmaWindowInterface::leavePlasmaActivityRequested, this, [this](const QString &activityId) {
        setOnActivity(activityId, false);
    });
    connect(w, &PlasmaWindowInterface::sendToOutput, this, [this](OutputInterface *output) {
        sendToOutput(output->handle());
    });

    if (readyForPainting()) {
        w->map();
    } else {
        connect(this, &Window::readyForPaintingChanged, w, &PlasmaWindowInterface::map, Qt::SingleShotConnection);
    }

    m_windowManagementInterface = w;
}

void Window::destroyWindowManagementInterface()
{
    delete m_windowManagementInterface;
    m_windowManagementInterface = nullptr;
}

std::optional<Options::MouseCommand> Window::getMousePressCommand(Qt::MouseButton button) const
{
    if (button == Qt::NoButton) {
        return std::nullopt;
    }
    if (isActive()) {
        if (options->isClickRaise() && !isMostRecentlyRaised()) {
            return Options::MouseActivateRaiseAndPassClick;
        }
    } else {
        switch (button) {
        case Qt::LeftButton:
            return options->commandWindow1();
        case Qt::MiddleButton:
            return options->commandWindow2();
        case Qt::RightButton:
            return options->commandWindow3();
        default:
            // all other buttons pass Activate & Pass Client
            return Options::MouseActivateAndPassClick;
        }
    }
    return std::nullopt;
}

std::optional<Options::MouseCommand> Window::getMouseReleaseCommand(Qt::MouseButton button) const
{
    switch (button) {
    case Qt::LeftButton:
        return options->commandWindow1();
    case Qt::MiddleButton:
        return options->commandWindow2();
    case Qt::RightButton:
        return options->commandWindow3();
    default:
        return std::nullopt;
    }
}

std::optional<Options::MouseCommand> Window::getWheelCommand(Qt::Orientation orientation) const
{
    if (orientation != Qt::Vertical || isActive()) {
        return std::nullopt;
    }
    return options->commandWindowWheel();
}

bool Window::mousePressCommandConsumesEvent(Options::MouseCommand command) const
{
    switch (command) {
    case Options::MouseRaise:
    case Options::MouseLower:
    case Options::MouseOperationsMenu:
    case Options::MouseToggleRaiseAndLower:
    case Options::MouseMaximize:
    case Options::MouseRestore:
    case Options::MouseMinimize:
    case Options::MouseAbove:
    case Options::MouseBelow:
    case Options::MousePreviousDesktop:
    case Options::MouseNextDesktop:
    case Options::MouseOpacityMore:
    case Options::MouseOpacityLess:
    case Options::MouseClose:
    case Options::MouseResize:
    case Options::MouseUnrestrictedResize:
    case Options::MouseActivateRaiseAndResize:
    case Options::MouseActivateRaiseAndUnrestrictedResize:
        return true;
    case Options::MouseActivateRaiseAndPassClick:
    case Options::MouseActivateRaiseOnReleaseAndPassClick:
    case Options::MouseActivateAndPassClick:
    case Options::MouseNothing:
        return false;
    case Options::MouseActivateAndLower:
    case Options::MouseActivateAndRaise:
    case Options::MouseActivate:
        return !isActive() && rules()->checkAcceptFocus(acceptsFocus());
    case Options::MouseActivateRaiseAndMove:
    case Options::MouseActivateRaiseAndUnrestrictedMove:
    case Options::MouseMove:
    case Options::MouseUnrestrictedMove:
        return isMovableAcrossScreens();
    }
    return false;
}

bool Window::performMousePressCommand(Options::MouseCommand cmd, const QPointF &globalPos)
{
    // NOTE that this has to be checked before running the command
    // as raising the window may change the return value
    const bool consumes = mousePressCommandConsumesEvent(cmd);
    switch (cmd) {
    case Options::MouseRaise:
        workspace()->raiseWindow(this);
        break;
    case Options::MouseLower: {
        workspace()->lowerWindow(this);
        // used to be activateNextWindow(this), then topWindowOnDesktop
        // since this is a mouseOp it's however safe to use the window under the mouse instead
        if (isActive() && options->focusPolicyIsReasonable()) {
            Window *next = workspace()->windowUnderMouse(output());
            if (next && next != this) {
                workspace()->requestFocus(next, false);
            }
        }
        break;
    }
    case Options::MouseOperationsMenu:
        if (isActive() && options->isClickRaise()) {
            autoRaise();
        }
        workspace()->showWindowMenu(Rect(globalPos.toPoint(), globalPos.toPoint()), this);
        break;
    case Options::MouseToggleRaiseAndLower:
        workspace()->raiseOrLowerWindow(this);
        break;
    case Options::MouseActivateAndRaise: {
        workspace()->raiseWindow(this);
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        break;
    }
    case Options::MouseActivateAndLower:
        workspace()->requestFocus(this);
        workspace()->lowerWindow(this);
        workspace()->setActiveOutput(globalPos);
        break;
    case Options::MouseActivate:
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        break;
    case Options::MouseActivateRaiseAndPassClick:
        workspace()->raiseWindow(this);
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        break;
    case Options::MouseActivateRaiseOnReleaseAndPassClick:
        m_delayedRaise = true;
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        break;
    case Options::MouseActivateAndPassClick:
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        break;
    case Options::MouseMaximize:
        maximize(MaximizeFull);
        break;
    case Options::MouseRestore:
        maximize(MaximizeRestore);
        break;
    case Options::MouseMinimize:
        setMinimized(true);
        break;
    case Options::MouseAbove: {
        StackingUpdatesBlocker blocker(workspace());
        if (keepBelow()) {
            setKeepBelow(false);
        } else {
            setKeepAbove(true);
        }
        break;
    }
    case Options::MouseBelow: {
        StackingUpdatesBlocker blocker(workspace());
        if (keepAbove()) {
            setKeepAbove(false);
        } else {
            setKeepBelow(true);
        }
        break;
    }
    case Options::MousePreviousDesktop:
        workspace()->windowToPreviousDesktop(this);
        break;
    case Options::MouseNextDesktop:
        workspace()->windowToNextDesktop(this);
        break;
    case Options::MouseOpacityMore:
        if (!isDesktop()) { // No point in changing the opacity of the desktop
            setOpacity(std::min(opacity() + 0.1, 1.0));
        }
        break;
    case Options::MouseOpacityLess:
        if (!isDesktop()) { // No point in changing the opacity of the desktop
            setOpacity(std::max(opacity() - 0.1, 0.1));
        }
        break;
    case Options::MouseClose:
        closeWindow();
        break;
    case Options::MouseActivateRaiseAndMove:
    case Options::MouseActivateRaiseAndUnrestrictedMove:
        workspace()->raiseWindow(this);
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        [[fallthrough]];
    case Options::MouseMove:
    case Options::MouseUnrestrictedMove: {
        if (!isMovableAcrossScreens()) {
            break;
        }
        if (isInteractiveMoveResize()) {
            finishInteractiveMoveResize(false);
        }
        setInteractiveMoveResizeGravity(Gravity::Center);
        setInteractiveMoveResizePointerButtonDown(true);
        setInteractiveMoveResizeAnchor(globalPos);
        setInteractiveMoveResizeModifiers(Qt::KeyboardModifiers());
        setInteractiveMoveOffset(QPointF(qreal(globalPos.x() - x()) / width(), qreal(globalPos.y() - y()) / height())); // map from global
        setUnrestrictedInteractiveMoveResize((cmd == Options::MouseActivateRaiseAndUnrestrictedMove
                                              || cmd == Options::MouseUnrestrictedMove));
        if (!startInteractiveMoveResize()) {
            setInteractiveMoveResizePointerButtonDown(false);
        }
        updateInteractiveMoveResizeCursor();
        break;
    }
    case Options::MouseActivateRaiseAndResize:
    case Options::MouseActivateRaiseAndUnrestrictedResize:
        workspace()->raiseWindow(this);
        workspace()->requestFocus(this);
        workspace()->setActiveOutput(globalPos);
        [[fallthrough]];
    case Options::MouseResize:
    case Options::MouseUnrestrictedResize: {
        if (!isResizable()) {
            break;
        }
        if (isInteractiveMoveResize()) {
            finishInteractiveMoveResize(false);
        }
        setInteractiveMoveResizePointerButtonDown(true);
        setInteractiveMoveResizeAnchor(globalPos);
        setInteractiveMoveResizeModifiers(Qt::KeyboardModifiers());
        const QPointF moveOffset = QPointF(globalPos.x() - x(), globalPos.y() - y()); // map from global
        setInteractiveMoveOffset(QPointF(moveOffset.x() / width(), moveOffset.y() / height()));
        int x = moveOffset.x(), y = moveOffset.y();
        bool left = x < width() / 3;
        bool right = x >= 2 * width() / 3;
        bool top = y < height() / 3;
        bool bot = y >= 2 * height() / 3;
        Gravity gravity;
        if (top) {
            gravity = left ? Gravity::TopLeft : (right ? Gravity::TopRight : Gravity::Top);
        } else if (bot) {
            gravity = left ? Gravity::BottomLeft : (right ? Gravity::BottomRight : Gravity::Bottom);
        } else {
            gravity = (x < width() / 2) ? Gravity::Left : Gravity::Right;
        }
        setInteractiveMoveResizeGravity(gravity);
        setUnrestrictedInteractiveMoveResize((cmd == Options::MouseUnrestrictedResize));
        if (!startInteractiveMoveResize()) {
            setInteractiveMoveResizePointerButtonDown(false);
        }
        updateInteractiveMoveResizeCursor();
        break;
    }
    case Options::MouseNothing:
        break;
    }
    return consumes;
}

void Window::performMouseReleaseCommand(Options::MouseCommand command, const QPointF &globalPos)
{
    if (command != Options::MouseActivateRaiseOnReleaseAndPassClick) {
        return;
    }
    performDelayedRaise();
    workspace()->setActiveOutput(globalPos);
}

void Window::setTransientFor(Window *transientFor)
{
    if (transientFor == this) {
        // cannot be transient for one self
        return;
    }
    if (m_transientFor == transientFor) {
        return;
    }
    m_transientFor = transientFor;
    if (m_transientFor) {
        setExcludeFromCapture(m_transientFor->excludeFromCapture());
    }
    Q_EMIT transientChanged();
}

const Window *Window::transientFor() const
{
    return m_transientFor;
}

Window *Window::transientFor()
{
    return m_transientFor;
}

bool Window::hasTransient(const Window *c, bool indirect) const
{
    return c->transientFor() == this;
}

QList<Window *> Window::mainWindows() const
{
    if (const Window *t = transientFor()) {
        return QList<Window *>{const_cast<Window *>(t)};
    }
    return QList<Window *>();
}

QList<Window *> Window::allMainWindows() const
{
    auto result = mainWindows();
    const auto copy = result; // we need a copy to ensure we don't assign into the container we loop over
    for (const auto *window : copy) {
        result += window->allMainWindows();
    }
    return result;
}

void Window::setModal(bool m)
{
    // Qt-3.2 can have even modal normal windows :(
    if (m_modal == m) {
        return;
    }
    m_modal = m;
    doSetModal();
    Q_EMIT modalChanged();
    // Changing modality for a mapped window is weird (?)
    // _NET_WM_STATE_MODAL should possibly rather be _NET_WM_WINDOW_TYPE_MODAL_DIALOG
}

bool Window::isModal() const
{
    return m_modal;
}

Window *Window::findModal() const
{
    for (Window *transient : m_transients) {
        if (transient->isDeleted()) {
            continue;
        }
        if (transient->isModal()) {
            if (Window *modal = transient->findModal()) {
                return modal;
            }
            return transient;
        }
        if (Window *modal = transient->findModal()) {
            return modal;
        }
    }
    return nullptr;
}

bool Window::isTransient() const
{
    return false;
}

// check whether a transient should be actually kept above its mainwindow
// there may be some special cases where this rule shouldn't be enforced
static bool shouldKeepTransientAbove(const Window *parent, const Window *transient)
{
    // #93832 - don't keep splashscreens above dialogs
    if (transient->isSplash() && parent->isDialog()) {
        return false;
    }
    // This is rather a hack for #76026. Don't keep non-modal dialogs above
    // the mainwindow, but only if they're group transient (since only such dialogs
    // have taskbar entry in Kicker). A proper way of doing this (both kwin and kicker)
    // needs to be found.
    if (transient->isDialog() && !transient->isModal() && transient->groupTransient()) {
        return false;
    }
    return true;
}

void Window::addTransient(Window *cl)
{
    Q_ASSERT(!m_transients.contains(cl));
    Q_ASSERT(cl != this);
    m_transients.append(cl);
    if (shouldKeepTransientAbove(this, cl)) {
        workspace()->constrain(this, cl);
    }
}

void Window::removeTransient(Window *cl)
{
    m_transients.removeAll(cl);
    if (cl->transientFor() == this) {
        cl->setTransientFor(nullptr);
    }
    workspace()->unconstrain(this, cl);
}

void Window::removeTransientFromList(Window *cl)
{
    m_transients.removeAll(cl);
}

bool Window::isActiveFullScreen() const
{
    if (!isFullScreen()) {
        return false;
    }

    const auto ac = workspace()->activeWindow();
    // according to NETWM spec implementation notes suggests
    // "focused windows having state _NET_WM_STATE_FULLSCREEN" to be on the highest layer.
    // we'll also take the screen into account
    return ac && (ac == this || !ac->isOnOutput(output()) || ac->allMainWindows().contains(const_cast<Window *>(this)));
}

void Window::performDelayedRaise()
{
    if (!m_delayedRaise) {
        return;
    }

    m_delayedRaise = false;

    if (isActive()) {
        workspace()->raiseWindow(this);
    } else if (Window *modal = findModal(); modal && modal->isActive()) {
        workspace()->raiseWindow(this);
    }
}

qreal Window::borderBottom() const
{
    return isDecorated() ? decoration()->borderBottom() : 0;
}

qreal Window::borderLeft() const
{
    return isDecorated() ? decoration()->borderLeft() : 0;
}

qreal Window::borderRight() const
{
    return isDecorated() ? decoration()->borderRight() : 0;
}

qreal Window::borderTop() const
{
    return isDecorated() ? decoration()->borderTop() : 0;
}

void Window::updateInteractiveMoveResizeCursor()
{
    if (isDeleted()) {
        return;
    }
    Gravity gravity = interactiveMoveResizeGravity();
    if (!isResizable()) {
        gravity = Gravity::Center;
    }
    CursorShape c = Qt::ArrowCursor;
    switch (gravity) {
    case Gravity::TopLeft:
        c = KWin::ExtendedCursor::SizeNorthWest;
        break;
    case Gravity::BottomRight:
        c = KWin::ExtendedCursor::SizeSouthEast;
        break;
    case Gravity::BottomLeft:
        c = KWin::ExtendedCursor::SizeSouthWest;
        break;
    case Gravity::TopRight:
        c = KWin::ExtendedCursor::SizeNorthEast;
        break;
    case Gravity::Top:
        c = KWin::ExtendedCursor::SizeNorth;
        break;
    case Gravity::Bottom:
        c = KWin::ExtendedCursor::SizeSouth;
        break;
    case Gravity::Left:
        c = KWin::ExtendedCursor::SizeWest;
        break;
    case Gravity::Right:
        c = KWin::ExtendedCursor::SizeEast;
        break;
    default:
        if (isInteractiveMoveResize()) {
            c = Qt::ClosedHandCursor;
        } else {
            c = Qt::ArrowCursor;
        }
        break;
    }
    if (c == m_interactiveMoveResize.cursor) {
        return;
    }
    m_interactiveMoveResize.cursor = c;
    Q_EMIT moveResizeCursorChanged(c);
}

void Window::leaveInteractiveMoveResize()
{
    disconnect(this, &Window::outputChanged, this, &Window::finishInteractiveOutputChange);
    workspace()->setMoveResizeWindow(nullptr);
    setInteractiveMoveResize(false);
    if (workspace()->screenEdges()->isDesktopSwitchingMovingClients()) {
        workspace()->screenEdges()->reserveDesktopSwitching(false, Qt::Vertical | Qt::Horizontal);
    }
    if (isElectricBorderMaximizing()) {
        workspace()->outline()->hide();
    }
}

bool Window::doStartInteractiveMoveResize()
{
    return true;
}

void Window::doFinishInteractiveMoveResize()
{
}

bool Window::isWaitingForInteractiveResizeSync() const
{
    return false;
}

void Window::doInteractiveResizeSync(const RectF &rect)
{
    moveResize(rect);
}

void Window::checkQuickTilingMaximizationZones(int xroot, int yroot)
{
    std::optional<ElectricBorderMode> mode = std::nullopt;
    bool innerBorder = false;

    const auto outputs = workspace()->outputs();
    for (const LogicalOutput *output : outputs) {
        if (!output->geometry().contains(QPoint(xroot, yroot))) {
            continue;
        }

        auto isInScreen = [&output, &outputs](const QPoint &pt) {
            for (const LogicalOutput *other : outputs) {
                if (other == output) {
                    continue;
                }
                if (other->geometry().contains(pt)) {
                    return true;
                }
            }
            return false;
        };

        RectF area = workspace()->clientArea(MaximizeArea, this, QPointF(xroot, yroot));
        QuickTileMode tile = QuickTileFlag::None;
        if (options->electricBorderTiling()) {
            if (xroot <= area.x() + 20) {
                tile |= QuickTileFlag::Left;
                innerBorder = isInScreen(QPoint(area.x() - 1, yroot));
            } else if (xroot >= area.x() + area.width() - 20) {
                tile |= QuickTileFlag::Right;
                innerBorder = isInScreen(QPoint(area.right() + 1, yroot));
            }
        }

        if (tile != QuickTileMode(QuickTileFlag::None)) {
            if (yroot <= area.y() + area.height() * options->electricBorderCornerRatio()) {
                tile |= QuickTileFlag::Top;
            } else if (yroot >= area.y() + area.height() - area.height() * options->electricBorderCornerRatio()) {
                tile |= QuickTileFlag::Bottom;
            }
            mode = tile;
        } else if (options->electricBorderMaximize() && yroot <= area.y() + 5 && isMaximizable()) {
            mode = MaximizeFull;
            innerBorder = isInScreen(QPoint(xroot, area.y() - 1));
        }
        break; // no point in checking other screens to contain this... "point"...
    }
    if (mode != electricBorderMode()) {
        setElectricBorderMode(mode);
        if (innerBorder) {
            if (!m_electricMaximizingDelay) {
                m_electricMaximizingDelay = new QTimer(this);
                m_electricMaximizingDelay->setInterval(250);
                m_electricMaximizingDelay->setSingleShot(true);
                connect(m_electricMaximizingDelay, &QTimer::timeout, this, [this]() {
                    if (isInteractiveMove()) {
                        setElectricBorderMaximizing(electricBorderMode().has_value());
                    }
                });
            }
            m_electricMaximizingDelay->start();
        } else {
            setElectricBorderMaximizing(mode.has_value());
        }
    }
}

void Window::resetQuickTilingMaximizationZones()
{
    if (electricBorderMode().has_value()) {
        if (m_electricMaximizingDelay) {
            m_electricMaximizingDelay->stop();
        }
        setElectricBorderMaximizing(false);
        setElectricBorderMode(std::nullopt);
    }
}

void Window::keyPressEvent(QKeyCombination key_combination)
{
    if (!isInteractiveMove() && !isInteractiveResize()) {
        return;
    }
    bool is_control = key_combination.keyboardModifiers() & Qt::CTRL;
    bool is_alt = key_combination.keyboardModifiers() & Qt::ALT;
    int delta = is_control ? 1 : is_alt ? 32
                                        : 8;
    QPointF pos = interactiveMoveResizeAnchor();
    switch (key_combination.key()) {
    case Qt::Key_Left:
        pos.rx() -= delta;
        break;
    case Qt::Key_Right:
        pos.rx() += delta;
        break;
    case Qt::Key_Up:
        pos.ry() -= delta;
        break;
    case Qt::Key_Down:
        pos.ry() += delta;
        break;
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        setInteractiveMoveResizePointerButtonDown(false);
        finishInteractiveMoveResize(false);
        updateInteractiveMoveResizeCursor();
        break;
    case Qt::Key_Escape:
        setInteractiveMoveResizePointerButtonDown(false);
        finishInteractiveMoveResize(true);
        updateInteractiveMoveResizeCursor();
        break;
    default:
        return;
    }
    input()->pointer()->warp(pos);
}

QSizeF Window::resizeIncrements() const
{
    return QSizeF(1, 1);
}

void Window::dontInteractiveMoveResize()
{
    setInteractiveMoveResizePointerButtonDown(false);
    stopDelayedInteractiveMoveResize();
    if (isInteractiveMoveResize()) {
        finishInteractiveMoveResize(false);
    }
}

Gravity Window::mouseGravity() const
{
    if (isDecorated()) {
        switch (decoration()->sectionUnderMouse()) {
        case Qt::BottomLeftSection:
            return Gravity::BottomLeft;
        case Qt::BottomRightSection:
            return Gravity::BottomRight;
        case Qt::BottomSection:
            return Gravity::Bottom;
        case Qt::LeftSection:
            return Gravity::Left;
        case Qt::RightSection:
            return Gravity::Right;
        case Qt::TopSection:
            return Gravity::Top;
        case Qt::TopLeftSection:
            return Gravity::TopLeft;
        case Qt::TopRightSection:
            return Gravity::TopRight;
        default:
            return Gravity::Center;
        }
    }
    return Gravity::Center;
}

void Window::endInteractiveMoveResize()
{
    setInteractiveMoveResizePointerButtonDown(false);
    stopDelayedInteractiveMoveResize();
    if (isInteractiveMoveResize()) {
        finishInteractiveMoveResize(false);
        setInteractiveMoveResizeGravity(mouseGravity());
    }
    updateInteractiveMoveResizeCursor();
}

void Window::cancelInteractiveMoveResize()
{
    setInteractiveMoveResizePointerButtonDown(false);
    stopDelayedInteractiveMoveResize();
    if (isInteractiveMoveResize()) {
        finishInteractiveMoveResize(true);
        setInteractiveMoveResizeGravity(mouseGravity());
    }
    updateInteractiveMoveResizeCursor();
}

void Window::setDecoration(std::shared_ptr<KDecoration3::Decoration> decoration)
{
    installDecoration(std::move(decoration), false);
}

bool Window::installDecoration(std::shared_ptr<KDecoration3::Decoration> decoration, bool guarded, quint64 *installedGeneration)
{
    if (m_decoration.decoration == decoration) {
        if (installedGeneration) {
            *installedGeneration = m_decorationGeneration;
        }
        return true;
    }
    if (decoration) {
        connect(decoration.get(), &KDecoration3::Decoration::shadowChanged, this, [this]() {
            if (!isDeleted()) {
                updateShadow();
            }
        });
        connect(decoration.get(), &KDecoration3::Decoration::bordersChanged, this, [this]() {
            if (!isDeleted()) {
                updateDecorationInputShape();
            }
        });
        connect(decoration.get(), &KDecoration3::Decoration::resizeOnlyBordersChanged, this, [this]() {
            if (!isDeleted()) {
                updateDecorationInputShape();
            }
        });
        connect(decoratedWindow()->decoratedWindow(), &KDecoration3::DecoratedWindow::sizeChanged, this, [this]() {
            if (!isDeleted()) {
                updateDecorationInputShape();
            }
        });
        connect(decoration.get(), &KDecoration3::Decoration::borderRadiusChanged, this, [this]() {
            if (!isDeleted()) {
                updateDecorationBorderRadius();
            }
        });
    }
    // The new pointer and the generation are installed before any observer runs (the border-radius
    // and input-shape updates, the displaced decoration's release, the notification); the displaced
    // decoration outlives the installation so its release sees the new state. A nested replacement
    // advances again, and nothing here writes the generation afterwards. The window outlives the
    // installation too (a handler may close it and drop its last reference), and the displaced
    // decoration, which the window parents, is released before that retention ends, in this order:
    // the window's destruction would otherwise delete it first. The incoming ownership is moved
    // into the member for the same reason: a copy kept by this parameter would outlive the
    // retention.
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const std::shared_ptr<KDecoration3::Decoration> displaced = std::move(m_decoration.decoration);
    m_decoration.decoration = std::move(decoration);
    advanceDecorationGeneration();
    const quint64 installed = m_decorationGeneration;
    if (installedGeneration) {
        *installedGeneration = installed;
    }
    updateDecorationInputShape();
    updateDecorationBorderRadius();
    if (guarded && (isDeleted() || m_decorationGeneration != installed)) {
        // Closed or replaced inside the border-radius notification: the newer installation
        // announced its own state, this one announces nothing more.
        return false;
    }
    Q_EMIT decorationChanged();
    return !guarded || (!isDeleted() && m_decorationGeneration == installed);
}

void Window::advanceDecorationGeneration()
{
    if (m_decorationGeneration != std::numeric_limits<quint64>::max()) {
        ++m_decorationGeneration;
    }
}

void Window::updateDecorationInputShape()
{
    if (!isDecorated()) {
        m_decoration.inputRegion = RegionF();
        return;
    }

    const QMarginsF borders = decoration()->borders();
    const QMarginsF resizeBorders = decoration()->resizeOnlyBorders();

    const RectF innerRect = RectF(QPointF(borderLeft(), borderTop()), decoratedWindow()->size());
    const RectF outerRect = innerRect + borders + resizeBorders;

    m_decoration.inputRegion = RegionF(outerRect) - innerRect;
}

void Window::updateDecorationBorderRadius()
{
    if (!isDecorated()) {
        setBorderRadius(BorderRadius());
    } else {
        setBorderRadius(BorderRadius::from(decoration()->borderRadius()));
    }
}

bool Window::decorationHasAlpha() const
{
    if (!isDecorated() || decoration()->isOpaque()) {
        // either no decoration or decoration has alpha disabled
        return false;
    }
    return true;
}

void Window::triggerDecorationRepaint()
{
    if (isDecorated()) {
        decoration()->update();
    }
}

void Window::layoutDecorationRects(RectF &left, RectF &top, RectF &right, RectF &bottom) const
{
    if (!isDecorated()) {
        return;
    }
    RectF r = decoration()->rect();

    top = RectF(r.x(), r.y(), r.width(), borderTop());
    bottom = RectF(r.x(), r.y() + r.height() - borderBottom(),
                   r.width(), borderBottom());
    left = RectF(r.x(), r.y() + top.height(),
                 borderLeft(), r.height() - top.height() - bottom.height());
    right = RectF(r.x() + r.width() - borderRight(), r.y() + top.height(),
                  borderRight(), r.height() - top.height() - bottom.height());
}

void Window::processDecorationMove(const QPointF &localPos, const QPointF &globalPos)
{
    if (isInteractiveMoveResizePointerButtonDown()) {
        if (!isInteractiveMoveResize()) {
            const QPointF offset(interactiveMoveOffset().x() * width(), interactiveMoveOffset().y() * height());
            const QPointF delta(localPos - offset);
            if (delta.manhattanLength() >= QApplication::startDragDistance()) {
                if (startInteractiveMoveResize()) {
                    updateInteractiveMoveResize(globalPos, input()->keyboardModifiers());
                } else {
                    setInteractiveMoveResizePointerButtonDown(false);
                }
                updateInteractiveMoveResizeCursor();
            }
        }
        return;
    }

    // TODO: handle modifiers
    Gravity newGravity = mouseGravity();
    if (newGravity != interactiveMoveResizeGravity()) {
        setInteractiveMoveResizeGravity(newGravity);
        updateInteractiveMoveResizeCursor();
    }
}

bool Window::processDecorationButtonPress(const QPointF &localPos, const QPointF &globalPos, Qt::MouseButton button, bool ignoreMenu)
{
    Options::MouseCommand com = Options::MouseNothing;
    bool active = isActive();
    if (!wantsInput()) { // we cannot be active, use it anyway
        active = true;
    }

    // check whether it is a double click
    if (button == Qt::LeftButton) {
        if (m_decoration.doubleClickTimer.isValid()) {
            const qint64 interval = m_decoration.doubleClickTimer.elapsed();
            m_decoration.doubleClickTimer.invalidate();
            if (interval > QGuiApplication::styleHints()->mouseDoubleClickInterval()) {
                m_decoration.doubleClickTimer.start(); // expired -> new first click and pot. init
            } else {
                Options::WindowOperation operation;
                switch (decoration()->sectionUnderMouse()) {
                case Qt::TitleBarArea:
                    operation = options->operationTitlebarDblClick();
                    break;
                case Qt::LeftSection:
                case Qt::RightSection:
                    operation = options->doubleClickBorderToMaximize() ? Options::HMaximizeOp : Options::NoOp;
                    break;
                case Qt::TopSection:
                case Qt::BottomSection:
                    operation = options->doubleClickBorderToMaximize() ? Options::VMaximizeOp : Options::NoOp;
                    break;
                case Qt::TopLeftSection:
                case Qt::TopRightSection:
                case Qt::BottomLeftSection:
                case Qt::BottomRightSection:
                    operation = options->doubleClickBorderToMaximize() ? Options::MaximizeOp : Options::NoOp;
                    break;
                default:
                    operation = Options::NoOp;
                    break;
                }
                workspace()->performWindowOperation(this, operation);
                dontInteractiveMoveResize();
                return false;
            }
        } else {
            m_decoration.doubleClickTimer.start(); // new first click and pot. init, could be invalidated by release - see below
        }
    }

    if (button == Qt::LeftButton) {
        com = active ? options->commandActiveTitlebar1() : options->commandInactiveTitlebar1();
    } else if (button == Qt::MiddleButton) {
        com = active ? options->commandActiveTitlebar2() : options->commandInactiveTitlebar2();
    } else if (button == Qt::RightButton) {
        com = active ? options->commandActiveTitlebar3() : options->commandInactiveTitlebar3();
    }
    if (button == Qt::LeftButton
        && com != Options::MouseOperationsMenu // actions where it's not possible to get the matching
        && com != Options::MouseMinimize) // mouse release event
    {
        setInteractiveMoveResizeGravity(mouseGravity());
        setInteractiveMoveResizePointerButtonDown(true);
        setInteractiveMoveResizeAnchor(globalPos);
        setInteractiveMoveResizeModifiers(Qt::KeyboardModifiers());
        setInteractiveMoveOffset(QPointF(qreal(localPos.x()) / width(), qreal(localPos.y()) / height()));
        setUnrestrictedInteractiveMoveResize(false);
        startDelayedInteractiveMoveResize();
        updateInteractiveMoveResizeCursor();
    }
    // In the new API the decoration may process the menu action to display an inactive tab's menu.
    // If the event is unhandled then the core will create one for the active window in the group.
    if (!ignoreMenu || com != Options::MouseOperationsMenu) {
        performMousePressCommand(com, globalPos);
    }
    return !( // Return events that should be passed to the decoration in the new API
        com == Options::MouseRaise || com == Options::MouseOperationsMenu || com == Options::MouseActivateAndRaise || com == Options::MouseActivate || com == Options::MouseActivateRaiseAndPassClick || com == Options::MouseActivateAndPassClick || com == Options::MouseNothing);
}

void Window::processDecorationButtonRelease(Qt::MouseButton button)
{
    if (button == Qt::LeftButton) {
        setInteractiveMoveResizePointerButtonDown(false);
        stopDelayedInteractiveMoveResize();
        if (isInteractiveMoveResize()) {
            finishInteractiveMoveResize(false);
            setInteractiveMoveResizeGravity(mouseGravity());
        }
        updateInteractiveMoveResizeCursor();
    }
}

void Window::startDecorationDoubleClickTimer()
{
    m_decoration.doubleClickTimer.start();
}

void Window::invalidateDecorationDoubleClickTimer()
{
    m_decoration.doubleClickTimer.invalidate();
}

bool Window::providesContextHelp() const
{
    return false;
}

void Window::showContextHelp()
{
}

KDecoration3::Decoration *Window::nextDecoration() const
{
    return decoration();
}

Decoration::DecoratedWindowImpl *Window::decoratedWindow() const
{
    return m_decoration.client;
}

void Window::setDecoratedWindow(Decoration::DecoratedWindowImpl *client)
{
    m_decoration.client = client;
}

void Window::pointerEnterEvent(const QPointF &globalPos)
{
    if (options->focusPolicy() == Options::ClickToFocus || workspace()->userActionsMenu()->isShown()) {
        return;
    }

    if (options->isAutoRaise() && !isDesktop() && !isDock() && workspace()->focusChangeEnabled() && globalPos != workspace()->focusMousePosition() && workspace()->topWindowOnDesktop(VirtualDesktopManager::self()->currentDesktop(output()), options->isSeparateScreenFocus() ? output() : nullptr) != this) {
        startAutoRaise();
    }

    if (isDesktop() || isDock()) {
        return;
    }
    // for FocusFollowsMouse, change focus only if the mouse has actually been moved, not if the focus
    // change came because of window changes (e.g. closing a window) - #92290
    if (options->focusPolicy() != Options::FocusFollowsMouse
        || globalPos != workspace()->focusMousePosition()) {
        workspace()->requestDelayFocus(this);
    }
}

void Window::pointerLeaveEvent()
{
    cancelAutoRaise();
    workspace()->cancelDelayFocus();
    // TODO: send hover leave to deco
    // TODO: handle Options::FocusStrictlyUnderMouse
}

RectF Window::iconGeometry() const
{
    if (!windowManagementInterface()) {
        return RectF();
    }

    const QPointF center = m_frameGeometry.center();
    int minDistance = INT_MAX;
    Window *candidatePanel = nullptr;
    RectF candidateGeom;

    const auto minGeometries = windowManagementInterface()->minimizedGeometries();
    for (auto i = minGeometries.constBegin(), end = minGeometries.constEnd(); i != end; ++i) {
        Window *panel = waylandServer()->findWindow(i.key());
        if (!panel) {
            continue;
        }
        const int distance = QPointF(panel->m_frameGeometry.center() - center).manhattanLength();
        if (distance < minDistance) {
            minDistance = distance;
            candidatePanel = panel;
            candidateGeom = i.value();
        }
    }
    if (!candidatePanel) {
        // Check all mainwindows of this window.
        const auto windows = mainWindows();
        for (Window *mainWindow : windows) {
            const auto geom = mainWindow->iconGeometry();
            if (geom.isValid()) {
                return geom;
            }
        }
        return RectF();
    }
    return candidateGeom.translated(candidatePanel->pos());
}

RectF Window::virtualKeyboardGeometry() const
{
    return m_virtualKeyboardGeometry;
}

void Window::setVirtualKeyboardGeometry(const RectF &geo)
{
    // No keyboard anymore
    if (geo.isEmpty() && !m_keyboardGeometryRestore.isEmpty()) {
        const RectF availableArea = workspace()->clientArea(MaximizeArea, this);
        RectF newWindowGeometry = (requestedMaximizeMode() & MaximizeHorizontal) ? availableArea : m_keyboardGeometryRestore;
        moveResize(newWindowGeometry);
        m_keyboardGeometryRestore = RectF();
    } else if (geo.isEmpty()) {
        return;
        // The keyboard has just been opened (rather than resized) save window geometry for a restore
    } else if (m_keyboardGeometryRestore.isEmpty()) {
        m_keyboardGeometryRestore = moveResizeGeometry();
    }

    m_virtualKeyboardGeometry = geo;

    // Don't resize Desktop and fullscreen windows
    if (isRequestedFullScreen() || isDesktop()) {
        return;
    }

    // If we want to overlay the vkbd over windows, don't resize them
    if (options->overlayVirtualKeyboardOnWindows()) {
        return;
    }

    if (!geo.intersects(m_keyboardGeometryRestore)) {
        return;
    }

    const RectF availableArea = workspace()->clientArea(MaximizeArea, this);
    RectF newWindowGeometry = (requestedMaximizeMode() & MaximizeHorizontal) ? availableArea : m_keyboardGeometryRestore;
    newWindowGeometry.setHeight(std::min(newWindowGeometry.height(), geo.top() - availableArea.top()));
    newWindowGeometry.moveTop(std::max(geo.top() - newWindowGeometry.height(), availableArea.top()));
    newWindowGeometry = newWindowGeometry.intersected(availableArea);
    moveResize(newWindowGeometry);
}

RectF Window::keyboardGeometryRestore() const
{
    return m_keyboardGeometryRestore;
}

void Window::setKeyboardGeometryRestore(const RectF &geom)
{
    m_keyboardGeometryRestore = geom;
}

void Window::setDesktopFileName(const QString &name)
{
    const QString effectiveName = rules()->checkDesktopFile(name);
    if (effectiveName == m_desktopFileName) {
        return;
    }
    m_desktopFileName = effectiveName;
    updateWindowRules(Rules::DesktopFile);
    Q_EMIT desktopFileNameChanged();
}

QString Window::iconFromDesktopFile(const QString &desktopFileName)
{
    const QString absolutePath = findDesktopFile(desktopFileName);
    if (absolutePath.isEmpty()) {
        return {};
    }

    KDesktopFile df(absolutePath);
    return df.readIcon();
}

QString Window::iconFromDesktopFile() const
{
    return iconFromDesktopFile(m_desktopFileName);
}

QString Window::findDesktopFile(const QString &desktopFileName)
{
    if (desktopFileName.isEmpty()) {
        return {};
    }

    const QLatin1StringView suffix(".desktop");
    const QString desktopFileNameWithPrefix = desktopFileName + suffix;

    if (QDir::isAbsolutePath(desktopFileName)) {
        if (QFile::exists(desktopFileNameWithPrefix)) {
            return desktopFileNameWithPrefix;
        }

        if (desktopFileName.endsWith(suffix)) {
            if (QFile::exists(desktopFileName)) {
                return desktopFileName;
            }
        }

        return QString();
    }

    const QString filePath = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopFileNameWithPrefix);
    if (!filePath.isEmpty()) {
        return filePath;
    }

    if (desktopFileName.endsWith(suffix)) {
        return QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopFileName);
    }

    return QString();
}

bool Window::hasApplicationMenu() const
{
    return Workspace::self()->applicationMenu()->applicationMenuEnabled() && !m_applicationMenuServiceName.isEmpty() && !m_applicationMenuObjectPath.isEmpty();
}

void Window::updateApplicationMenuServiceName(const QString &serviceName)
{
    const bool old_hasApplicationMenu = hasApplicationMenu();

    m_applicationMenuServiceName = serviceName;

    const bool new_hasApplicationMenu = hasApplicationMenu();

    Q_EMIT applicationMenuChanged();
    if (old_hasApplicationMenu != new_hasApplicationMenu) {
        Q_EMIT hasApplicationMenuChanged(new_hasApplicationMenu);
    }
}

void Window::updateApplicationMenuObjectPath(const QString &objectPath)
{
    const bool old_hasApplicationMenu = hasApplicationMenu();

    m_applicationMenuObjectPath = objectPath;

    const bool new_hasApplicationMenu = hasApplicationMenu();

    Q_EMIT applicationMenuChanged();
    if (old_hasApplicationMenu != new_hasApplicationMenu) {
        Q_EMIT hasApplicationMenuChanged(new_hasApplicationMenu);
    }
}

void Window::setApplicationMenuActive(bool applicationMenuActive)
{
    if (m_applicationMenuActive != applicationMenuActive) {
        m_applicationMenuActive = applicationMenuActive;
        Q_EMIT applicationMenuActiveChanged(applicationMenuActive);
    }
}

void Window::showApplicationMenu(int actionId)
{
    if (isDecorated()) {
        decoration()->showApplicationMenu(actionId);
    } else {
        // we don't know where the application menu button will be, show it in the top left corner instead
        Workspace::self()->showApplicationMenu(Rect(), this, actionId);
    }
}

bool Window::unresponsive() const
{
    return m_unresponsive;
}

void Window::setUnresponsive(bool unresponsive)
{
    if (m_unresponsive != unresponsive) {
        m_unresponsive = unresponsive;
        Q_EMIT unresponsiveChanged(m_unresponsive);
        Q_EMIT captionChanged();
    }
}

QString Window::shortcutCaptionSuffix() const
{
    if (shortcut().isEmpty()) {
        return QString();
    }
    return QLatin1StringView(" {") + shortcut().toString() + QLatin1Char('}');
}

QString Window::caption() const
{
    QString cap = captionNormal() + captionSuffix();
    if (unresponsive()) {
        cap += QLatin1StringView(" ");
        cap += i18nc("Application is not responding, appended to window title", "(Not Responding)");
    }
    return cap;
}

/**
 * Returns the list of activities the window window is on.
 * if it's on all activities, the list will be empty.
 * Don't use this, use isOnActivity() and friends (from class Window)
 */
QStringList Window::activities() const
{
    return m_activityList;
}

bool Window::isOnCurrentActivity() const
{
#if KWIN_BUILD_ACTIVITIES
    if (!Workspace::self()->activities()) {
        return true;
    }
    return isOnActivity(Workspace::self()->activities()->current());
#else
    return true;
#endif
}

/**
 * Sets whether the window is on @p activity.
 * If you remove it from its last activity, then it's on all activities.
 *
 * Note: If it was on all activities and you try to remove it from one, nothing will happen;
 * I don't think that's an important enough use case to handle here.
 */
void Window::setOnActivity(const QString &activity, bool enable)
{
#if KWIN_BUILD_ACTIVITIES
    if (!Workspace::self()->activities()) {
        return;
    }
    QStringList newActivitiesList = activities();
    if (newActivitiesList.contains(activity) == enable) {
        // nothing to do
        return;
    }
    if (enable) {
        QStringList allActivities = Workspace::self()->activities()->all();
        if (!allActivities.contains(activity)) {
            // bogus ID
            return;
        }
        newActivitiesList.append(activity);
    } else {
        newActivitiesList.removeOne(activity);
    }
    setOnActivities(newActivitiesList);
#endif
}

/**
 * set exactly which activities this window is on
 */
void Window::setOnActivities(const QStringList &newActivitiesList)
{
#if KWIN_BUILD_ACTIVITIES
    if (!Workspace::self()->activities()) {
        return;
    }
    if (Workspace::self()->activities()->serviceStatus() != KActivities::Consumer::Running) {
        return;
    }

    QStringList effectiveActivities = rules()->checkActivity(newActivitiesList);

    const QStringList allActivities = Workspace::self()->activities()->all();
    effectiveActivities.removeIf([&allActivities](const QString &activity) {
        return !allActivities.contains(activity);
    });

    const bool allActivityExplicitlyRequested = effectiveActivities.isEmpty() || effectiveActivities.contains(Activities::nullUuid());
    const bool allActivitiesCovered = effectiveActivities.size() > 1 && effectiveActivities.size() == allActivities.size();
    if (allActivityExplicitlyRequested || allActivitiesCovered) {
        effectiveActivities = QStringList();
    }

    if (m_activityList != effectiveActivities) {
        invalidateTransientRestoreAttempt();
        m_activityList = effectiveActivities;
        doSetOnActivities(m_activityList);
        updateActivities(false);
    }
#endif
}

void Window::doSetOnActivities(const QStringList &activityList)
{
}

/**
 * if @p all is true, sets on all activities.
 * if it's false, sets it to only be on the current activity
 */
void Window::setOnAllActivities(bool all)
{
#if KWIN_BUILD_ACTIVITIES
    if (all == isOnAllActivities()) {
        return;
    }
    if (all) {
        setOnActivities(QStringList());
    } else {
        setOnActivity(Workspace::self()->activities()->current(), true);
    }
#endif
}

/**
 * update after activities changed
 */
void Window::updateActivities(bool includeTransients)
{
    if (m_activityUpdatesBlocked) {
        m_blockedActivityUpdatesRequireTransients |= includeTransients;
        return;
    }
    Q_EMIT activitiesChanged();
    m_blockedActivityUpdatesRequireTransients = false; // reset
    Workspace::self()->focusChain()->update(this, FocusChain::MakeFirst);
    updateWindowRules(Rules::Activity);
}

void Window::blockActivityUpdates(bool b)
{
    if (b) {
        ++m_activityUpdatesBlocked;
    } else {
        Q_ASSERT(m_activityUpdatesBlocked);
        --m_activityUpdatesBlocked;
        if (!m_activityUpdatesBlocked) {
            updateActivities(m_blockedActivityUpdatesRequireTransients);
        }
    }
}

bool Window::groupTransient() const
{
    return false;
}

const Group *Window::group() const
{
    return nullptr;
}

Group *Window::group()
{
    return nullptr;
}

QPointF Window::framePosToClientPos(const QPointF &point) const
{
    QMarginsF borders;
    if (decoration()) {
        borders = decoration()->currentState()->borders();
    }
    return point + QPointF(borders.left(), borders.top());
}

QPointF Window::nextFramePosToClientPos(const QPointF &point) const
{
    QMarginsF borders;
    if (auto decoration = nextDecoration()) {
        borders = decoration->nextState()->borders();
    }
    return point + QPointF(borders.left(), borders.top());
}

QPointF Window::clientPosToFramePos(const QPointF &point) const
{
    QMarginsF borders;
    if (decoration()) {
        borders = decoration()->currentState()->borders();
    }
    return point - QPointF(borders.left(), borders.top());
}

QPointF Window::nextClientPosToFramePos(const QPointF &point) const
{
    QMarginsF borders;
    if (auto decoration = nextDecoration()) {
        borders = decoration->nextState()->borders();
    }
    return point - QPointF(borders.left(), borders.top());
}

QSizeF Window::frameSizeToClientSize(const QSizeF &size) const
{
    QMarginsF borders;
    if (decoration()) {
        borders = decoration()->currentState()->borders();
    }
    return size.shrunkBy(borders);
}

QSizeF Window::nextFrameSizeToClientSize(const QSizeF &size) const
{
    QMarginsF borders;
    if (auto decoration = nextDecoration()) {
        borders = decoration->nextState()->borders();
    }
    return size.shrunkBy(borders);
}

QSizeF Window::clientSizeToFrameSize(const QSizeF &size) const
{
    QMarginsF borders;
    if (decoration()) {
        borders = decoration()->currentState()->borders();
    }
    return size.grownBy(borders);
}

QSizeF Window::nextClientSizeToFrameSize(const QSizeF &size) const
{
    QMarginsF borders;
    if (auto decoration = nextDecoration()) {
        borders = decoration->nextState()->borders();
    }
    return size.grownBy(borders);
}

RectF Window::frameRectToClientRect(const RectF &rect) const
{
    return RectF(framePosToClientPos(rect.topLeft()),
                 frameSizeToClientSize(rect.size()));
}

RectF Window::nextFrameRectToClientRect(const RectF &rect) const
{
    return RectF(nextFramePosToClientPos(rect.topLeft()),
                 nextFrameSizeToClientSize(rect.size()));
}

RectF Window::clientRectToFrameRect(const RectF &rect) const
{
    return RectF(clientPosToFramePos(rect.topLeft()),
                 clientSizeToFrameSize(rect.size()));
}

RectF Window::nextClientRectToFrameRect(const RectF &rect) const
{
    return RectF(nextClientPosToFramePos(rect.topLeft()),
                 nextClientSizeToFrameSize(rect.size()));
}

RectF Window::moveResizeGeometry() const
{
    return m_moveResizeGeometry;
}

void Window::setMoveResizeGeometry(const RectF &geo)
{
    setMoveResizeGeometryInternal(geo);
}

Window::AssignmentStepResult Window::setMoveResizeGeometryForAssignment(RectF desired, const TileAssignmentFence &before)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    AssignmentStepResult refused;
    if (!before.isCurrentFor(*this)) {
        refused.outcome = AssignmentStepOutcome::Superseded;
        return refused;
    }
    if (m_transientOwnership || m_activeTransientRequest) {
        return refused;
    }
    AssignmentGeometrySync sync;
    sync.result.outcome = AssignmentStepOutcome::Superseded;
    sync.result.checkpoint.reset(new TileAssignmentFence(*this));
    const quint64 writes = prepareAssignmentGeometrySync(desired, sync);
    if (!assignmentGeometrySyncIsCurrent(sync)) {
        return std::move(sync.result);
    }
    if (writes > std::numeric_limits<quint64>::max() - 1 - m_placementContinuationRevision) {
        return refused;
    }
    setMoveResizeGeometryInternal(desired, &sync);
    if (assignmentGeometrySyncIsCurrent(sync)) {
        sync.result.outcome = AssignmentStepOutcome::AppliedCurrent;
    }
    return std::move(sync.result);
}

quint64 Window::prepareAssignmentGeometrySync(const RectF &desired, AssignmentGeometrySync &sync) const
{
    // Freeze the target output properties before any owned write; the writers must not re-read
    // them after callbacks. This is a query only and advances no counter.
    sync.output = workspace()->outputAt(desired.center());
    sync.hadOutput = bool(sync.output);
    if (sync.output) {
        sync.scale = sync.output->scale();
        sync.transform = sync.output->transform();
        sync.color = sync.output->blendingColor();
    }
    const bool outputChanged = m_moveResizeOutput != sync.output;
    const bool scaleChanged = sync.output && m_nextTargetScale != sync.scale;
    const bool transformChanged = sync.output && m_preferredBufferTransform != sync.transform;
    const bool colorChanged = sync.output && m_preferredColorDescription != sync.color;
    sync.synchronize = outputChanged || m_outputPropertiesPending || scaleChanged || transformChanged || colorChanged;
    quint64 writes = (m_moveResizeGeometry != desired) + outputChanged;
    if (sync.output && sync.synchronize) {
        // Starting and finishing the obligation are distinct owned state transitions.
        writes += !m_outputPropertiesPending;
        writes += scaleChanged + transformChanged + colorChanged;
        ++writes;
    } else if (!sync.output) {
        writes += m_outputPropertiesPending;
    }
    return writes;
}

Window::AssignmentGeometryRequest Window::requestMoveResizeForAssignment(RectF desired, MoveResizeMode mode, const TileAssignmentFence &before)
{
    return requestMoveResizeGuarded(desired, mode, before, OwnedProvenance());
}

Window::AssignmentGeometryRequest Window::requestMoveResizeForAssignment(TransientTileRequest &request, const TileAssignmentFence &before)
{
    // The adoption target is a complete frame rectangle; it never inherits pending manual resize work.
    return requestMoveResizeGuarded(request.m_target, MoveResizeMode::MoveResize, before, OwnedProvenance::forRequest(&request));
}

Window::AssignmentGeometryRequest Window::requestMoveResizeForOwnership(RectF desired, MoveResizeMode mode, const TileAssignmentFence &before, quint64 generation, std::function<bool()> constraint)
{
    return requestMoveResizeGuarded(desired, mode, before, OwnedProvenance::forOwnership(generation, std::move(constraint)));
}

Window::AssignmentGeometryRequest Window::requestMoveResizeGuarded(RectF desired, MoveResizeMode mode, const TileAssignmentFence &before, OwnedProvenance owned)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    AssignmentGeometryRequest request;
    if (!before.isCurrentFor(*this) || !ownedProvenanceIsCurrent(owned)) {
        request.result.outcome = AssignmentStepOutcome::Superseded;
        return request;
    }
    // Refusals here have no effects. Only owned provenance, whose identity the predicate above
    // already checked, passes the ownership boundary; ordinary callers never inherit that
    // exemption. A backend without a guarded dispatcher could not report what it did.
    const bool ordinary = owned.kind == OwnedProvenance::Kind::None || owned.kind == OwnedProvenance::Kind::Operation;
    if ((ordinary && (m_transientOwnership || m_activeTransientRequest)) || !supportsAssignmentGeometryRequest()) {
        return request;
    }
    if (owned.kind == OwnedProvenance::Kind::Restoration) {
        const auto &ownership = *m_transientOwnership;
        const auto &attempt = *ownership.restoreAttempt;
        // The provenance can remain current after dispatch; it is not permission to submit
        // another request, a different target or an inherited Move-only operation.
        if (!attempt.requesting() || attempt.requestGeneration || mode != MoveResizeMode::MoveResize
            || !exactFrameGeometry(desired, ownership.floating) || !canStartTransientRestoreRequest(desired)
            || workspace()->outputAt(desired.center()) != ownership.output) {
            return request;
        }
    }
    AssignmentGeometrySync sync;
    const quint64 writes = prepareAssignmentGeometrySync(desired, sync);
    const auto exhausted = std::numeric_limits<quint64>::max();
    // Reserve everything a synchronous request may need before the first mutation: one request
    // generation below the value fences treat as exhausted, the exact synchronization writes plus
    // one committed-geometry change, and one publication generation. Near exhaustion this refuses
    // requests whose eventual queued or no-op branch would have needed less.
    if (m_geometryRequestGeneration >= exhausted - 1 || m_geometryUpdateGeneration >= exhausted - 1
        || writes + 1 > exhausted - 1 - m_placementContinuationRevision) {
        return request;
    }
    // A Move inside an unsubmitted resize inherits that resize's requested size, as in performMoveResize().
    if (ordinary && mode == MoveResizeMode::Move && m_geometryResizePending) {
        mode = MoveResizeMode::MoveResize;
    }
    const auto generation = ++m_geometryRequestGeneration;
    request.requestGeneration = generation;
    switch (owned.kind) {
    case OwnedProvenance::Kind::Request:
        m_geometryRequestOrigin = {GeometryRequestOrigin::Kind::OwnedGeometry, generation, owned.request->m_ownerGeneration, 0};
        break;
    case OwnedProvenance::Kind::Ownership:
        m_geometryRequestOrigin = {GeometryRequestOrigin::Kind::OwnedGeometry, generation, owned.ownership, 0};
        break;
    case OwnedProvenance::Kind::Restoration:
        m_geometryRequestOrigin = {GeometryRequestOrigin::Kind::Restoration, generation, owned.ownership, owned.restoreSequence};
        break;
    case OwnedProvenance::Kind::None:
    case OwnedProvenance::Kind::Operation:
        m_geometryRequestOrigin = {GeometryRequestOrigin::Kind::Ordinary, generation, 0, 0};
        break;
    }
    m_geometryResizePending = (uint(mode) & uint(MoveResizeMode::Resize)) != 0;
    const auto finish = qScopeGuard([this, generation]() {
        // An obsolete stack frame must not clear a newer request's pending submission.
        if (generation == m_geometryRequestGeneration) {
            m_geometryResizePending = false;
        }
    });
    if (owned.kind == OwnedProvenance::Kind::Request) {
        // Progress is published before the first synchronized write and stays Requested whatever
        // happens afterwards; teardown callbacks must see it before any automatic geometry.
        auto *const adoption = owned.request;
        adoption->m_geometryGeneration = generation;
        adoption->m_progress = TransientRequestProgress::Requested;
        if (m_transientOwnership && m_transientOwnership->generation == adoption->m_ownerGeneration
            && m_transientOwnership->nativeSequence == adoption->m_sequence) {
            m_transientOwnership->progress = TransientRequestProgress::Requested;
        }
    } else if (owned.kind == OwnedProvenance::Kind::Restoration) {
        // No callback has run since validating the matching wrapper. Record this allocation
        // before the first synchronized write or its notifications can reenter the Window.
        m_transientOwnership->restoreAttempt->requestGeneration = generation;
    }
    // The allocation invalidated every earlier fence, so this request's own checkpoint starts here.
    sync.result.outcome = AssignmentStepOutcome::Superseded;
    sync.owned = owned;
    sync.result.checkpoint.reset(new TileAssignmentFence(*this));
    setMoveResizeGeometryInternal(desired, &sync);
    request.result = std::move(sync.result);
    const auto current = [&]() {
        return assignmentResultIsCurrent(request.result) && geometryRequestIsCurrent(generation)
            && transientGeometryRequestIsCurrent(generation);
    };
    const auto supersede = [&]() {
        // After the first mutation nothing is rolled back and nothing is refused: the request ends
        // without authority over the state it left behind.
        request.result.outcome = AssignmentStepOutcome::Superseded;
        request.result.checkpoint.reset();
    };
    if (!current()) {
        supersede();
        return request;
    }
    dispatchAssignmentGeometryRequest(desired, mode, request);
    if (request.dispatch == AssignmentRequestDispatch::None || request.result.outcome != AssignmentStepOutcome::AppliedCurrent || !current()) {
        supersede();
        return request;
    }
    notifyRequestedGeometryChanged();
    if (!current()) {
        supersede();
        return request;
    }
    return request;
}

bool Window::supportsAssignmentGeometryRequest() const
{
    return false;
}

bool Window::assignmentPublishesImmediately() const
{
    return false;
}

void Window::publishRequestedTileForAssignment(AssignmentStepResult *)
{
    doSetQuickTileMode();
}

void Window::dispatchAssignmentGeometryRequest(const RectF &, MoveResizeMode, AssignmentGeometryRequest &)
{
    // Only a backend that opted in through supportsAssignmentGeometryRequest() is dispatched to.
}

bool Window::ownedProvenanceIsCurrent(const OwnedProvenance &owned) const
{
    switch (owned.kind) {
    case OwnedProvenance::Kind::None:
        return true;
    case OwnedProvenance::Kind::Request:
        return owned.request && transientRequestIsCurrent(*owned.request) && (!owned.constraint || owned.constraint());
    case OwnedProvenance::Kind::Ownership:
        return transientOwnershipIsCurrent(owned.ownership) && (!owned.constraint || owned.constraint());
    case OwnedProvenance::Kind::Restoration:
        return transientRestoreOperationIsCurrent(owned.ownership, owned.restoreSequence) && (!owned.constraint || owned.constraint());
    case OwnedProvenance::Kind::Operation:
        return !owned.constraint || owned.constraint();
    }
    return false;
}

std::unique_ptr<TileAssignmentFence> Window::makeTileAssignmentCheckpoint()
{
    return std::unique_ptr<TileAssignmentFence>(new TileAssignmentFence(*this));
}

Window::AssignmentStepResult Window::normalizeMaximizeForAssignment(const TileAssignmentFence &, OwnedProvenance)
{
    // A backend without guarded requests cannot report what its normalization did: refused without
    // effect, so an ingress selects its legacy path before any mutation.
    return AssignmentStepResult();
}

bool Window::canNormalizeMaximizeForAssignment() const
{
    return false;
}

bool Window::canRequestTileForAssignment(Tile *tile) const
{
    const auto exhausted = std::numeric_limits<quint64>::max();
    if (isDeleted() || (tile && tile->isTransient()) || !supportsAssignmentGeometryRequest()
        || m_transientOwnership || m_activeTransientRequest || m_requestedTransientTileGeneration != 0
        || isInteractiveMoveResize()) {
        return false;
    }
    if (tile && requestedMaximizeMode() != MaximizeRestore && !canNormalizeMaximizeForAssignment()) {
        return false;
    }
    // A conservative bound for the whole path, not a count of expected increments: the
    // normalization's capture and request, then this step's capture and request; two request
    // generations; one association change; for a backend that commits immediately, the
    // publication's committed-association change and its placement increment too.
    const quint64 publication = assignmentPublishesImmediately() ? 1 : 0;
    return m_requestedTileChange < exhausted - 1
        && (!publication || m_committedTileChange < exhausted - 1)
        && m_placementContinuationRevision < exhausted - 2 * assignmentPlacementBudget - publication
        && m_geometryRequestGeneration < exhausted - 2 && m_geometryUpdateGeneration < exhausted - 2;
}

Window::AssignmentStepResult Window::requestTileForAssignment(Tile *tile, const TileAssignmentFence &before, OwnedProvenance owned)
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
    if (!canRequestTileForAssignment(tile)) {
        return result;
    }
    // The destination's identity and its original presence, captured before the first callback: a
    // destination destroyed by a callback ends the step and is never read as explicit null.
    const QPointer<Tile> target(tile);
    const bool hadTarget = tile;
    const auto targetAlive = [&]() {
        return !hadTarget || target;
    };
    if (m_requestedTile == tile) {
        // An operation-owned no-op: nothing written (the transient marker is zero, the preflight
        // refuses otherwise), a fresh checkpoint, the incoming provenance preserved.
        result.owned = owned;
        result.checkpoint = makeTileAssignmentCheckpoint();
        result.outcome = assignmentResultIsCurrent(result) ? AssignmentStepOutcome::AppliedCurrent : AssignmentStepOutcome::Superseded;
        if (result.outcome != AssignmentStepOutcome::AppliedCurrent) {
            result.checkpoint.reset();
        }
        return result;
    }
    // The validated entry state as an owned snapshot; the caller's fence is never read again.
    std::unique_ptr<TileAssignmentFence> checkpoint = makeTileAssignmentCheckpoint();
    if (tile && requestedMaximizeMode() != MaximizeRestore) {
        auto normalization = normalizeMaximizeForAssignment(*checkpoint, owned);
        if (normalization.outcome == AssignmentStepOutcome::Refused) {
            // Mutation-free, before any effect of this step: still a refusal of the whole step.
            return result;
        }
        if (normalization.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(normalization) || !targetAlive()) {
            result.outcome = AssignmentStepOutcome::Superseded;
            return result;
        }
        // The normalization's receipt (its requested-mode revision and frozen limits) and its
        // checkpoint are this step's continuation from here.
        owned = normalization.owned;
        checkpoint = std::move(normalization.checkpoint);
    }
    if (tile && !m_requestedTile && !m_tile) {
        // Entering tiling from no association: the legacy capture, through the guarded writer. A
        // failure here, after the normalization may have run, is a supersession, never a fallback.
        auto restore = setGeometryRestoreForAssignment(quickTileGeometryRestore(), *checkpoint, owned);
        if (restore.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(restore) || !targetAlive()) {
            result.outcome = AssignmentStepOutcome::Superseded;
            return result;
        }
        checkpoint = std::move(restore.checkpoint);
    }
    // The association write, sealed by its own checkpoint; from here every stop is Superseded.
    m_requestedTile = tile;
    m_requestedTransientTileGeneration = 0;
    m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
    const auto association = m_requestedTileChange;
    const auto written = makeTileAssignmentFence();
    result.outcome = AssignmentStepOutcome::Superseded;
    RectF geometry = moveResizeGeometry();
    if (tile) {
        geometry = tile->windowGeometry();
    } else if (geometryRestore().isValid()) {
        geometry = geometryRestore();
    }
    auto request = requestMoveResizeGuarded(geometry, MoveResizeMode::MoveResize, written, owned);
    const bool applied = request.result.outcome == AssignmentStepOutcome::AppliedCurrent && assignmentResultIsCurrent(request.result);
    // The obligation of the association write, discharged while that write is current whatever
    // the geometry request did: the backend's quick-tile protocol state, then the notification,
    // each behind the same recheck (live window, unchanged and unexhausted association revision,
    // the destination's identity with its original presence, no transient marker).
    const auto obligationCurrent = [&]() {
        return !isDeleted() && association != std::numeric_limits<quint64>::max() && m_requestedTileChange == association
            && m_requestedTile.data() == target.data() && targetAlive() && m_requestedTransientTileGeneration == 0;
    };
    if (obligationCurrent()) {
        // The publication carries the step's continuation only while the operation succeeded; a
        // superseded request still publishes the still-current association without being
        // resurrected by it. Commit first, then the notification, each behind the same recheck.
        publishRequestedTileForAssignment(applied ? &request.result : nullptr);
        if (obligationCurrent()) {
            Q_EMIT requestedTileChanged();
        }
    }
    // Terminal validation, before the result is returned: the request's carrier (its checkpoint and
    // the provenance) only if it applied and nothing foreign happened since, including inside the
    // obligation's own callbacks.
    if (applied && assignmentResultIsCurrent(request.result)) {
        result = std::move(request.result);
    }
    return result;
}

bool Window::canForgetTileForAssignment() const
{
    // Mutation-free refusals (a backend without guarded requests included) and the headroom of
    // the one association change the forget step makes, plus the publication's committed change
    // and placement increment for a backend that commits immediately.
    const auto exhausted = std::numeric_limits<quint64>::max();
    return !isDeleted() && supportsAssignmentGeometryRequest() && !m_transientOwnership && !m_activeTransientRequest
        && m_requestedTransientTileGeneration == 0 && m_requestedTileChange < exhausted - 1
        && (!assignmentPublishesImmediately() || (m_committedTileChange < exhausted - 1 && m_placementContinuationRevision < exhausted - 1));
}

Window::AssignmentStepResult Window::forgetTileForAssignment(Tile *tile, const TileAssignmentFence &before, OwnedProvenance owned)
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
    if (!canForgetTileForAssignment()) {
        return result;
    }
    if (m_requestedTile != tile) {
        // An operation-owned no-op: nothing written, the incoming provenance preserved.
        result.owned = owned;
        result.checkpoint = makeTileAssignmentCheckpoint();
        result.outcome = assignmentResultIsCurrent(result) ? AssignmentStepOutcome::AppliedCurrent : AssignmentStepOutcome::Superseded;
        if (result.outcome != AssignmentStepOutcome::AppliedCurrent) {
            result.checkpoint.reset();
        }
        return result;
    }
    m_requestedTile = nullptr;
    m_requestedTransientTileGeneration = 0;
    m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
    const auto association = m_requestedTileChange;
    result.outcome = AssignmentStepOutcome::Superseded;
    result.owned = owned;
    result.checkpoint = makeTileAssignmentCheckpoint();
    const auto obligationCurrent = [&]() {
        return !isDeleted() && association != std::numeric_limits<quint64>::max() && m_requestedTileChange == association
            && m_requestedTile.isNull() && m_requestedTransientTileGeneration == 0;
    };
    if (obligationCurrent()) {
        publishRequestedTileForAssignment(&result);
        if (obligationCurrent()) {
            Q_EMIT requestedTileChanged();
        }
    }
    // Terminal validation: a publication that promoted the carrier does not survive a later
    // supersession.
    if (assignmentResultIsCurrent(result)) {
        result.outcome = AssignmentStepOutcome::AppliedCurrent;
    } else {
        result.outcome = AssignmentStepOutcome::Superseded;
        result.checkpoint.reset();
    }
    return result;
}

bool Window::assignmentResultIsCurrent(const AssignmentStepResult &result) const
{
    return result.checkpoint && result.checkpoint->isCurrentFor(*this) && ownedProvenanceIsCurrent(result.owned);
}

bool Window::transientOwnershipIsCurrent(quint64 generation) const
{
    if (isDeleted() || !generation || !m_transientOwnership || m_transientOwnership->generation != generation
        || m_transientOwnership->phase != TransientOwnershipPhase::Owned || m_transientOwnership->executing
        || m_transientOwnership->releasing || m_activeTransientRequest) {
        return false;
    }
    const auto &ownership = *m_transientOwnership;
    if (!ownership.writer || !ownership.writer->isProviderCurrent(ownership.provider, ownership.providerRevision)
        || !ownership.writer->ownsReservation(internalId(), QString::number(generation))) {
        return false;
    }
    Tile *const tile = ownership.tile.data();
    if (!tile || !tile->isTransient() || !ownership.manager || !ownership.output || !ownership.desktop
        || tile->manager() != ownership.manager || ownership.manager->tearingDown()
        || ownership.manager->output() != ownership.output || tile->desktop() != ownership.desktop
        || !workspace()->outputs().contains(ownership.output)
        || !VirtualDesktopManager::self()->desktops().contains(ownership.desktop)
        || !tile->windows().contains(const_cast<Window *>(this))) {
        return false;
    }
    // The window's own current context, not only the recorded objects: a handler can move it to
    // another desktop without touching any fence or releasing the ownership.
    if (desktops().size() != 1 || desktops().constFirst() != ownership.desktop || moveResizeOutput() != ownership.output) {
        return false;
    }
    return m_requestedTile == tile && m_requestedTransientTileGeneration == generation && (!m_tile || m_tile == tile);
}

bool Window::assignmentGeometrySyncIsCurrent(AssignmentGeometrySync &sync) const
{
    if (assignmentResultIsCurrent(sync.result) && (!sync.hadOutput || sync.output)) {
        return true;
    }
    sync.result.outcome = AssignmentStepOutcome::Superseded;
    sync.result.checkpoint.reset();
    return false;
}

void Window::setMoveResizeGeometryInternal(const RectF &rect, AssignmentGeometrySync *sync)
{
    if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
        return;
    }
    if (m_moveResizeGeometry != rect) {
        m_moveResizeGeometry = rect;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        if (sync) {
            sync->result.checkpoint.reset(new TileAssignmentFence(*this));
        }
    }
    if (sync) {
        // No callback ran since the entry check; from here every synchronized write validates the
        // owned provenance next to the checkpoint.
        sync->result.owned = sync->owned;
    }
    setMoveResizeOutputInternal(sync ? sync->output.data() : workspace()->outputAt(rect.center()), sync);
}

void Window::notifyRequestedGeometryChanged()
{
    const RectF previous = s_notifiedRequestedGeometry.value(this);
    if (m_moveResizeGeometry == previous) {
        return;
    }
    s_notifiedRequestedGeometry.insert(this, m_moveResizeGeometry);
    Q_EMIT requestedGeometryChanged();
}

LogicalOutput *Window::moveResizeOutput() const
{
    return m_moveResizeOutput;
}

void Window::setMoveResizeOutput(LogicalOutput *output)
{
    setMoveResizeOutputInternal(output);
}

void Window::setOutputPropertiesPending(bool pending, AssignmentGeometrySync *sync)
{
    if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
        return;
    }
    if (m_outputPropertiesPending != pending) {
        m_outputPropertiesPending = pending;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        if (sync) {
            sync->result.checkpoint.reset(new TileAssignmentFence(*this));
        }
    }
}

void Window::setMoveResizeOutputInternal(LogicalOutput *output, AssignmentGeometrySync *sync)
{
    if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
        return;
    }
    if (m_moveResizeOutput == output && !m_outputPropertiesPending && (!sync || !sync->synchronize)) {
        return;
    }

    if (m_moveResizeOutput != output) {
        invalidateTransientRestoreAttempt();
        if (m_moveResizeOutput) {
            disconnect(m_moveResizeOutput, &LogicalOutput::scaleChanged, this, &Window::updateNextTargetScale);
            disconnect(m_moveResizeOutput, &LogicalOutput::transformChanged, this, &Window::updatePreferredBufferTransform);
            disconnect(m_moveResizeOutput, &LogicalOutput::blendingColorChanged, this, &Window::updatePreferredColorDescription);
        }
        m_moveResizeOutput = output;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        if (sync) {
            sync->result.checkpoint.reset(new TileAssignmentFence(*this));
        }
        if (output) {
            connect(output, &LogicalOutput::scaleChanged, this, &Window::updateNextTargetScale);
            connect(output, &LogicalOutput::transformChanged, this, &Window::updatePreferredBufferTransform);
            connect(output, &LogicalOutput::blendingColorChanged, this, &Window::updatePreferredColorDescription);
        }
    }
    if (!m_moveResizeOutput) {
        setOutputPropertiesPending(false, sync);
        return;
    }

    // A nested request to this output must finish synchronization if this request is superseded.
    setOutputPropertiesPending(true, sync);
    const auto generation = m_geometryRequestGeneration;
    const auto current = [&]() {
        return sync ? assignmentGeometrySyncIsCurrent(*sync) : geometryRequestIsCurrent(generation);
    };
    setNextTargetScaleInternal(sync ? sync->scale : m_moveResizeOutput->scale(), sync);
    if (!current()) {
        return;
    }
    setPreferredBufferTransformInternal(sync ? sync->transform : m_moveResizeOutput->transform(), sync);
    if (!current()) {
        return;
    }
    setPreferredColorDescriptionInternal(sync ? sync->color : m_moveResizeOutput->blendingColor(), sync);
    if (current()) {
        setOutputPropertiesPending(false, sync);
    }
}

void Window::move(const QPointF &point)
{
    if (isDeleted()) {
        return;
    }

    const RectF rect = RectF(point, m_moveResizeGeometry.size());

    performMoveResize(rect, MoveResizeMode::Move);
}

void Window::resize(const QSizeF &size)
{
    if (isDeleted()) {
        return;
    }

    const RectF rect = RectF(m_moveResizeGeometry.topLeft(), size);

    performMoveResize(rect, MoveResizeMode::Resize);
}

void Window::moveResize(const RectF &rect)
{
    if (isDeleted()) {
        return;
    }

    performMoveResize(rect, MoveResizeMode::MoveResize);
}

quint64 Window::geometryRequestGeneration() const
{
    return m_geometryRequestGeneration;
}

bool Window::geometryRequestIsCurrent(quint64 generation) const
{
    return !isDeleted() && generation == m_geometryRequestGeneration;
}

void Window::geometryResizeSubmitted(quint64 generation)
{
    if (geometryRequestIsCurrent(generation)) {
        m_geometryResizePending = false;
    }
}

void Window::performMoveResize(const RectF &rect, MoveResizeMode mode)
{
    // Public geometry intent: a native request in flight is cancelled, never inherited.
    cancelTransientRequest();
    if (isDeleted() || m_geometryRequestGeneration == std::numeric_limits<quint64>::max()) {
        return;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // A Move inside an unsubmitted resize inherits that resize's requested size.
    if (mode == MoveResizeMode::Move && m_geometryResizePending) {
        mode = MoveResizeMode::MoveResize;
    }
    const auto generation = ++m_geometryRequestGeneration;
    m_geometryRequestOrigin = {GeometryRequestOrigin::Kind::Ordinary, generation, 0, 0};
    m_geometryResizePending = (uint(mode) & uint(MoveResizeMode::Resize)) != 0;
    const auto finish = qScopeGuard([this, generation]() {
        // An obsolete stack frame must not clear a newer request's pending submission.
        if (generation == m_geometryRequestGeneration) {
            m_geometryResizePending = false;
        }
    });
    setMoveResizeGeometry(rect);
    if (!geometryRequestIsCurrent(generation) || !transientGeometryRequestIsCurrent(generation)) {
        return;
    }
    moveResizeInternal(rect, mode);
    if (geometryRequestIsCurrent(generation) && transientGeometryRequestIsCurrent(generation)) {
        notifyRequestedGeometryChanged();
    }
}

bool Window::isPlaced() const
{
    return m_placed;
}

void Window::place(const PlacementCommand &placement)
{
    if (auto position = std::get_if<QPointF>(&placement)) {
        move(*position);
    } else if (auto rect = std::get_if<RectF>(&placement)) {
        moveResize(*rect);
    } else if (auto maximizeMode = std::get_if<MaximizeMode>(&placement)) {
        maximize(*maximizeMode);
    }
    markAsPlaced();
}

void Window::markAsPlaced()
{
    m_placed = true;
}

void Window::setElectricBorderMode(std::optional<ElectricBorderMode> mode)
{
    m_electricMode = mode;
}

void Window::setElectricBorderMaximizing(bool maximizing)
{
    m_electricMaximizing = maximizing;
    if (maximizing) {
        if (auto quickTileMode = std::get_if<QuickTileMode>(&m_electricMode.value())) {
            workspace()->outline()->show(quickTileGeometry(*quickTileMode, interactiveMoveResizeAnchor()).toRect(), moveResizeGeometry().toRect());
        } else if (std::get_if<MaximizeMode>(&m_electricMode.value())) {
            workspace()->outline()->show(workspace()->clientArea(MaximizeArea, this, interactiveMoveResizeAnchor()).toRect(), moveResizeGeometry().toRect());
        }
    } else {
        workspace()->outline()->hide();
    }
}

RectF Window::quickTileGeometry(QuickTileMode mode, const QPointF &pos) const
{
    LogicalOutput *output = workspace()->outputAt(pos);

    if (mode & QuickTileFlag::Custom) {
        const auto root = workspace()->rootTile(output);
        Tile *tile = root ? root->pick(pos) : nullptr;
        if (tile) {
            return tile->windowGeometry();
        } else {
            return RectF();
        }
    }

    const auto manager = workspace()->tileManager(output);
    Tile *tile = manager ? manager->quickTile(mode) : nullptr;
    if (tile) {
        return tile->windowGeometry();
    }
    return workspace()->clientArea(MaximizeArea, this, pos);
}

void Window::exitQuickTileMode()
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    QList<QPointer<LogicalOutput>> outputs;
    for (auto output : workspace()->outputs()) {
        outputs.append(output);
    }
    for (const auto &output : outputs) {
        if (isDeleted()) {
            return;
        }
        if (auto manager = output ? workspace()->tileManager(output) : nullptr) {
            manager->forgetWindow(this, nullptr);
        }
    }
}

void Window::updateElectricGeometryRestore()
{
    m_electricGeometryRestore = geometryRestore();
    if (m_interactiveMoveResize.initialQuickTileMode == QuickTileMode(QuickTileFlag::None)) {
        if (!(requestedMaximizeMode() & MaximizeHorizontal)) {
            m_electricGeometryRestore.setLeft(m_frameGeometry.left());
            m_electricGeometryRestore.setRight(m_frameGeometry.right());
        }
        if (!(requestedMaximizeMode() & MaximizeVertical)) {
            m_electricGeometryRestore.setTop(m_frameGeometry.top());
            m_electricGeometryRestore.setBottom(m_frameGeometry.bottom());
        }
    }
}

RectF Window::quickTileGeometryRestore() const
{
    if (requestedQuickTileMode() != QuickTileMode(QuickTileFlag::None)) {
        // If the window is tiled, geometryRestore() already has a good value.
        return geometryRestore();
    }

    if (isElectricBorderMaximizing()) {
        return m_electricGeometryRestore;
    } else {
        return moveResizeGeometry();
    }
}

static QuickTileMode combineQuickTileMode(QuickTileMode mode, QuickTileMode other)
{
    const bool isHorizontal = (other & (QuickTileFlag::Left | QuickTileFlag::Right)) == QuickTileFlag::None;
    const bool isVertical = (other & (QuickTileFlag::Top | QuickTileFlag::Bottom)) == QuickTileFlag::None;
    if (isHorizontal != isVertical) {
        if (mode == QuickTileFlag::Left) {
            return (other & ~QuickTileFlag::Right) | QuickTileFlag::Left;
        } else if (mode == QuickTileFlag::Right) {
            return (other & ~QuickTileFlag::Left) | QuickTileFlag::Right;
        } else if (mode == QuickTileFlag::Top) {
            return (other & ~QuickTileFlag::Bottom) | QuickTileFlag::Top;
        } else if (mode == QuickTileFlag::Bottom) {
            return (other & ~QuickTileFlag::Top) | QuickTileFlag::Bottom;
        }
    }

    QuickTileMode combined = other;

    if (mode & QuickTileFlag::Left) {
        if (other & QuickTileFlag::Right) {
            combined &= ~QuickTileFlag::Right;
        } else {
            combined |= QuickTileFlag::Left;
        }
    }
    if (mode & QuickTileFlag::Right) {
        if (other & QuickTileFlag::Left) {
            combined &= ~QuickTileFlag::Left;
        } else {
            combined |= QuickTileFlag::Right;
        }
    }
    if (mode & QuickTileFlag::Top) {
        if (other & QuickTileFlag::Bottom) {
            combined &= ~QuickTileFlag::Bottom;
        } else {
            combined |= QuickTileFlag::Top;
        }
    }
    if (mode & QuickTileFlag::Bottom) {
        if (other & QuickTileFlag::Top) {
            combined &= ~QuickTileFlag::Top;
        } else {
            combined |= QuickTileFlag::Bottom;
        }
    }

    return combined;
}

void Window::handleQuickTileShortcut(QuickTileMode mode)
{
    QPointF tileAtPoint = moveResizeGeometry().center();
    if (mode != QuickTileFlag::None) {
        const QuickTileMode oldMode = requestedQuickTileMode();
        if (oldMode == QuickTileMode(QuickTileFlag::None) && requestedMaximizeMode() == MaximizeMode::MaximizeRestore) {
            // Not coming out of an existing tile, not shifting monitors, we're setting a brand new tile.
            // Store geometry first, so we can go out of this tile later.
            setGeometryRestore(quickTileGeometryRestore());
        } else {
            // If the window is asked to be tiled in a screen corner, don't combine the new mode with the old one.
            QuickTileMode combined;
            switch (mode) {
            case QuickTileMode(QuickTileFlag::Left):
            case QuickTileMode(QuickTileFlag::Top):
            case QuickTileMode(QuickTileFlag::Right):
            case QuickTileMode(QuickTileFlag::Bottom):
                combined = combineQuickTileMode(mode, oldMode);
                break;
            default:
                combined = mode;
            }

            // If trying to tile to the side that the window is already tiled to, move the window to the next
            // screen in that direction if it exists and swap the tile side.
            if (combined == oldMode) {
                LogicalOutput *currentOutput = moveResizeOutput();
                LogicalOutput *nextOutput = currentOutput;
                LogicalOutput *candidateOutput = currentOutput;
                if ((mode & QuickTileFlag::Horizontal) == QuickTileMode(QuickTileFlag::Left)) {
                    candidateOutput = workspace()->findOutput(nextOutput, Workspace::DirectionWest);
                } else if ((mode & QuickTileFlag::Horizontal) == QuickTileMode(QuickTileFlag::Right)) {
                    candidateOutput = workspace()->findOutput(nextOutput, Workspace::DirectionEast);
                }
                bool shiftHorizontal = candidateOutput != nextOutput;
                nextOutput = candidateOutput;
                if ((mode & QuickTileFlag::Vertical) == QuickTileMode(QuickTileFlag::Top)) {
                    candidateOutput = workspace()->findOutput(nextOutput, Workspace::DirectionNorth);
                } else if ((mode & QuickTileFlag::Vertical) == QuickTileMode(QuickTileFlag::Bottom)) {
                    candidateOutput = workspace()->findOutput(nextOutput, Workspace::DirectionSouth);
                }
                bool shiftVertical = candidateOutput != nextOutput;
                nextOutput = candidateOutput;

                if (nextOutput != currentOutput) {
                    // Move to other screen
                    tileAtPoint = nextOutput->geometry().center();

                    // Swap sides
                    if (shiftHorizontal) {
                        combined = (~combined & QuickTileFlag::Horizontal) | (combined & QuickTileFlag::Vertical);
                    }
                    if (shiftVertical) {
                        combined = (~combined & QuickTileFlag::Vertical) | (combined & QuickTileFlag::Horizontal);
                    }
                }
            }

            mode = combined;
        }
    }
    setQuickTileMode(mode, tileAtPoint);
}

void Window::handleCustomQuickTileShortcut(QuickTileMode mode)
{
    if (mode == QuickTileFlag::None) {
        return;
    }
    // if window is not tiled already, set it to nearest one
    const auto root = workspace()->rootTile(workspace()->outputAt(moveResizeGeometry().center()));
    Tile *tileAtPoint = root ? root->pick(moveResizeGeometry().center()) : nullptr;
    if (!tileAtPoint) {
        return;
    }
    if (tileAtPoint != m_requestedTile) {
        tileAtPoint->manage(this);
        return;
    }
    const auto customTile = qobject_cast<CustomTile *>(tileAtPoint);
    if (!customTile) {
        return;
    }
    // Get the next tile that is not a layout tile from the edge
    Qt::Edge edge;
    if (mode & QuickTileFlag::Left) {
        edge = Qt::LeftEdge;
    }
    if (mode & QuickTileFlag::Right) {
        edge = Qt::RightEdge;
    }
    if (mode & QuickTileFlag::Top) {
        edge = Qt::TopEdge;
    }
    if (mode & QuickTileFlag::Bottom) {
        edge = Qt::BottomEdge;
    }
    CustomTile *next = customTile->nextNonLayoutTileAt(edge);
    if (next) {
        next->manage(this);
    }
}

void Window::setQuickTileModeAtCurrentPosition(QuickTileMode mode)
{
    setQuickTileMode(mode, m_moveResizeGeometry.center());
}

void Window::setQuickTileMode(QuickTileMode mode, const QPointF &tileAtPoint)
{
    // sanitize the mode, ie. simplify "invalid" combinations
    if ((mode & QuickTileFlag::Horizontal) == QuickTileMode(QuickTileFlag::Horizontal)) {
        mode &= ~QuickTileMode(QuickTileFlag::Horizontal);
    }
    if ((mode & QuickTileFlag::Vertical) == QuickTileMode(QuickTileFlag::Vertical)) {
        mode &= ~QuickTileMode(QuickTileFlag::Vertical);
    }

    QPointer<Tile> tile;
    if (mode == QuickTileMode(QuickTileFlag::Custom)) {
        const auto root = workspace()->rootTile(workspace()->outputAt(tileAtPoint));
        tile = root ? root->pick(tileAtPoint) : nullptr;
    } else if (mode != QuickTileMode(QuickTileFlag::None)) {
        const auto manager = workspace()->tileManager(workspace()->outputAt(tileAtPoint));
        tile = manager ? manager->quickTile(mode) : nullptr;
    }
    if (!tile && mode != QuickTileMode(QuickTileFlag::None)) {
        return;
    }

    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // The outer quick-tile boundary: the qualified cancellation, whose continuation must still be
    // current when the public management below establishes its own operation (two stamps, with
    // currentness verified between them); at the exhaustion edge the legacy cancellation and its
    // geometry-generation check keep today's behaviour.
    auto preparation = cancelTransientRequestForAssignment();
    if (preparation.outcome == AssignmentStepOutcome::Superseded) {
        return;
    }
    if (preparation.outcome == AssignmentStepOutcome::Refused) {
        const auto generation = geometryRequestGeneration();
        cancelTransientRequest();
        if (!geometryRequestIsCurrent(generation)) {
            return;
        }
        workspace()->updateFocusMousePosition(Cursors::self()->mouse()->pos()); // may cause leave event
        if (!geometryRequestIsCurrent(generation) || (!tile && mode != QuickTileMode(QuickTileFlag::None))) {
            return;
        }
    } else {
        workspace()->updateFocusMousePosition(Cursors::self()->mouse()->pos()); // may cause leave event
        if (!assignmentResultIsCurrent(preparation) || (!tile && mode != QuickTileMode(QuickTileFlag::None))) {
            return;
        }
    }

    if (m_requestedTile != tile) {
        if (tile) {
            tile->manage(this);
        } else if (m_requestedTile && m_requestedTile->isActive()) {
            m_requestedTile->unmanage(this);
        }
    }
}

QuickTileMode Window::quickTileMode() const
{
    if (m_tile) {
        return m_tile->quickTileMode();
    } else {
        return QuickTileFlag::None;
    }
}

quint64 Window::ownedTransientPlacementGeneration() const
{
    if (isDeleted() || !m_transientOwnership || m_transientOwnership->phase != TransientOwnershipPhase::Owned
        || m_transientOwnership->releasing) {
        return 0;
    }
    return m_transientOwnership->generation;
}

QuickTileMode Window::requestedQuickTileMode() const
{
    if (m_requestedTile) {
        return m_requestedTile->quickTileMode();
    }

    return QuickTileFlag::None;
}

void Window::commitTile(Tile *tile)
{
    if (tile && tile->isTransient()) {
        return;
    }
    if (m_tile != tile) {
        invalidateTransientRestoreAttempt();
    }
    commitTileInternal(tile);
}

quint64 Window::requestedTransientTileGeneration() const
{
    // Keep the origin even when QObject destruction clears the requested Tile pointer.
    return m_requestedTransientTileGeneration;
}

void Window::commitTileFromConfigure(Tile *tile, quint64 transientGeneration)
{
    if (!transientGeneration) {
        // Native content is not a public placement intent. Keep the existing rejection of an
        // untagged transient target without passing through the public cancellation boundary.
        if (!tile || !tile->isTransient()) {
            commitTileInternal(tile);
        }
        return;
    }
    if (isDeleted() || !tile || !tile->isTransient() || !m_transientOwnership
        || m_transientOwnership->phase != TransientOwnershipPhase::Owned
        || m_transientOwnership->generation != transientGeneration || m_transientOwnership->tile != tile
        || m_requestedTransientTileGeneration != transientGeneration || m_requestedTile != tile
        || !m_transientOwnership->writer
        || !m_transientOwnership->writer->ownsReservation(internalId(), QString::number(transientGeneration))) {
        return;
    }
    m_transientOwnership->committed = true;
    commitTileInternal(tile);
}

Window::AssignmentStepResult Window::commitTileForAssignment(Tile *target, const TileAssignmentFence &before)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    AssignmentStepResult result;
    if (!before.isCurrentFor(*this)) {
        result.outcome = AssignmentStepOutcome::Superseded;
        return result;
    }
    if (target != m_requestedTile.data() || (target && target->isTransient())) {
        return result;
    }
    const auto lastUsable = std::numeric_limits<quint64>::max() - 1;
    if (m_tile != target && (m_committedTileChange >= lastUsable || m_placementContinuationRevision >= lastUsable)) {
        return result;
    }
    result.outcome = AssignmentStepOutcome::Superseded;
    commitTileInternal(target, &result);
    return result;
}

void Window::commitTileInternal(Tile *tile, AssignmentStepResult *guardedResult)
{
    if (isDeleted()) {
        return;
    }
    if (m_tile == tile) {
        if (guardedResult) {
            guardedResult->checkpoint.reset(new TileAssignmentFence(*this));
            guardedResult->outcome = AssignmentStepOutcome::AppliedCurrent;
        }
        return;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const QPointer<Tile> committed(tile);

    QuickTileMode oldTileMode = quickTileMode();

    m_tile = tile;
    m_committedTileChange = nextAssociationChange(m_committedTileChange);
    // A native commit can supersede placement without a geometry request or cancellation.
    m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
    const auto change = m_committedTileChange;
    if (guardedResult) {
        // Seal only this write, before callbacks can create newer placement intent.
        guardedResult->checkpoint.reset(new TileAssignmentFence(*this));
    }

    Q_EMIT tileChanged(tile);

    const auto current = [&]() {
        // The full carrier (its checkpoint and its provenance), before any further notification.
        return !isDeleted() && m_tile == committed && m_committedTileChange == change
            && change != std::numeric_limits<quint64>::max()
            && (!guardedResult || assignmentResultIsCurrent(*guardedResult));
    };
    if (!current()) {
        if (guardedResult) {
            guardedResult->checkpoint.reset();
        }
        return;
    }
    if (oldTileMode != quickTileMode()) {
        Q_EMIT quickTileModeChanged();
    }
    if (guardedResult) {
        if (current()) {
            guardedResult->outcome = AssignmentStepOutcome::AppliedCurrent;
        } else {
            guardedResult->checkpoint.reset();
        }
    }
}

Tile *Window::tile() const
{
    return m_tile;
}

Tile *Window::requestedTile() const
{
    return m_requestedTile;
}

void Window::requestTile(Tile *tile)
{
    if (tile && tile->isTransient()) {
        return;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // The public entry point establishes its own operation: the qualified cancellation is its
    // single stamp, a current continuation enters the guarded step, a refusal without effect
    // selects the legacy remainder before any mutation, and a supersession stops here (never a
    // fresh fence). The destination's identity is captured before the cancellation's callbacks.
    const QPointer<Tile> target(tile);
    const bool hadTarget = tile;
    auto preparation = cancelTransientRequestForAssignment();
    if (preparation.outcome == AssignmentStepOutcome::Superseded) {
        return;
    }
    if (preparation.outcome == AssignmentStepOutcome::AppliedCurrent) {
        if (hadTarget && !target) {
            return;
        }
        if (requestTileForAssignment(tile, *preparation.checkpoint).outcome != AssignmentStepOutcome::Refused) {
            return;
        }
        requestTileLegacy(tile);
        return;
    }
    // Refused for exhaustion before the stamp: the legacy cancellation keeps today's behaviour at
    // that edge. This is a legacy exclusion, not a guarded continuation.
    cancelTransientRequest();
    requestTileLegacy(tile);
}

void Window::requestTileLegacy(Tile *tile)
{
    if (isDeleted()) {
        return;
    }

    if (m_requestedTile == tile) {
        m_requestedTransientTileGeneration = 0;
        return;
    }

    if (tile && requestedMaximizeMode() != MaximizeRestore) {
        setMaximize(false, false);
        if (requestedMaximizeMode() != MaximizeRestore) {
            // window rules may enforce a different maximize mode, we can't do anything here
            return;
        }
    }

    if (tile && !m_requestedTile && !m_tile) {
        setGeometryRestore(quickTileGeometryRestore());
    }

    m_requestedTile = tile;
    m_requestedTransientTileGeneration = 0;
    m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
    if (tile) {
        moveResize(tile->windowGeometry());
    } else {
        RectF geometry = moveResizeGeometry();
        if (geometryRestore().isValid()) {
            geometry = geometryRestore();
        }
        if (isInteractiveMove()) {
            const QPointF anchor = interactiveMoveResizeAnchor();
            const QPointF offset = interactiveMoveOffset();
            geometry.moveTopLeft(QPointF(anchor.x() - geometry.width() * offset.x(),
                                         anchor.y() - geometry.height() * offset.y()));
        }
        moveResize(geometry);
    }
    doSetQuickTileMode();
    Q_EMIT requestedTileChanged();
}

void Window::forgetTile(Tile *tile)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    auto preparation = cancelTransientRequestForAssignment();
    if (preparation.outcome == AssignmentStepOutcome::Superseded) {
        return;
    }
    if (preparation.outcome == AssignmentStepOutcome::AppliedCurrent) {
        if (forgetTileForAssignment(tile, *preparation.checkpoint).outcome != AssignmentStepOutcome::Refused) {
            return;
        }
        forgetTileLegacy(tile);
        return;
    }
    cancelTransientRequest();
    forgetTileLegacy(tile);
}

void Window::forgetTileLegacy(Tile *tile)
{
    if (m_requestedTile != tile) {
        return;
    }

    m_requestedTile = nullptr;
    m_requestedTransientTileGeneration = 0;
    m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
    doSetQuickTileMode();
    Q_EMIT requestedTileChanged();
}

TileAssignmentFence Window::makeTileAssignmentFence()
{
    return TileAssignmentFence(*this);
}

void Window::noteTileMembershipChanged()
{
    m_tileMembershipChange = nextAssociationChange(m_tileMembershipChange);
}

TileAssignmentFence::TileAssignmentFence(Window &window)
    : m_window(&window)
    , m_windowId(window.internalId())
    , m_placementRevision(window.m_placementContinuationRevision)
    , m_requestedRevision(window.m_requestedTileChange)
    , m_committedRevision(window.m_committedTileChange)
    , m_geometryGeneration(window.m_geometryRequestGeneration)
    , m_membershipRevision(window.m_tileMembershipChange)
    , m_requestedTile(window.m_requestedTile)
    , m_committedTile(window.m_tile)
    , m_hadRequestedTile(!window.m_requestedTile.isNull())
    , m_hadCommittedTile(!window.m_tile.isNull())
{
    window.ref();
}

TileAssignmentFence::~TileAssignmentFence()
{
    if (m_window) {
        m_window->unref();
    }
}

bool TileAssignmentFence::isCurrentFor(const Window &window) const
{
    const auto exhausted = std::numeric_limits<quint64>::max();
    return m_window.data() == &window && !window.isDeleted() && window.internalId() == m_windowId
        && m_placementRevision != exhausted && m_requestedRevision != exhausted
        && m_committedRevision != exhausted && m_geometryGeneration != exhausted && m_membershipRevision != exhausted
        && window.m_placementContinuationRevision == m_placementRevision
        && window.m_requestedTileChange == m_requestedRevision
        && window.m_committedTileChange == m_committedRevision
        && window.m_geometryRequestGeneration == m_geometryGeneration
        && window.m_tileMembershipChange == m_membershipRevision
        && (!m_hadRequestedTile || !m_requestedTile.isNull())
        && (!m_hadCommittedTile || !m_committedTile.isNull())
        && window.m_requestedTile.data() == m_requestedTile.data()
        && window.m_tile.data() == m_committedTile.data();
}

void Window::setTileCompatibility(Tile *tile)
{
    qCWarning(KWIN_CORE) << "Writing to the property window.tile is deprecated: use tile.manage() instead";

    if (isDeleted() || (tile && tile->isTransient())) {
        return;
    }
    if (!supportsAssignmentGeometryRequest()) {
        // A backend without guarded requests keeps the legacy sequence, selected before any
        // guarded mutation.
        setTileCompatibilityLegacy(tile);
        return;
    }

    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // Captured before the first callback: the destination's identity, presence and frozen context
    // (validated after the preparation and chained into the ingress's provenance, so a destination
    // changed during the cancellation is never taken as fresh); the original association's
    // identity, presence and membership. The cleanup names only that original, never a tile a
    // callback selected.
    const QPointer<Tile> target(tile);
    const bool hadTarget = tile;
    const std::function<bool()> context = tile ? tile->assignmentContext(this) : std::function<bool()>();
    const QPointer<Tile> previous(m_requestedTile);
    const bool hadOriginalMembership = m_requestedTile && m_requestedTile->windows().contains(this);
    auto preparation = cancelTransientRequestForAssignment();
    if (preparation.outcome == AssignmentStepOutcome::Superseded) {
        return;
    }
    if (preparation.outcome == AssignmentStepOutcome::Refused) {
        // Exhaustion known before the stamp: the full legacy sequence with its own cancellation (a
        // legacy exclusion, not a guarded continuation).
        setTileCompatibilityLegacy(tile);
        return;
    }
    if ((hadTarget && !target) || (context && !context()) || !assignmentResultIsCurrent(preparation)) {
        return;
    }
    if (m_requestedTile == tile) {
        // The setter's public equal path after its stamp: the marker cleared, nothing else.
        m_requestedTransientTileGeneration = 0;
        return;
    }
    // The complete selected path preflighted before any guarded mutation: the original's cleanup
    // takes one membership increment (reserved in the ingress's headroom, checked here when no
    // ingress runs) and, when the original stays requested (no destination, or an inactive one
    // that does not evacuate it), the null association. A shortage known here selects the legacy
    // remainder; one consumed by a later callback stops the setter without rollback.
    const bool cleans = hadOriginalMembership && previous && previous->windows().contains(this);
    const bool clearsOriginal = cleans && m_requestedTile == previous.data()
        && (!tile || (!tile->isActive() && tile->desktop() != previous->desktop()));
    if ((clearsOriginal && !canRequestTileForAssignment(nullptr))
        || (cleans && !tile && m_tileMembershipChange >= std::numeric_limits<quint64>::max() - 1)) {
        setTileCompatibilityLegacyRemainder(tile);
        return;
    }
    std::unique_ptr<TileAssignmentFence> checkpoint = std::move(preparation.checkpoint);
    OwnedProvenance owned;
    if (context) {
        owned = owned.withConstraint(context);
    }
    if (tile) {
        auto ingress = target->manageForAssignment(this, *checkpoint, owned, cleans ? 1 : 0);
        if (ingress.outcome == AssignmentStepOutcome::Refused) {
            // Mutation-free: the legacy remainder after the qualified cancellation (a legacy
            // exclusion; its own public stamps are part of it).
            setTileCompatibilityLegacyRemainder(tile);
            return;
        }
        if (ingress.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(ingress)) {
            // Superseded, not refused: an incomplete assignment whose writes stand (a membership
            // may remain without a requested association); the setter stops.
            return;
        }
        checkpoint = std::move(ingress.checkpoint);
        owned = ingress.owned;
    }
    if (!cleans || !previous || !previous->windows().contains(this)) {
        // Absent, destroyed (skipped, never replaced), or already evacuated by the ingress: nothing
        // to clean and nothing stamped.
        return;
    }
    // The cleanup of the captured original only, chaining the ingress's checkpoint and provenance:
    // a revision moved since stops it at its entry; any outcome but current success ends the
    // setter with no retry, fallback, replacement target or refreshed checkpoint.
    const auto cleanup = previous->unmanageForAssignment(this, *checkpoint, owned);
    (void)cleanup;
}

void Window::setTileCompatibilityLegacy(Tile *tile)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const QPointer<Tile> target(tile);
    const bool hadTarget = tile;
    const auto geometryGeneration = geometryRequestGeneration();
    if (!cancelTransientRequest() || geometryRequestGeneration() != geometryGeneration || (hadTarget && !target)) {
        return;
    }
    tile = target.data();

    if (m_requestedTile == tile) {
        m_requestedTransientTileGeneration = 0;
        return;
    }
    setTileCompatibilityLegacyRemainder(tile);
}

void Window::setTileCompatibilityLegacyRemainder(Tile *tile)
{
    const QPointer<Tile> previousTile = m_requestedTile;
    if (tile && !tile->manage(this)) {
        // A refused destination must not release the previous assignment.
        return;
    }

    if (previousTile) {
        previousTile->unmanage(this);
    }
}

Window::TransientTileRequest::TransientTileRequest(Window *window, Tile *tile, quint64 ownershipGeneration)
    : m_window(window)
    , m_tile(tile)
    , m_manager(tile ? tile->manager() : nullptr)
    , m_output(m_manager ? m_manager->output() : nullptr)
    , m_desktop(tile ? tile->desktop() : nullptr)
    , m_floating(window ? window->moveResizeGeometry() : RectF())
    , m_windowId(window ? window->internalId() : QUuid())
    , m_ownerGeneration(ownershipGeneration)
    , m_initialGeometryGeneration(window ? window->geometryRequestGeneration() : 0)
{
    if (m_tile && m_manager && m_output && m_desktop) {
        m_target = m_tile->windowGeometry();
    }
}

bool Window::cancelTransientRequest()
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    invalidateTransientRestoreAttempt();
    m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
    const auto revision = m_placementContinuationRevision;
    if (m_activeTransientRequest) {
        m_activeTransientRequest->m_cancelled = true;
    }
    if (m_transientOwnership) {
        revokeTransientOwnership(m_transientOwnership->generation);
    }
    // This qualifies only the caller's continuation, not restoration completion.
    return !isDeleted() && revision != std::numeric_limits<quint64>::max()
        && m_placementContinuationRevision == revision;
}

Window::AssignmentStepResult Window::cancelTransientRequestForAssignment()
{
    AssignmentStepResult result;
    if (isDeleted()) {
        // Nothing can continue from a deleted window: stale, not refused.
        result.outcome = AssignmentStepOutcome::Superseded;
        return result;
    }
    if (m_placementContinuationRevision >= std::numeric_limits<quint64>::max() - 1) {
        // Exhaustion known before the stamp: refused without effect.
        return result;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // The stamp and the cancellation mark are this caller's own writes; from here nothing is a
    // refusal any more.
    invalidateTransientRestoreAttempt();
    m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
    if (m_activeTransientRequest) {
        m_activeTransientRequest->m_cancelled = true;
    }
    if (m_transientOwnership) {
        // The result enters as Refused and a release that runs always leaves its verdict; an
        // untouched result means no release was acquired (one is already running in an outer
        // frame, or the ownership is not its controller's to release): this frame's own writes are
        // complete and no callback ran in it, so the continuation starts here. The owner stays
        // for the consumer's entry refusal to decide.
        revokeTransientOwnership(m_transientOwnership->generation, &result);
        if (result.outcome == AssignmentStepOutcome::Refused) {
            result.checkpoint = makeTileAssignmentCheckpoint();
        }
    } else {
        result.checkpoint = makeTileAssignmentCheckpoint();
    }
    qualifyReleaseContinuation(result);
    return result;
}

Window::TransientOwnership::TransientOwnership(TransientPlacementWriter *writer, const QUuid &provider, quint64 providerRevision,
                                               quint64 generation, std::unique_ptr<TransientTileRequest> request)
    : writer(writer)
    , provider(provider)
    , providerRevision(providerRevision)
    , generation(generation)
    , floating(request->m_floating)
    , target(request->m_target)
    , tile(request->m_tile)
    , manager(request->m_manager)
    , output(request->m_output)
    , desktop(request->m_desktop)
    , initialGeometryGeneration(request->m_initialGeometryGeneration)
    , request(std::move(request))
{
}

quint64 Window::prepareTransientOwnership(TransientPlacementWriter *writer, const QUuid &provider, const QString &key,
                                          quint64 providerRevision, quint64 mapRevision, std::unique_ptr<TransientTileRequest> request)
{
    if (isDeleted() || m_transientOwnership || m_activeTransientRequest || !writer || !request
        || request->m_window != this || !writer->isProviderCurrent(provider, providerRevision)
        || !transientRequestTargetIsCurrent(*request) || !request->authorityIsCurrent()
        || !canStartTransientTileRequest(request->m_target)
        || m_geometryRequestGeneration != request->m_initialGeometryGeneration
        || !exactFrameGeometry(frameGeometry(), request->m_floating)
        || !exactFrameGeometry(moveResizeGeometry(), request->m_floating)) {
        return 0;
    }
    const auto previous = std::max(m_transientOwnershipGeneration, m_lastTransientOwnershipGeneration);
    if (previous == std::numeric_limits<quint64>::max()) {
        return 0;
    }
    const auto generation = m_transientOwnershipGeneration = previous + 1;
    request->m_ownerGeneration = generation;
    request->m_sequence = 0;
    auto ownership = std::make_unique<TransientOwnership>(writer, provider, providerRevision, generation, std::move(request));
    const auto result = writer->reserve(provider, {internalId(), QString::number(generation), ownership->floating, providerRevision, mapRevision, key});
    if (result != TransientPlacementWriter::Result::Accepted) {
        return 0;
    }
    // QObject::destroyed runs after Window's fields have gone: capture no Window state here.
    ownership->closeCleanup = connect(this, &QObject::destroyed, writer, [writer, id = internalId()]() {
        if (!writer->forgetWindow(id)) {
            qCWarning(KWIN_CORE) << "Failed to remove transient placement tracking for" << id;
        }
    });
    m_transientOwnership = std::move(ownership);
    return generation;
}

bool Window::activateTransientOwnership(quint64 generation)
{
    if (isDeleted() || !m_transientOwnership || m_transientOwnership->generation != generation
        || m_transientOwnership->phase != TransientOwnershipPhase::Prepared) {
        return false;
    }
    auto &ownership = *m_transientOwnership;
    const auto &request = *ownership.request;
    if (!ownership.writer || !ownership.writer->isProviderCurrent(ownership.provider, ownership.providerRevision)
        || !transientRequestTargetIsCurrent(request) || !request.authorityIsCurrent()
        || !canStartTransientTileRequest(request.m_target)
        || m_geometryRequestGeneration != request.m_initialGeometryGeneration
        || !exactFrameGeometry(frameGeometry(), ownership.floating)
        || !exactFrameGeometry(moveResizeGeometry(), ownership.floating)
        || generation <= m_lastTransientOwnershipGeneration
        || m_transientRequestSequence == std::numeric_limits<quint64>::max()
        || m_geometryRequestGeneration == std::numeric_limits<quint64>::max()) {
        return false;
    }
    if (ownership.writer->activateReservation(ownership.provider, internalId(), QString::number(generation)) != TransientPlacementWriter::Result::Accepted) {
        return false;
    }
    // Seal the pre-request attempt before membership can emit its first notification.
    ownership.nativeSequence = ++m_transientRequestSequence;
    ownership.request->m_sequence = ownership.nativeSequence;
    m_lastTransientOwnershipGeneration = generation;
    ownership.phase = TransientOwnershipPhase::Owned;
    return true;
}

Window::TransientRequestReceipt Window::applyTransientOwnership(quint64 generation)
{
    return applyTransientOwnershipGuarded(generation).receipt;
}

Window::TransientApplication Window::applyTransientOwnershipGuarded(quint64 generation, std::function<bool()> admissionConstraint)
{
    TransientApplication application;
    if (isDeleted() || !m_transientOwnership || m_transientOwnership->generation != generation
        || m_transientOwnership->phase != TransientOwnershipPhase::Owned || m_transientOwnership->executing
        || !m_transientOwnership->request) {
        // Nothing started: the continuation stays Refused, without a checkpoint.
        return application;
    }
    const auto conclude = [&application]() {
        // An attempt that ended without a current application; its checkpoint and provenance are
        // not evidence of anything, and nothing is refreshed.
        application.continuation.outcome = AssignmentStepOutcome::Superseded;
        application.continuation.checkpoint.reset();
        application.continuation.owned = {};
    };
    if (!m_transientOwnership->writer
        || !m_transientOwnership->writer->isProviderCurrent(m_transientOwnership->provider, m_transientOwnership->providerRevision)
        || !m_transientOwnership->writer->ownsReservation(internalId(), QString::number(generation))) {
        revokeTransientOwnership(generation);
        conclude();
        return application;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    m_transientOwnership->executing = true;
    application.receipt = requestTransientTile(std::move(m_transientOwnership->request), &application.continuation, admissionConstraint);
    // The request is destroyed with that call's argument, before the execution ends: its
    // destructor runs while the owner is still executing, and the terminal validation below sees
    // whatever it changed. Revocation during the call retains the record until its authoritative
    // progress returns.
    m_transientOwnership->executing = false;
    m_transientOwnership->progress = application.receipt.progress;
    if (application.receipt.outcome != TransientRequestOutcome::Applied || m_transientOwnership->phase != TransientOwnershipPhase::Owned) {
        revokeTransientOwnership(generation);
        conclude();
        return application;
    }
    // The terminal validation, after the request's destruction and the end of the execution: the
    // preserved checkpoint and the stable provenance against the record and the window as they
    // are now. Current success is exported from here only; the ownership itself is left as the
    // receipt describes it, the caller's continuation decides what follows.
    if (assignmentResultIsCurrent(application.continuation)) {
        application.continuation.outcome = AssignmentStepOutcome::AppliedCurrent;
    } else {
        conclude();
    }
    return application;
}

void Window::invalidateTransientRestoreAttempt()
{
    if (m_transientOwnership && m_transientOwnership->restoreAttempt) {
        m_transientOwnership->restoreAttempt->cancelled = true;
    }
}

bool Window::transientRestoreAttemptIsCurrent(quint64 generation, quint64 sequence) const
{
    if (isDeleted() || !generation || !sequence || !m_transientOwnership
        || m_transientOwnership->generation != generation || !m_transientOwnership->restoreAttempt) {
        return false;
    }
    const auto &ownership = *m_transientOwnership;
    const auto &attempt = *ownership.restoreAttempt;
    if (attempt.sequence != sequence || attempt.cancelled || ownership.phase != TransientOwnershipPhase::Restoring
        || ownership.executing || ownership.releasing || ownership.request || m_activeTransientRequest
        || ownership.progress != TransientRequestProgress::Requested || m_requestedTile || m_tile || m_requestedTransientTileGeneration) {
        return false;
    }
    if (!ownership.writer || !ownership.writer->isProviderCurrent(ownership.provider, ownership.providerRevision)
        || !ownership.writer->ownsRestoringReservation(internalId(), QString::number(generation))) {
        return false;
    }
    // Bind this Window's membership, not global desktop/activity navigation. The detached tree
    // and controller need not survive. R3 supplies the geometry request's additional constraints.
    return ownership.output && ownership.desktop && workspace()->outputs().contains(ownership.output)
        && VirtualDesktopManager::self()->desktops().contains(ownership.desktop)
        && output() == ownership.output && moveResizeOutput() == ownership.output
        && desktops() == QList<VirtualDesktop *>{ownership.desktop} && activities() == attempt.activities
        && attempt.stateIsCurrent && attempt.stateIsCurrent();
}

bool Window::transientRestoreClaimIsCurrent(const TransientRestoreClaim &claim) const
{
    return claim.windowId == internalId() && claim.continuation.outcome == AssignmentStepOutcome::AppliedCurrent
        && transientRestoreAttemptIsCurrent(claim.ownershipGeneration, claim.sequence)
        && assignmentResultIsCurrent(claim.continuation);
}

Window::TransientRestoreClaim Window::claimTransientRestore(quint64 generation)
{
    if (!m_transientOwnership || !isTransientTileTargetFeasible(m_transientOwnership->floating)) {
        return {};
    }
    return claimTransientRestoreWithState(generation, false, false, transientRestoreStateConstraint(m_transientOwnership->floating, false, true));
}

Window::TransientRestoreConsumption Window::consumeExplicitUntileOrRestore()
{
    if (!m_transientOwnership) {
        return TransientRestoreConsumption::NotOwned;
    }
    if (m_transientOwnership->phase == TransientOwnershipPhase::Restoring) {
        // A restoration or release already in flight consumes the action; the ordinary ladder
        // never runs for a record that owns or is releasing the placement.
        return TransientRestoreConsumption::RestoreInFlight;
    }
    if (m_transientOwnership->phase != TransientOwnershipPhase::Owned || m_transientOwnership->restoreAttempt) {
        return m_transientOwnership->restoreAttempt ? TransientRestoreConsumption::RestoreInFlight
                                                    : TransientRestoreConsumption::NotOwned;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    auto claim = claimTransientRestoreWithMaximizeNormalization(m_transientOwnership->generation);
    if (claim.continuation.outcome != AssignmentStepOutcome::AppliedCurrent) {
        // A refusal before any mutation leaves everything unchanged: no start. A claim that
        // lost its authority after the revoke prefix (the attempt exists, the record is
        // Restoring) is a stop whose writes stand, not a clean no-start.
        if (m_transientOwnership && m_transientOwnership->restoreAttempt) {
            return TransientRestoreConsumption::RestoreStopped;
        }
        return TransientRestoreConsumption::NoStart;
    }
    const auto request = requestTransientRestore(std::move(claim));
    if (request.result.outcome == AssignmentStepOutcome::AppliedCurrent) {
        return TransientRestoreConsumption::RestoreStarted;
    }
    return TransientRestoreConsumption::RestoreStopped;
}

Window::TransientRestoreClaim Window::claimTransientRestoreWithMaximizeNormalization(quint64 generation)
{
    return {};
}

std::function<bool()> Window::transientRestoreStateConstraint(const RectF &target, bool normalizeMaximize, bool geometryReady) const
{
    return {};
}

Window::TransientRestoreClaim Window::claimTransientRestoreWithState(quint64 generation, bool normalizeMaximize, bool normalizeFullScreen, std::function<bool()> stateIsCurrent)
{
    TransientRestoreClaim claim;
    const auto exhausted = std::numeric_limits<quint64>::max();
    if (!transientOwnershipIsCurrent(generation) || m_transientOwnership->restoreAttempt
        || m_transientOwnership->progress != TransientRequestProgress::Requested || m_transientOwnership->request
        || output() != m_transientOwnership->output || activities().size() > 1
        || !stateIsCurrent || !stateIsCurrent()
        || m_transientRestoreSequence >= exhausted - 1 || m_tileMembershipChange >= exhausted - 1
        || m_requestedTileChange >= exhausted - 1 || m_committedTileChange >= exhausted - 1
        || m_placementContinuationRevision == exhausted || m_geometryRequestGeneration == exhausted) {
        return claim;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    claim.windowId = internalId();
    claim.ownershipGeneration = generation;
    claim.sequence = ++m_transientRestoreSequence;
    m_transientOwnership->restoreAttempt.emplace(claim.sequence, activities(), normalizeMaximize, normalizeFullScreen, std::move(stateIsCurrent));
    claim.continuation.outcome = AssignmentStepOutcome::Superseded;
    // The claim is published before any detach notification. Do not retain a reference into the
    // record across this call: callbacks can close the Window or invalidate its writer/context.
    revokeTransientOwnership(generation, &claim.continuation);
    if (claim.continuation.outcome != AssignmentStepOutcome::AppliedCurrent
        || !transientRestoreAttemptIsCurrent(generation, claim.sequence) || !assignmentResultIsCurrent(claim.continuation)) {
        if (m_transientOwnership && m_transientOwnership->generation == generation && m_transientOwnership->restoreAttempt
            && m_transientOwnership->restoreAttempt->sequence == claim.sequence) {
            invalidateTransientRestoreAttempt();
        }
        claim.continuation.outcome = AssignmentStepOutcome::Superseded;
        claim.continuation.checkpoint.reset();
    }
    return claim;
}

Window::ConfigurePlacementPolicy Window::configurePlacementPolicy(const GeometryRequestOrigin &origin) const
{
    if (isDeleted()) {
        return ConfigurePlacementPolicy::ContentOnly;
    }
    if (!m_transientOwnership || !m_transientOwnership->restoreAttempt) {
        // Preserve ordinary and owned behavior outside explicit restoration. A lost restore
        // record must not turn its old request into ordinary placement authority.
        return origin.kind == GeometryRequestOrigin::Kind::Restoration
            ? ConfigurePlacementPolicy::ContentOnly
            : ConfigurePlacementPolicy::Synchronize;
    }
    if (!origin.geometryGeneration || origin.geometryGeneration != m_geometryRequestGeneration || origin != m_geometryRequestOrigin) {
        return ConfigurePlacementPolicy::ContentOnly;
    }
    if (origin.kind == GeometryRequestOrigin::Kind::Ordinary) {
        return ConfigurePlacementPolicy::Synchronize;
    }
    if (origin.kind == GeometryRequestOrigin::Kind::Restoration
        && transientRestoreRequestIsCurrent(origin.ownershipGeneration, origin.restoreSequence)) {
        return ConfigurePlacementPolicy::PlacementOnly;
    }
    return ConfigurePlacementPolicy::ContentOnly;
}

Window::AssignmentStepResult Window::synchronizeConfigureGeometry(RectF desired, GeometryRequestOrigin origin, std::function<bool()> dispatchIsCurrent)
{
    AssignmentStepResult refused;
    if (!dispatchIsCurrent || !dispatchIsCurrent() || configurePlacementPolicy(origin) != ConfigurePlacementPolicy::Synchronize) {
        refused.outcome = AssignmentStepOutcome::Superseded;
        return refused;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    AssignmentGeometrySync sync;
    sync.owned = OwnedProvenance().withConstraint([this, origin, dispatchIsCurrent = std::move(dispatchIsCurrent)]() {
        return dispatchIsCurrent() && configurePlacementPolicy(origin) == ConfigurePlacementPolicy::Synchronize;
    });
    sync.result.owned = sync.owned;
    sync.result.outcome = AssignmentStepOutcome::Superseded;
    sync.result.checkpoint.reset(new TileAssignmentFence(*this));
    auto writes = prepareAssignmentGeometrySync(desired, sync);
    if (sync.output == m_moveResizeOutput && !m_outputPropertiesPending) {
        // Native content synchronization preserves the legacy same-output fast path. An
        // intentional next scale is reconciled by a geometry request, not by this commit.
        sync.synchronize = false;
        writes = m_moveResizeGeometry != desired;
    }
    if (!assignmentGeometrySyncIsCurrent(sync)) {
        return std::move(sync.result);
    }
    if (writes > std::numeric_limits<quint64>::max() - 1 - m_placementContinuationRevision) {
        return refused;
    }
    setMoveResizeGeometryInternal(desired, &sync);
    if (assignmentGeometrySyncIsCurrent(sync)) {
        sync.result.outcome = AssignmentStepOutcome::AppliedCurrent;
    }
    return std::move(sync.result);
}

bool Window::transientRestoreOperationIsCurrent(quint64 generation, quint64 sequence) const
{
    if (!transientRestoreAttemptIsCurrent(generation, sequence)) {
        return false;
    }
    const auto &ownership = *m_transientOwnership;
    const auto &attempt = *ownership.restoreAttempt;
    if (!attempt.requestConsumed()) {
        return false;
    }
    if (attempt.stage == TransientRestoreStage::Normalizing) {
        return !attempt.requestGeneration;
    }
    if (!canStartTransientRestoreRequest(ownership.floating)
        || workspace()->outputAt(ownership.floating.center()) != ownership.output) {
        return false;
    }
    if (!attempt.requestGeneration) {
        return attempt.requesting();
    }
    return attempt.requestGeneration == m_geometryRequestGeneration
        && exactFrameGeometry(moveResizeGeometry(), ownership.floating);
}

bool Window::transientRestoreRequestIsCurrent(quint64 generation, quint64 sequence) const
{
    return transientRestoreOperationIsCurrent(generation, sequence)
        && m_transientOwnership->restoreAttempt->stage != TransientRestoreStage::Normalizing
        && m_transientOwnership->restoreAttempt->requestGeneration;
}

Window::AssignmentStepResult Window::normalizeTransientRestore(AssignmentStepResult continuation, quint64 generation, quint64 sequence)
{
    if (continuation.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(continuation)
        || !transientRestoreOperationIsCurrent(generation, sequence)
        || m_transientOwnership->restoreAttempt->normalizeMaximize || m_transientOwnership->restoreAttempt->normalizeFullScreen) {
        continuation.outcome = AssignmentStepOutcome::Superseded;
        continuation.checkpoint.reset();
    }
    return continuation;
}

Window::AssignmentGeometryRequest Window::requestTransientRestore(TransientRestoreClaim claim)
{
    AssignmentGeometryRequest request;
    const auto generation = claim.ownershipGeneration;
    const auto sequence = claim.sequence;
    if (claim.windowId != internalId() || !m_transientOwnership || m_transientOwnership->generation != generation
        || !m_transientOwnership->restoreAttempt || m_transientOwnership->restoreAttempt->sequence != sequence) {
        request.result.outcome = AssignmentStepOutcome::Superseded;
        return request;
    }
    if (m_transientOwnership->restoreAttempt->requestConsumed()) {
        return request;
    }
    if (!transientRestoreClaimIsCurrent(claim)) {
        m_transientOwnership->restoreAttempt->stage = TransientRestoreStage::Returned;
        invalidateTransientRestoreAttempt();
        request.result.outcome = AssignmentStepOutcome::Superseded;
        return request;
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    const RectF target = m_transientOwnership->floating;
    m_transientOwnership->restoreAttempt->stage = TransientRestoreStage::Normalizing;
    {
        const auto finish = qScopeGuard([this, generation, sequence]() {
            if (m_transientOwnership && m_transientOwnership->generation == generation && m_transientOwnership->restoreAttempt
                && m_transientOwnership->restoreAttempt->sequence == sequence) {
                m_transientOwnership->restoreAttempt->stage = TransientRestoreStage::Returned;
            }
        });
        claim.continuation.owned = OwnedProvenance::forRestoration(generation, sequence).withConstraint(std::move(claim.continuation.owned.constraint));
        auto normalized = normalizeTransientRestore(std::move(claim.continuation), generation, sequence);
        if (normalized.outcome == AssignmentStepOutcome::AppliedCurrent && assignmentResultIsCurrent(normalized)
            && transientRestoreOperationIsCurrent(generation, sequence)) {
            m_transientOwnership->restoreAttempt->stage = TransientRestoreStage::Dispatching;
            request = requestMoveResizeGuarded(target, MoveResizeMode::MoveResize, *normalized.checkpoint, normalized.owned);
        } else {
            request.result = std::move(normalized);
        }
    }
    // The input claim may be destroyed on return; only value-bound provenance survives it.
    // A child refusal is now a consumed failed attempt, not a retryable mutation-free refusal.
    if (request.result.outcome != AssignmentStepOutcome::AppliedCurrent || request.dispatch == AssignmentRequestDispatch::None
        || !request.requestGeneration || !assignmentResultIsCurrent(request.result)) {
        if (m_transientOwnership && m_transientOwnership->generation == generation && m_transientOwnership->restoreAttempt
            && m_transientOwnership->restoreAttempt->sequence == sequence) {
            invalidateTransientRestoreAttempt();
        }
        request.result.outcome = AssignmentStepOutcome::Superseded;
        request.result.checkpoint.reset();
    }
    return request;
}

bool Window::completeTransientRestoration(const GeometryRequestOrigin &origin)
{
    // NT04's native completion boundary: only the live restoration's own acknowledged and
    // committed configure completes it. The origin's generation, ownership and sequence must all
    // be current; a cancelled or superseded attempt, or an older configure's late
    // acknowledgement, never finalizes anything. ACK alone, equal geometry or elapsed time are
    // not the boundary (docs/NT03-RESTORATION-SPEC.md, "Completion boundary").
    if (isDeleted() || !m_transientOwnership || !m_transientOwnership->restoreAttempt) {
        return false;
    }
    if (origin.kind != GeometryRequestOrigin::Kind::Restoration
        || !origin.geometryGeneration || origin.geometryGeneration != m_geometryRequestGeneration
        || origin != m_geometryRequestOrigin
        || !transientRestoreRequestIsCurrent(origin.ownershipGeneration, origin.restoreSequence)) {
        return false;
    }
    const auto generation = origin.ownershipGeneration;
    // The currency predicate already guarantees a live writer holding the restoring reservation.
    if (const auto writer = m_transientOwnership->writer) {
        if (writer->completeRestore(internalId(), QString::number(generation)) != TransientPlacementWriter::Result::Accepted) {
            // The reservation's own validation refused: nothing is cleared, the boundary did
            // not happen, and a later qualifying commit can complete again.
            return false;
        }
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // Unlike a no-request finalization, the completion installed a restore watermark that must
    // keep suppressing stale provider events until this window is destroyed; the close cleanup
    // stays connected so the writer forgets the tracked record (and releases the key for a new
    // window) exactly then.
    m_transientOwnership.reset();
    return true;
}

bool Window::revokeTransientOwnership(quint64 generation, AssignmentStepResult *guarded)
{
    if (!m_transientOwnership || m_transientOwnership->generation != generation) {
        return false;
    }
    if (const auto controller = m_transientOwnership->controller) {
        return controller->releaseWindow(this, generation, guarded);
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    if (!beginTransientRelease(generation)) {
        return false;
    }
    const auto changes = detachTransientAssociation(generation);
    if (guarded) {
        // After this release's own callback-free writes, before its notifications; never refreshed.
        guarded->checkpoint = makeTileAssignmentCheckpoint();
    }
    notifyTransientAssociation(generation, changes);
    const bool finalized = finishTransientRelease(generation);
    if (guarded) {
        qualifyReleaseContinuation(*guarded);
    }
    return finalized;
}

void Window::qualifyReleaseContinuation(AssignmentStepResult &guarded) const
{
    // One terminal validation of the checkpoint taken after the release's writes: the ownership's
    // deletion itself is not fenced and needs no replacement checkpoint.
    if (!isDeleted() && guarded.checkpoint && guarded.checkpoint->isCurrentFor(*this)) {
        guarded.outcome = AssignmentStepOutcome::AppliedCurrent;
    } else {
        guarded.outcome = AssignmentStepOutcome::Superseded;
        guarded.checkpoint.reset();
    }
}

bool Window::beginTransientRelease(quint64 generation)
{
    if (!m_transientOwnership || m_transientOwnership->generation != generation || m_transientOwnership->releasing) {
        return false;
    }
    auto &ownership = *m_transientOwnership;
    ownership.releasing = true;
    ownership.releaseWriterReady = bool(ownership.writer);
    if (ownership.phase != TransientOwnershipPhase::Prepared) {
        ownership.phase = TransientOwnershipPhase::Restoring;
        if (m_activeTransientRequest) {
            m_activeTransientRequest->m_cancelled = true;
        }
        ownership.releaseWriterReady = ownership.writer
            && ownership.writer->beginRestore(internalId(), QString::number(generation)) == TransientPlacementWriter::Result::Accepted;
    }
    return true;
}

Window::TransientAssociationChanges Window::detachTransientAssociation(quint64 generation)
{
    TransientAssociationChanges changes;
    if (!m_transientOwnership || m_transientOwnership->generation != generation || !m_transientOwnership->releasing) {
        return changes;
    }
    auto &ownership = *m_transientOwnership;
    const auto owned = ownership.tile;
    const auto oldMode = quickTileMode();
    if (m_requestedTransientTileGeneration == generation && (m_requestedTile == owned || !m_requestedTile)) {
        m_requestedTile.clear();
        m_requestedTransientTileGeneration = 0;
        m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
        changes.requestedChanged = true;
    }
    if ((owned && m_tile == owned) || (!owned && !m_tile && ownership.committed)) {
        m_tile.clear();
        m_committedTileChange = nextAssociationChange(m_committedTileChange);
        changes.committedChanged = true;
    }
    changes.modeChanged = changes.committedChanged && (oldMode != quickTileMode() || (!owned && ownership.committed));
    ownership.committed = false;
    changes.requested = m_requestedTile;
    changes.committed = m_tile;
    changes.requestedChange = m_requestedTileChange;
    changes.committedChange = m_committedTileChange;
    return changes;
}

void Window::notifyTransientAssociation(quint64 generation, const TransientAssociationChanges &changes)
{
    const auto current = [&]() {
        return !isDeleted() && m_transientOwnership && m_transientOwnership->generation == generation
            && m_transientOwnership->releasing;
    };
    const auto requestedCurrent = [&]() {
        return current() && changes.requestedChange != std::numeric_limits<quint64>::max()
            && m_requestedTileChange == changes.requestedChange && m_requestedTile == changes.requested;
    };
    const auto committedCurrent = [&]() {
        return current() && changes.committedChange != std::numeric_limits<quint64>::max()
            && m_committedTileChange == changes.committedChange && m_tile == changes.committed;
    };
    if (changes.requestedChanged && requestedCurrent()) {
        doSetQuickTileMode();
        if (requestedCurrent()) {
            Q_EMIT requestedTileChanged();
        }
    }
    if (changes.committedChanged && committedCurrent()) {
        Q_EMIT tileChanged(changes.committed);
    }
    if (changes.modeChanged && committedCurrent()) {
        Q_EMIT quickTileModeChanged();
    }
}

bool Window::finishTransientRelease(quint64 generation)
{
    if (!m_transientOwnership || m_transientOwnership->generation != generation || !m_transientOwnership->releasing) {
        return false;
    }
    auto &ownership = *m_transientOwnership;
    ownership.releasing = false;
    if (isDeleted() || !ownership.writer || !ownership.releaseWriterReady) {
        return false;
    }
    if (ownership.phase == TransientOwnershipPhase::Prepared) {
        if (ownership.writer->cancelReservation(internalId(), QString::number(generation)) != TransientPlacementWriter::Result::Accepted) {
            return false;
        }
    } else {
        if (ownership.executing || ownership.progress != TransientRequestProgress::NoRequest) {
            return false;
        }
        if (!ownership.noRequestProof && ownership.request) {
            ownership.noRequestProof = makeTransientNoRequestProof(*ownership.request);
        }
        if (ownership.writer->finalizeNoRequest(ownership.noRequestProof) != TransientPlacementWriter::Result::Accepted) {
            return false;
        }
    }
    disconnect(ownership.closeCleanup);
    m_transientOwnership.reset();
    return true;
}

std::shared_ptr<const TransientNoRequestProof> Window::makeTransientNoRequestProof(const TransientTileRequest &request)
{
    if (request.m_window != this || request.m_windowId != internalId() || !request.m_sequence
        || !request.m_ownerGeneration || request.m_progress != TransientRequestProgress::NoRequest) {
        return {};
    }
    if (m_transientOwnership) {
        if (m_transientOwnership->generation != request.m_ownerGeneration
            || m_transientOwnership->nativeSequence != request.m_sequence
            || m_transientOwnership->progress != TransientRequestProgress::NoRequest) {
            return {};
        }
        if (m_transientOwnership->noRequestProof) {
            return m_transientOwnership->noRequestProof;
        }
    }
    const QPointer<Window> window(this);
    const auto sequence = request.m_sequence;
    auto proof = std::shared_ptr<const TransientNoRequestProof>(new TransientNoRequestProof(
        request.m_windowId, request.m_ownerGeneration, sequence, [window, sequence]() {
        if (!window || window->isDeleted()) {
            return TransientNoRequestProof::Validity::Closing;
        }
        if (window->m_transientRequestSequence != sequence) {
            return TransientNoRequestProof::Validity::Superseded;
        }
        return TransientNoRequestProof::Validity::Current;
    }));
    if (m_transientOwnership) {
        // Internal finalization and returned receipts share this attempt's single consumed bit.
        m_transientOwnership->noRequestProof = proof;
    }
    return proof;
}

bool Window::canStartTransientTileRequest(const RectF &target) const
{
    return false;
}

bool Window::canStartTransientRestoreRequest(const RectF &target) const
{
    return false;
}

bool Window::isTransientTileTargetFeasible(const RectF &target) const
{
    return false;
}

bool Window::transientRequestTargetIsCurrent(const TransientTileRequest &request) const
{
    if (request.m_window != this || !request.m_tile || !request.m_manager || !request.m_output || !request.m_desktop
        || !request.m_tile->isTransient() || request.m_tile->manager() != request.m_manager
        || request.m_manager->tearingDown() || request.m_manager->output() != request.m_output
        || request.m_tile->desktop() != request.m_desktop
        || !workspace()->outputs().contains(request.m_output)
        || !VirtualDesktopManager::self()->desktops().contains(request.m_desktop)
        || moveResizeOutput() != request.m_output || !request.m_tile->isActive() || !isOnDesktop(request.m_desktop)) {
        return false;
    }
    return exactFrameGeometry(request.m_target, request.m_tile->windowGeometry())
        && request.m_output->geometryF().contains(request.m_target)
        && isTransientTileTargetFeasible(request.m_target);
}

bool Window::transientRequestIsCurrent(const TransientTileRequest &request) const
{
    if (m_transientOwnership
        && (m_transientOwnership->generation != request.m_ownerGeneration
            || m_transientOwnership->phase != TransientOwnershipPhase::Owned || !m_transientOwnership->writer
            || !m_transientOwnership->writer->isProviderCurrent(m_transientOwnership->provider, m_transientOwnership->providerRevision)
            || !m_transientOwnership->writer->ownsReservation(internalId(), QString::number(request.m_ownerGeneration)))) {
        return false;
    }
    if (isDeleted() || m_activeTransientRequest != &request || request.m_cancelled
        || request.m_sequence != m_transientRequestSequence || !transientRequestTargetIsCurrent(request)
        || !request.authorityIsCurrent()) {
        return false;
    }
    if (request.m_associationInstalled) {
        if (m_requestedTile != request.m_tile || (m_tile && m_tile != request.m_tile)) {
            return false;
        }
    } else if (m_requestedTile || m_tile) {
        return false;
    }
    if (request.m_progress == TransientRequestProgress::NoRequest) {
        return m_geometryRequestGeneration == request.m_initialGeometryGeneration
            && exactFrameGeometry(moveResizeGeometry(), request.m_floating)
            && exactFrameGeometry(frameGeometry(), request.m_floating);
    }
    return m_geometryRequestGeneration == request.m_geometryGeneration
        && exactFrameGeometry(moveResizeGeometry(), request.m_target);
}

bool Window::transientGeometryRequestIsCurrent(quint64 generation) const
{
    if (!m_activeTransientRequest || m_activeTransientRequest->m_geometryGeneration != generation) {
        return true;
    }
    return transientRequestIsCurrent(*m_activeTransientRequest);
}

bool Window::transientAdmissionEligible(const RectF &target) const
{
    return !isDeleted() && !m_transientOwnership && !m_activeTransientRequest && !m_requestedTile && !m_tile
        && !m_requestedTransientTileGeneration && supportsAssignmentGeometryRequest() && canStartTransientTileRequest(target)
        && exactFrameGeometry(frameGeometry(), moveResizeGeometry());
}

bool Window::transientAdmissionHeadroom() const
{
    const auto exhausted = std::numeric_limits<quint64>::max();
    return m_tileMembershipChange < exhausted - 2 && m_requestedTileChange < exhausted - 2 && m_committedTileChange < exhausted - 1
        && m_placementContinuationRevision < exhausted - assignmentPlacementBudget
        && m_geometryRequestGeneration < exhausted - 1 && m_geometryUpdateGeneration < exhausted - 1
        && std::max(m_transientOwnershipGeneration, m_lastTransientOwnershipGeneration) < exhausted
        && m_transientRequestSequence < exhausted;
}

Window::TransientRequestReceipt Window::requestTransientTile(std::unique_ptr<TransientTileRequest> request)
{
    return requestTransientTile(std::move(request), nullptr, {});
}

Window::TransientRequestReceipt Window::requestTransientTile(std::unique_ptr<TransientTileRequest> request, AssignmentStepResult *continuation,
                                                             const std::function<bool()> &admissionConstraint)
{
    if (!request) {
        return {};
    }
    const auto receipt = [&](TransientRequestOutcome outcome) {
        std::shared_ptr<const TransientNoRequestProof> proof;
        if (request->m_sequence && request->m_progress == TransientRequestProgress::NoRequest && outcome != TransientRequestOutcome::Applied) {
            proof = makeTransientNoRequestProof(*request);
        }
        return TransientRequestReceipt{outcome, request->m_progress, request->m_ownerGeneration,
                                       request->m_sequence, request->m_windowId, std::move(proof)};
    };
    const bool prepared = m_transientOwnership && m_transientOwnership->executing
        && m_transientOwnership->generation == request->m_ownerGeneration
        && m_transientOwnership->nativeSequence == request->m_sequence;
    if (isDeleted() || m_activeTransientRequest || request->m_window != this || !request->m_ownerGeneration
        || (m_transientOwnership && !prepared)
        || (!prepared && (request->m_ownerGeneration <= m_lastTransientOwnershipGeneration || m_transientRequestSequence == std::numeric_limits<quint64>::max()))
        || (prepared && (request->m_sequence != m_transientRequestSequence || m_transientOwnership->phase != TransientOwnershipPhase::Owned))
        // The owned geometry step reserves one request generation below the exhausted value, and
        // the association needs its install and a possible removal; refuse both here, before any effect.
        || m_geometryRequestGeneration >= std::numeric_limits<quint64>::max() - 1
        || m_requestedTileChange >= std::numeric_limits<quint64>::max() - 1) {
        return receipt(TransientRequestOutcome::Rejected);
    }
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    // Consume ownership identity before preflight: rejection must not erase earlier Requested history.
    if (!prepared) {
        m_lastTransientOwnershipGeneration = request->m_ownerGeneration;
        request->m_sequence = ++m_transientRequestSequence;
    }
    // A backend without a guarded dispatcher is refused here rather than after the restore and
    // association writes, which the owned geometry step must never leave behind as a refusal.
    if (!transientRequestTargetIsCurrent(*request) || !request->authorityIsCurrent()
        || !canStartTransientTileRequest(request->m_target) || !supportsAssignmentGeometryRequest()
        || m_geometryRequestGeneration != request->m_initialGeometryGeneration
        || !exactFrameGeometry(frameGeometry(), request->m_floating)
        || !exactFrameGeometry(moveResizeGeometry(), request->m_floating)) {
        return receipt(TransientRequestOutcome::Rejected);
    }
    m_activeTransientRequest = request.get();
    bool applied = false;
    const auto finish = qScopeGuard([&]() {
        // Remove only this attempt's association. Newer native placement and geometry win.
        if (!applied && request->m_associationInstalled && request->m_tile && m_requestedTile == request->m_tile
            && m_requestedTransientTileGeneration == request->m_ownerGeneration) {
            m_requestedTile = nullptr;
            m_requestedTransientTileGeneration = 0;
            m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
            if (!isDeleted()) {
                doSetQuickTileMode();
                if (!isDeleted()) {
                    Q_EMIT requestedTileChanged();
                }
            }
        }
        if (m_activeTransientRequest == request.get()) {
            m_activeTransientRequest = nullptr;
        }
    });
    // From here on every step borrows this request: a lapse of its external authority, like any
    // Window change, ends the attempt where it stands. Nothing is rolled back or refreshed.
    const auto restore = setGeometryRestoreForAssignment(request->m_floating, makeTileAssignmentFence(), OwnedProvenance::forRequest(request.get()));
    if (restore.outcome == AssignmentStepOutcome::Refused) {
        // The restore writer refuses only for counter headroom, before any effect.
        return receipt(TransientRequestOutcome::Rejected);
    }
    if (restore.outcome != AssignmentStepOutcome::AppliedCurrent || !canStartTransientTileRequest(request->m_target)) {
        return receipt(TransientRequestOutcome::Superseded);
    }
    // The association is installed inline: no callback separates these writes from the checkpoint
    // taken right after them, the fence already observes the association revision, and preflight
    // reserved the headroom for this install and its possible removal.
    m_requestedTile = request->m_tile;
    m_requestedTransientTileGeneration = request->m_ownerGeneration;
    m_requestedTileChange = nextAssociationChange(m_requestedTileChange);
    request->m_associationInstalled = true;
    const TileAssignmentFence installed(*this);
    // Not const: the continuation takes its checkpoint.
    auto geometry = requestMoveResizeForAssignment(*request, installed);
    // The geometry result's checkpoint and borrowed request validate the rest of this attempt:
    // a notification handler that changes the restore field or any other fenced state, with the
    // request's authority intact, supersedes it as well.
    if (geometry.result.outcome != AssignmentStepOutcome::AppliedCurrent || !assignmentResultIsCurrent(geometry.result)) {
        return receipt(TransientRequestOutcome::Superseded);
    }
    doSetQuickTileMode();
    if (!assignmentResultIsCurrent(geometry.result)) {
        return receipt(TransientRequestOutcome::Superseded);
    }
    Q_EMIT requestedTileChanged();
    if (!assignmentResultIsCurrent(geometry.result)) {
        return receipt(TransientRequestOutcome::Superseded);
    }
    applied = true;
    if (continuation) {
        // Validated through the borrowed request up to here; translated now, while it exists.
        translateTransientContinuation(*request, geometry.result, admissionConstraint, *continuation);
    }
    return receipt(TransientRequestOutcome::Applied);
}

void Window::translateTransientContinuation(const TransientTileRequest &request, AssignmentStepResult &validated,
                                            const std::function<bool()> &admissionConstraint, AssignmentStepResult &continuation) const
{
    // The checkpoint the request validated itself against, preserved exactly: nothing is minted
    // after the request's callbacks. The borrowed authority is replaced by what survives the
    // request: the ownership generation's own currentness (an Owned record for exactly that
    // generation, not executing or releasing, no active request, its writer, provider and
    // reservation valid, its tile live with this window as member in the recorded context, the
    // association unchanged), this attempt's native sequence and progress on that record, and the
    // post-request clauses of the request's own currentness by value (its allocated request
    // generation and its target, or the initial generation and the floating frame for an attempt
    // that requested nothing). The initial readiness predicate is not reused: the association is
    // installed on purpose. The caller's admission constraint is chained; the provenance exempts
    // the owner from nothing it did not already have.
    continuation.checkpoint = std::move(validated.checkpoint);
    const quint64 sequence = request.m_sequence;
    const TransientRequestProgress progress = request.m_progress;
    const quint64 geometryGeneration = request.m_geometryGeneration;
    const quint64 initialGeneration = request.m_initialGeometryGeneration;
    const RectF target = request.m_target;
    const RectF floating = request.m_floating;
    std::function<bool()> attempt = [this, sequence, progress, geometryGeneration, initialGeneration, target, floating]() {
        if (!m_transientOwnership || m_transientOwnership->nativeSequence != sequence || m_transientOwnership->progress != progress) {
            return false;
        }
        if (progress == TransientRequestProgress::NoRequest) {
            return m_geometryRequestGeneration == initialGeneration && exactFrameGeometry(moveResizeGeometry(), floating)
                && exactFrameGeometry(frameGeometry(), floating);
        }
        return m_geometryRequestGeneration == geometryGeneration && exactFrameGeometry(moveResizeGeometry(), target);
    };
    continuation.owned = OwnedProvenance::forOwnership(request.m_ownerGeneration, std::move(attempt));
    if (admissionConstraint) {
        continuation.owned = continuation.owned.withConstraint(admissionConstraint);
    }
    // Provisional: the terminal validation after the request's destruction promotes it.
    continuation.outcome = AssignmentStepOutcome::Superseded;
}

void Window::doSetQuickTileMode()
{
}

void Window::doSetHidden()
{
}

void Window::doSetHiddenByShowDesktop()
{
}

RectF Window::moveToArea(const RectF &geometry, const RectF &oldArea, const RectF &newArea)
{
    RectF ret = geometry;
    // move the window to have the same relative position to the center of the screen
    // (i.e. one near the middle of the right edge will also end up near the middle of the right edge)
    QPointF center = geometry.center() - oldArea.center();
    center.setX(center.x() * newArea.width() / oldArea.width());
    center.setY(center.y() * newArea.height() / oldArea.height());
    center += newArea.center();
    ret.moveCenter(center);

    // If the window was inside the old screen area, explicitly make sure its inside also the new screen area
    if (oldArea.contains(geometry)) {
        ret = keepInArea(ret, newArea);
    }
    return ret;
}

RectF Window::ensureSpecialStateGeometry(const RectF &geometry)
{
    if (isRequestedFullScreen()) {
        return workspace()->clientArea(FullScreenArea, this, geometry.center());
    } else if (requestedMaximizeMode() != MaximizeRestore) {
        const RectF maximizeArea = workspace()->clientArea(MaximizeArea, this, geometry.center());
        RectF ret = geometry;
        if (requestedMaximizeMode() & MaximizeHorizontal) {
            ret.setLeft(maximizeArea.left());
            ret.setRight(maximizeArea.right());
        }
        if (requestedMaximizeMode() & MaximizeVertical) {
            ret.setTop(maximizeArea.top());
            ret.setBottom(maximizeArea.bottom());
        }
        return keepInArea(ret, maximizeArea, false);
    } else if (requestedTile()) {
        return requestedTile()->windowGeometry();
    } else {
        return geometry;
    }
}

void Window::sendToOutput(LogicalOutput *newOutput)
{
    ref();
    const auto lifetime = qScopeGuard([this]() {
        unref();
    });
    newOutput = rules()->checkOutput(newOutput);
    if (isActive()) {
        workspace()->setActiveOutput(newOutput);
        // might impact the layer of a fullscreen window
        const auto windows = workspace()->windows();
        for (Window *other : windows) {
            if (other->isFullScreen() && other->output() == newOutput) {
                other->updateLayer();
            }
        }
    }
    if (moveResizeOutput() == newOutput) {
        return;
    }

    LogicalOutput *oldOutput = m_output;
    const RectF oldGeom = moveResizeGeometry();
    const RectF oldScreenArea = workspace()->clientArea(MaximizeArea, this, moveResizeOutput());
    const RectF screenArea = workspace()->clientArea(MaximizeArea, this, newOutput);

    const auto destinationManager = workspace()->tileManager(newOutput);
    if (requestedQuickTileMode() == QuickTileMode(QuickTileFlag::Custom) || !destinationManager) {
        const auto generation = geometryRequestGeneration();
        if (auto manager = workspace()->tileManager(moveResizeOutput()); requestedQuickTileMode() == QuickTileMode(QuickTileFlag::Custom) && manager) {
            manager->forgetWindow(this, nullptr);
        } else if (auto oldTile = m_requestedTile) {
            oldTile->forget(this);
        }
        // A release callback may close the Window or choose a newer placement.
        if (isDeleted() || m_requestedTile || geometryRequestGeneration() != generation) {
            return;
        }
    } else {
        Tile *newTile = destinationManager->quickTile(requestedQuickTileMode());
        if (newTile) {
            newTile->manage(this);
        }
    }

    RectF newGeom = moveToArea(oldGeom, oldScreenArea, screenArea);
    newGeom = ensureSpecialStateGeometry(newGeom);
    moveResize(newGeom);

    // move geometry restores to the new output as well
    setFullscreenGeometryRestore(moveToArea(m_fullscreenGeometryRestore, oldScreenArea, screenArea));
    setGeometryRestore(moveToArea(m_maximizeGeometryRestore, oldScreenArea, screenArea));

    auto tso = workspace()->ensureStackingOrder(transients());
    for (auto it = tso.constBegin(), end = tso.constEnd(); it != end; ++it) {
        (*it)->sendToOutput(newOutput);
    }
    finishInteractiveOutputChange(oldOutput);
}

void Window::checkWorkspacePosition(RectF oldGeometry, LogicalOutput *oldOutput)
{
    if (isDeleted()) {
        qCWarning(KWIN_CORE) << "Window::checkWorkspacePosition: called for a closed window. Consider this a bug";
        return;
    }
    // The decision is read-only; this wrapper keeps the legacy writes and their order: the special
    // branch moves first and moves the restore fields afterwards, and an ordinary rectangle is
    // requested publicly even when it equals the current one (the request records intent).
    const WorkspacePositionDecision decision = workspacePositionDecision(oldGeometry, oldOutput, moveResizeGeometry());
    switch (decision.kind) {
    case WorkspacePositionDecision::Kind::NoOp:
        return;
    case WorkspacePositionDecision::Kind::LegacySpecial:
        moveResize(ensureSpecialStateGeometry(decision.geometry));
        setFullscreenGeometryRestore(moveToArea(m_fullscreenGeometryRestore, decision.oldScreenArea, decision.screenArea));
        setGeometryRestore(moveToArea(m_maximizeGeometryRestore, decision.oldScreenArea, decision.screenArea));
        return;
    case WorkspacePositionDecision::Kind::Geometry:
        moveResize(decision.geometry);
        return;
    }
}

Window::WorkspacePositionDecision Window::workspacePositionDecision(RectF oldGeometry, LogicalOutput *oldOutput, const RectF &current) const
{
    WorkspacePositionDecision decision;
    if (isDeleted() || isDock() || isDesktop() || !isPlaceable()) {
        return decision;
    }

    RectF newGeom = current;

    if (!oldGeometry.isValid()) {
        oldGeometry = newGeom;
    }
    if (!oldOutput) {
        oldOutput = moveResizeOutput();
    }

    RectF oldScreenArea;
    RectF screenArea;
    if (workspace()->inRearrange()) {
        // check if the window is on an about to be destroyed output
        LogicalOutput *newOutput = oldOutput;
        if (!workspace()->outputs().contains(newOutput)) {
            newOutput = workspace()->outputAt(newGeom.center());
        }
        // we need to find the screen area as it was before the change
        oldScreenArea = workspace()->previousScreenSizes().value(oldOutput);
        if (oldScreenArea.isNull()) {
            oldScreenArea = newOutput->geometry();
        }
        screenArea = newOutput->geometry();
        newGeom.translate(screenArea.topLeft() - oldScreenArea.topLeft());
    } else {
        oldScreenArea = workspace()->clientArea(ScreenArea, workspace()->outputAt(oldGeometry.center()));
        screenArea = workspace()->clientArea(ScreenArea, this, newGeom.center());
    }

    if (isRequestedFullScreen() || requestedMaximizeMode() != MaximizeRestore || requestedQuickTileMode() != QuickTileMode(QuickTileFlag::None)) {
        decision.kind = WorkspacePositionDecision::Kind::LegacySpecial;
        decision.geometry = newGeom;
        decision.oldScreenArea = oldScreenArea;
        decision.screenArea = screenArea;
        return decision;
    }

    checkOffscreenPosition(&newGeom, screenArea);
    // Obey size hints. TODO: We really should make sure it stays in the right place
    newGeom.setSize(constrainFrameSize(newGeom.size()));

    decision.kind = WorkspacePositionDecision::Kind::Geometry;
    decision.geometry = m_rules.checkGeometry(newGeom);
    decision.oldScreenArea = oldScreenArea;
    decision.screenArea = screenArea;
    return decision;
}

void Window::checkOffscreenPosition(RectF *geom, const RectF &screenArea) const
{
    if (geom->left() > screenArea.right()) {
        geom->moveLeft(screenArea.right() - std::min(geom->width(), screenArea.width() / 4));
    } else if (geom->right() < screenArea.left()) {
        geom->moveRight(screenArea.left() + std::min(geom->width(), screenArea.width() / 4));
    }
    if (geom->top() > screenArea.bottom()) {
        geom->moveTop(screenArea.bottom() - std::min(geom->height(), screenArea.height() / 4));
    } else if (geom->bottom() < screenArea.top()) {
        geom->moveBottom(screenArea.top() + std::min(geom->height(), screenArea.height() / 4));
    }
}

/**
 * Constrains the client size @p size according to a set of the window's size hints.
 *
 * Default implementation applies only minimum and maximum size constraints.
 */
QSizeF Window::constrainClientSize(const QSizeF &size) const
{
    qreal width = size.width();
    qreal height = size.height();

    // When user is resizing the window, the move resize geometry may have negative width or
    // height. In which case, we need to set negative dimensions to reasonable values.
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }

    const QSizeF minimumSize = minSize();
    const QSizeF maximumSize = maxSize();

    width = std::clamp(width, minimumSize.width(), maximumSize.width());
    height = std::clamp(height, minimumSize.height(), maximumSize.height());

    return QSizeF(width, height);
}

/**
 * Constrains the frame size @p size according to a set of the window's size hints.
 */
QSizeF Window::constrainFrameSize(const QSizeF &size) const
{
    const QSizeF unconstrainedClientSize = nextFrameSizeToClientSize(size);
    const QSizeF constrainedClientSize = constrainClientSize(unconstrainedClientSize);
    return nextClientSizeToFrameSize(constrainedClientSize);
}

RectF Window::fullscreenGeometryRestore() const
{
    return m_fullscreenGeometryRestore;
}

void Window::setFullscreenGeometryRestore(const RectF &geom)
{
    setFullscreenGeometryRestoreInternal(geom);
}

/**
 * Returns @c true if the Window can be shown in full screen mode; otherwise @c false.
 *
 * Default implementation returns @c false.
 */
bool Window::isFullScreenable() const
{
    return false;
}

/**
 * Returns @c true if the Window is currently being shown in full screen mode; otherwise @c false.
 *
 * A window in full screen mode occupies the entire screen with no window frame around it.
 *
 * Default implementation returns @c false.
 */
bool Window::isFullScreen() const
{
    return false;
}

bool Window::isRequestedFullScreen() const
{
    return isFullScreen();
}

/**
 * Asks the Window to enter or leave full screen mode.
 *
 * Default implementation does nothing.
 *
 * @p set @c true if the Window has to be shown in full screen mode, otherwise @c false
 */
void Window::setFullScreen(bool set)
{
    qCWarning(KWIN_CORE, "%s doesn't support setting fullscreen state", metaObject()->className());
}

bool Window::wantsAdaptiveSync() const
{
    return rules()->checkAdaptiveSync(isFullScreen());
}

bool Window::wantsTearing(bool tearingRequested) const
{
    return rules()->checkTearing(tearingRequested);
}

/**
 * Returns @c true if the Window can be minimized; otherwise @c false.
 *
 * Default implementation returns @c false.
 */
bool Window::isMinimizable() const
{
    return false;
}

/**
 * Returns @c true if the Window can be maximized; otherwise @c false.
 *
 * Default implementation returns @c false.
 */
bool Window::isMaximizable() const
{
    return false;
}

/**
 * Returns the currently applied maximize mode.
 *
 * Default implementation returns MaximizeRestore.
 */
MaximizeMode Window::maximizeMode() const
{
    return MaximizeRestore;
}

/**
 * Returns the last requested maximize mode.
 *
 * On X11, this method always matches maximizeMode(). On Wayland, it is asynchronous.
 *
 * Default implementation matches maximizeMode().
 */
MaximizeMode Window::requestedMaximizeMode() const
{
    return maximizeMode();
}

/**
 * Returns the geometry of the Window before it was maximized or quick tiled.
 */
RectF Window::geometryRestore() const
{
    return m_maximizeGeometryRestore;
}

/**
 * Sets the geometry of the Window before it was maximized or quick tiled to @p rect.
 */
void Window::setGeometryRestore(const RectF &rect)
{
    setGeometryRestoreInternal(rect);
}

Window::AssignmentStepResult Window::setGeometryRestoreForAssignment(RectF desired, const TileAssignmentFence &before, OwnedProvenance owned)
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
    if (m_maximizeGeometryRestore != desired && m_placementContinuationRevision >= std::numeric_limits<quint64>::max() - 1) {
        return result;
    }
    result.outcome = AssignmentStepOutcome::Superseded;
    result.owned = owned;
    setGeometryRestoreInternal(desired, &result);
    return result;
}

void Window::setGeometryRestoreInternal(const RectF &desired, AssignmentStepResult *guardedResult)
{
    writeRestoreFieldInternal(m_maximizeGeometryRestore, desired, &Window::maximizeGeometryRestoreChanged, guardedResult);
}

Window::AssignmentStepResult Window::setFullscreenGeometryRestoreForAssignment(RectF desired, const TileAssignmentFence &before, OwnedProvenance owned)
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
    if (m_fullscreenGeometryRestore != desired && m_placementContinuationRevision >= std::numeric_limits<quint64>::max() - 1) {
        return result;
    }
    result.outcome = AssignmentStepOutcome::Superseded;
    result.owned = owned;
    setFullscreenGeometryRestoreInternal(desired, &result);
    return result;
}

void Window::setFullscreenGeometryRestoreInternal(const RectF &desired, AssignmentStepResult *guardedResult)
{
    writeRestoreFieldInternal(m_fullscreenGeometryRestore, desired, &Window::fullscreenGeometryRestoreChanged, guardedResult);
}

void Window::writeRestoreFieldInternal(RectF &field, const RectF &desired, void (Window::*changed)(), AssignmentStepResult *guardedResult)
{
    if (field == desired) {
        if (guardedResult) {
            guardedResult->checkpoint.reset(new TileAssignmentFence(*this));
            if (assignmentResultIsCurrent(*guardedResult)) {
                guardedResult->outcome = AssignmentStepOutcome::AppliedCurrent;
            } else {
                guardedResult->checkpoint.reset();
            }
        }
        return;
    }
    field = desired;
    // Restore-state callbacks must invalidate older native continuation snapshots too. The
    // checkpoint is taken after the write and before the notification; it is never refreshed.
    m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
    if (guardedResult) {
        guardedResult->checkpoint.reset(new TileAssignmentFence(*this));
    }
    // Emits the field's change signal through the member pointer.
    std::invoke(changed, this);
    if (guardedResult) {
        if (assignmentResultIsCurrent(*guardedResult)) {
            guardedResult->outcome = AssignmentStepOutcome::AppliedCurrent;
        } else {
            guardedResult->checkpoint.reset();
        }
    }
}

void Window::invalidateDecoration()
{
}

bool Window::noBorder() const
{
    return decorationPolicy() == DecorationPolicy::None;
}

void Window::setNoBorder(bool set)
{
    if (set) {
        setDecorationPolicy(DecorationPolicy::None);
    } else {
        setDecorationPolicy(DecorationPolicy::ClientPreference);
    }
}

bool Window::userCanSetNoBorder() const
{
    // In other words, check that no specific decoration mode is forced.
    return rules()->checkDecorationPolicy(DecorationPolicy::None) == DecorationPolicy::None
        && rules()->checkDecorationPolicy(DecorationPolicy::ClientPreference) == DecorationPolicy::ClientPreference;
}

DecorationPolicy Window::decorationPolicy() const
{
    return DecorationPolicy::ClientPreference;
}

void Window::setDecorationPolicy(DecorationPolicy policy)
{
}

void Window::showOnScreenEdge()
{
    qCWarning(KWIN_CORE, "%s doesn't support screen edge activation", metaObject()->className());
}

bool Window::isPlaceable() const
{
    return true;
}

bool Window::supportsWindowRules() const
{
    return false;
}

void Window::removeRule(Rules *rule)
{
    m_rules.remove(rule);
}

void Window::evaluateWindowRules()
{
    setupWindowRules();
    applyWindowRules();
}

void Window::setupWindowRules()
{
    disconnect(this, &Window::captionNormalChanged, this, &Window::evaluateWindowRules);
    m_rules = workspace()->rulebook()->find(this);
    // check only after getting the rules, because there may be a rule forcing window type
}

void Window::updateWindowRules(Rules::Types selection)
{
    if (workspace()->rulebook()->areUpdatesDisabled()) {
        return;
    }
    m_rules.update(this, selection);
}

void Window::finishWindowRules()
{
    disconnect(this, &Window::captionNormalChanged, this, &Window::evaluateWindowRules);
    updateWindowRules(Rules::All);
    m_rules = WindowRules();
}

// Applies Force, ForceTemporarily and ApplyNow rules
// Used e.g. after the rules have been modified using the kcm.
void Window::applyWindowRules()
{
    Q_ASSERT(!isDeleted());
    // apply force rules
    // Placement - does need explicit update, just like some others below
    // Geometry : setGeometry() doesn't check rules
    auto client_rules = rules();
    const RectF oldGeometry = moveResizeGeometry();
    const RectF geometry = client_rules->checkGeometrySafe(oldGeometry);
    if (geometry != oldGeometry) {
        moveResize(geometry);
    }
    // MinSize, MaxSize handled by Geometry
    // IgnoreGeometry
    setDesktops(desktops());
    sendToOutput(moveResizeOutput());
    setOnActivities(activities());
    // Type
    maximize(requestedMaximizeMode());
    setMinimized(isMinimized());
    setOriginalSkipTaskbar(skipTaskbar());
    setSkipPager(skipPager());
    setSkipSwitcher(skipSwitcher());
    setKeepAbove(keepAbove());
    setKeepBelow(keepBelow());
    setFullScreen(isRequestedFullScreen());
    setDecorationPolicy(decorationPolicy());
    updateColorScheme();
    updateLayer();
    // FSP
    // AcceptFocus :
    if (workspace()->activeWindow() == this
        && !client_rules->checkAcceptFocus(true)) {
        workspace()->activateNextWindow(this);
    }
    // Autogrouping : Only checked on window manage
    // AutogroupInForeground : Only checked on window manage
    // AutogroupById : Only checked on window manage
    // StrictGeometry
    setShortcut(rules()->checkShortcut(shortcut().toString()));
    // see also X11Window::setActive()
    if (isActive()) {
        setOpacity(rules()->checkOpacityActive(opacity()));
        workspace()->disableGlobalShortcutsForClient(rules()->checkDisableGlobalShortcuts(false));
    } else {
        setOpacity(rules()->checkOpacityInactive(opacity()));
    }
    setDesktopFileName(rules()->checkDesktopFile(desktopFileName()));
    setExcludeFromCapture(rules()->checkExcludeFromCapture(excludeFromCapture()));
}

void Window::setLastUsageSerial(quint32 serial)
{
    if (m_lastUsageSerial < serial) {
        m_lastUsageSerial = serial;
    }
}

quint32 Window::lastUsageSerial() const
{
    return m_lastUsageSerial;
}

uint32_t Window::interactiveMoveResizeCount() const
{
    return m_interactiveMoveResize.counter;
}

void Window::setLockScreenOverlay(bool allowed)
{
    if (m_lockScreenOverlay == allowed) {
        return;
    }
    m_lockScreenOverlay = allowed;
    Q_EMIT lockScreenOverlayChanged();
}

bool Window::isLockScreenOverlay() const
{
    return m_lockScreenOverlay;
}

void Window::refOffscreenRendering()
{
    m_offscreenRenderCount++;
    if (m_offscreenRenderCount == 1) {
        m_offscreenFramecallbackTimer.start(1'000'000 / output()->refreshRate());
        Q_EMIT offscreenRenderingChanged();
    }
}

void Window::unrefOffscreenRendering()
{
    Q_ASSERT(m_offscreenRenderCount);
    m_offscreenRenderCount--;
    if (m_offscreenRenderCount == 0) {
        m_offscreenFramecallbackTimer.stop();
        Q_EMIT offscreenRenderingChanged();
    }
}

bool Window::isOffscreenRendering() const
{
    return m_offscreenRenderCount > 0;
}

void Window::maybeSendFrameCallback()
{
    if (m_windowItem && !m_windowItem->isVisible()) {
        ref();
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
        m_windowItem->framePainted(nullptr, output(), nullptr, timestamp);
        // update refresh rate, it might have changed
        m_offscreenFramecallbackTimer.start(1'000'000 / output()->refreshRate());
        unref();
    }
}

qreal Window::targetScale() const
{
    return m_targetScale;
}

bool Window::isShown() const
{
    return !isDeleted() && !isHidden() && !isHiddenByShowDesktop() && !isMinimized();
}

bool Window::isHidden() const
{
    return m_hidden;
}

void Window::setHidden(bool hidden)
{
    if (m_hidden == hidden) {
        return;
    }
    m_hidden = hidden;
    doSetHidden();
    if (hidden) {
        workspace()->activateNextWindow(this);
    }
    Q_EMIT hiddenChanged();
}

bool Window::isHiddenByShowDesktop() const
{
    return m_hiddenByShowDesktop;
}

void Window::setHiddenByShowDesktop(bool hidden)
{
    if (m_hiddenByShowDesktop != hidden) {
        m_hiddenByShowDesktop = hidden;
        doSetHiddenByShowDesktop();
        Q_EMIT hiddenByShowDesktopChanged();
    }
}

bool Window::isSuspended() const
{
    return m_suspended;
}

void Window::setSuspended(bool suspended)
{
    if (isDeleted()) {
        return;
    }
    if (m_suspended != suspended) {
        m_suspended = suspended;
        doSetSuspended();
    }
}

void Window::doSetSuspended()
{
}

void Window::doSetModal()
{
}

qreal Window::nextTargetScale() const
{
    return m_nextTargetScale;
}

void Window::setNextTargetScale(qreal scale)
{
    setNextTargetScaleInternal(scale);
}

void Window::setNextTargetScaleInternal(qreal scale, AssignmentGeometrySync *sync)
{
    if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
        return;
    }
    if (m_nextTargetScale != scale) {
        m_nextTargetScale = scale;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        if (sync) {
            sync->result.checkpoint.reset(new TileAssignmentFence(*this));
        }
        doSetNextTargetScale();
        if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
            return;
        }
        Q_EMIT nextTargetScaleChanged();
        if (sync) {
            assignmentGeometrySyncIsCurrent(*sync);
        }
    }
}

void Window::doSetNextTargetScale()
{
}

void Window::updateNextTargetScale()
{
    setNextTargetScale(m_moveResizeOutput->scale());
}

void Window::setTargetScale(qreal scale)
{
    if (m_targetScale != scale) {
        m_targetScale = scale;
        Q_EMIT targetScaleChanged();
    }
}

OutputTransform Window::preferredBufferTransform() const
{
    return m_preferredBufferTransform;
}

void Window::setPreferredBufferTransform(OutputTransform transform)
{
    setPreferredBufferTransformInternal(transform);
}

void Window::setPreferredBufferTransformInternal(OutputTransform transform, AssignmentGeometrySync *sync)
{
    if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
        return;
    }
    if (m_preferredBufferTransform != transform) {
        m_preferredBufferTransform = transform;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        if (sync) {
            sync->result.checkpoint.reset(new TileAssignmentFence(*this));
        }
        doSetPreferredBufferTransform();
        if (sync) {
            assignmentGeometrySyncIsCurrent(*sync);
        }
    }
}

void Window::doSetPreferredBufferTransform()
{
}

void Window::updatePreferredBufferTransform()
{
    setPreferredBufferTransform(m_moveResizeOutput->transform());
}

const std::shared_ptr<ColorDescription> &Window::preferredColorDescription() const
{
    return m_preferredColorDescription;
}

void Window::setPreferredColorDescription(const std::shared_ptr<ColorDescription> &description)
{
    setPreferredColorDescriptionInternal(description);
}

void Window::setPreferredColorDescriptionInternal(const std::shared_ptr<ColorDescription> &description, AssignmentGeometrySync *sync)
{
    if (sync && !assignmentGeometrySyncIsCurrent(*sync)) {
        return;
    }
    if (m_preferredColorDescription != description) {
        m_preferredColorDescription = description;
        m_placementContinuationRevision = nextAssociationChange(m_placementContinuationRevision);
        if (sync) {
            sync->result.checkpoint.reset(new TileAssignmentFence(*this));
        }
        doSetPreferredColorDescription();
        if (sync) {
            assignmentGeometrySyncIsCurrent(*sync);
        }
    }
}

void Window::doSetPreferredColorDescription()
{
}

void Window::updatePreferredColorDescription()
{
    setPreferredColorDescription(m_moveResizeOutput->blendingColor());
}

QString Window::tag() const
{
    return m_tag;
}

QString Window::description() const
{
    return m_description;
}

void Window::setDescription(const QString &description)
{
    if (m_description != description) {
        m_description = description;
        Q_EMIT descriptionChanged();
    }
}

void Window::finishInteractiveOutputChange(LogicalOutput *oldOutput)
{
    // We want to preserve the window being on current desktop during interactive output changes:
    // - interactive move/resize
    // - "Move to screen" user action
    // - Move window to another screen via keyboard shortcut.
    // ...
    // On the other hand, this behavior doesn't make sense for non-interactive output changes (e.g. if the output is disabled and all the windows are placed
    // on another output).
    if (!oldOutput || m_desktops.empty()) {
        return;
    }

    VirtualDesktop *currentOutputDesktop = VirtualDesktopManager::self()->currentDesktop(moveResizeOutput());

    if (m_desktops.contains(currentOutputDesktop)) {
        return;
    }

    VirtualDesktop *prevOutputDesktop = VirtualDesktopManager::self()->currentDesktop(oldOutput);

    if (currentOutputDesktop == prevOutputDesktop) {
        return;
    }

    auto desktops = m_desktops;
    desktops.removeOne(prevOutputDesktop);
    desktops.append(currentOutputDesktop);
    setDesktops(desktops);
}

void Window::setActivationToken(const QString &token)
{
    m_activationToken = token;
}

QString Window::activationToken() const
{
    return m_activationToken;
}

bool Window::excludeFromCapture() const
{
    return m_excludeFromCapture;
}

void Window::setExcludeFromCapture(bool newExcludeFromCapture)
{
    newExcludeFromCapture = rules()->checkExcludeFromCapture(newExcludeFromCapture);

    if (m_excludeFromCapture == newExcludeFromCapture) {
        return;
    }

    m_excludeFromCapture = newExcludeFromCapture;

    for (auto window : std::as_const(m_transients)) {
        window->setExcludeFromCapture(newExcludeFromCapture);
    }

    Q_EMIT excludeFromCaptureChanged();
}

} // namespace KWin

#include "moc_window.cpp"
