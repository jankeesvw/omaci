import QtQuick
import "format.js" as Format

// The earlier builds like this one as a sparkline, oldest on the left: each
// character as tall as the build was long, coloured by how it ended. The
// last one is this build. ▅▆▅▆▃▅▆▁▆▅▇
Row {
    id: strip
    property var builds: []       // { key, seconds, state, title, sha, current, selectable }
    property real currentSeconds: 0
    property real typical: 0
    property string hovered: ""
    signal picked(string key)

    readonly property real tallest: {
        let most = Math.max(currentSeconds, typical, 1)
        for (const build of builds) most = Math.max(most, build.seconds)
        // One build that took an hour mustn't flatten the others into a line.
        return typical > 0 ? Math.min(most, Math.max(typical * 1.6, currentSeconds)) : most
    }

    Repeater {
        model: strip.builds
        delegate: Mono {
            required property var modelData
            readonly property real seconds: modelData.current ? strip.currentSeconds : modelData.seconds
            readonly property bool going: modelData.state === "running" || modelData.state === "queued"
            text: "▁▂▃▄▅▆▇█"[Math.max(0, Math.min(7, Math.round(seconds / strip.tallest * 8) - 1))]
            font.bold: modelData.current
            color: area.containsMouse ? theme.text
                 : modelData.current && going ? theme.accent
                 : modelData.state === "success" ? theme.ok
                 : modelData.state === "failure" ? theme.bad : theme.faint
            opacity: modelData.current || area.containsMouse ? 1 : 0.8
            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: modelData.selectable ? Qt.PointingHandCursor : Qt.ArrowCursor
                onContainsMouseChanged: strip.hovered = !containsMouse ? ""
                    : modelData.title + " · " + (parent.going ? "running"
                        : Format.duration(modelData.seconds) + ", " + ({ "success": "passed", "failure": "failed", "cancelled": "cancelled" }[modelData.state] || modelData.state))
                onClicked: if (modelData.selectable) strip.picked(modelData.key)
            }
        }
    }
}
