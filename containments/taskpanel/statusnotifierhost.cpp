/*
    SPDX-FileCopyrightText: 2026 watermeko

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "statusnotifierhost.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QIcon>
#include <QImage>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QTimer>
#include <QtEndian>

#include <qqml.h>

static const int s_typeRegistration = qmlRegisterType<StatusNotifierHost>("org.kde.plasma.mobile.taskpanel", 1, 0, "StatusNotifierHost");

namespace
{
constexpr auto s_watcherService = "org.kde.StatusNotifierWatcher";
constexpr auto s_watcherPath = "/StatusNotifierWatcher";
constexpr auto s_watcherInterface = "org.kde.StatusNotifierWatcher";
constexpr auto s_itemInterface = "org.kde.StatusNotifierItem";

// Pixmaps that an item publishes through IconPixmap (instead of an icon theme
// name) are handed to QML through an image provider, keyed by the item.
QHash<QString, QPixmap> &pixmapCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}

class PixmapProvider : public QQuickImageProvider
{
public:
    PixmapProvider()
        : QQuickImageProvider(QQuickImageProvider::Pixmap)
    {
    }

    QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        QPixmap pixmap = pixmapCache().value(id);
        if (pixmap.isNull()) {
            return pixmap;
        }
        if (requestedSize.isValid()) {
            pixmap = pixmap.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (size) {
            *size = pixmap.size();
        }
        return pixmap;
    }
};

// QDBusInterface::property() cannot read the properties whose type is a
// struct (IconPixmap, ToolTip) unless the type is registered with Qt D-Bus, so
// ask the properties interface directly and demarshal the argument by hand.
QVariant dbusProperty(QDBusInterface *iface, const QString &name)
{
    QDBusMessage message = QDBusMessage::createMethodCall(iface->service(),
                                                          iface->path(),
                                                          QStringLiteral("org.freedesktop.DBus.Properties"),
                                                          QStringLiteral("Get"));
    message << iface->interface() << name;

    const QDBusMessage reply = QDBusConnection::sessionBus().call(message);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    return reply.arguments().first().value<QDBusVariant>().variant();
}

QPixmap pixmapFromItem(const QVariant &value)
{
    if (!value.canConvert<QDBusArgument>()) {
        return {};
    }

    const QDBusArgument arg = value.value<QDBusArgument>();
    QPixmap best;

    // a(iiay): width, height, ARGB32 in network byte order (big endian)
    arg.beginArray();
    while (!arg.atEnd()) {
        int width = 0;
        int height = 0;
        QByteArray data;
        arg.beginStructure();
        arg >> width >> height >> data;
        arg.endStructure();

        if (width <= 0 || height <= 0 || data.size() < width * height * 4) {
            continue;
        }
        if (!best.isNull() && best.width() >= width) {
            continue;
        }

        QImage image(width, height, QImage::Format_ARGB32);
        const uchar *source = reinterpret_cast<const uchar *>(data.constData());
        for (int y = 0; y < height; ++y) {
            const uchar *row = source + y * width * 4;
            QRgb *target = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                target[x] = qFromBigEndian<quint32>(row + x * 4);
            }
        }
        best = QPixmap::fromImage(image);
    }
    arg.endArray();

    return best;
}

QString toolTipText(const QVariant &value)
{
    if (!value.canConvert<QDBusArgument>()) {
        return {};
    }

    const QDBusArgument arg = value.value<QDBusArgument>();
    // (sa(iiay)ss): icon name, pixmaps, title, subtitle
    arg.beginStructure();
    QString iconName;
    QString title;
    QString subtitle;
    arg >> iconName;
    arg.beginArray();
    while (!arg.atEnd()) {
        int width = 0;
        int height = 0;
        QByteArray data;
        arg.beginStructure();
        arg >> width >> height >> data;
        arg.endStructure();
    }
    arg.endArray();
    arg >> title >> subtitle;
    arg.endStructure();

    if (subtitle.isEmpty()) {
        return title;
    }
    return title.isEmpty() ? subtitle : title + QLatin1Char('\n') + subtitle;
}

// Walks one (ia{sv}av) layout entry, appending what the popup needs.  The
// entries are flattened with their depth, so a submenu shows up as indented
// lines rather than as a second popup.
void appendLayoutEntry(QVariantList &out, const QDBusArgument &entry, int depth)
{
    entry.beginStructure();

    int id = 0;
    entry >> id;

    QVariantMap properties;
    entry >> properties;

    const QString type = properties.value(QStringLiteral("type")).toString();
    const QString label = properties.value(QStringLiteral("label")).toString();
    const bool visible = properties.value(QStringLiteral("visible"), true).toBool();
    const bool enabled = properties.value(QStringLiteral("enabled"), true).toBool();
    const bool separator = type == QLatin1String("separator");
    const bool submenu = properties.contains(QStringLiteral("children-display"));
    const bool checked = properties.value(QStringLiteral("toggle-state")).toInt() != 0;

    // The outermost entry is the menu's root: it is not an entry itself, even
    // when an exporter gives it a placeholder label.  Separators have no place
    // in the flat list either.
    if (depth > 0 && visible && !separator && !label.isEmpty()) {
        QVariantMap entryData;
        entryData.insert(QStringLiteral("id"), id);
        entryData.insert(QStringLiteral("label"), label);
        entryData.insert(QStringLiteral("enabled"), enabled);
        entryData.insert(QStringLiteral("submenu"), submenu);
        entryData.insert(QStringLiteral("checked"), checked);
        entryData.insert(QStringLiteral("depth"), depth);
        out.append(entryData);
    }

    entry.beginArray();
    while (!entry.atEnd()) {
        QDBusVariant child;
        entry >> child;
        const QVariant childData = child.variant();
        if (childData.canConvert<QDBusArgument>()) {
            appendLayoutEntry(out, childData.value<QDBusArgument>(), depth + 1);
        }
    }
    entry.endArray();
    entry.endStructure();
}

} // namespace

StatusNotifierHost::StatusNotifierHost(QObject *parent)
    : QAbstractListModel(parent)
{
    m_watcher = new QDBusInterface(QString::fromLatin1(s_watcherService),
                                   QString::fromLatin1(s_watcherPath),
                                   QString::fromLatin1(s_watcherInterface),
                                   QDBusConnection::sessionBus(),
                                   this);

    if (!m_watcher->isValid()) {
        qWarning() << "StatusNotifierWatcher is not reachable:" << m_watcher->lastError().message();
        return;
    }

    // Tell the applications there is somebody to draw their icons.
    m_watcher->call(QStringLiteral("RegisterStatusNotifierHost"), QDBusConnection::sessionBus().baseService());

    QDBusConnection connection = QDBusConnection::sessionBus();
    connection.connect(QString::fromLatin1(s_watcherService),
                        QString::fromLatin1(s_watcherPath),
                        QString::fromLatin1(s_watcherInterface),
                        QStringLiteral("StatusNotifierItemRegistered"),
                        this,
                        SLOT(onItemRegistered(QString)));
    connection.connect(QString::fromLatin1(s_watcherService),
                        QString::fromLatin1(s_watcherPath),
                        QString::fromLatin1(s_watcherInterface),
                        QStringLiteral("StatusNotifierItemUnregistered"),
                        this,
                        SLOT(onItemUnregistered(QString)));

    // Read the registry once the caller has finished constructing us, so that
    // the QML engine already knows the object and can take our image provider.
    QTimer::singleShot(0, this, &StatusNotifierHost::readItems);
}

StatusNotifierHost::~StatusNotifierHost()
{
    qDeleteAll(m_items);
}

int StatusNotifierHost::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_items.size();
}

QVariant StatusNotifierHost::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return {};
    }

    const Item *item = m_items.at(index.row());
    switch (role) {
    case IconNameRole:
        return item->iconName;
    case IconSourceRole:
        return item->iconKey.isEmpty() ? QString() : QStringLiteral("image://sni/") + item->iconKey;
    case TitleRole:
        return item->title;
    case ToolTipRole:
        return item->tooltip;
    case ItemStatusRole:
        return item->status;
    case ItemIsMenuRole:
        return item->isMenu;
    default:
        return {};
    }
}

QHash<int, QByteArray> StatusNotifierHost::roleNames() const
{
    return {
        {IconNameRole, "iconName"},
        {IconSourceRole, "iconSource"},
        {TitleRole, "title"},
        {ToolTipRole, "toolTip"},
        {ItemStatusRole, "itemStatus"},
        {ItemIsMenuRole, "itemIsMenu"},
    };
}

void StatusNotifierHost::activate(int row)
{
    call(row, QStringLiteral("Activate"));
}

void StatusNotifierHost::secondaryActivate(int row)
{
    call(row, QStringLiteral("SecondaryActivate"));
}

void StatusNotifierHost::contextMenu(int row)
{
    call(row, QStringLiteral("ContextMenu"));
}

void StatusNotifierHost::call(int row, const QString &method)
{
    if (row < 0 || row >= m_items.size()) {
        return;
    }

    Item *item = m_items.at(row);
    if (!item->iface) {
        return;
    }

    // The coordinates are only a hint for where a popup should appear; the
    // panel has no way to know where the pointer is on a touch screen.
    qInfo() << "tray-click:" << method << "on" << item->servicePath;
    item->iface->call(method, 0, 0);
}

void StatusNotifierHost::readItems()
{
    if (!m_watcher || !m_watcher->isValid()) {
        qWarning() << "StatusNotifierWatcher is not reachable:" << m_watcher->lastError().message();
        return;
    }

    // RegisteredStatusNotifierItems is a property, not a method.
    const QStringList items = m_watcher->property("RegisteredStatusNotifierItems").toStringList();

    for (const QString &servicePath : items) {
        addItem(servicePath);
    }
}

void StatusNotifierHost::onItemRegistered(const QString &servicePath)
{
    addItem(servicePath);
}

void StatusNotifierHost::onItemUnregistered(const QString &servicePath)
{
    removeItem(servicePath);
}

void StatusNotifierHost::addItem(const QString &servicePath)
{
    if (servicePath.isEmpty() || rowFor(servicePath) >= 0) {
        return;
    }

    const int slash = servicePath.indexOf(QLatin1Char('/'));
    if (slash <= 0) {
        return;
    }

    auto *item = new Item;
    item->servicePath = servicePath;
    item->service = servicePath.left(slash);
    item->path = servicePath.mid(slash);
    item->iface = new QDBusInterface(item->service, item->path, QString::fromLatin1(s_itemInterface), QDBusConnection::sessionBus(), this);

    if (!item->iface->isValid()) {
        delete item->iface;
        delete item;
        return;
    }

    updateItem(item);
    updateIcon(item);

    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append(item);
    endInsertRows();
}

void StatusNotifierHost::removeItem(const QString &servicePath)
{
    const int row = rowFor(servicePath);
    if (row < 0) {
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    Item *item = m_items.takeAt(row);
    endRemoveRows();

    pixmapCache().remove(item->iconKey);
    delete item->iface;
    delete item;
}

void StatusNotifierHost::updateItem(Item *item)
{
    item->title = item->iface->property("Title").toString();
    item->status = item->iface->property("Status").toString();
    item->tooltip = toolTipText(dbusProperty(item->iface, QStringLiteral("ToolTip")));
    item->isMenu = item->iface->property("ItemIsMenu").toBool();
    if (item->title.isEmpty()) {
        item->title = item->iface->property("Id").toString();
    }
}

void StatusNotifierHost::updateIcon(Item *item)
{
    item->iconName = item->iface->property("IconName").toString();
    item->iconKey.clear();

    const QPixmap pixmap = pixmapFromItem(dbusProperty(item->iface, QStringLiteral("IconPixmap")));
    if (!pixmap.isNull()) {
        // The icon name of an item is usually its application id, which two
        // items can share; the service path is unique.
        item->iconKey = item->servicePath;
        pixmapCache().insert(item->iconKey, pixmap);

        if (auto *engine = qmlEngine(this)) {
            static bool providerInstalled = false;
            if (!providerInstalled) {
                engine->addImageProvider(QStringLiteral("sni"), new PixmapProvider);
                providerInstalled = true;
            }
        }
    }
}

QString StatusNotifierHost::menuObjectPath(Item *item)
{
    if (!item->menuPath.isEmpty()) {
        return item->menuPath;
    }

    // Ayatana indicators keep their menu next to the item, some applications
    // publish it in the Menu property, and a few only in MenuPath of the
    // MenuBar interface.
    const QVariant menu = dbusProperty(item->iface, QStringLiteral("Menu"));
    if (menu.isValid()) {
        const QString path = menu.value<QDBusObjectPath>().path();
        if (!path.isEmpty() && path != QLatin1String("/")) {
            item->menuPath = path;
            return item->menuPath;
        }
    }

    item->menuPath = item->path + QStringLiteral("/Menu");
    return item->menuPath;
}

QVariantList StatusNotifierHost::menu(int row)
{
    if (row < 0 || row >= m_items.size()) {
        return {};
    }

    Item *item = m_items.at(row);
    QDBusInterface menuInterface(item->service,
                                 menuObjectPath(item),
                                 QStringLiteral("com.canonical.dbusmenu"),
                                 QDBusConnection::sessionBus());
    if (!menuInterface.isValid()) {
        return {};
    }

    // Tell the application the menu is about to be shown: many populate their
    // entries lazily in response.
    menuInterface.call(QStringLiteral("AboutToShow"), 0);

    QDBusMessage message = menuInterface.call(QStringLiteral("GetLayout"), 0, -1, QStringList{QStringLiteral("label"),
                                                                                             QStringLiteral("enabled"),
                                                                                             QStringLiteral("visible"),
                                                                                             QStringLiteral("type"),
                                                                                             QStringLiteral("children-display")});
    if (message.type() != QDBusMessage::ReplyMessage || message.arguments().size() < 2) {
        return {};
    }

    const QVariant layout = message.arguments().at(1);
    if (!layout.canConvert<QDBusArgument>()) {
        return {};
    }

    QVariantList entries;
    appendLayoutEntry(entries, layout.value<QDBusArgument>(), 0);
    return entries;
}

void StatusNotifierHost::activateMenuEntry(int row, int id)
{
    if (row < 0 || row >= m_items.size()) {
        return;
    }

    Item *item = m_items.at(row);
    QDBusInterface menuInterface(item->service,
                                 menuObjectPath(item),
                                 QStringLiteral("com.canonical.dbusmenu"),
                                 QDBusConnection::sessionBus());
    if (!menuInterface.isValid()) {
        return;
    }

    // the data argument is a variant, so it has to be wrapped in a QDBusVariant
    // or it is sent as a plain int and the call is rejected
    qInfo() << "tray-menu: activating entry" << id << "of" << item->servicePath;
    const QDBusMessage reply = menuInterface.call(QStringLiteral("Event"),
                                                  id,
                                                  QStringLiteral("clicked"),
                                                  QVariant::fromValue(QDBusVariant(QVariant(0))),
                                                  0u);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "could not activate menu entry" << id << "of" << item->servicePath << ":" << reply.errorMessage();
    }
}

int StatusNotifierHost::rowFor(const QString &servicePath) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i)->servicePath == servicePath) {
            return i;
        }
    }
    return -1;
}
