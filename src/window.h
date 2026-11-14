/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2019 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include "core/rect.h"
#include "cursor.h"
#include "effect/globals.h"
#include "options.h"
#include "rules.h"
#include "scene/borderradius.h"
#include "utils/common.h"
#include "utils/gravity.h"

#include <functional>
#include <memory>
#include <optional>

#include <QElapsedTimer>
#include <QIcon>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#if KWIN_BUILD_X11
#include <xcb/xcb.h>
#endif

class QMouseEvent;
class TestXdgShellWindow;

namespace KDecoration3
{

class Decoration;

}

namespace KWin
{

class PlasmaWindowInterface;
class SurfaceInterface;
class Group;
class LogicalOutput;
class ClientMachine;
class EffectWindow;
class Tile;
class Shadow;
class SurfaceItem;
class VirtualDesktop;
class WindowItem;
class TileManager;
class TileAssignmentFence;
class TilesTest;
class TransientTileController;
class TransientNoRequestProof;
class TransientPlacementWriter;

namespace Decoration
{

class DecoratedWindowImpl;
class DecorationPalette;

}

using ElectricBorderMode = std::variant<QuickTileMode, MaximizeMode>;

/*!
 * The PlacementCommand type specifies how a window should be placed in the workspace when it's shown.
 */
using PlacementCommand = std::variant<QPointF, RectF, MaximizeMode>;

/*!
 * The DecorationMode enum specifies who draws the window decoration, if at all.
 *
 * \value None No decoration.
 * \value Client The window decoration is drawn by the client. The client is responsible for
 *               drawing both the titlebar and the drop shadow.
 * \value Shadow The window decoration is drawn by the client, but the compositor can draw
 *               a drop shadow around the window and potentially round corners.
 * \value Server The window decoration is drawn by the compositor. The compositor is responsible
 *               for drawing both the titlebar and the drop shadow.
 */
enum class DecorationMode {
    None,
    Client,
    Shadow,
    Server,
};

/*!
 * \qmluncreatabletype Window
 * \inqmlmodule org.kde.kwin
 *
 * \brief The window type represents a window.
 */
class KWIN_EXPORT Window : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    /*!
     * \qmlproperty RectF Window::bufferGeometry
     *
     * This property holds the rectangle that the pixmap or buffer of this Window
     * occupies on the screen. This rectangle includes invisible portions of the
     * window, e.g. client-side drop shadows, etc.
     */
    Q_PROPERTY(KWin::RectF bufferGeometry READ bufferGeometry NOTIFY bufferGeometryChanged)

    /*!
     * \qmlproperty RectF Window::clientGeometry
     *
     * The geometry of the Window without frame borders.
     */
    Q_PROPERTY(KWin::RectF clientGeometry READ clientGeometry NOTIFY clientGeometryChanged)

    /*!
     * \qmlproperty point Window::pos
     *
     * This property holds the position of the Window's frame geometry.
     */
    Q_PROPERTY(QPointF pos READ pos)

    /*!
     * \qmlproperty size Window::size
     *
     * This property holds the size of the Window's frame geometry.
     */
    Q_PROPERTY(QSizeF size READ size)

    /*!
     * \qmlproperty real Window::x
     *
     * This property holds the x position of the Window's frame geometry.
     */
    Q_PROPERTY(qreal x READ x NOTIFY frameGeometryChanged)

    /*!
     * \qmlproperty real Window::y
     *
     * This property holds the y position of the Window's frame geometry.
     */
    Q_PROPERTY(qreal y READ y NOTIFY frameGeometryChanged)

    /*!
     * \qmlproperty real Window::width
     *
     * This property holds the width of the Window's frame geometry.
     */
    Q_PROPERTY(qreal width READ width NOTIFY frameGeometryChanged)

    /*!
     * \qmlproperty real Window::height
     *
     * This property holds the height of the Window's frame geometry.
     */
    Q_PROPERTY(qreal height READ height NOTIFY frameGeometryChanged)

