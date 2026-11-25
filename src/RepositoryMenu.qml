import QtQuick
import QtQuick.Controls

// Pick a repository or watch all accessible repositories in an organization.
Rectangle {
    id: menu
    objectName: "repositoryMenu"
    property bool open: false
    // Nothing to go back to: the window has no repository yet.
    property bool required: false
    property bool allowOrganizations: true
    property var repositories: []
    property bool loading: false
    property string error: ""
    property string current: ""
    property real now: Date.now()
    signal picked(string name)
    signal closed()

    readonly property real cw: theme.cellWidth
    readonly property real ch: theme.cellHeight
    readonly property int rowHeight: Math.round(ch * 1.4)
    property int index: 0

    readonly property string typed: input.text.trim()
    // Every word has to be in the name, so "wbso app" finds wbso-ai/application.
    readonly property var shown: {
        const orgPrefix = /^org:/i.test(typed)
        const slug = (orgPrefix ? typed.slice(4) : typed).toLowerCase()
        const organizations = allowOrganizations
            ? Array.from(new Set(repositories.map(repo => repo.organization).filter(Boolean))) : []
        const orgs = organizations.filter(name => orgPrefix ? name.toLowerCase().includes(slug)
                                                           : name.toLowerCase() === slug)
        if (allowOrganizations && orgPrefix && /^[a-z0-9](?:[a-z0-9-]{0,37}[a-z0-9])?$/.test(slug)
                && !orgs.some(name => name.toLowerCase() === slug))
            orgs.unshift(slug)
        orgs.sort((a, b) => Number(b.toLowerCase() === slug) - Number(a.toLowerCase() === slug))
        const targets = orgs.map(name => ({ name: "org:" + name.toLowerCase(), org: name }))
        if (orgPrefix) return targets
        const words = typed.toLowerCase().split(/\s+/).filter(word => word)
        const found = repositories.filter(repo => {
            const name = repo.name.toLowerCase()
            return words.every(word => name.includes(word))
        })
        const whole = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(typed)
        if (whole && !repositories.some(repo => repo.name.toLowerCase() === typed.toLowerCase()))
            found.unshift({ name: typed, typed: true })
        return targets.concat(found)
    }

    function ago(iso) {
        if (!iso) return ""
        const s = Math.max(0, Math.round((menu.now - new Date(iso).getTime()) / 1000))
        if (s < 60) return "just now"
        if (s < 3600) return Math.floor(s / 60) + "m ago"
        if (s < 86400) return Math.floor(s / 3600) + "h ago"
        if (s < 86400 * 60) return Math.floor(s / 86400) + "d ago"
        return Math.floor(s / 86400 / 30) + " months ago"
    }
    function move(delta) {
        if (shown.length === 0) return
        index = Math.max(0, Math.min(shown.length - 1, index + delta))
    }
    function pick(i) {
        const repo = shown[i]
        if (repo) picked(repo.name)
    }
    function close() {
        if (!required) closed()
    }

    anchors.fill: parent
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.82)
    visible: open
    onOpenChanged: {
        if (!open) return
        input.text = ""
        index = 0
        input.forceActiveFocus()
    }
    onTypedChanged: index = 0
    Component.onCompleted: if (open) input.forceActiveFocus()

    MouseArea { anchors.fill: parent; onClicked: menu.close() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.round(Math.min(parent.width - menu.cw * 6, menu.cw * 100))
        height: Math.round(Math.min(parent.height - menu.ch * 6, menu.ch * 36))
        color: theme.background
        radius: theme.radius
        // Clicks inside the frame stay inside it.
        MouseArea { anchors.fill: parent; onClicked: input.forceActiveFocus() }

        Panel {
            anchors.fill: parent
            title: menu.allowOrganizations ? "repositories & organizations" : "repositories"
            note: menu.loading ? "asking github…" : menu.shown.length + " available"
            corner: menu.required ? "pick one to watch" : "esc closes"
            focused: true

            Item {
                id: prompt
                width: parent.width
                height: menu.ch + 8

                Mono { id: caret; x: menu.cw; y: 4; text: ">"; color: theme.accent; font.bold: true }
                TextInput {
                    id: input
                    x: caret.x + menu.cw * 2
                    y: 4
                    width: parent.width - x - menu.cw
                    font.family: theme.font
                    font.pixelSize: theme.textSize
                    font.hintingPreference: Font.PreferFullHinting
                    renderType: Text.NativeRendering
                    color: theme.text
                    selectionColor: theme.accentSoft
                    selectedTextColor: theme.text
                    cursorDelegate: Rectangle {
                        width: menu.cw
                        color: theme.accent
                        opacity: 0.7
                        visible: input.activeFocus
                    }
                    clip: true
                    // Hidden is not enough: a hidden field keeps the keyboard,
                    // and the window's keys would type into it.
                    enabled: menu.open
                    // The window's one-letter keys would take what is typed
                    // here; while the menu is open, every key is the menu's.
                    Keys.onShortcutOverride: event => event.accepted = true
                    Keys.onPressed: event => {
                        const ctrl = event.modifiers & Qt.ControlModifier
                        if (event.key === Qt.Key_Down || (ctrl && (event.key === Qt.Key_N || event.key === Qt.Key_J))) menu.move(1)
                        else if (event.key === Qt.Key_Up || (ctrl && (event.key === Qt.Key_P || event.key === Qt.Key_K))) menu.move(-1)
                        else if (event.key === Qt.Key_PageDown) menu.move(Math.max(1, Math.floor(list.height / menu.rowHeight) - 1))
                        else if (event.key === Qt.Key_PageUp) menu.move(-Math.max(1, Math.floor(list.height / menu.rowHeight) - 1))
                        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) menu.pick(menu.index)
                        else if (event.key === Qt.Key_Escape) menu.close()
                        else return
                        event.accepted = true
                    }
                }
                Mono {
                    x: input.x
                    y: 4
                    visible: input.text === ""
                    text: menu.allowOrganizations ? "search, or enter owner/name or org:name" : "search, or enter owner/name"
                    color: theme.faint
                }
            }

            Mono {
                id: problem
                y: prompt.height + 4
                x: menu.cw
                width: parent.width - menu.cw * 2
                visible: menu.error !== ""
                height: visible ? implicitHeight + 6 : 0
                text: menu.error
                color: theme.bad
                wrapMode: Text.Wrap
            }

            ListView {
                id: list
                y: prompt.height + problem.height + 6
                width: parent.width
                height: parent.height - y
                clip: true
                model: menu.shown
                currentIndex: menu.index
                highlightMoveDuration: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: Scroll {}

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool picked: index === menu.index
                    readonly property bool watching: modelData.name === menu.current
                    readonly property int nameCells: Math.min(40, Math.max(24, Math.floor(width / menu.cw * 0.42)))
                    width: ListView.view.width
                    height: menu.rowHeight
                    radius: Math.min(4, theme.radius)
                    color: picked ? theme.raised : area.containsMouse ? theme.hover : "transparent"

                    Rectangle { width: Math.max(2, Math.round(menu.cw / 3)); height: parent.height; color: theme.accent; visible: row.picked }

                    Row {
                        id: name
                        x: menu.cw
                        anchors.verticalCenter: parent.verticalCenter
                        width: row.nameCells * menu.cw
                        clip: true
                        Mono { text: row.modelData.typed ? "open " : ""; color: theme.accent; visible: text !== "" }
                        Mono { text: row.modelData.org ? "" : row.modelData.name.split("/")[0] + "/"; color: theme.dim }
                        Mono {
                            text: row.modelData.org || row.modelData.name.split("/").slice(1).join("/")
                            color: row.modelData.archived ? theme.dim : theme.text
                            font.bold: row.picked || row.watching
                        }
                    }
                    Mono {
                        x: name.x + name.width + menu.cw * 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: when.x - x - menu.cw * 2
                        text: row.modelData.org ? "watch this organization"
                            : row.modelData.typed ? "opens if your GitHub account has access" : (row.modelData.description || "")
                        color: theme.dim
                        elide: Text.ElideRight
                    }
                    Mono {
                        id: when
                        anchors.right: parent.right
                        anchors.rightMargin: menu.cw
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.watching ? "watching" : row.modelData.org ? "organization"
                            : row.modelData.archived ? "archived" : menu.ago(row.modelData.pushedAt)
                        color: row.watching ? theme.accent : theme.faint
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: menu.pick(row.index)
                    }
                }

                Mono {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    text: menu.loading ? "asking github which repositories you can see…"
                        : menu.typed ? "no matching repositories or organizations"
                        : menu.error ? "" : "no repositories"
                    color: theme.dim
                }
            }
        }
    }
}
