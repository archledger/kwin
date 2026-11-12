/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TRANSIENTTILINGAPI_H
#define TRANSIENTTILINGAPI_H

#include "tiles/transientplacementwriter.h"
#include "tiles/transienttilecontroller.h"

#include <QJsonObject>
#include <QObject>

namespace KWin
{

class Window;
class Workspace;

/**
 * The QML-facing facade for the transient tiling system (docs/NATIVE-TILING-CONTRACT.md).
 *
 * The writer's and controller's C++ methods take QUuid tokens and struct parameters that
 * cannot cross the QML boundary; this facade exposes them as Q_INVOKABLE methods with
 * QML-friendly arguments: string tokens, Window pointers, QVariantMap results. The token
 * is opaque text to QML; the facade does not interpret it, only passes it through.
 *
 * No QML acknowledgment setter exists (the contract forbids it): the writer validates
 * every submission natively.
 */
class TransientTilingApi : public QObject
{
    Q_OBJECT

public:
    explicit TransientTilingApi(Workspace *workspace, QObject *parent = nullptr);
    ~TransientTilingApi() override;

    // ------------------------------------------------------------------ the writer
    //
    // The paired provider's side: registration, window tracking, record submission,
    // the snapshot. String tokens (the QUuid as text); the result is the writer's
    // Result enum as an int (-1 on a null writer).

    /**
     * Registers the provider (a QObject that outlives this call). Returns the opaque
     * token as text, or an empty string when the writer refused.
     */
    Q_INVOKABLE QString registerProvider(QObject *provider);

    /**
     * Tracks a window under its placement key. The key follows the controller's rule:
     * the desktop file's basename minus .desktop, else the resource class, lowercased.
     */
    Q_INVOKABLE bool trackWindow(Window *window, const QString &key);

    /**
     * The key the writer holds the window under, or an empty string when it does not
     * hold it: a refused trackWindow (an earlier provider incarnation's binding) leaves
     * the caller unable to tell which key a record would file under, and assuming one
     * can overwrite another app's entry.
     */
    Q_INVOKABLE QString windowBinding(Window *window);

    /**
     * Submits a placement record. The origin is 0=Manual, 1=Ordinary, 2=Restoration,
     * 3=Close. The eventSequence must be strictly increasing per window.
     */
    Q_INVOKABLE int record(const QString &token, int revision, Window *window,
                            const QRectF &frame, int eventSequence, int origin);

    /**
     * The writer's current snapshot: entries (key -> rect), mapRevision,
     * providerRevision. Null on a null writer.
     */
    Q_INVOKABLE QVariantMap snapshot();

    // ------------------------------------------------------------------ the controller
    //
    // The tiler's side: tree creation, admission, release, the master's geometry.

    /**
     * Creates the transient tile. The relativeRect is normalized to the output's root
     * tile (0..1). Returns a JSON object: {generation: int, result: int} where result
     * follows the controller's Result enum.
     */
    Q_INVOKABLE QVariantMap createTile(LogicalOutput *output, VirtualDesktop *desktop,
                                        const QRectF &relativeRect);

    /**
     * Admits a window into the transient tile. The providerToken is the opaque text
     * from registerProvider. Returns the controller's Result enum as an int.
     */
    Q_INVOKABLE int admitTile(int generation, Window *window, const QString &key,
                               const QString &providerToken);

    /**
     * Releases the tree (the script's teardown). Returns the controller's Result enum.
     */
    Q_INVOKABLE int releaseTile(int generation);

    /**
     * The transient leaf's absolute window geometry (the master's placed rectangle,
     * including the tile padding and strut adjustments).
     */
    Q_INVOKABLE QRectF masterWindowRect() const;

    /**
     * True when the controller holds a live tree.
     */
    Q_INVOKABLE bool hasTree() const;

    /**
     * The current tree generation (0 when no tree).
     */
    Q_INVOKABLE int generation() const;

Q_SIGNALS:
    /**
     * Forwarded from the controller's treeChanged: the tiler treats a change with no
     * live tree as session-end (native teardown of any cause).
     */
    void treeChanged();

private:
    Workspace *m_workspace;
};

} // namespace KWin

#endif // TRANSIENTTILINGAPI_H