    /*!
     * \qmlproperty real Window::opacity
     */
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity NOTIFY opacityChanged)

    /*!
     * \qmlproperty LogicalOutput Window::output
     *
     * The output where the window center is on
     */
    Q_PROPERTY(KWin::LogicalOutput *output READ output NOTIFY outputChanged)

    /*!
     * \qmlproperty RectF Window::rect
     */
    Q_PROPERTY(KWin::RectF rect READ rect)

    /*!
     * \qmlproperty string Window::resourceName
     */
    Q_PROPERTY(QString resourceName READ resourceName NOTIFY windowClassChanged)

    /*!
     * \qmlproperty string Window::resourceClass
     */
    Q_PROPERTY(QString resourceClass READ resourceClass NOTIFY windowClassChanged)

    /*!
     * \qmlproperty string Window::windowRole
     */
    Q_PROPERTY(QString windowRole READ windowRole NOTIFY windowRoleChanged)

    /*!
     * \qmlproperty bool Window::desktopWindow
     *
     * Returns whether the window is a desktop background window (the one with wallpaper).
     * See _NET_WM_WINDOW_TYPE_DESKTOP at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool desktopWindow READ isDesktop CONSTANT)

    /*!
     * \qmlproperty bool Window::dock
     *
     * Returns whether the window is a dock (i.e. a panel).
     * See _NET_WM_WINDOW_TYPE_DOCK at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool dock READ isDock CONSTANT)

    /*!
     * \qmlproperty bool Window::toolbar
     *
     * Returns whether the window is a standalone (detached) toolbar window.
     * See _NET_WM_WINDOW_TYPE_TOOLBAR at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool toolbar READ isToolbar CONSTANT)

    /*!
     * \qmlproperty bool Window::menu
     *
     * Returns whether the window is a torn-off menu.
     * See _NET_WM_WINDOW_TYPE_MENU at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool menu READ isMenu CONSTANT)

    /*!
     * \qmlproperty bool Window::normalWindow
     *
     * Returns whether the window is a "normal" window, i.e. an application or any other window
     * for which none of the specialized window types fit.
     * See _NET_WM_WINDOW_TYPE_NORMAL at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool normalWindow READ isNormalWindow CONSTANT)

    /*!
     * \qmlproperty bool Window::dialog
     *
     * Returns whether the window is a dialog window.
     * See _NET_WM_WINDOW_TYPE_DIALOG at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool dialog READ isDialog CONSTANT)

    /*!
     * \qmlproperty bool Window::splash
     *
     * Returns whether the window is a splashscreen. Note that many (especially older) applications
     * do not support marking their splash windows with this type.
     * See _NET_WM_WINDOW_TYPE_SPLASH at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool splash READ isSplash CONSTANT)

    /*!
     * \qmlproperty bool Window::utility
     *
     * Returns whether the window is a utility window, such as a tool window.
     * See _NET_WM_WINDOW_TYPE_UTILITY at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool utility READ isUtility CONSTANT)

    /*!
     * \qmlproperty bool Window::dropdownMenu
     *
     * Returns whether the window is a dropdown menu (i.e. a popup directly or indirectly open
     * from the application's menubar).
     * See _NET_WM_WINDOW_TYPE_DROPDOWN_MENU at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool dropdownMenu READ isDropdownMenu CONSTANT)

    /*!
     * \qmlproperty bool Window::popupMenu
     *
     * Returns whether the window is a popup menu (that is not a torn-off or dropdown menu).
     * See _NET_WM_WINDOW_TYPE_POPUP_MENU at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool popupMenu READ isPopupMenu CONSTANT)

    /*!
     * \qmlproperty bool Window::tooltip
     *
     * Returns whether the window is a tooltip.
     * See _NET_WM_WINDOW_TYPE_TOOLTIP at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool tooltip READ isTooltip CONSTANT)

    /*!
     * \qmlproperty bool Window::notification
     *
     * Returns whether the window is a window with a notification.
     * See _NET_WM_WINDOW_TYPE_NOTIFICATION at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool notification READ isNotification CONSTANT)

    /*!
     * \qmlproperty bool Window::criticalNotification
     *
     * Returns whether the window is a window with a critical notification.
     */
    Q_PROPERTY(bool criticalNotification READ isCriticalNotification CONSTANT)

    /*!
     * \qmlproperty bool Window::appletPopup
     *
     * Returns whether the window is an applet popup.
     */
    Q_PROPERTY(bool appletPopup READ isAppletPopup CONSTANT)

    /*!
     * \qmlproperty bool Window::onScreenDisplay
     *
     * Returns whether the window is an On Screen Display.
     */
    Q_PROPERTY(bool onScreenDisplay READ isOnScreenDisplay CONSTANT)

    /*!
     * \qmlproperty bool Window::comboBox
     *
     * Returns whether the window is a combobox popup.
     * See _NET_WM_WINDOW_TYPE_COMBO at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool comboBox READ isComboBox CONSTANT)

    /*!
     * \qmlproperty bool Window::dndIcon
     *
     * Returns whether the window is a Drag&Drop icon.
     * See _NET_WM_WINDOW_TYPE_DND at https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(bool dndIcon READ isDNDIcon CONSTANT)

    /*!
     * \qmlproperty enumeration Window::windowType
     *
     * Returns the NETWM window type.
     * See https://specifications.freedesktop.org/wm-spec .
     */
    Q_PROPERTY(WindowType windowType READ windowType CONSTANT)

    /*!
     * \qmlproperty bool Window::managed
     *
     * Whether this Window is managed by KWin (it has control over its placement and other
     * aspects, as opposed to override-redirect windows that are entirely handled by the application).
     */
    Q_PROPERTY(bool managed READ isClient CONSTANT)

    /*!
     * \qmlproperty bool Window::deleted
     *
     * Whether this Window represents an already deleted window and only kept for the compositor for animations.
     */
    Q_PROPERTY(bool deleted READ isDeleted CONSTANT)

    /*!
     * \qmlproperty bool Window::skipsCloseAnimation
     *
     * Whether the window does not want to be animated on window close.
     * There are legit reasons for this like a screenshot application which does not want its
     * window being captured.
     */
    Q_PROPERTY(bool skipsCloseAnimation READ skipsCloseAnimation WRITE setSkipCloseAnimation NOTIFY skipCloseAnimationChanged)

    /*!
     * \qmlproperty bool Window::popupWindow
     *
     * Whether the window is a popup.
     */
    Q_PROPERTY(bool popupWindow READ isPopupWindow)

    /*!
     * \qmlproperty bool Window::outline
     *
     * Whether this Window represents the outline.
     *
     * \note It's always \c false if compositing is turned off.
     */
    Q_PROPERTY(bool outline READ isOutline)

    /*!
     * \qmlproperty uuid Window::internalId
     *
     * This property holds a UUID to uniquely identify this Window.
     */
    Q_PROPERTY(QUuid internalId READ internalId CONSTANT)

    /*!
     * \qmlproperty int Window::pid
     *
     * The pid of the process owning this window.
     *
     * \since 5.20
     */
    Q_PROPERTY(int pid READ pid CONSTANT)

    /*!
     * \qmlproperty int Window::stackingOrder
     *
     * The position of this window within Workspace's window stack.
     */
    Q_PROPERTY(int stackingOrder READ stackingOrder NOTIFY stackingOrderChanged)

    /*!
     * \qmlproperty bool Window::fullScreen
     *
     * Whether this Window is fullScreen. A Window might either be fullScreen due to the _NET_WM property
     * or through a legacy support hack. The fullScreen state can only be changed if the Window does not
     * use the legacy hack. To be sure whether the state changed, connect to the notify signal.
     */
    Q_PROPERTY(bool fullScreen READ isFullScreen WRITE setFullScreen NOTIFY fullScreenChanged)

    /*!
     * \qmlproperty bool Window::fullScreenable
     *
     * Whether the Window can be set to fullScreen. The property is evaluated each time it is invoked.
     * Because of that there is no notify signal.
     */
    Q_PROPERTY(bool fullScreenable READ isFullScreenable)

    /*!
     * \qmlproperty bool Window::active
     *
     * Whether this Window is active or not. Use Workspace::activateWindow() to activate a Window.
     * \sa Workspace::activateWindow
     */
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

    /*!
     * \qmlproperty list<VirtualDesktop> Window::desktops
     *
     * The virtual desktops this client is on. If it's on all desktops, the list is empty.
     */
    Q_PROPERTY(QList<KWin::VirtualDesktop *> desktops READ desktops WRITE setDesktops NOTIFY desktopsChanged)

    /*!
     * \qmlproperty bool Window::onAllDesktops
     *
     * Whether the Window is on all desktops. That is desktop is -1.
     */
    Q_PROPERTY(bool onAllDesktops READ isOnAllDesktops WRITE setOnAllDesktops NOTIFY desktopsChanged)

    /*!
     * \qmlproperty list<string> Window::activities
     *
     * The activities this client is on. If it's on all activities the property is empty.
     */
    Q_PROPERTY(QStringList activities READ activities WRITE setOnActivities NOTIFY activitiesChanged)

    /*!
     * \qmlproperty bool Window::skipTaskbar
     *
     * Indicates that the window should not be included on a taskbar.
     */
    Q_PROPERTY(bool skipTaskbar READ skipTaskbar WRITE setSkipTaskbar NOTIFY skipTaskbarChanged)

    /*!
     * \qmlproperty bool Window::skipPager
     *
     * Indicates that the window should not be included on a Pager.
     */
    Q_PROPERTY(bool skipPager READ skipPager WRITE setSkipPager NOTIFY skipPagerChanged)

    /*!
     * \qmlproperty bool Window::skipSwitcher
     *
     * Whether the Window should be excluded from window switching effects.
     */
    Q_PROPERTY(bool skipSwitcher READ skipSwitcher WRITE setSkipSwitcher NOTIFY skipSwitcherChanged)

    /*!
     * \qmlproperty bool Window::closeable
     *
     * Whether the window can be closed by the user.
     */
    Q_PROPERTY(bool closeable READ isCloseable NOTIFY closeableChanged)

    /*!
     * \qmlproperty icon Window::icon
     */
    Q_PROPERTY(QIcon icon READ icon NOTIFY iconChanged)

    /*!
     * \qmlproperty bool Window::keepAbove
     *
     * Whether the Window is set to be kept above other windows.
     */
    Q_PROPERTY(bool keepAbove READ keepAbove WRITE setKeepAbove NOTIFY keepAboveChanged)

    /*!
     * \qmlproperty bool Window::keepBelow
     *
     * Whether the Window is set to be kept below other windows.
     */
    Q_PROPERTY(bool keepBelow READ keepBelow WRITE setKeepBelow NOTIFY keepBelowChanged)

    /*!
     * \qmlproperty bool Window::minimizable
     *
     * Whether the Window can be minimized. The property is evaluated each time it is invoked.
     * Because of that there is no notify signal.
     */
    Q_PROPERTY(bool minimizable READ isMinimizable)

    /*!
     * \qmlproperty bool Window::minimized
     *
     * Whether the Window is minimized.
     */
    Q_PROPERTY(bool minimized READ isMinimized WRITE setMinimized NOTIFY minimizedChanged)

    /*!
     * \qmlproperty RectF Window::iconGeometry
     *
     * The optional geometry representing the minimized Window in e.g a taskbar.
     * See _NET_WM_ICON_GEOMETRY at https://specifications.freedesktop.org/wm-spec .
     * The value is evaluated each time the getter is called.
     * Because of that no changed signal is provided.
     */
    Q_PROPERTY(KWin::RectF iconGeometry READ iconGeometry)

    /*!
     * \qmlproperty bool Window::specialWindow
     *
     * Returns whether the window is any of special windows types (desktop, dock, splash, ...),
     * i.e. window types that usually don't have a window frame and the user does not use window
     * management (moving, raising,...) on them.
     * The value is evaluated each time the getter is called.
     * Because of that no changed signal is provided.
     */
    Q_PROPERTY(bool specialWindow READ isSpecialWindow)

    /*!
     * \qmlproperty bool Window::demandsAttention
     *
     * Whether window state _NET_WM_STATE_DEMANDS_ATTENTION is set. This state indicates that some
     * action in or with the window happened. For example, it may be set by the Window Manager if
     * the window requested activation but the Window Manager refused it, or the application may set
     * it if it finished some work. This state may be set by both the Window and the Window Manager.
     * It should be unset by the Window Manager when it decides the window got the required attention
     * (usually, that it got activated).
     */
    Q_PROPERTY(bool demandsAttention READ isDemandingAttention WRITE demandAttention NOTIFY demandsAttentionChanged)

    /*!
     * \qmlproperty string Window::caption
     *
     * The Caption of the Window. Read from WM_NAME property together with a suffix for hostname and shortcut.
     * To read only the caption as provided by WM_NAME, use \c captionNormal.
     */
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)

    /*!
     * \qmlproperty string Window::captionNormal
     *
     * The Caption of the Window. Read from WM_NAME property.
     */
    Q_PROPERTY(QString captionNormal READ captionNormal NOTIFY captionNormalChanged)

    /*!
     * \qmlproperty size Window::minSize
     *
     * Minimum size as specified in WM_NORMAL_HINTS
     */
    Q_PROPERTY(QSizeF minSize READ minSize)

    /*!
     * \qmlproperty size Window::maxSize
     *
     * Maximum size as specified in WM_NORMAL_HINTS
     */
    Q_PROPERTY(QSizeF maxSize READ maxSize)

    /*!
     * \qmlproperty bool Window::wantsInput
     *
     * Whether the Window can accept keyboard focus.
     * The value is evaluated each time the getter is called.
     * Because of that no changed signal is provided.
     */
    Q_PROPERTY(bool wantsInput READ wantsInput)

    /*!
     * \qmlproperty bool Window::transient
     *
     * Whether the Window is a transient Window to another Window.
     * \sa transientFor
     */
    Q_PROPERTY(bool transient READ isTransient NOTIFY transientChanged)

    /*!
     * \qmlproperty Window Window::transientFor
     *
     * The Window to which this Window is a transient if any.
     */
    Q_PROPERTY(KWin::Window *transientFor READ transientFor NOTIFY transientChanged)

    /*!
     * \qmlproperty bool Window::modal
     *
     * Whether the Window represents a modal window.
     */
    Q_PROPERTY(bool modal READ isModal NOTIFY modalChanged)

    /*!
     * \qmlproperty RectF Window::frameGeometry
     *
     * The geometry of this Window. Be aware that depending on resize mode the frameGeometryChanged
     * signal might be emitted at each resize step or only at the end of the resize operation.
     *
     * \sa requestedGeometry
     */
    Q_PROPERTY(KWin::RectF frameGeometry READ frameGeometry WRITE moveResize NOTIFY frameGeometryChanged)

    /*!
     * \qmlproperty bool Window::move
     *
     * Whether the Window is currently being moved by the user.
     * Notify signal is emitted when the Window starts or ends move/resize mode.
     */
    Q_PROPERTY(bool move READ isInteractiveMove NOTIFY moveResizedChanged)

    /*!
     * \qmlproperty bool Window::resize
     *
     * Whether the Window is currently being resized by the user.
     * Notify signal is emitted when the Window starts or ends move/resize mode.
     */
    Q_PROPERTY(bool resize READ isInteractiveResize NOTIFY moveResizedChanged)

    /*!
     * \qmlproperty bool Window::decorationHasAlpha
     *
     * Whether the decoration is currently using an alpha channel.
     */
    Q_PROPERTY(bool decorationHasAlpha READ decorationHasAlpha)

    /*!
     * \qmlproperty bool Window::noBorder
     *
     * Whether the window has a decoration or not.
     * This property is not allowed to be set by applications themselves.
     * The decision whether a window has a border or not belongs to the window manager.
     * If this property gets abused by application developers, it will be removed again.
     */
    Q_PROPERTY(bool noBorder READ noBorder WRITE setNoBorder NOTIFY decorationPolicyChanged)

    /*!
     * \qmlproperty bool Window::providesContextHelp
     *
     * Whether the Window provides context help. Mostly needed by decorations to decide whether to
     * show the help button or not.
     */
    Q_PROPERTY(bool providesContextHelp READ providesContextHelp CONSTANT)

    /*!
     * \qmlproperty bool Window::maximizable
     *
     * Whether the Window can be maximized both horizontally and vertically.
     * The property is evaluated each time it is invoked.
     * Because of that there is no notify signal.
     */
    Q_PROPERTY(bool maximizable READ isMaximizable)

    /*!
     * \qmlproperty enumeration Window::maximizeMode
     * \qmlenumeratorsfrom KWin::MaximizeMode
     *
     * Whether the window is maximized horizontally, vertically or fully.
     * This is read only, in order to maximize from a script use
     * the setMaximize function
     * \sa setMaximize
     */
    Q_PROPERTY(KWin::MaximizeMode maximizeMode READ maximizeMode NOTIFY maximizedChanged)

    /*!
     * \qmlproperty bool Window::moveable
     *
     * Whether the Window is moveable. Even if it is not moveable, it might be possible to move
     * it to another screen. The property is evaluated each time it is invoked.
     * Because of that there is no notify signal.
     * \sa moveableAcrossScreens
     */
    Q_PROPERTY(bool moveable READ isMovable)

    /*!
     * \qmlproperty bool Window::moveableAcrossScreens
     *
     * Whether the Window can be moved to another screen. The property is evaluated each time it is invoked.
     * Because of that there is no notify signal.
     * \sa moveable
     */
    Q_PROPERTY(bool moveableAcrossScreens READ isMovableAcrossScreens)

    /*!
     * \qmlproperty bool Window::resizeable
     *
     * Whether the Window can be resized. The property is evaluated each time it is invoked.
     * Because of that there is no notify signal.
     */
    Q_PROPERTY(bool resizeable READ isResizable)

    /*!
     * \qmlproperty string Window::desktopFileName
     *
     * The desktop file name of the application this Window belongs to.
     *
     * This is either the base name without full path and without file extension of the
     * desktop file for the window's application (e.g. "org.kde.foo").
     *
     * The application's desktop file name can also be the full path to the desktop file
     * (e.g. "/opt/kde/share/org.kde.foo.desktop") in case it's not in a standard location.
     */
    Q_PROPERTY(QString desktopFileName READ desktopFileName NOTIFY desktopFileNameChanged)

    /*!
     * \qmlproperty bool Window::hasApplicationMenu
     *
     * Whether an application menu is available for this Window
     */
    Q_PROPERTY(bool hasApplicationMenu READ hasApplicationMenu NOTIFY hasApplicationMenuChanged)

    /*!
     * \qmlproperty bool Window::applicationMenuActive
     *
     * Whether the application menu for this Window is currently opened
     */
    Q_PROPERTY(bool applicationMenuActive READ applicationMenuActive NOTIFY applicationMenuActiveChanged)

    /*!
     * \qmlproperty bool Window::unresponsive
     *
     * Whether this window is unresponsive.
     *
     * When an application failed to react on a ping request in time, it is
     * considered unresponsive. This usually indicates that the application froze or crashed.
     */
    Q_PROPERTY(bool unresponsive READ unresponsive NOTIFY unresponsiveChanged)

    /*!
     * \qmlproperty string Window::colorScheme
     *
     * The color scheme set on this window
     * Absolute file path, or name of palette in the user's config directory following KColorSchemes format.
     * An empty string indicates the default palette from kdeglobals is used.
     * \note This indicates the colour scheme requested, which might differ from the theme applied if the colorScheme cannot be found
     */
    Q_PROPERTY(QString colorScheme READ colorScheme NOTIFY colorSchemeChanged)

    /*!
     * \qmlproperty enumeration Window::layer
     *
     */
    Q_PROPERTY(KWin::Layer layer READ layer)

    /*!
     * \qmlproperty bool Window::hidden
     *
     * Whether this window is hidden. It's usually the case with auto-hide panels.
     */
    Q_PROPERTY(bool hidden READ isHidden NOTIFY hiddenChanged)

    /*!
     * \qmlproperty Tile Window::tile
     *
     * The Tile this window is associated to, if any
     */
    Q_PROPERTY(KWin::Tile *tile READ requestedTile WRITE setTileCompatibility NOTIFY tileChanged)

    /*!
     * \qmlproperty RectF Window::requestedGeometry
     *
     * The geometry KWin most recently asked this Window to take, whereas frameGeometry is the
     * geometry it currently has. They differ while a Wayland client has not yet committed in
     * response to a geometry-changing configure event, and on X11 while a synchronized resize is
     * in progress. An empty size means that the client has been asked to choose its own size.
     *
     * On xdg-shell windows (including picture-in-picture windows), KWin synchronizes this with
     * frameGeometry on a commit when no configure event is pending or scheduled. On other windows
     * they can stay different, for example because of pixel rounding, and it is not meaningful
     * for unmanaged X11 windows.
     *
     * Compare rectangles by their x, y, width and height.
     *
     * \sa frameGeometry
     */
    Q_PROPERTY(KWin::RectF requestedGeometry READ moveResizeGeometry NOTIFY requestedGeometryChanged)

    /*!
     * \qmlproperty bool Window::inputMethod
     *
     * Returns whether this window is an input method window.
     * This is only used for Wayland.
     */
    Q_PROPERTY(bool inputMethod READ isInputMethod)

    /*!
     * \qmlproperty string Window::tag
     *
     * A client-provided tag of the window.
     * Not necessarily unique, but can be used to identify similar windows
     * across application restarts
     */
    Q_PROPERTY(QString tag READ tag NOTIFY tagChanged)

    /*!
     * \qmlproperty string Window::description
     *
     * A client-provided description of the window
     */
    Q_PROPERTY(QString description READ description NOTIFY descriptionChanged)

    /*!
     * \qmlproperty bool Window::excludeFromCapture
     *
     * Exclude this window from ScreenCast.
     * It will also be applied for all transient windows recursively.
     */
    Q_PROPERTY(bool excludeFromCapture READ excludeFromCapture WRITE setExcludeFromCapture NOTIFY excludeFromCaptureChanged FINAL)

