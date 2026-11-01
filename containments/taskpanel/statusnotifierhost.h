/*
    SPDX-FileCopyrightText: 2026 watermeko

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPixmap>
#include <QVector>

class QDBusInterface;

/**
 * A minimal StatusNotifierItem host.
 *
 * The mobile shell has no place to show tray icons (KDE bug 483089): the items
 * register with the session's org.kde.StatusNotifierWatcher and nothing ever
 * draws them.  This model reads the registry, follows each item's icon, title
 * and status, and exposes them as a list so the task panel can put them next to
 * the navigation buttons.
 */
class StatusNotifierHost : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IconNameRole = Qt::UserRole + 1,
        IconSourceRole,
        TitleRole,
        ToolTipRole,
        ItemStatusRole,
        ItemIsMenuRole,
    };

    explicit StatusNotifierHost(QObject *parent = nullptr);
    ~StatusNotifierHost() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void activate(int row);
    Q_INVOKABLE void secondaryActivate(int row);
    Q_INVOKABLE void contextMenu(int row);

    // Applications that are Ayatana/StatusNotifier "menu" items (FlClash,
    // Telegram, most AppIndicators) ignore Activate and expect the host to draw
    // the menu they publish over com.canonical.dbusmenu.
    Q_INVOKABLE QVariantList menu(int row);
    Q_INVOKABLE void activateMenuEntry(int row, int id);

private Q_SLOTS:
    void onItemRegistered(const QString &servicePath);
    void onItemUnregistered(const QString &servicePath);

private:
    struct Item {
        QString servicePath;
        QString service;
        QString path;
        QString iconName;
        QString iconKey;
        QString title;
        QString tooltip;
        QString status;
        bool isMenu = false;
        QString menuPath;
        QDBusInterface *iface = nullptr;
    };

    void readItems();
    void addItem(const QString &servicePath);
    void removeItem(const QString &servicePath);
    void updateItem(Item *item);
    void updateIcon(Item *item);
    int rowFor(const QString &servicePath) const;
    void call(int row, const QString &method);
    QString menuObjectPath(Item *item);

    QDBusInterface *m_watcher = nullptr;
    QVector<Item *> m_items;
};
