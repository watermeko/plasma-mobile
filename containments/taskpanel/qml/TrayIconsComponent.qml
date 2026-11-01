/*
    SPDX-FileCopyrightText: 2026 watermeko

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Window

import org.kde.kirigami as Kirigami
import org.kde.plasma.mobile.taskpanel 1.0

/**
 * The session's StatusNotifierItem icons, for the navigation panel.
 *
 * Plasma Mobile has no place to show tray icons (KDE bug 483089), so the items
 * register with org.kde.StatusNotifierWatcher and nothing draws them.  This is
 * the host: the model follows the registry, this row draws the icons, a tap
 * activates the item or opens the menu it publishes.
 */
RowLayout {
    id: root

    readonly property real iconSize: Math.round(Kirigami.Units.iconSizes.smallMedium * 1.1)

    spacing: Math.round(Kirigami.Units.smallSpacing * 1.5)

    // which item's menu is open, if any
    property int menuRow: -1
    property var menuEntries: []

    function openMenu(row) {
        const entries = trayModel.menu(row);
        if (entries.length === 0) {
            return false;
        }
        menuRow = row;
        // the row travels with the entries: closing the menu must not make a
        // click on an entry meaningless
        menuEntries = entries.map(function(entry) {
            entry.row = row;
            return entry;
        });
        menuWindow.visibility = Window.FullScreen;
        menuWindow.show();
        return true;
    }

    function closeMenu() {
        menuRow = -1;
        menuEntries = [];
        menuWindow.hide();
    }

    StatusNotifierHost {
        id: trayModel
    }

    // The menu is drawn in its own full-screen window: a popup anchored to the
    // bottom panel is clipped to the panel's 60-pixel window here, and a
    // smaller window cannot be positioned on Wayland.  A maximised window is
    // positioned by the compositor and covers the whole screen, so the menu is
    // simply placed at the bottom left inside it.
    Window {
        id: menuWindow

        flags: Qt.Window | Qt.FramelessWindowHint
        color: "transparent"

        // clicking anywhere else closes the menu
        Item {
            anchors.fill: parent

            TapHandler {
                onSingleTapped: root.closeMenu()
            }
        }

        Rectangle {
            id: menuBackground

            readonly property int margin: Math.round(Kirigami.Units.smallSpacing / 2)

            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: Math.round(Kirigami.Units.gridUnit * 2)
            width: Math.round(root.iconSize * 10)
            height: menuColumn.implicitHeight + margin * 2

            color: Kirigami.Theme.backgroundColor
            radius: Kirigami.Units.smallSpacing / 2
            border.color: Qt.rgba(Kirigami.Theme.textColor.r,
                                  Kirigami.Theme.textColor.g,
                                  Kirigami.Theme.textColor.b, 0.25)

            Column {
                id: menuColumn

                anchors.fill: parent
                anchors.margins: menuBackground.margin
                spacing: 0

                Repeater {
                    model: root.menuEntries

                    delegate: Controls.MenuItem {
                        width: menuColumn.width
                        text: "    ".repeat(Math.max(0, modelData.depth - 1))
                              + (modelData.checked ? "\u2713 " : "") + modelData.label
                        enabled: modelData.enabled
                        onClicked: {
                            // activate while the entry still exists: closing the
                            // menu first destroys this delegate and its scope
                            trayModel.activateMenuEntry(modelData.row, modelData.id);
                            root.closeMenu();
                        }
                    }
                }
            }
        }
    }

    Repeater {
        model: trayModel

        delegate: Item {
            id: item

            required property int index
            required property string iconName
            required property string iconSource
            required property string title
            required property string itemStatus
            required property bool itemIsMenu

            Layout.preferredWidth: root.iconSize
            Layout.preferredHeight: root.iconSize

            // an item that only reports status (rather than needing attention)
            // is dimmed, like the desktop's tray does
            opacity: item.itemStatus === "Passive" ? 0.55 : 1.0

            Kirigami.Icon {
                anchors.fill: parent
                smooth: true
                source: item.iconSource.length > 0 ? item.iconSource : item.iconName
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                longPressThreshold: 0.6

                onSingleTapped: (eventPoint, button) => {
                    if (button === Qt.MiddleButton) {
                        trayModel.secondaryActivate(item.index);
                    } else if (button === Qt.RightButton || item.itemIsMenu || !root.openMenu(item.index)) {
                        // right click, a menu item, or a plain StatusNotifierItem
                        // without a menu: use the item's own methods
                        if (button === Qt.RightButton || item.itemIsMenu) {
                            if (!root.openMenu(item.index)) {
                                trayModel.contextMenu(item.index);
                            }
                        } else {
                            trayModel.activate(item.index);
                        }
                    }
                }

                onLongPressed: {
                    if (!root.openMenu(item.index)) {
                        trayModel.contextMenu(item.index);
                    }
                }
            }
        }
    }
}