public:
    ~Window() override;

    void ref();
    void unref();

    /*!
     * Returns the last requested geometry. The returned value indicates the bounding
     * geometry, meaning that the client can commit smaller window geometry if the window
     * is resized.
     *
     * The main difference between the frame geometry and the move-resize geometry is
     * that the former specifies the current geometry while the latter specifies the next
     * geometry.
     */
    RectF moveResizeGeometry() const;

    /*!
     * Returns the output where the last move or resize operation has occurred. The
     * window is expected to land on this output after the move/resize operation completes.
     */
    LogicalOutput *moveResizeOutput() const;
    void setMoveResizeOutput(LogicalOutput *output);

    /*!
     * Returns the geometry of the pixmap or buffer attached to this Window.
     *
     * For X11 windows, this method returns server-side geometry of the Window.
     *
     * For Wayland windows, this method returns rectangle that the main surface
     * occupies on the screen, in global screen coordinates.
     */
    RectF bufferGeometry() const;
    /*!
     * Returns the geometry of the Window, excluding invisible portions, e.g.
     * server-side and client-side drop shadows, etc.
     */
    RectF frameGeometry() const;
    /*!
     * Returns the geometry of the client window, in global screen coordinates.
     */
    RectF clientGeometry() const;
    /*!
     * Returns the extents of the server-side decoration.
     *
     * Note that the returned margins object will have all margins set to 0 if
     * the window doesn't have a server-side decoration.
     */
    QMargins frameMargins() const;

    BorderRadius borderRadius() const;
    void setBorderRadius(const BorderRadius &radius);

    virtual QSizeF minSize() const;
    virtual QSizeF maxSize() const;
    QSizeF size() const;
    QPointF pos() const;
    RectF rect() const;
    qreal x() const;
    qreal y() const;
    qreal width() const;
    qreal height() const;
    bool isOnOutput(LogicalOutput *output) const;
    bool isOnActiveOutput() const;
    LogicalOutput *output() const;
    void setOutput(LogicalOutput *output);
    QSizeF clientSize() const;
    /*!
     * Returns a rectangle that the window occupies on the screen, including drop-shadows.
     */
    RectF visibleGeometry() const;

    /*!
     * Maps the specified \a point from the global screen coordinates to the frame coordinates.
     */
    QPointF mapToFrame(const QPointF &point) const;
    /*!
     * Maps the specified \a point from the global screen coordinates to the surface-local
     * coordinates of the main surface. For X11 windows, this function maps the specified point
     * from the global screen coordinates to the buffer-local coordinates.
     */
    QPointF mapToLocal(const QPointF &point) const;
    QPointF mapFromLocal(const QPointF &point) const;

    /*!
     * Calculates the matching client position for the given frame position \a point.
     */
    virtual QPointF framePosToClientPos(const QPointF &point) const;
    virtual QPointF nextFramePosToClientPos(const QPointF &point) const;
    /*!
     * Calculates the matching frame position for the given client position \a point.
     */
    virtual QPointF clientPosToFramePos(const QPointF &point) const;
    virtual QPointF nextClientPosToFramePos(const QPointF &point) const;
    /*!
     * Calculates the matching client size for the given frame size \a size.
     *
     * Notice that size constraints won't be applied.
     *
     * Default implementation returns the frame size with frame margins being excluded.
     */
    virtual QSizeF frameSizeToClientSize(const QSizeF &size) const;
    virtual QSizeF nextFrameSizeToClientSize(const QSizeF &size) const;
    /*!
     * Calculates the matching frame size for the given client size \a size.
     *
     * Notice that size constraints won't be applied.
     *
     * Default implementation returns the client size with frame margins being included.
     */
    virtual QSizeF clientSizeToFrameSize(const QSizeF &size) const;
    virtual QSizeF nextClientSizeToFrameSize(const QSizeF &size) const;
    /*!
     * Calculates the matching client rect for the given frame rect \a rect.
     *
     * Notice that size constraints won't be applied.
     */
    RectF frameRectToClientRect(const RectF &rect) const;
    RectF nextFrameRectToClientRect(const RectF &rect) const;
    /*!
     * Calculates the matching frame rect for the given client rect \a rect.
     *
     * Notice that size constraints won't be applied.
     */
    RectF clientRectToFrameRect(const RectF &rect) const;
    RectF nextClientRectToFrameRect(const RectF &rect) const;

    virtual QSizeF constrainClientSize(const QSizeF &size) const;
    QSizeF constrainFrameSize(const QSizeF &size) const;

    /*!
     * Moves the window so that the new topLeft corner of the frame is \a topLeft.
     */
    void move(const QPointF &topLeft);

    /*!
     * Resizes the window to have a new \a size but stay with the top-left corner in the same position.
     */
    void resize(const QSizeF &size);

    /*!
     * Requests a new \a geometry for the window that the implementation will need to adopt
     * within its possibilities.
     */
    void moveResize(const RectF &geometry);
    // Native request identity, including no-op intents; this is not a scripting property.
    quint64 geometryRequestGeneration() const;

    /*!
     * Returns \c true if the window has already been placed in the workspace; otherwise returns \c false.
     */
    bool isPlaced() const;

    /*!
     * Places the window in the workspace as specified by the \a placement command.
     */
    void place(const PlacementCommand &placement);

    void growHorizontal();
    void shrinkHorizontal();
    void growVertical();
    void shrinkVertical();

    virtual RectF resizeWithChecks(const RectF &geometry, const QSizeF &s) const = 0;
    RectF keepInArea(RectF geometry, RectF area, bool partial = false) const;

    // prefer isXXX() instead
    virtual WindowType windowType() const = 0;
    bool hasNETSupport() const;
    bool isDesktop() const;
    bool isDock() const;
    bool isToolbar() const;
    bool isMenu() const;
    bool isNormalWindow() const; // normal as in 'NET::Normal or NET::Unknown non-transient'
    bool isDialog() const;
    bool isSplash() const;
    bool isUtility() const;
    bool isDropdownMenu() const;
    bool isPopupMenu() const; // a context popup, not dropdown, not torn-off
    bool isTooltip() const;
    bool isNotification() const;
    bool isCriticalNotification() const;
    bool isAppletPopup() const;
    bool isOnScreenDisplay() const;
    bool isComboBox() const;
    bool isDNDIcon() const;

    virtual bool isLockScreen() const;
    virtual bool isInputMethod() const;
    virtual bool isOutline() const;
    virtual bool isInternal() const;
    virtual bool isPopupWindow() const;

    virtual bool isClient() const;
    bool isDeleted() const;
    virtual bool isUnmanaged() const;
    virtual bool isPictureInPicture() const;

    bool isLockScreenOverlay() const;
    void setLockScreenOverlay(bool allowed);

    QStringList desktopIds() const;
    QList<VirtualDesktop *> desktops() const;
    void setDesktops(QList<VirtualDesktop *> desktops);
    void enterDesktop(VirtualDesktop *desktop);
    void leaveDesktop(VirtualDesktop *desktop);
    bool isOnDesktop(VirtualDesktop *desktop) const;
    bool isOnCurrentDesktop() const;
    bool isOnAllDesktops() const;
    void setOnAllDesktops(bool set);

    virtual QStringList activities() const;
    bool isOnActivity(const QString &activity) const;
    bool isOnCurrentActivity() const;
    bool isOnAllActivities() const;
    void setOnActivity(const QString &activity, bool enable);
    void setOnActivities(const QStringList &newActivitiesList);
    void setOnAllActivities(bool all);
    virtual void updateActivities(bool includeTransients);
    void blockActivityUpdates(bool b = true);

    /*!
     * Refresh Window's cache of activities
     * Called when activity daemon status changes
     */
    virtual void checkActivities()
    {
    }

    virtual QString windowRole() const;
    QString resourceName() const;
    QString resourceClass() const;
    QString wmClientMachine(bool use_localhost) const;
    ClientMachine *clientMachine() const;
    virtual bool isLocalhost() const;
    virtual pid_t pid() const;

    bool readyForPainting() const; // true if the window has been already painted its contents
    void setOpacity(qreal opacity);
    qreal opacity() const;
    void setupItem();
    EffectWindow *effectWindow();
    const EffectWindow *effectWindow() const;
    SurfaceItem *surfaceItem() const;
    WindowItem *windowItem() const;

    /*!
     * Returns the Shadow associated with this Window or \c null if it has no shadow.
     */
    Shadow *shadow() const;
    /*!
     * Updates the Shadow associated with this Window from X11 Property.
     * Call this method when the Property changes or Compositing is started.
     */
    void updateShadow();
    /*!
     * Whether the Window currently wants the shadow to be rendered.
     */
    bool wantsShadowToBeRendered() const;

    bool skipsCloseAnimation() const;
    void setSkipCloseAnimation(bool set);

    SurfaceInterface *surface() const;
    void setSurface(SurfaceInterface *surface);

    /*!
     * Returns Transformation to map from global to window coordinates.
     */
    QMatrix4x4 inputTransformation() const;

    /*!
     * Returns \c true if the window can accept input at the specified position \a point.
     */
    virtual bool hitTest(const QPointF &point) const;

    /*!
     * The window has a popup grab. This means that when it got mapped the
     * parent window had an implicit (pointer) grab.
     *
     * Normally this is only relevant for transient windows.
     *
     * Once the popup grab ends (e.g. pointer press outside of any Window of
     * the client), the method popupDone should be invoked.
     *
     * The default implementation returns \c false.
     * \sa popupDone
     * \since 5.10
     */
    virtual bool hasPopupGrab() const
    {
        return false;
    }
    /*!
     * This method should be invoked for windows with a popup grab when
     * the grab ends.
     *
     * The default implementation does nothing.
     * \sa hasPopupGrab
     * \since 5.10
     */
    virtual void popupDone()
    {
    }

    /*!
     * \brief Finds the Window matching the condition expressed in \a func in \a list.
     *
     * The method is templated to operate on either a list of windows or on a list of
     * a subclass type of Window.
     * \a list The list to search in
     * \a func The condition function (compare std::find_if)
     * Returns T* The found Window or \c null if there is no matching Window
     */
    template<class T, class U>
    static T *findInList(const QList<T *> &list, std::function<bool(const U *)> func);

    /*!
     * A UUID to uniquely identify this Window independent of windowing system.
     */
    QUuid internalId() const
    {
        return m_internalId;
    }

    int stackingOrder() const;
    void setStackingOrder(int order);

    bool skipSwitcher() const
    {
        return m_skipSwitcher;
    }
    void setSkipSwitcher(bool set);

    bool skipTaskbar() const
    {
        return m_skipTaskbar;
    }
    void setSkipTaskbar(bool set);
    void setOriginalSkipTaskbar(bool set);
    bool originalSkipTaskbar() const
    {
        return m_originalSkipTaskbar;
    }

    bool skipPager() const
    {
        return m_skipPager;
    }
    void setSkipPager(bool set);

    const QIcon &icon() const
    {
        return m_icon;
    }

    bool isActive() const
    {
        return m_active;
    }
    /*!
     * Sets the window's active state to \a act.
     *
     * This function does only change the visual appearance of the window,
     * it does not change the focus setting. Use
     * Workspace::activateClient() or Workspace::requestFocus() instead.
     *
     * If a window receives or looses the focus, it calls setActive() on
     * its own.
     */
    void setActive(bool);

    bool keepAbove() const
    {
        return m_keepAbove;
    }
    void setKeepAbove(bool);
    bool keepBelow() const
    {
        return m_keepBelow;
    }
    void setKeepBelow(bool);

    void demandAttention(bool set = true);
    bool isDemandingAttention() const
    {
        return m_demandsAttention;
    }

    void cancelAutoRaise();

    /*!
     * Returns The caption consisting of captionNormal and captionSuffix
     * \sa captionNormal
     * \sa captionSuffix
     */
    QString caption() const;
    /*!
     * Returns The caption as set by the Window without any suffix.
     * \sa caption
     * \sa captionSuffix
     */
    virtual QString captionNormal() const = 0;
    /*!
     * Returns The suffix added to the caption (e.g. shortcut, machine name, etc.)
     * \sa caption
     * \sa captionNormal
     */
    virtual QString captionSuffix() const = 0;
    virtual bool isPlaceable() const;
    virtual bool isCloseable() const = 0;
    bool isShown() const;
    bool isHidden() const;
    void setHidden(bool hidden);
    bool isHiddenByShowDesktop() const;
    void setHiddenByShowDesktop(bool hidden);
    Window *findModal() const;
    virtual bool isTransient() const;
    const Window *transientFor() const;
    Window *transientFor();
    void setTransientFor(Window *transientFor);
    /*!
     * Returns \c true if transient is the transient_for window for this window,
     *  or recursively the transient_for window
     * @todo: remove boolean trap
     */
    virtual bool hasTransient(const Window *transient, bool indirect) const;
    const QList<Window *> &transients() const; // Is not indirect
    virtual void addTransient(Window *transient);
    virtual void removeTransient(Window *transient);
    void removeTransientFromList(Window *cl);
    virtual QList<Window *> mainWindows() const; // Call once before loop , is not indirect
    QList<Window *> allMainWindows() const; // Call once before loop , is indirect
    /*!
     * Returns true for "special" windows and false for windows which are "normal"
     * (normal=window which has a border, can be moved by the user, can be closed, etc.)
     * true for Desktop, Dock, Splash, Override and TopMenu (and Toolbar??? - for now)
     * false for Normal, Dialog, Utility and Menu (and Toolbar??? - not yet) TODO
     */
    bool isSpecialWindow() const;
    void sendToOutput(LogicalOutput *output);
    const QKeySequence &shortcut() const
    {
        return _shortcut;
    }
    void setShortcut(const QString &cut);

    RectF iconGeometry() const;

    void setMinimized(bool set);
    bool isMinimized() const
    {
        return m_minimized;
    }
    virtual bool isMinimizable() const;

    bool isSuspended() const;
    void setSuspended(bool suspended);

    RectF fullscreenGeometryRestore() const;
    void setFullscreenGeometryRestore(const RectF &geom);
    virtual bool isFullScreenable() const;
    virtual bool isFullScreen() const;
    virtual bool isRequestedFullScreen() const;
    virtual void setFullScreen(bool set);

    bool wantsAdaptiveSync() const;
    bool wantsTearing(bool tearingRequested) const;

    RectF geometryRestore() const;
    void setGeometryRestore(const RectF &rect);
    virtual bool isMaximizable() const;
    virtual MaximizeMode maximizeMode() const;
    virtual MaximizeMode requestedMaximizeMode() const;
    virtual void maximize(MaximizeMode mode, const RectF &restore = RectF());
    /*!
     * \qmlmethod void setMaximize(bool vertically, bool horizontally, RectF restore={})
     *
     * Sets the maximization according to \a vertically and \a horizontally.
     */
    Q_INVOKABLE void setMaximize(bool vertically, bool horizontally, const RectF &restore = RectF());

    QPalette palette();
    const Decoration::DecorationPalette *decorationPalette();

    /*!
     * Returns whether the window is resizable or has a fixed size.
     */
    virtual bool isResizable() const = 0;
    /*!
     * Returns whether the window is moveable or has a fixed position.
     */
    virtual bool isMovable() const = 0;
    /*!
     * Returns whether the window can be moved to another screen.
     */
    virtual bool isMovableAcrossScreens() const = 0;

    const WindowRules *rules() const
    {
        return &m_rules;
    }
    void removeRule(Rules *r);
    void setupWindowRules();
    void finishWindowRules();
    void evaluateWindowRules();
    virtual void updateWindowRules(Rules::Types selection);
    virtual void applyWindowRules();
    virtual bool supportsWindowRules() const;

    bool wantsTabFocus() const;
    virtual void takeFocus();
    virtual bool wantsInput() const = 0;
    void checkWorkspacePosition(RectF oldGeometry = RectF(), LogicalOutput *oldOutput = nullptr);
    // The workspace-position decision of checkWorkspacePosition, read-only: nothing for a deleted,
    // dock, desktop or unplaceable window; the legacy special-state branch (which also moves the
    // restore fields between the areas) for a fullscreen, maximized or quick-tiled request, with
    // the rectangle it starts from and both areas; otherwise the rectangle after the strut, edge,
    // offscreen, size-constraint and rule checks, which the wrapper requests publicly even when it
    // equals the current one.
    struct WorkspacePositionDecision
    {
        enum class Kind {
            NoOp,
            LegacySpecial,
            Geometry,
        };
        Kind kind = Kind::NoOp;
        RectF geometry;
        RectF oldScreenArea;
        RectF screenArea;
    };
    WorkspacePositionDecision workspacePositionDecision(RectF oldGeometry, LogicalOutput *oldOutput, const RectF &current) const;
#if KWIN_BUILD_X11
    virtual xcb_timestamp_t userTime() const;
#endif

    void keyPressEvent(QKeyCombination key_code);

    virtual void pointerEnterEvent(const QPointF &globalPos);
    virtual void pointerLeaveEvent();

    // a helper for the workspace window packing. tests for screen validity and updates since in maximization case as with normal moving
    void packTo(qreal left, qreal top);

    Tile *tile() const;
    void commitTile(Tile *tile);
    Tile *requestedTile() const;
    void requestTile(Tile *tile);
    void forgetTile(Tile *tile);
    void setTileCompatibility(Tile *tile);

    void handleQuickTileShortcut(QuickTileMode mode);
    void setQuickTileModeAtCurrentPosition(QuickTileMode mode);
    void setQuickTileMode(QuickTileMode mode, const QPointF &tileAtPoint);
    QuickTileMode quickTileMode() const;
    QuickTileMode requestedQuickTileMode() const;
    /**
     * The generation of the live transient placement ownership this window currently holds (an
     * Owned record that is not being released), or zero. Routing only: it identifies the ownership
     * across a callback-producing call and says nothing about the record's validity.
     */
    quint64 ownedTransientPlacementGeneration() const;
    /**
     * Whether any transient-ownership record lives on this window (owned, restoring or
     * suppressed). The placement tracker's ownership-aware capture filter: while a record
     * lives, the window's geometry and modes are the owner's automatic placement, not the
     * user's, so the tracker keeps the last user placement instead of recording it.
     */
    bool hasTransientOwnershipRecord() const
    {
        return m_transientOwnership != nullptr;
    }
    /**
     * The explicit untile-or-restore action's consumption. Only an owned record may claim one
     * qualified restoration attempt; a record already Restoring (an attempt or a release in
     * flight) consumes the action without another attempt or legacy fallthrough; an unsupported
     * state consumes it with no start. NotOwned leaves the ordinary action path to the caller;
     * a prepared record does not own its placement yet and also reports NotOwned.
     */
    enum class TransientRestoreConsumption {
        NotOwned,
        RestoreStarted,
        RestoreStopped,
        RestoreInFlight,
        NoStart,
    };
    TransientRestoreConsumption consumeExplicitUntileOrRestore();
    /**
     * The end of a released record that no explicit restoration will complete. A release that
     * is not the explicit restore is detach-only: the window stays where it stands and its
     * record (Restoring) keeps the automatic rectangle out of the placement memory. The policy
     * that admitted the window ends that record once the window stands on a frame that is
     * known not to be automatic: its floating frame (the policy's own ordinary request for
     * transientFloatingFrame(), or the compositor's replay of it), or where the user's own
     * interactive move or resize left it and no later request has moved it. Any other frame
     * can be an automatic rectangle (the
     * tile's frame the window was admitted to, the tile's rectangle a work-area re-check asks
     * for as it releases the window, either of them carried to another output) and is
     * answered AtManagedRectangle. Settled only for the provider that admitted the window,
     * or once that provider has retired for the writer's current one (StaleProvider
     * otherwise, also when the writer no longer holds the record's reservation), with no
     * live restoration (NotReleased), in an ordinary mode outside
     * every tile and with the native geometry settled: nothing scheduled, sent or
     * acknowledged, the frame the requested geometry (Unsettled otherwise). Every answer but
     * Settled changes nothing. The writer finalizes as it does for a completed restoration:
     * a deferred update for the key is applied, else the entry stays.
     */
    enum class TransientSettleResult {
        Settled,
        NotReleased,
        StaleProvider,
        Unsettled,
        AtManagedRectangle,
        Refused,
    };
    TransientSettleResult settleReleasedTransientOwnership(const QUuid &provider);
    /**
     * The floating frame of this window's transient record. Of an owned record: where an
     * explicit restoration returns the window, exactly. Of a released one: that frame,
     * carried along when the window was sent to another output or its output went, as the
     * window takes it on the output it is on now (transientFloatingImage()) - the rectangle
     * the policy asks for to return the window, and on which the record settles. Null
     * without a record.
     */
    RectF transientFloatingFrame() const;
    /**
     * NT04's read-only state/completion contract. The state query reflects the native record
     * only - no protocol logging and no timeout participates - for the placement policy that
     * must leave a restoring window alone. A suppressed (cancelled) record still holds its
     * window as pending; a configure-less published restoration is its own state awaiting the
     * client's next commit. The completion signal fires exactly at the boundary with the
     * restored frame; cancellation, supersession and close never emit it.
     */
    enum class TransientRestorationState {
        NotRestoring,
        RestoringPending,
        RestoringPublished,
    };
    TransientRestorationState transientRestorationState() const;
    /**
     * NT06: translate the saved floating frame when the window is sent to another output. The
     * record is already released (Restoring); the frame moves by the same area translation the
     * transfer applies to the window's own geometry, so a later explicit restore returns to the
     * translated floating position on the destination, and the ownership is released exactly
     * once - no destination adoption.
     */
    void rebaseTransientFloatingOnOutputTransfer(const RectF &fromArea, const RectF &toArea);

    void handleCustomQuickTileShortcut(QuickTileMode mode);

    Layer layer() const;
    void updateLayer();

    /*!
     * Returns \c true if the Client is being interactively moved; otherwise \c false.
     */
    bool isInteractiveMove() const
    {
        return isInteractiveMoveResize() && interactiveMoveResizeGravity() == Gravity::Center;
    }
    /*!
     * Returns \c true if the Client is being interactively resized; otherwise \c false.
     */
    bool isInteractiveResize() const
    {
        return isInteractiveMoveResize() && interactiveMoveResizeGravity() != Gravity::Center;
    }
    Gravity interactiveMoveResizeGravity() const
    {
        return m_interactiveMoveResize.gravity;
    }
    QPointF interactiveMoveResizeAnchor() const
    {
        return m_interactiveMoveResize.anchor;
    }
    /*!
     * Cursor shape for move/resize mode.
     */
    CursorShape interactiveMoveResizeCursor() const
    {
        return m_interactiveMoveResize.cursor;
    }
    uint32_t interactiveMoveResizeCount() const;

    void updateInteractiveMoveResize(const QPointF &global, Qt::KeyboardModifiers modifiers);
    /*!
     * Ends move resize when all pointer buttons are up again.
     */
    void endInteractiveMoveResize();
    void cancelInteractiveMoveResize();

    virtual StrutRect strutRect(StrutArea area) const;
    StrutRects strutRects() const;
    virtual bool hasStrut() const;

    void setModal(bool modal);
    bool isModal() const;

    /*!
     * Determines the mouse command for the given \a button in the current state.
     */
    std::optional<Options::MouseCommand> getMousePressCommand(Qt::MouseButton button) const;
    std::optional<Options::MouseCommand> getMouseReleaseCommand(Qt::MouseButton button) const;
    std::optional<Options::MouseCommand> getWheelCommand(Qt::Orientation orientation) const;
    bool mousePressCommandConsumesEvent(Options::MouseCommand command) const;
    /*!
     * Returns whether or not the command consumes the event that triggered it
     */
    bool performMousePressCommand(Options::MouseCommand, const QPointF &globalPos);
    void performMouseReleaseCommand(Options::MouseCommand, const QPointF &globalPos);

    // decoration related
    Qt::Edge titlebarPosition() const;
    bool titlebarPositionUnderMouse() const;
    KDecoration3::Decoration *decoration() const
    {
        return m_decoration.decoration.get();
    }
    virtual KDecoration3::Decoration *nextDecoration() const;
    bool isDecorated() const
    {
        return m_decoration.decoration != nullptr;
    }
    Decoration::DecoratedWindowImpl *decoratedWindow() const;
    void setDecoratedWindow(Decoration::DecoratedWindowImpl *client);
    bool decorationHasAlpha() const;
    void triggerDecorationRepaint();
    void layoutDecorationRects(RectF &left, RectF &top, RectF &right, RectF &bottom) const;
    void processDecorationMove(const QPointF &localPos, const QPointF &globalPos);
    bool processDecorationButtonPress(const QPointF &localPos, const QPointF &globalPos, Qt::MouseButton button, bool ignoreMenu = false);
    void processDecorationButtonRelease(Qt::MouseButton button);

    virtual void invalidateDecoration();

    bool noBorder() const;
    void setNoBorder(bool set);
    bool userCanSetNoBorder() const;

    virtual DecorationPolicy decorationPolicy() const;
    virtual void setDecorationPolicy(DecorationPolicy policy);

    /*!
     * Returns whether the window provides context help or not. If it does,
     * you should show a help menu item or a help button like '?' and call
     * contextHelp() if this is invoked.
     *
     * Default implementation returns \c false.
     * \sa showContextHelp;
     */
    virtual bool providesContextHelp() const;

    /*!
     * Invokes context help on the window. Only works if the window
     * actually provides context help.
     *
     * Default implementation does nothing.
     *
     * \sa providesContextHelp()
     */
    virtual void showContextHelp();

    /*!
     * Returns the geometry of the virtual keyboard
     * This geometry is in global coordinates
     */
    RectF virtualKeyboardGeometry() const;

    /*!
     * Sets the geometry of the virtual keyboard, The window may resize itself in order to make space for the keyboard
     * This geometry is in global coordinates
     */
    virtual void setVirtualKeyboardGeometry(const RectF &geo);

    /*!
     * Restores the Window after it had been hidden due to show on screen edge functionality.
     * The Window also gets raised (e.g. Panel mode windows can cover) and the Window
     * gets informed in a window specific way that it is shown and raised again.
     */
    virtual void showOnScreenEdge();

    QString desktopFileName() const
    {
        return m_desktopFileName;
    }

    /*!
     * Helper function to compute the icon out of an application id defined by \a fileName
     *
     * Returns an icon name that can be used with QIcon::fromTheme()
     */
    static QString iconFromDesktopFile(const QString &fileName);

    static QString findDesktopFile(const QString &fileName);

    /*!
     * Tries to terminate the process of this Window.
     *
     * Implementing subclasses can perform a windowing system solution for terminating.
     */
    virtual void killWindow() = 0;
    virtual void destroyWindow() = 0;

    enum class SameApplicationCheck {
        RelaxedForActive = 1 << 0,
        AllowCrossProcesses = 1 << 1,
    };
    Q_DECLARE_FLAGS(SameApplicationChecks, SameApplicationCheck)
    static bool belongToSameApplication(const Window *c1, const Window *c2, SameApplicationChecks checks = SameApplicationChecks());
    virtual bool belongsToDesktop() const;

    bool hasApplicationMenu() const;
    bool applicationMenuActive() const
    {
        return m_applicationMenuActive;
    }
    void setApplicationMenuActive(bool applicationMenuActive);

    QString applicationMenuServiceName() const
    {
        return m_applicationMenuServiceName;
    }
    QString applicationMenuObjectPath() const
    {
        return m_applicationMenuObjectPath;
    }

    /*!
     * Request showing the application menu bar
     * \a actionId The DBus menu ID of the action that should be highlighted, 0 for the root menu
     */
    void showApplicationMenu(int actionId);

    virtual QString preferredColorScheme() const;
    QString colorScheme() const;
    void setColorScheme(const QString &colorScheme);

    bool unresponsive() const;

    /*!
     * Default implementation returns \c null.
     * Mostly intended for X11 clients, from EWMH:
     * @verbatim
     * If the WM_TRANSIENT_FOR property is set to None or Root window, the window should be
     * treated as a transient for all other windows in the same group. It has been noted that this
     * is a slight ICCCM violation, but as this behavior is pretty standard for many toolkits and
     * window managers, and is extremely unlikely to break anything, it seems reasonable to document
     * it as standard.
     * @endverbatim
     */
    virtual bool groupTransient() const;
    /*!
     * Default implementation returns \c null.
     *
     * Mostly for X11 clients, holds the client group
     */
    virtual const Group *group() const;
    /*!
     * Default implementation returns \c null.
     *
     * Mostly for X11 clients, holds the client group
     */
    virtual Group *group();

    /*!
     * Return window management interface
     */
    PlasmaWindowInterface *windowManagementInterface() const
    {
        return m_windowManagementInterface;
    }

    /*!
     * Sets the last user usage serial of the surface as \a serial
     * TODO make the QPA responsible for this, it's only needed
     * for KWindowSystem / for internal windows
     */
    void setLastUsageSerial(quint32 serial);
    quint32 lastUsageSerial() const;

    void refOffscreenRendering();
    void unrefOffscreenRendering();
    bool isOffscreenRendering() const;

    qreal targetScale() const;
    qreal nextTargetScale() const;
    void setNextTargetScale(qreal scale);

    OutputTransform preferredBufferTransform() const;
    void setPreferredBufferTransform(OutputTransform transform);

    const std::shared_ptr<ColorDescription> &preferredColorDescription() const;
    void setPreferredColorDescription(const std::shared_ptr<ColorDescription> &description);

    QString tag() const;
    QString description() const;

    void setActivationToken(const QString &token);
    QString activationToken() const;

    bool excludeFromCapture() const;
    void setExcludeFromCapture(bool newExcludeFromCapture);

public Q_SLOTS:
    virtual void closeWindow() = 0;

protected Q_SLOTS:
    void setReadyForPainting();

Q_SIGNALS:
    void stackingOrderChanged();
    void opacityChanged(KWin::Window *window, qreal oldOpacity);
    void damaged(KWin::Window *window);
    void inputTransformationChanged();
    void closed();
    /*!
     * Emitted whenever the Window's screen changes. This can happen either in consequence to
     * a screen being removed/added or if the Window's geometry changes.
     * \since 4.11
     * \a oldOutput The window's previous output.
     */
    void outputChanged(LogicalOutput *oldOutput);
    void skipCloseAnimationChanged();
    /*!
     * Emitted whenever the window role of the window changes.
     * \since 5.0
     */
    void windowRoleChanged();
    /*!
     * Emitted whenever the window class name or resource name of the window changes.
     * \since 5.0
     */
    void windowClassChanged();

    /*!
     * Emitted whenever the Surface for this Window changes.
     */
    void surfaceChanged();

    /*!
     * Emitted whenever the window's shadow changes.
     * \since 5.15
     */
    void shadowChanged();

    /*!
     * This signal is emitted when the Window's buffer geometry changes.
     */
    void bufferGeometryChanged(const KWin::RectF &oldGeometry);
    /*!
     * This signal is emitted when the Window's frame geometry changes.
     */
    void frameGeometryChanged(const KWin::RectF &oldGeometry);
    /*!
     * This signal is emitted when the Window's client geometry has changed.
     */
    void clientGeometryChanged(const KWin::RectF &oldGeometry);

    /*!
     * This signal is emitted when the frame geometry is about to change. The new geometry is not known yet
     */
    void frameGeometryAboutToChange();

    /*!
     * This signal is emitted when associated tile has changed, including from and to none
     */
    void tileChanged(KWin::Tile *tile);
    void requestedTileChanged();
    /**
     * This signal is emitted when the requested geometry (moveResizeGeometry()) differs from the
     * value it was last emitted for, after KWin has processed the move, resize or commit that
     * changed it.
     */
    void requestedGeometryChanged();

    void fullScreenChanged();
    void skipTaskbarChanged();
    void skipPagerChanged();
    void skipSwitcherChanged();
    void iconChanged();
    void activeChanged();
    void keepAboveChanged(bool);
    void keepBelowChanged(bool);
    /*!
     * Emitted whenever the demands attention state changes.
     */
    void demandsAttentionChanged();
    void desktopsChanged();
    void activitiesChanged();
    void minimizedChanged();
    void paletteChanged(const QPalette &p);
    void colorSchemeChanged();
    void captionChanged();
    void captionNormalChanged();
    void maximizedAboutToChange(MaximizeMode mode);
    void maximizedChanged();
    void transientChanged();
    void modalChanged();
    void quickTileModeChanged();
    void moveResizedChanged();
    void moveResizeCursorChanged(CursorShape);
    void interactiveMoveResizeStarted();
    void interactiveMoveResizeStepped(const KWin::RectF &geometry);
    void interactiveMoveResizeFinished();
    void closeableChanged(bool);
    void minimizeableChanged(bool);
    void maximizeableChanged(bool);
    void desktopFileNameChanged();
    void applicationMenuChanged();
    void hasApplicationMenuChanged(bool);
    void applicationMenuActiveChanged(bool);
    void unresponsiveChanged(bool);
    void decorationChanged();
    void hiddenChanged();
    void hiddenByShowDesktopChanged();
    void lockScreenOverlayChanged();
    void readyForPaintingChanged();
    void maximizeGeometryRestoreChanged();
    void transientRestorationCompleted(const QRectF &restoredFrame);
    void transientReleaseSettled(const QRectF &frame);
    void fullscreenGeometryRestoreChanged();
    void offscreenRenderingChanged();
    void targetScaleChanged();
    void nextTargetScaleChanged();
    void tagChanged();
    void descriptionChanged();
    void borderRadiusChanged();
    void excludeFromCaptureChanged();
    void decorationPolicyChanged();

private:
    friend class TileAssignmentFence;
    friend class Tile;
    friend class WaylandWindow;
    friend class XdgSurfaceWindow;
    friend class XdgToplevelWindow;
    friend class XdgSurfaceConfigure;
    friend class X11Window;
    friend class TransientTileController;
    friend class TilesTest;
    friend class ::TestXdgShellWindow;

    TileAssignmentFence makeTileAssignmentFence();
    class TransientTileRequest;

    struct GeometryRequestOrigin
    {
        enum class Kind {
            Unclassified,
            Ordinary,
            OwnedGeometry,
            Restoration,
        };
        Kind kind = Kind::Unclassified;
        quint64 geometryGeneration = 0;
        quint64 ownershipGeneration = 0;
        quint64 restoreSequence = 0;
        bool operator==(const GeometryRequestOrigin &) const = default;
    };
    enum class ConfigurePlacementPolicy {
        ContentOnly,
        PlacementOnly,
        Synchronize,
    };

    enum class AssignmentStepOutcome {
        Refused,
        Superseded,
        AppliedCurrent,
    };
    // Provenance of one native call: an exact borrowed adoption request, an owner acting on its
    // placement, or an explicit restoration attempt. Each can lapse without a Window counter
    // changing, so every child validates it beside its checkpoint; public callbacks inherit none.
    struct OwnedProvenance
    {
        enum class Kind {
            None,
            Request,
            Ownership,
            Restoration,
            // An ordinary operation's own validity (for example a normalization's requested-mode
            // revision), evaluated like an ownership constraint but without any ownership
            // exemption: the request path treats it as ordinary provenance.
            Operation,
        };
        // A declared constructor rather than member initializers: those would be a complete-class
        // context of Window and unusable in the default arguments below.
        OwnedProvenance()
            : kind(Kind::None)
            , request(nullptr)
            , ownership(0)
            , restoreSequence(0)
        {
        }
        static OwnedProvenance forRequest(TransientTileRequest *request)
        {
            OwnedProvenance owned;
            owned.kind = Kind::Request;
            owned.request = request;
            return owned;
        }
        // The constraint carries the owner operation's own validity (for example that the frozen
        // target is still feasible under the client's size limits), which neither the fence nor
        // the ownership record can observe; every child step evaluates it next to them.
        static OwnedProvenance forOwnership(quint64 generation, std::function<bool()> constraint = {})
        {
            OwnedProvenance owned;
            owned.kind = Kind::Ownership;
            owned.ownership = generation;
            owned.constraint = std::move(constraint);
            return owned;
        }
        static OwnedProvenance forRestoration(quint64 generation, quint64 sequence)
        {
            OwnedProvenance owned;
            owned.kind = Kind::Restoration;
            owned.ownership = generation;
            owned.restoreSequence = sequence;
            return owned;
        }
        // The incoming provenance with one more constraint chained onto it (both must hold); a
        // provenance without kind becomes an Operation one. Every kind evaluates its constraint
        // in addition to its own identity, so the operation's validity is carried into every
        // child boundary and every later step that receives the result.
        OwnedProvenance withConstraint(std::function<bool()> extra) const
        {
            OwnedProvenance chained = *this;
            if (chained.kind == Kind::None) {
                chained.kind = Kind::Operation;
            }
            if (!chained.constraint) {
                chained.constraint = std::move(extra);
            } else {
                chained.constraint = [previous = chained.constraint, extra = std::move(extra)]() {
                    return previous() && extra();
                };
            }
            return chained;
        }

        Kind kind;
        TransientTileRequest *request;
        quint64 ownership;
        quint64 restoreSequence;
        std::function<bool()> constraint;
    };
    struct AssignmentStepResult
    {
        AssignmentStepOutcome outcome = AssignmentStepOutcome::Refused;
        std::unique_ptr<TileAssignmentFence> checkpoint;
        OwnedProvenance owned;
    };
    bool ownedProvenanceIsCurrent(const OwnedProvenance &owned) const;
    bool assignmentResultIsCurrent(const AssignmentStepResult &result) const;
    // An owner may act on its own placement only while this holds: live Owned record for exactly
    // this generation, nothing executing or releasing, writer/provider/reservation valid, live tile
    // with this window as member in a current context, and the association unchanged. Admission
    // feasibility is deliberately not reused: an owner may legitimately be maximized.
    bool transientOwnershipIsCurrent(quint64 generation) const;
    // A heap checkpoint for a result carrier; the fence constructor itself stays Window-private.
    std::unique_ptr<TileAssignmentFence> makeTileAssignmentCheckpoint();
    // Normalizes a maximized window to the Restore mode for an ordinary guarded ingress, with its
    // own recorded writes and guarded child calls. Refused without effect on a backend without
    // guarded requests, for an owner or an active request, and for every mutation-free refusal of
    // the backend; Superseded once a boundary ends it after the mode write; AppliedCurrent carries
    // the request's checkpoint and a provenance whose constraint every later step evaluates.
    [[nodiscard]] virtual AssignmentStepResult normalizeMaximizeForAssignment(const TileAssignmentFence &before, OwnedProvenance owned = {});
    // A read-only check, never a grant: whether the backend's normalization step would refuse now
    // (its mutation-free refusals, including its own headroom). The base backend has no step.
    virtual bool canNormalizeMaximizeForAssignment() const;
    // The guarded request's worst case in placement continuation increments: one restore capture,
    // at most seven synchronization writes and one committed change.
    static constexpr quint64 assignmentPlacementBudget = 1 + 7 + 1;
    // A read-only check, never a grant: whether requestTileForAssignment(tile, ...) would refuse now.
    // Mutation-free refusals (a transient tile, a backend without guarded requests, an owner or an
    // active request, a transient-generation marker no ordinary step may consume, an interactive
    // operation, the normalization's own refusals when tiling a maximized window) and the whole
    // path's conservative headroom (the normalization's capture and request, then this step's).
    bool canRequestTileForAssignment(Tile *tile) const;
    // The guarded requested-association step for an ordinary ingress: its own recorded writes and
    // guarded child calls (the 2d2 normalization when needed, whose receipt is carried forward; the
    // restore capture; the association write; the geometry request), the quick-tile protocol state
    // and notification as an obligation of the association write discharged while that write is
    // current even when the geometry request stopped, and one terminal validation before the
    // result is returned. Refused only without any effect; after the first effect every stop is
    // Superseded. The interactive untiling adjustment stays on the legacy path, which this step
    // never reaches because interactive operations are refused.
    [[nodiscard]] AssignmentStepResult requestTileForAssignment(Tile *tile, const TileAssignmentFence &before, OwnedProvenance owned = {});
    [[nodiscard]] AssignmentStepResult forgetTileForAssignment(Tile *tile, const TileAssignmentFence &before, OwnedProvenance owned = {});
    // A read-only check, never a grant: whether forgetTileForAssignment would refuse now.
    bool canForgetTileForAssignment() const;
    // The public bodies after their cancellation, verbatim: selected by the public entry points
    // when the guarded step refused without effect, or at the exhaustion edge.
    void requestTileLegacy(Tile *tile);
    void forgetTileLegacy(Tile *tile);
    // The compatibility setter's legacy sequence (its own cancellation, verbatim) and the remainder
    // after a cancellation: selected for a backend without guarded requests, at the exhaustion
    // edge, or on an ingress refused without effect.
    void setTileCompatibilityLegacy(Tile *tile);
    void setTileCompatibilityLegacyRemainder(Tile *tile);
    [[nodiscard]] AssignmentStepResult commitTileForAssignment(Tile *target, const TileAssignmentFence &before);
    [[nodiscard]] AssignmentStepResult setGeometryRestoreForAssignment(RectF desired, const TileAssignmentFence &before, OwnedProvenance owned = {});
    void setGeometryRestoreInternal(const RectF &desired, AssignmentStepResult *guardedResult = nullptr);
    // The fullscreen restore field is versioned like the maximize one: its callbacks invalidate
    // older continuation snapshots, so an owner's write can tell a handler's replacement (or a
    // change away and back) from its own value.
    [[nodiscard]] AssignmentStepResult setFullscreenGeometryRestoreForAssignment(RectF desired, const TileAssignmentFence &before, OwnedProvenance owned = {});
    void setFullscreenGeometryRestoreInternal(const RectF &desired, AssignmentStepResult *guardedResult = nullptr);
    void writeRestoreFieldInternal(RectF &field, const RectF &desired, void (Window::*changed)(), AssignmentStepResult *guardedResult);
    struct AssignmentGeometrySync
    {
        AssignmentStepResult result;
        QPointer<LogicalOutput> output;
        qreal scale = 1;
        OutputTransform transform = OutputTransform::Normal;
        std::shared_ptr<ColorDescription> color;
        bool hadOutput = false;
        bool synchronize = false;
        // Joins the result carrier once the requested rectangle is written: a borrowed request's own
        // predicate cannot hold between generation allocation and that callback-free write.
        OwnedProvenance owned;
    };
    [[nodiscard]] AssignmentStepResult setMoveResizeGeometryForAssignment(RectF desired, const TileAssignmentFence &before);
    bool assignmentGeometrySyncIsCurrent(AssignmentGeometrySync &sync) const;
    void setMoveResizeGeometryInternal(const RectF &rect, AssignmentGeometrySync *sync = nullptr);
    void setMoveResizeOutputInternal(LogicalOutput *output, AssignmentGeometrySync *sync = nullptr);
    void setOutputPropertiesPending(bool pending, AssignmentGeometrySync *sync = nullptr);
    void setNextTargetScaleInternal(qreal scale, AssignmentGeometrySync *sync = nullptr);
    void setPreferredBufferTransformInternal(OutputTransform transform, AssignmentGeometrySync *sync = nullptr);
    void setPreferredColorDescriptionInternal(const std::shared_ptr<ColorDescription> &description, AssignmentGeometrySync *sync = nullptr);
    quint64 prepareAssignmentGeometrySync(const RectF &desired, AssignmentGeometrySync &sync) const;

    enum class AssignmentRequestDispatch {
        None,
        Published,
        Queued,
    };
    struct AssignmentGeometryRequest
    {
        AssignmentStepResult result;
        AssignmentRequestDispatch dispatch = AssignmentRequestDispatch::None;
        // The geometry-request generation this attempt allocated; zero while nothing was allocated.
        quint64 requestGeneration = 0;
    };
    ConfigurePlacementPolicy configurePlacementPolicy(const GeometryRequestOrigin &origin) const;
    [[nodiscard]] AssignmentStepResult synchronizeConfigureGeometry(RectF desired, GeometryRequestOrigin origin, std::function<bool()> dispatchIsCurrent);

    enum class TransientRequestOutcome {
        Rejected,
        Superseded,
        Applied,
    };
    enum class TransientRequestProgress {
        NoRequest,
        Requested,
    };
    struct TransientRequestReceipt
    {
        TransientRequestOutcome outcome = TransientRequestOutcome::Rejected;
        TransientRequestProgress progress = TransientRequestProgress::NoRequest;
        quint64 ownershipGeneration = 0;
        // Identifies this private attempt, not a geometry generation or Wayland serial.
        quint64 nativeSequence = 0;
        QUuid windowId;
        std::shared_ptr<const TransientNoRequestProof> noRequestProof;
    };
    // The application of an ownership with its qualified continuation, for a caller that goes on
    // after it (the admission's publication). The receipt is the one applyTransientOwnership
    // returns: this attempt's progress, identity and NoRequest proof, the restoration's evidence.
    // The continuation supplements it: AppliedCurrent only for an applied attempt whose checkpoint
    // (the final checkpoint of the native request's geometry result, which includes that request's
    // own subsequent writes; validated by the request up to its last boundary and preserved
    // exactly) and stable provenance (the ownership generation, with this
    // attempt's native sequence and progress, its allocated request generation and its target
    // bound by value, chained with the caller's admission constraint) still hold at the terminal
    // validation, after the request's destruction and the end of the execution. Superseded, with
    // no checkpoint, for an attempt that ended without a current application, whether it was
    // rejected, superseded or applied and then invalidated before that validation; Refused when
    // the application did not start. It is evidence of this attempt, never a permission to replay
    // it, and is never refreshed: a later change is seen by assignmentResultIsCurrent, not here.
    struct TransientApplication
    {
        TransientRequestReceipt receipt;
        AssignmentStepResult continuation;
    };
    class TransientTileRequest
    {
    public:
        virtual ~TransientTileRequest() = default;

    protected:
        TransientTileRequest(Window *window, Tile *tile, quint64 ownershipGeneration);
        // Read-only: validate published controller identity and full window context/membership.
        virtual bool authorityIsCurrent() const = 0;

    private:
        friend class Window;
        Q_DISABLE_COPY_MOVE(TransientTileRequest)
        QPointer<Window> m_window;
        QPointer<Tile> m_tile;
        QPointer<TileManager> m_manager;
        QPointer<LogicalOutput> m_output;
        QPointer<VirtualDesktop> m_desktop;
        RectF m_target;
        RectF m_floating;
        QUuid m_windowId;
        quint64 m_ownerGeneration;
        quint64 m_initialGeometryGeneration = 0;
        quint64 m_geometryGeneration = 0;
        quint64 m_sequence = 0;
        TransientRequestProgress m_progress = TransientRequestProgress::NoRequest;
        bool m_cancelled = false;
        bool m_associationInstalled = false;
    };

    enum class TransientOwnershipPhase {
        Prepared,
        Owned,
        Restoring,
    };
    enum class TransientRestoreStage {
        Claimed,
        Normalizing,
        Dispatching,
        // The synchronous wrapper returned; this is not native completion.
        Returned,
    };
    struct TransientRestoreAttempt
    {
        TransientRestoreAttempt(quint64 sequence, QStringList activities, bool normalizeMaximize, bool normalizeFullScreen, std::function<bool()> stateIsCurrent)
            : sequence(sequence)
            , activities(std::move(activities))
            , normalizeMaximize(normalizeMaximize)
            , normalizeFullScreen(normalizeFullScreen)
            , stateIsCurrent(std::move(stateIsCurrent))
        {
        }
        const quint64 sequence;
        const QStringList activities;
        const bool normalizeMaximize;
        const bool normalizeFullScreen;
        // A configure-less (published) restoration waits for the client's next plain commit.
        bool publishedWithoutConfigure = false;
        // The request returned current: set at requestTransientRestore's success exit, after its
        // last check. From then on the restoration is the client's to answer, and a provider
        // that retires no longer supersedes it (transientRestoreAttemptIsCurrent).
        bool requestReturned = false;
        // Backend state only: captured values, no record/stack references or retained Window.
        std::function<bool()> stateIsCurrent;
        bool cancelled = false;
        TransientRestoreStage stage = TransientRestoreStage::Claimed;
        bool requestConsumed() const
        {
            return stage != TransientRestoreStage::Claimed;
        }
        bool requesting() const
        {
            return stage == TransientRestoreStage::Dispatching;
        }
        // Published before any callback from this attempt's geometry request; never reused.
        quint64 requestGeneration = 0;
    };
    struct TransientRestoreClaim
    {
        QUuid windowId;
        quint64 ownershipGeneration = 0;
        quint64 sequence = 0;
        AssignmentStepResult continuation;
    };
    struct TransientOwnership
    {
        TransientOwnership(TransientPlacementWriter *writer, const QUuid &provider, quint64 providerRevision,
                           quint64 generation, std::unique_ptr<TransientTileRequest> request);

        QPointer<TransientPlacementWriter> writer;
        const QUuid provider;
        const quint64 providerRevision;
        const quint64 generation;
        RectF floating;
        const RectF target;
        const QPointer<Tile> tile;
        const QPointer<TileManager> manager;
        const QPointer<LogicalOutput> output;
        const QPointer<VirtualDesktop> desktop;
        const quint64 initialGeometryGeneration;
        QPointer<TransientTileController> controller;
        TransientOwnershipPhase phase = TransientOwnershipPhase::Prepared;
        // Set when the controller's admission has returned Accepted. Until then the provider's
        // revision is part of the admission's proof and a record supersedes the admission;
        // from then on only the provider's incarnation binds the record (the contract: "While K
        // is reserved, accepted writes for other keys proceed normally", and "completion may
        // re-record the restored frame subject to all normal guards").
        bool admitted = false;
        TransientRequestProgress progress = TransientRequestProgress::NoRequest;
        quint64 nativeSequence = 0;
        bool executing = false;
        bool releasing = false;
        bool releaseWriterReady = false;
        bool committed = false;
        // A released record's window was moved or resized by the user's own interactive
        // operation, which ended without being cancelled: the window's geometry request
        // generation at that end. An operation that moved nothing leaves no mark. While the
        // generation is still the window's, where the window stands is
        // the user's placement, not an automatic rectangle; any later request (the
        // workspace's re-check, a script's) makes the evidence stale
        // (settleReleasedTransientOwnership).
        std::optional<quint64> placedByUser;
        QMetaObject::Connection closeCleanup;
        std::shared_ptr<const TransientNoRequestProof> noRequestProof;
        std::unique_ptr<TransientTileRequest> request;
        std::optional<TransientRestoreAttempt> restoreAttempt;
    };

    struct TransientAssociationChanges
    {
        QPointer<Tile> requested;
        QPointer<Tile> committed;
        quint64 requestedChange = 0;
        quint64 committedChange = 0;
        bool requestedChanged = false;
        bool committedChanged = false;
        bool modeChanged = false;
    };

    quint64 prepareTransientOwnership(TransientPlacementWriter *writer, const QUuid &provider, const QString &key,
                                      quint64 providerRevision, quint64 mapRevision, std::unique_ptr<TransientTileRequest> request);
    bool activateTransientOwnership(quint64 generation);
    TransientRequestReceipt applyTransientOwnership(quint64 generation);
    // The same application with its continuation (see TransientApplication). The admission
    // constraint is the caller's own stable validity (the controller, tree and admission bindings
    // the window cannot observe), evaluated with the continuation at every later boundary; it must
    // own its state, never borrow the request's.
    TransientApplication applyTransientOwnershipGuarded(quint64 generation, std::function<bool()> admissionConstraint = {});
    // The window-side part of a transient admission's complete preflight, mutation-free, reported
    // apart so the admission can refuse a known shortage without calling the window ineligible.
    // Eligible: no ownership, active request, requested or committed association or unresolved
    // transient marker; a backend that can start the request and report what it did; the native
    // geometry settled. Headroom: the whole synchronous admission and its structural failure
    // cleanup (the membership addition and a possible removal, the association install and a
    // possible detach, a committed detach, the restore capture with the guarded request's
    // placement bound, the request and update generations, the ownership generation and the
    // native sequence under their non-reuse rules), never only the successful prefix; the later
    // asynchronous commit's own headroom is separate.
    bool transientAdmissionEligible(const RectF &target) const;
    bool transientAdmissionHeadroom() const;
    // The provider side of an ownership's currency: the admission's exact revision until the
    // admission is done, the provider's incarnation after it (TransientOwnership::admitted).
    bool transientProviderIsCurrent(const TransientOwnership &ownership) const;
    // Claim one explicit, normal-mode restore without requesting geometry. The result retains
    // the Window externally; the ownership record never stores its own lifetime checkpoint.
    // AppliedCurrent qualifies the claim/release continuation, not restoration completion.
    [[nodiscard]] TransientRestoreClaim claimTransientRestore(quint64 generation);
    // The explicit action's consumption, declared publicly for the action dispatch.
    [[nodiscard]] virtual TransientRestoreClaim claimTransientRestoreWithMaximizeNormalization(quint64 generation);
    TransientRestoreClaim claimTransientRestoreWithState(quint64 generation, bool normalizeMaximize, bool normalizeFullScreen, std::function<bool()> stateIsCurrent);
    virtual std::function<bool()> transientRestoreStateConstraint(const RectF &target, bool normalizeMaximize, bool geometryReady) const;
    virtual AssignmentStepResult normalizeTransientRestore(AssignmentStepResult continuation, quint64 generation, quint64 sequence);
    // Consumes the original claim continuation, not a newly sampled fence. A matching request
    // is attempted at most once; a duplicate cannot cancel the original. No completion is implied.
    [[nodiscard]] AssignmentGeometryRequest requestTransientRestore(TransientRestoreClaim claim);
    // NT04: complete the restoration on its own acknowledged and committed configure. True only
    // when the boundary qualified and the record with its reservation was cleared.
    bool completeTransientRestoration(const GeometryRequestOrigin &origin);
    bool transientRestoreRequestIsCurrent(quint64 generation, quint64 sequence) const;
    bool transientRestoreOperationIsCurrent(quint64 generation, quint64 sequence) const;
    bool transientRestoreClaimIsCurrent(const TransientRestoreClaim &claim) const;
    // Identity/context validity only. A consumer must also validate its synchronous continuation;
    // this predicate is not a way to refresh a superseded claim or to authorize geometry by itself.
    bool transientRestoreAttemptIsCurrent(quint64 generation, quint64 sequence) const;
    void invalidateTransientRestoreAttempt();
    // With a result, the release's own callback-free writes are inside a checkpoint taken before
    // its notifications and validated once after its finalization (never refreshed): the caller's
    // continuation is AppliedCurrent only if nothing foreign changed. Without one, unchanged.
    bool revokeTransientOwnership(quint64 generation, AssignmentStepResult *guarded = nullptr);
    void qualifyReleaseContinuation(AssignmentStepResult &guarded) const;
    bool beginTransientRelease(quint64 generation);
    TransientAssociationChanges detachTransientAssociation(quint64 generation);
    void notifyTransientAssociation(quint64 generation, const TransientAssociationChanges &changes);
    bool finishTransientRelease(quint64 generation);
    std::shared_ptr<const TransientNoRequestProof> makeTransientNoRequestProof(const TransientTileRequest &request);
    TransientRequestReceipt requestTransientTile(std::unique_ptr<TransientTileRequest> request);
    // The request with its continuation translated while the request still exists: written only
    // for an applied attempt (its outcome provisional until the caller's terminal validation);
    // untouched otherwise, the caller concludes it.
    TransientRequestReceipt requestTransientTile(std::unique_ptr<TransientTileRequest> request, AssignmentStepResult *continuation,
                                                 const std::function<bool()> &admissionConstraint);
    void translateTransientContinuation(const TransientTileRequest &request, AssignmentStepResult &validated,
                                        const std::function<bool()> &admissionConstraint, AssignmentStepResult &continuation) const;
    bool transientRequestTargetIsCurrent(const TransientTileRequest &request) const;
    bool transientRequestIsCurrent(const TransientTileRequest &request) const;
    bool cancelTransientRequest();
    // The public cancellation boundary as a qualified continuation for a guarded ingress: the
    // placement stamp and, for an owner, the release's writes are the caller's own; the release's
    // notifications are callback boundaries. Refused only for exhaustion detected before the stamp;
    // afterwards Superseded when the continuation is stale (a deleted window, a foreign change), else
    // AppliedCurrent with the checkpoint to continue from, even when the release could not finalize
    // (the owner then stays, and a consumer's own entry refusal decides). The release completes
    // exactly as without a result; nothing is rolled back.
    [[nodiscard]] AssignmentStepResult cancelTransientRequestForAssignment();
    void commitTileInternal(Tile *tile, AssignmentStepResult *guardedResult = nullptr);
    // Every actual tile membership change of this window (ordinary or transient) is recorded here
    // before its notification, as are the otherwise-valid public membership no-ops (managing an
    // existing member, unmanaging or forgetting a non-member), which are newer same-window intent.
    // Refusals and internal evacuation misses record nothing. Saturating.
    void noteTileMembershipChanged();

protected:
    Window();

    virtual std::unique_ptr<WindowItem> createItem(Item *parentItem) = 0;

    void setResourceClass(const QString &name, const QString &className = QString());
    void setIcon(const QIcon &icon);
    void startAutoRaise();
    void autoRaise();
    bool isMostRecentlyRaised() const;
    void markAsDeleted();
    void markAsPlaced();
    /*!
     * Whether the window accepts focus.
     * The difference to wantsInput is that the implementation should not check rules and return
     * what the window effectively supports.
     */
    virtual bool acceptsFocus() const = 0;
    /*!
     * Called from setActive once the active value got updated, but before the changed signal
     * is emitted.
     *
     * Default implementation does nothing.
     */
    virtual void doSetActive();
    /*!
     * Called from setKeepAbove once the keepBelow value got updated, but before the changed signal
     * is emitted.
     *
     * Default implementation does nothing.
     */
    virtual void doSetKeepAbove();
    /*!
     * Called from setKeepBelow once the keepBelow value got updated, but before the changed signal
     * is emitted.
     *
     * Default implementation does nothing.
     */
    virtual void doSetKeepBelow();
    /*!
     * Called from setDeskop once the desktop value got updated, but before the changed signal
     * is emitted.
     *
     * Default implementation does nothing.
     */
    virtual void doSetDesktop();
    /*!
     * Called from setOnActivities just after the activity list member has been updated, but before
     * updateActivities is called.
     *
     * \a activityList the new list of activities set on that window
     *
     * Default implementation does nothing
     */
    virtual void doSetOnActivities(const QStringList &activityList);
    /*!
     * Called from minimize and unminimize once the minimized value got updated, but before the
     * changed signal is emitted.
     *
     * Default implementation does nothig.
     */
    virtual void doMinimize();
    virtual bool belongsToSameApplication(const Window *other, SameApplicationChecks checks) const = 0;

    virtual void doSetSkipTaskbar();
    virtual void doSetSkipPager();
    virtual void doSetSkipSwitcher();
    virtual void doSetDemandsAttention();
    virtual void doSetQuickTileMode();
    virtual void doSetHidden();
    virtual void doSetHiddenByShowDesktop();
    virtual void doSetSuspended();
    virtual void doSetModal();
    virtual void doSetNextTargetScale();
    virtual void doSetPreferredBufferTransform();
    virtual void doSetPreferredColorDescription();

    void setupWindowManagementInterface();
    void destroyWindowManagementInterface();
    void updateColorScheme();
    void ensurePalette();
    void handlePaletteChange();

    virtual Layer belongsToLayer() const;
    bool isActiveFullScreen() const;
    void performDelayedRaise();

    // electric border / quick tiling
    void setElectricBorderMode(std::optional<ElectricBorderMode> mode);
    std::optional<ElectricBorderMode> electricBorderMode() const
    {
        return m_electricMode;
    }
    void setElectricBorderMaximizing(bool maximizing);
    bool isElectricBorderMaximizing() const
    {
        return m_electricMaximizing;
    }
    void updateElectricGeometryRestore();
    RectF quickTileGeometryRestore() const;
    RectF quickTileGeometry(QuickTileMode mode, const QPointF &pos) const;
    void exitQuickTileMode();

    // geometry handling
    void checkOffscreenPosition(RectF *geom, const RectF &screenArea) const;
    qreal borderLeft() const;
    qreal borderRight() const;
    qreal borderTop() const;
    qreal borderBottom() const;

    enum class MoveResizeMode : uint {
        None,
        Move = 0x1,
        Resize = 0x2,
        MoveResize = Move | Resize,
    };
    virtual void moveResizeInternal(const RectF &rect, MoveResizeMode mode) = 0;
    void performMoveResize(const RectF &rect, MoveResizeMode mode);
    bool geometryRequestIsCurrent(quint64 generation) const;
    void geometryResizeSubmitted(quint64 generation);
    bool transientGeometryRequestIsCurrent(quint64 generation) const;
    quint64 requestedTransientTileGeneration() const;
    void commitTileFromConfigure(Tile *tile, quint64 transientGeneration);
    virtual bool canStartTransientTileRequest(const RectF &target) const;
    // Nothing of the native geometry is in flight and the frame is the requested geometry.
    virtual bool isTransientGeometrySettled() const;
    virtual bool canStartTransientRestoreRequest(const RectF &target) const;
    virtual bool isTransientTileTargetFeasible(const RectF &target) const;
    /**
     * The frame this window takes in a transient tile. A tile's rectangle is whole logical
     * pixels; a frame is on the output's device pixels over a client size on the client's own
     * grid, and at a fractional scale most whole rectangles are neither. The window takes the
     * tile's top-left corner and the largest size it reaches inside the tile, which is the
     * rectangle itself where that is reachable. A rectangle nothing inside which is reachable
     * is returned as it is, and refused where its feasibility is checked.
     */
    virtual RectF transientTileFrame(const RectF &tile) const;
    /**
     * Whether @p frame is a frame this window takes in a tile of rectangle @p tile: the one
     * transientTileFrame() gives now, or one it gave under other borders. A decoration that
     * changes its borders when its window is tiled changes, in the middle of the admission,
     * which sizes the client reaches; the request's frame is then still at the tile's corner
     * and inside the tile, short of it by no more than a fit ever is. Reachability is the
     * caller's separate check (isTransientTileTargetFeasible).
     */
    virtual bool isTransientTileFrame(const RectF &frame, const RectF &tile) const;
    /**
     * The frame a request for a transient record's floating frame gives this window on the
     * output it is on now. The floating frame was a frame of the window where it was taken.
     * Carried to an output of another scale (sendToOutput, a removed output) its size is not
     * on the pixels there and the window cannot take it: a request for it ends with the
     * frame and the requested geometry on its image, which is not the floating frame, so a
     * released window asked for it stands on a rectangle nothing vouches for, however often
     * it is asked. This is @p floating where the window reaches it, and otherwise its corner
     * with its size taken to this output's pixels and to the client's grid and back, as a
     * request and its commit take it.
     */
    virtual RectF transientFloatingImage(const RectF &floating) const;
    // Backend hooks of requestMoveResizeForAssignment(). Support is consulted before any mutation;
    // a dispatcher reports what it staged or published through the stack-bound request.
    virtual bool supportsAssignmentGeometryRequest() const;
    virtual void dispatchAssignmentGeometryRequest(const RectF &rect, MoveResizeMode mode, AssignmentGeometryRequest &request);
    // Whether the backend's quick-tile protocol state commits immediately (an X11 window): the
    // association write's publication is then a committed-association change the steps budget.
    virtual bool assignmentPublishesImmediately() const;
    // The association write's publication, the obligation of the guarded association and forget
    // steps. With a carrier (the step's current continuation) a backend that commits immediately
    // records that commit as the carrier's own write; without one it publishes the still-current
    // association of an operation already superseded, which it never resurrects. The base
    // implementation stages the protocol state (doSetQuickTileMode) and touches no counter.
    virtual void publishRequestedTileForAssignment(AssignmentStepResult *carrier);

private:
    // Declared after MoveResizeMode; they belong with the private guarded entries above. The owned
    // form takes the exact active native request and derives its target and mode from it.
    [[nodiscard]] AssignmentGeometryRequest requestMoveResizeForAssignment(RectF desired, MoveResizeMode mode, const TileAssignmentFence &before);
    [[nodiscard]] AssignmentGeometryRequest requestMoveResizeForAssignment(TransientTileRequest &request, const TileAssignmentFence &before);
    // An owner acting on its own placement passes its ownership generation; the rectangle is the
    // caller's complete frame target and is dispatched as given.
    [[nodiscard]] AssignmentGeometryRequest requestMoveResizeForOwnership(RectF desired, MoveResizeMode mode, const TileAssignmentFence &before, quint64 generation, std::function<bool()> constraint = {});
    AssignmentGeometryRequest requestMoveResizeGuarded(RectF desired, MoveResizeMode mode, const TileAssignmentFence &before, OwnedProvenance owned);

protected:
    /*!
     * Returns whether the Window is currently in move resize mode
     */
    bool isInteractiveMoveResize() const
    {
        return m_interactiveMoveResize.enabled;
    }
    /*!
     * Sets whether the Window is in move resize mode to \a enabled.
     */
    void setInteractiveMoveResize(bool enabled)
    {
        m_interactiveMoveResize.enabled = enabled;
    }
    void setInteractiveMoveResizeAnchor(const QPointF &anchor)
    {
        m_interactiveMoveResize.anchor = anchor;
    }
    void setInteractiveMoveResizeModifiers(Qt::KeyboardModifiers modifiers)
    {
        m_interactiveMoveResize.modifiers = modifiers;
    }
    /*!
     * Returns whether the move resize mode is unrestricted.
     */
    bool isUnrestrictedInteractiveMoveResize() const
    {
        return m_interactiveMoveResize.unrestricted;
    }
    /*!
     * Sets whether move resize mode is unrestricted to \a set.
     */
    void setUnrestrictedInteractiveMoveResize(bool set)
    {
        m_interactiveMoveResize.unrestricted = set;
    }
    /*!
     * Normalized position of the move resize anchor point relative to the top-left window
     * corner when the move resize operation started.
     *
     * QPointF(0, 0) corresponds to the top left window corner, QPointF(1, 1) corresponds to
     * the bottom right window corner.
     */
    QPointF interactiveMoveOffset() const
    {
        return m_interactiveMoveResize.offset;
    }
    void setInteractiveMoveOffset(const QPointF &offset)
    {
        m_interactiveMoveResize.offset = offset;
    }
    RectF initialInteractiveMoveResizeGeometry() const
    {
        return m_interactiveMoveResize.initialGeometry;
    }
    void setMoveResizeGeometry(const RectF &geo);
    /**
     * Emits requestedGeometryChanged() if moveResizeGeometry() differs from the value it was last
     * emitted for. Call it once the operation that may have changed the requested geometry is
     * complete, not from setMoveResizeGeometry(): a handler that moves the window would otherwise
     * be overridden by the rest of the operation.
     */
    void notifyRequestedGeometryChanged();
    void setInteractiveMoveResizeGravity(Gravity gravity)
    {
        m_interactiveMoveResize.gravity = gravity;
    }
    bool isInteractiveMoveResizePointerButtonDown() const
    {
        return m_interactiveMoveResize.buttonDown;
    }
    void setInteractiveMoveResizePointerButtonDown(bool down)
    {
        m_interactiveMoveResize.buttonDown = down;
    }
    /*!
     * Sets an appropriate cursor shape for the logical mouse position.
     */
    void updateInteractiveMoveResizeCursor();
    void startDelayedInteractiveMoveResize();
    void stopDelayedInteractiveMoveResize();
    bool startInteractiveMoveResize();
    /*!
     * Called from startMoveResize.
     *
     * Implementing classes should return \c false if starting move resize should
     * get aborted. In that case startMoveResize will also return \c false.
     *
     * Base implementation returns \c true.
     */
    virtual bool doStartInteractiveMoveResize();
    virtual void doFinishInteractiveMoveResize();
    void finishInteractiveMoveResize(bool cancel);
    /*!
     * Leaves the move resize mode.
     *
     * Inheriting classes must invoke the base implementation which
     * ensures that the internal mode is properly ended.
     */
    virtual void leaveInteractiveMoveResize();
    /*
     * Checks if the mouse cursor is near the edge of the screen and if so
     * activates quick tiling or maximization
     */
    void checkQuickTilingMaximizationZones(int xroot, int yroot);
    void resetQuickTilingMaximizationZones();
    /*!
     * Whether a sync request is still pending.
     * Default implementation returns \c false.
     */
    virtual bool isWaitingForInteractiveResizeSync() const;
    /*!
     * Called during handling a resize. Implementing subclasses can use this
     * method to perform windowing system specific syncing.
     */
    virtual void doInteractiveResizeSync(const RectF &rect);
    QSizeF minVisibleArea() const;
    RectF nextInteractiveMoveGeometry(const RectF &rect) const;
    RectF nextInteractiveResizeGeometry(const QPointF &global) const;
    std::optional<QPointF> confineInteractiveMove(const RectF &geometry) const;
    std::optional<QPointF> confineInteractiveResize(const RectF &geometry, Gravity gravity) const;
    void dontInteractiveMoveResize();

    virtual QSizeF resizeIncrements() const;

    /*!
     * Returns the interactive move resize gravity depending on the Decoration's section
     * under mouse. If no decoration it returns Gravity::None.
     */
    Gravity mouseGravity() const;

    void setDecoration(std::shared_ptr<KDecoration3::Decoration> decoration);
    void startDecorationDoubleClickTimer();
    void invalidateDecorationDoubleClickTimer();
    void updateDecorationInputShape();
    void updateDecorationBorderRadius();

    void setDesktopFileName(const QString &name);
    QString iconFromDesktopFile() const;

    void updateApplicationMenuServiceName(const QString &serviceName);
    void updateApplicationMenuObjectPath(const QString &objectPath);

    void setUnresponsive(bool unresponsive);

    virtual void setShortcutInternal();
    QString shortcutCaptionSuffix() const;
    virtual void updateCaption() = 0;

    // The geometry that the window should be restored when the virtual keyboard closes
    RectF keyboardGeometryRestore() const;
    void setKeyboardGeometryRestore(const RectF &geom);

    RectF moveToArea(const RectF &geometry, const RectF &oldArea, const RectF &newArea);
    RectF ensureSpecialStateGeometry(const RectF &geometry);

    void maybeSendFrameCallback();

    void updateNextTargetScale();
    void updatePreferredBufferTransform();
    void updatePreferredColorDescription();
    void setTargetScale(qreal scale);

    void setDescription(const QString &description);

    void finishInteractiveOutputChange(LogicalOutput *oldOutput);

    LogicalOutput *m_output = nullptr;
    RectF m_frameGeometry;
    RectF m_clientGeometry;
    RectF m_bufferGeometry;
    quint64 m_geometryUpdateGeneration = 0;
    struct PendingGeometryNotifications
    {
        std::optional<RectF> buffer;
        std::optional<RectF> client;
        std::optional<RectF> frame;
        QPointer<LogicalOutput> output;
        bool outputPending = false;
        bool outputWasNull = false;
    } m_pendingGeometryNotifications;
    bool ready_for_painting;
    bool m_hidden = false;
    bool m_hiddenByShowDesktop = false;

    BorderRadius m_borderRadius;

    qreal m_nextTargetScale = 1;
    qreal m_targetScale = 1;
    OutputTransform m_preferredBufferTransform = OutputTransform::Normal;
    std::shared_ptr<ColorDescription> m_preferredColorDescription = ColorDescription::sRGB;

    int m_refCount = 1;
    QUuid m_internalId;
    std::unique_ptr<WindowItem> m_windowItem;
    std::unique_ptr<Shadow> m_shadow;
    QString resource_name;
    QString resource_class;
    ClientMachine *m_clientMachine;
    bool m_skipCloseAnimation;
    QPointer<SurfaceInterface> m_surface;
    qreal m_opacity = 1.0;
    int m_stackingOrder = 0;

    bool m_skipTaskbar = false;
    /*!
     * Unaffected by KWin
     */
    bool m_originalSkipTaskbar = false;
    bool m_skipPager = false;
    bool m_skipSwitcher = false;
    QIcon m_icon;
    bool m_active = false;
    bool m_deleted = false;
    bool m_placed = false;
    bool m_keepAbove = false;
    bool m_keepBelow = false;
    bool m_demandsAttention = false;
    bool m_minimized = false;
    bool m_suspended = false;
    bool m_excludeFromCapture = false;
    QTimer *m_autoRaiseTimer = nullptr;
    QList<VirtualDesktop *> m_desktops;

    QStringList m_activityList;
    int m_activityUpdatesBlocked = 0;
    bool m_blockedActivityUpdatesRequireTransients = false;

    QString m_colorScheme;
    std::shared_ptr<Decoration::DecorationPalette> m_palette;
    static QHash<QString, std::weak_ptr<Decoration::DecorationPalette>> s_palettes;
    static std::shared_ptr<Decoration::DecorationPalette> s_defaultPalette;

    PlasmaWindowInterface *m_windowManagementInterface = nullptr;

    Window *m_transientFor = nullptr;
    QList<Window *> m_transients;
    bool m_modal = false;
    Layer m_layer = UnknownLayer;
    bool m_delayedRaise = false;
    QPointer<Tile> m_requestedTile;
    quint64 m_requestedTransientTileGeneration = 0;
    QPointer<Tile> m_tile;
    quint64 m_requestedTileChange = 0;
    quint64 m_committedTileChange = 0;
    quint64 m_placementContinuationRevision = 0;
    quint64 m_tileMembershipChange = 0;

    // electric border/quick tiling
    std::optional<ElectricBorderMode> m_electricMode = std::nullopt;
    RectF m_electricGeometryRestore;
    bool m_electricMaximizing = false;
    QTimer *m_electricMaximizingDelay = nullptr;

    // geometry
    LogicalOutput *m_moveResizeOutput = nullptr;
    RectF m_moveResizeGeometry;
    quint64 m_geometryRequestGeneration = 0;
    GeometryRequestOrigin m_geometryRequestOrigin;
    bool m_geometryResizePending = false;
    bool m_outputPropertiesPending = false;
    TransientTileRequest *m_activeTransientRequest = nullptr;
    quint64 m_transientRequestSequence = 0;
    quint64 m_lastTransientOwnershipGeneration = 0;
    quint64 m_transientOwnershipGeneration = 0;
    quint64 m_transientRestoreSequence = 0;
    std::unique_ptr<TransientOwnership> m_transientOwnership;
    RectF m_keyboardGeometryRestore;
    RectF m_maximizeGeometryRestore;
    RectF m_fullscreenGeometryRestore;
    RectF m_virtualKeyboardGeometry;

    struct
    {
        bool enabled = false;
        bool unrestricted = false;
        QPointF anchor;
        Qt::KeyboardModifiers modifiers;
        QPointF offset;
        RectF initialGeometry;
        RectF initialGeometryRestore;
        Gravity gravity = Gravity::Center;
        bool buttonDown = false;
        CursorShape cursor = Qt::ArrowCursor;
        QString initialOutputId;
        RectF initialScreenArea;
        QTimer *delayedTimer = nullptr;
        uint32_t counter = 0;
        // Whether the operation has moved or resized the window: one that started and ended
        // where the window stood (a press on the title bar held and released) chose no place.
        bool stepped = false;
        MaximizeMode initialMaximizeMode;
        QuickTileMode initialQuickTileMode;
    } m_interactiveMoveResize;

    struct
    {
        std::shared_ptr<KDecoration3::Decoration> decoration;
        QPointer<Decoration::DecoratedWindowImpl> client;
        QElapsedTimer doubleClickTimer;
        RegionF inputRegion;
    } m_decoration;
    // Versions every installation, removal and replacement of the decoration: advanced, saturating,
    // together with the pointer and before any observer runs. Changes within the same decoration
    // (its borders) are not versioned here.
    quint64 m_decorationGeneration = 0;
    void advanceDecorationGeneration();
    // The installation behind setDecoration, with its own boundaries: the generation of the
    // installed incarnation is reported at its commit, before any observer runs. Guarded, the final
    // notification is emitted only while that incarnation is still the current one (a closure or a
    // nested replacement inside the earlier notifications stops the obsolete installation), and the
    // return says whether it was; unguarded, the legacy path.
    bool installDecoration(std::shared_ptr<KDecoration3::Decoration> decoration, bool guarded, quint64 *installedGeneration = nullptr);
    QString m_desktopFileName;

    bool m_applicationMenuActive = false;
    QString m_applicationMenuServiceName;
    QString m_applicationMenuObjectPath;

    bool m_unresponsive = false;

    QKeySequence _shortcut;

    WindowRules m_rules;
    quint32 m_lastUsageSerial = 0;
    bool m_lockScreenOverlay = false;
    uint32_t m_offscreenRenderCount = 0;
    QTimer m_offscreenFramecallbackTimer;

    QString m_tag;
    QString m_description;

    QString m_activationToken;
};

// Synchronous observation only; this fence grants no mutation or admission authority.
class KWIN_EXPORT TileAssignmentFence final
{
public:
    ~TileAssignmentFence();
    Q_DISABLE_COPY_MOVE(TileAssignmentFence)

    bool isCurrentFor(const Window &window) const;

private:
    friend class Window;
    friend class Tile;
    friend class WaylandWindow;
    explicit TileAssignmentFence(Window &window);

    const QPointer<Window> m_window;
    const QUuid m_windowId;
    const quint64 m_placementRevision;
    const quint64 m_requestedRevision;
    const quint64 m_committedRevision;
    const quint64 m_geometryGeneration;
    const quint64 m_membershipRevision;
    const QPointer<Tile> m_requestedTile;
    const QPointer<Tile> m_committedTile;
    const bool m_hadRequestedTile;
    const bool m_hadCommittedTile;
};

inline RectF Window::bufferGeometry() const
{
    return m_bufferGeometry;
}

inline RectF Window::clientGeometry() const
{
    return m_clientGeometry;
}

inline QSizeF Window::clientSize() const
{
    return m_clientGeometry.size();
}

inline RectF Window::frameGeometry() const
{
    return m_frameGeometry;
}

inline QSizeF Window::size() const
{
    return m_frameGeometry.size();
}

inline QPointF Window::pos() const
{
    return m_frameGeometry.topLeft();
}

inline qreal Window::x() const
{
    return m_frameGeometry.x();
}

inline qreal Window::y() const
{
    return m_frameGeometry.y();
}

inline qreal Window::width() const
{
    return m_frameGeometry.width();
}

inline qreal Window::height() const
{
    return m_frameGeometry.height();
}

inline RectF Window::rect() const
{
    return RectF(0, 0, width(), height());
}

inline bool Window::readyForPainting() const
{
    return ready_for_painting;
}

inline bool Window::isDesktop() const
{
    return windowType() == WindowType::Desktop;
}

inline bool Window::isDock() const
{
    return windowType() == WindowType::Dock;
}

inline bool Window::isMenu() const
{
    return windowType() == WindowType::Menu;
}

inline bool Window::isToolbar() const
{
    return windowType() == WindowType::Toolbar;
}

inline bool Window::isSplash() const
{
    return windowType() == WindowType::Splash;
}

inline bool Window::isUtility() const
{
    return windowType() == WindowType::Utility;
}

inline bool Window::isDialog() const
{
    return windowType() == WindowType::Dialog;
}

inline bool Window::isNormalWindow() const
{
    return windowType() == WindowType::Normal;
}

inline bool Window::isDropdownMenu() const
{
    return windowType() == WindowType::DropdownMenu;
}

inline bool Window::isPopupMenu() const
{
    return windowType() == WindowType::PopupMenu;
}

inline bool Window::isTooltip() const
{
    return windowType() == WindowType::Tooltip;
}

inline bool Window::isNotification() const
{
    return windowType() == WindowType::Notification;
}

inline bool Window::isCriticalNotification() const
{
    return windowType() == WindowType::CriticalNotification;
}

inline bool Window::isAppletPopup() const
{
    return windowType() == WindowType::AppletPopup;
}

inline bool Window::isOnScreenDisplay() const
{
    return windowType() == WindowType::OnScreenDisplay;
}

inline bool Window::isComboBox() const
{
    return windowType() == WindowType::ComboBox;
}

inline bool Window::isDNDIcon() const
{
    return windowType() == WindowType::DNDIcon;
}

inline bool Window::isLockScreen() const
{
    return false;
}

inline bool Window::isInputMethod() const
{
    return false;
}

inline bool Window::isOutline() const
{
    return false;
}

inline bool Window::isInternal() const
{
    return false;
}

inline bool Window::isPictureInPicture() const
{
    return false;
}

inline WindowItem *Window::windowItem() const
{
    return m_windowItem.get();
}

inline bool Window::isOnAllDesktops() const
{
    return desktops().isEmpty();
}

inline bool Window::isOnAllActivities() const
{
    return activities().isEmpty();
}

inline bool Window::isOnActivity(const QString &activity) const
{
    return activities().isEmpty() || activities().contains(activity);
}

inline QString Window::resourceName() const
{
    return resource_name; // it is always lowercase
}

inline QString Window::resourceClass() const
{
    return resource_class; // it is always lowercase
}

inline ClientMachine *Window::clientMachine() const
{
    return m_clientMachine;
}

template<class T, class U>
inline T *Window::findInList(const QList<T *> &list, std::function<bool(const U *)> func)
{
    static_assert(std::is_base_of<U, T>::value,
                  "U must be derived from T");
    const auto it = std::find_if(list.begin(), list.end(), func);
    if (it == list.end()) {
        return nullptr;
    }
    return *it;
}

inline bool Window::isPopupWindow() const
{
    switch (windowType()) {
    case WindowType::ComboBox:
    case WindowType::DropdownMenu:
    case WindowType::PopupMenu:
    case WindowType::Tooltip:
        return true;

    default:
        return false;
    }
}

inline const QList<Window *> &Window::transients() const
{
    return m_transients;
}

KWIN_EXPORT QDebug operator<<(QDebug debug, const Window *window);

} // namespace KWin

Q_DECLARE_METATYPE(KWin::Window *)
Q_DECLARE_METATYPE(QList<KWin::Window *>)
Q_DECLARE_OPERATORS_FOR_FLAGS(KWin::Window::SameApplicationChecks)
