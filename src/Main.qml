import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "format.js" as Format

// The window is drawn the way a good terminal program is: one monospace
// font at one size, everything on its character grid, panels as thin frames
// with their title in the top edge, bars made of block characters and
// actions written as `[key] what it does`. It is still a window: every one
// of those is something to click, and the log scrolls by the pixel.
ApplicationWindow {
    id: win
    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 600
    visible: true
    // The answer in the title too, where the compositor and the window
    // switcher show it without the window being in front.
    title: "Omaci" + (backend.repo ? " · " + backend.repo : "") + (win.buildActive && win.headline ? " · " + win.headline : "")
    color: theme.background

    readonly property real cw: theme.cellWidth
    readonly property real ch: theme.cellHeight
    readonly property int rowHeight: Math.round(ch * 1.4)
    readonly property var build: backend.selectedBuild
    readonly property var job: backend.selectedJob
    readonly property var estimate: backend.estimate
    readonly property bool buildActive: build.state === "running" || build.state === "queued"

    property bool helpVisible: false
    property bool repoMenuVisible: false
    // The log alone, for reading: the build and its timeline step aside.
    property bool tall: false
    // Long log lines wrap onto the next line instead of running off to the
    // right; W turns it off for logs that read better as one line each.
    property bool wrap: true
    // The panel the keys last acted on, which gets the accent on its frame.
    property string focusPanel: "builds"
    property real now: Date.now()

    Timer { interval: 1000; running: true; repeat: true; onTriggered: win.now = Date.now() }

    function seconds(from, to) {
        if (!from) return 0
        const end = to ? new Date(to).getTime() : win.now
        return Math.max(0, Math.round((end - new Date(from).getTime()) / 1000))
    }
    function ago(iso) {
        const s = seconds(iso)
        if (s < 60) return "now"
        if (s < 3600) return Math.floor(s / 60) + "m ago"
        if (s < 86400) return Math.floor(s / 3600) + "h ago"
        return Math.floor(s / 86400) + "d ago"
    }
    function stateColor(state) {
        return state === "success" ? theme.ok : state === "failure" ? theme.bad
             : state === "running" || state === "queued" ? theme.accent : theme.dim
    }

    // The answer to "how is it going", in a few words.
    readonly property string headline: {
        const e = win.estimate
        if (!win.build.key) return ""
        if (!win.buildActive) {
            const took = Format.duration(win.build.seconds)
            return ({ "success": "Passed in " + took, "failure": "Failed after " + took,
                      "cancelled": "Cancelled after " + took, "skipped": "Skipped" })[win.build.state] || "Finished in " + took
        }
        if (!e.known) return "Running for " + Format.duration(e.elapsed)
        if (e.late) return "Taking longer than usual"
        if (e.remaining < 3) return "Any moment now"
        return "About " + Format.duration(e.remainingRounded) + " left"
    }
    // What that answer rests on.
    readonly property string basis: {
        const e = win.estimate
        if (!win.build.key) return ""
        const usually = e.usualKnown
            ? "usually " + Format.duration(e.usual) + (e.usualHigh - e.usualLow >= 2
                ? ", between " + Format.duration(e.usualLow) + " and " + Format.duration(e.usualHigh) : "")
            : ""
        if (!win.buildActive) {
            if (!e.usualKnown) return ""
            const delta = win.build.seconds - e.usual
            const compared = win.build.state !== "success" ? ""
                : Math.abs(delta) < Math.max(5, e.usual * 0.05) ? " · about as long as usual"
                : delta > 0 ? " · " + Format.duration(delta) + " slower than usual"
                            : " · " + Format.duration(-delta) + " faster than usual"
            return usually + compared
        }
        if (!e.known)
            return "no estimate yet: fewer than three earlier builds like this one have passed"
        if (e.late)
            return Format.duration(e.elapsed) + " so far, " + Format.duration(e.over) + " over the usual"
        if (e.basis === "jobs")
            return (e.usualKnown ? "usually " + Format.duration(e.usual) + " · this one " : "this one ")
                 + "between " + Format.duration(e.low) + " and " + Format.duration(e.high)
                 + ", going by the jobs of the last " + e.samples + " builds like it"
        return usually + ", going by " + e.samples + " earlier builds"
    }
    // "2 running, 1 to come", for the edge of the jobs panel.
    readonly property string jobsNote: {
        const counts = {}
        for (const job of backend.jobs) counts[job.state] = (counts[job.state] || 0) + 1
        const parts = []
        if (counts.failure) parts.push(counts.failure + " failed")
        if (counts.running) parts.push(counts.running + " running")
        if (counts.queued) parts.push(counts.queued + " queued")
        if (counts.pending) parts.push(counts.pending + " to come")
        if (counts.success) parts.push(counts.success + " passed")
        return parts.join(", ")
    }

    // Where the log is, and what moved it there.
    function scrollLogBy(pages) {
        win.focusPanel = "log"
        logView.scripted = true
        logView.contentY = Math.max(logView.originY, Math.min(logView.contentY + pages * logView.height * 0.9,
                                    logView.originY + logView.contentHeight - logView.height))
        logView.scripted = false
        logView.follow = logView.nearEnd
        backend.selectStep(-1)
    }
    // Puts a line near the top of the view, with a little of what came
    // before it, so what follows from it is what you see.
    function jumpToLine(index, context) {
        if (index < 0) return
        logView.follow = false
        logView.highlighted = index
        logView.scripted = true
        followAnimation.stop()
        logView.positionViewAtIndex(Math.max(0, index - context), ListView.Beginning)
        logView.scripted = false
    }
    // The last line a step wrote: the one before the next step that wrote
    // anything begins, or the end of the log for the last one.
    function stepEnd(index) {
        const steps = backend.steps
        const first = steps[index].line
        for (let i = index + 1; i < steps.length; ++i)
            if (steps[i].line > first) return steps[i].line - 1
        return logView.count - 1
    }
    // A step opens on its end, at the bottom of the view, because that is
    // where its outcome is: the summary of a test run, the error a command
    // died on. The step still running is at the end of the log, where its
    // own log will come once GitHub has it, and is followed there.
    function showStep(index) {
        const step = backend.steps[index]
        if (!step || (step.line < 0 && !step.pending)) return
        const last = step.pending ? logView.count - 1 : win.stepEnd(index)
        logView.highlighted = -1
        followAnimation.stop()
        if (step.pending || (step.state === "running" && last >= logView.count - 1)) {
            logView.follow = true
            logView.toEnd(false)
            return
        }
        logView.follow = false
        logView.scripted = true
        logView.positionViewAtIndex(last, ListView.End)
        logView.scripted = false
    }
    function jumpToStep(index) {
        const step = backend.steps[index]
        if (!step || (step.line < 0 && !step.pending)) return
        win.focusPanel = "steps"
        backend.selectStep(index)
        win.showStep(index)
    }
    function moveStep(delta) {
        win.focusPanel = "steps"
        const from = backend.selectedStep >= 0 ? backend.selectedStep : logView.readingStep
        const index = backend.moveStep(delta > 0 ? from : (from < 0 ? backend.steps.length : from), delta)
        if (index >= 0) win.showStep(index)
    }
    property int failureIndex: -1
    function jumpToFailure(index) {
        const failures = backend.failures
        if (failures.length === 0) return
        win.focusPanel = "log"
        win.failureIndex = (index + failures.length) % failures.length
        const failure = failures[win.failureIndex]
        backend.selectStep(-1)
        // The first failure of a block shows the block from its heading.
        win.jumpToLine(failure.line, Math.min(2, failure.line - failure.first))
    }
    function copyLink() {
        const url = backend.copyLink()
        toast.show(url ? "copied " + url : "nothing to link to yet")
    }
    function copyFailure() {
        toast.show(backend.copyFailure() ? "copied the failure and its link" : "this job has no failure to copy")
    }
    function openOnGitHub() {
        const url = backend.openUrl()
        if (url) Qt.openUrlExternally(url)
    }
    // Wrapping changes how tall every line is, so the line at the top is
    // put back at the top afterwards, or the end if the log was following.
    function toggleWrap() {
        win.focusPanel = "log"
        const top = logView.indexAt(20, logView.contentY + 2)
        win.wrap = !win.wrap
        if (logView.follow) {
            Qt.callLater(logView.toEnd, false)
        } else if (top >= 0) {
            Qt.callLater(() => {
                logView.scripted = true
                logView.positionViewAtIndex(top, ListView.Beginning)
                logView.scripted = false
            })
        }
    }
    function moveBuild(delta) { win.focusPanel = "builds"; backend.moveBuild(delta) }
    function moveJob(delta) { win.focusPanel = "jobs"; backend.moveJob(delta) }
    // For screenshots: main.cpp walks the window through a scene.
    function scene(step) {
        if (step === "help") win.helpVisible = true
        else if (step === "repositories") win.repoMenuVisible = true
        else if (step === "tall") win.tall = true
        else if (step === "failure+") win.jumpToFailure(win.failureIndex + 1)
        else if (step === "step+") win.moveStep(1)
        else if (step.startsWith("step:")) win.jumpToStep(parseInt(step.slice(5)))
        else if (step === "end") { logView.follow = true; logView.toEnd(false) }
        else if (step === "previous") backend.selectPrevious()
        else if (step === "back") backend.selectCameFrom()
        else if (step === "copy") win.copyLink()
    }

    Shortcut { sequence: "?"; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut {
        sequence: "Esc"
        onActivated: win.helpVisible ? win.helpVisible = false : win.tall = false
    }
    Shortcut { sequence: "Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "G"; onActivated: { win.helpVisible = false; win.repoMenuVisible = true } }
    Shortcut { sequences: ["Space", "R", "Ctrl+R"]; onActivated: backend.refresh() }
    Shortcut { sequences: ["J", "Down"]; onActivated: win.moveBuild(1) }
    Shortcut { sequences: ["K", "Up"]; onActivated: win.moveBuild(-1) }
    Shortcut { sequences: ["L", "Right", "Tab"]; onActivated: win.moveJob(1) }
    Shortcut { sequences: ["H", "Left", "Shift+Tab"]; onActivated: win.moveJob(-1) }
    Shortcut { sequence: "N"; onActivated: win.moveStep(1) }
    Shortcut { sequence: "P"; onActivated: win.moveStep(-1) }
    Shortcut { sequence: "E"; onActivated: win.jumpToFailure(win.failureIndex + 1) }
    Shortcut { sequence: "Shift+E"; onActivated: win.jumpToFailure(win.failureIndex - 1) }
    Shortcut { sequence: "C"; onActivated: win.copyLink() }
    Shortcut { sequence: "Y"; onActivated: win.copyFailure() }
    Shortcut { sequence: "Shift+C"; onActivated: { backend.copyLog(); toast.show("copied the whole log") } }
    Shortcut { sequences: ["O", "Return"]; onActivated: win.openOnGitHub() }
    Shortcut { sequence: "Z"; onActivated: win.tall = !win.tall }
    Shortcut { sequence: "W"; onActivated: win.toggleWrap() }
    Shortcut { sequence: "B"; onActivated: { win.focusPanel = "builds"; backend.selectPrevious() } }
    Shortcut { sequence: "Shift+B"; onActivated: { win.focusPanel = "builds"; backend.selectCameFrom() } }
    Shortcut { sequence: "F"; onActivated: backend.toggleBranch() }
    Shortcut { sequence: "S"; onActivated: backend.toggleSkipped() }
    Shortcut { sequence: "M"; onActivated: { win.focusPanel = "builds"; backend.toggleMain() } }
    Shortcut { sequence: "A"; onActivated: { win.focusPanel = "builds"; backend.toggleRunning() } }
    Shortcut { sequence: "PgDown"; onActivated: win.scrollLogBy(1) }
    Shortcut { sequence: "PgUp"; onActivated: win.scrollLogBy(-1) }
    Shortcut { sequence: "Home"; onActivated: { win.focusPanel = "log"; logView.follow = false; backend.selectStep(-1); logView.positionViewAtBeginning() } }
    Shortcut { sequence: "End"; onActivated: { win.focusPanel = "log"; logView.follow = true; backend.selectStep(-1); logView.toEnd(true) } }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Math.round(win.cw / 2)
        spacing: 0

        // Which repository, what is filtered, how fresh the data is.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: win.ch + 10

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: win.cw
                anchors.rightMargin: win.cw
                spacing: win.cw

                Mono { text: "omaci"; color: theme.accent; font.bold: true }
                Keycap {
                    key: "g"
                    label: backend.repo || "pick a repository"
                    strong: true
                    onClicked: win.repoMenuVisible = true
                }
                // The filters, each a box that is filled while it is on, so
                // what the list leaves out is never a guess.
                Mono { text: "filter"; color: theme.faint }
                Toggle {
                    key: "m"
                    label: backend.mainBranch || "main"
                    checked: backend.mainOnly
                    onClicked: { win.focusPanel = "builds"; backend.toggleMain() }
                }
                Toggle {
                    key: "f"
                    label: backend.branchFilter && !backend.mainOnly ? backend.branchFilter : "this branch"
                    checked: backend.branchFilter !== "" && !backend.mainOnly
                    active: backend.branchFilter !== "" || win.build.branch !== undefined
                    onClicked: { win.focusPanel = "builds"; backend.toggleBranch() }
                }
                Toggle {
                    key: "a"
                    label: backend.activeCount > 0 ? backend.activeCount + " running" : "running"
                    checked: backend.runningOnly
                    labelColor: checked ? theme.text : backend.activeCount > 0 ? theme.accent : theme.dim
                    onClicked: { win.focusPanel = "builds"; backend.toggleRunning() }
                }
                Toggle {
                    key: "s"
                    label: "skipped"
                    checked: backend.showSkipped
                    onClicked: { win.focusPanel = "builds"; backend.toggleSkipped() }
                }
                Item { Layout.fillWidth: true }
                StatusIcon { state: "running"; opacity: backend.busy ? 1 : 0 }
                Keycap {
                    key: "r"
                    label: {
                        win.now
                        if (isNaN(backend.lastUpdated.getTime())) return "loading…"
                        const s = Math.round((win.now - backend.lastUpdated.getTime()) / 1000)
                        return "updated " + (s < 2 ? "just now" : s + "s ago")
                    }
                    onClicked: backend.refresh()
                }
                Keycap { key: "?"; label: "keys"; onClicked: win.helpVisible = true }
            }
        }

        // What went wrong, said plainly, instead of an empty window.
        Panel {
            Layout.fillWidth: true
            visible: backend.error !== ""
            title: "error"
            tint: theme.bad
            contentHeight: errorText.implicitHeight
            Mono {
                id: errorText
                width: parent.width
                text: backend.error
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Builds, newest first.
            Panel {
                Layout.preferredWidth: Math.round(Math.max(34, Math.min(42, win.width / win.cw * 0.23)) * win.cw)
                Layout.fillHeight: true
                title: "builds"
                note: [backend.branchFilter ? "only " + backend.branchFilter : "", backend.runningOnly ? "running" : ""].filter(part => part).join(" · ")
                corner: backend.builds.length ? (backend.selectedIndex + 1) + "/" + backend.builds.length : ""
                focused: win.focusPanel === "builds"

                ListView {
                    id: buildList
                    anchors.fill: parent
                    clip: true
                    model: backend.builds
                    currentIndex: backend.selectedIndex
                    highlightMoveDuration: 0
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: Scroll {}

                    delegate: Rectangle {
                        id: buildRow
                        required property var modelData
                        required property int index
                        readonly property bool current: modelData.key === backend.selectedKey
                        readonly property bool active: modelData.state === "running" || modelData.state === "queued"
                        readonly property real elapsed: { win.now; return win.seconds(modelData.startedAt) }
                        // The picked build has the better forecast; the others
                        // go by how long builds like them usually take.
                        readonly property real total: current && win.estimate.known ? win.estimate.total : (modelData.usual || 0)
                        // Main is the trunk the list hangs from, the way git
                        // draws a graph: its builds sit on the line, the
                        // builds of every other branch hang off it, set in.
                        readonly property bool onMain: modelData.isBuild && modelData.branch === backend.mainBranch
                        readonly property real trunkX: Math.round(win.cw * 1.5)
                        readonly property real rule: Math.max(1, Math.round(win.cw / 6))
                        readonly property real textX: win.cw * (onMain ? 3 : 5)
                        readonly property int barCells: Math.max(4, Math.floor((width - textX) / win.cw) - 2)
                        width: ListView.view.width
                        height: Math.round(win.ch * (active ? 3 : 2) + 10)
                        radius: Math.min(4, theme.radius)
                        color: current ? theme.raised : (mouse.containsMouse ? theme.hover : "transparent")

                        Rectangle { width: Math.max(2, Math.round(win.cw / 3)); height: parent.height; color: theme.accent; visible: buildRow.current }

                        // The trunk runs through every row; a main build's
                        // mark interrupts it, another branch's joins it.
                        Rectangle {
                            x: buildRow.trunkX
                            width: buildRow.rule
                            height: buildRow.onMain ? 4 : parent.height
                            color: theme.faint
                        }
                        Rectangle {
                            visible: buildRow.onMain
                            x: buildRow.trunkX
                            y: 6 + win.ch
                            width: buildRow.rule
                            height: parent.height - y
                            color: theme.faint
                        }
                        Rectangle {
                            visible: !buildRow.onMain
                            x: buildRow.trunkX
                            y: Math.round(5 + win.ch / 2)
                            width: win.cw * 3 - buildRow.trunkX - 2
                            height: buildRow.rule
                            color: theme.faint
                        }

                        StatusIcon { x: buildRow.textX - win.cw * 2; y: 5; state: modelData.failing ? "failure" : modelData.state }
                        Mono {
                            x: buildRow.textX
                            y: 5
                            width: parent.width - x - agoText.width - win.cw * 2
                            text: modelData.title
                            color: buildRow.onMain || buildRow.current ? theme.text : theme.dim
                            font.bold: buildRow.current
                            elide: Text.ElideRight
                        }
                        Mono { id: agoText; anchors.right: parent.right; anchors.rightMargin: win.cw; y: 5; text: win.ago(modelData.startedAt); color: theme.dim }
                        // The branch in its own colour, so main and the rest
                        // tell apart without reading.
                        Mono {
                            id: branchText
                            x: buildRow.textX
                            y: 5 + win.ch
                            width: Math.min(implicitWidth, parent.width - x - shaText.implicitWidth - timeText.width - win.cw * 2)
                            text: modelData.isBuild ? modelData.branch : modelData.workflows.join(", ")
                            color: buildRow.onMain ? theme.accent : modelData.isBuild ? theme.info : theme.dim
                            font.bold: buildRow.onMain
                            elide: Text.ElideMiddle
                        }
                        Mono {
                            id: shaText
                            x: branchText.x + branchText.width
                            y: 5 + win.ch
                            text: " · " + (modelData.isBuild ? modelData.sha : modelData.event)
                            color: theme.dim
                        }
                        Mono {
                            id: timeText
                            anchors.right: parent.right
                            anchors.rightMargin: win.cw
                            y: 5 + win.ch
                            text: !buildRow.active ? Format.duration(modelData.seconds)
                                : buildRow.total > buildRow.elapsed + 3 ? "~" + Format.duration(Math.round((buildRow.total - buildRow.elapsed) / 10) * 10) + " left"
                                : Format.duration(buildRow.elapsed)
                            color: buildRow.active ? theme.accent : theme.dim
                        }
                        // How far a running build is, on the row itself.
                        Item {
                            visible: buildRow.active
                            x: buildRow.textX
                            y: 5 + win.ch * 2
                            Blocks { cells: buildRow.barCells; color: theme.dim; shade: 0.28 }
                            Blocks {
                                visible: buildRow.total > 0
                                cells: Math.round(buildRow.barCells * Math.min(1, buildRow.elapsed / Math.max(1, buildRow.total)))
                                color: modelData.failing ? theme.bad : theme.accent
                            }
                        }
                        MouseArea {
                            id: mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: { win.focusPanel = "builds"; backend.selectBuild(modelData.key) }
                        }
                    }

                    Mono {
                        anchors.centerIn: parent
                        visible: buildList.count === 0 && !isNaN(backend.lastUpdated.getTime())
                        text: backend.runningOnly && backend.activeCount === 0 ? "nothing is running right now" : "no builds match this filter"
                        color: theme.dim
                    }
                }
            }

            // The picked build: how it is going, its jobs in time, and the
            // steps and log of one job.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0
                visible: backend.selectedKey !== ""

                Panel {
                    Layout.fillWidth: true
                    visible: !win.tall
                    title: "build"
                    note: !win.build.key ? ""
                        : win.build.branch === backend.mainBranch ? [win.build.branch, win.build.sha].join(" · ")
                        : [win.build.branch, "not on " + backend.mainBranch, win.build.sha].filter(part => part).join(" · ")
                    corner: win.build.failing ? "failing" : (win.build.state || "")
                    cornerColor: win.build.failing ? theme.bad : win.stateColor(win.build.state)
                    contentHeight: buildColumn.implicitHeight

                    Column {
                        id: buildColumn
                        width: parent.width
                        spacing: 3

                        Item {
                            width: parent.width
                            height: headlineText.implicitHeight
                            StatusIcon {
                                id: buildIcon
                                anchors.verticalCenter: parent.verticalCenter
                                state: win.build.failing ? "failure" : (win.build.state || "queued")
                            }
                            Mono {
                                x: win.cw * 2
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - x - headlineText.width - win.cw * 2
                                text: win.build.title || ""
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            // The one thing in the window that is larger than the
                            // grid: it is what the window is opened for.
                            Mono {
                                id: headlineText
                                anchors.right: parent.right
                                text: win.headline
                                color: win.buildActive ? (win.estimate.late ? theme.warn : win.build.failing ? theme.bad : theme.accent)
                                                       : win.stateColor(win.build.state)
                                font.pixelSize: Math.round(theme.textSize * 1.5)
                                font.bold: true
                            }
                        }
                        Item {
                            width: parent.width
                            height: win.ch
                            Mono {
                                x: win.cw * 2
                                width: parent.width - x - failingText.width - win.cw
                                color: theme.dim
                                elide: Text.ElideRight
                                text: {
                                    const b = win.build
                                    if (!b.key) return ""
                                    win.now
                                    return [b.event, b.actor, (b.workflows || []).join(" + "),
                                            "started " + win.ago(b.startedAt)].filter(part => part).join(" · ")
                                }
                            }
                            Mono {
                                id: failingText
                                anchors.right: parent.right
                                text: win.build.failing ? "a job has already failed; the rest is finishing" : ""
                                color: theme.bad
                            }
                        }
                        Item { width: 1; height: Math.round(win.ch / 4) }
                        Item {
                            width: parent.width
                            height: win.ch
                            EtaBar {
                                width: parent.width - (historyStrip.visible ? historyStrip.width + win.cw * 2 : 0)
                                active: win.buildActive
                                elapsed: win.estimate.elapsed || 0
                                typical: win.buildActive ? (win.estimate.known ? win.estimate.total : 0)
                                                         : (win.estimate.usualKnown ? win.estimate.usual : 0)
                                low: win.buildActive ? (win.estimate.low || 0) : (win.estimate.usualLow || 0)
                                high: win.buildActive ? (win.estimate.high || 0) : (win.estimate.usualHigh || 0)
                                fill: win.build.failing ? theme.bad : win.stateColor(win.build.state)
                            }
                            HistoryStrip {
                                id: historyStrip
                                visible: backend.history.length > 1
                                anchors.right: parent.right
                                builds: backend.history
                                currentSeconds: win.estimate.elapsed || 0
                                typical: win.estimate.usualKnown ? win.estimate.usual : 0
                                onPicked: key => backend.selectBuild(key)
                            }
                        }
                        Item {
                            width: parent.width
                            height: win.ch
                            Mono {
                                id: soFar
                                text: win.buildActive ? Format.duration(win.estimate.elapsed || 0) + " so far" : ""
                            }
                            Mono {
                                x: soFar.text ? soFar.width + win.cw * 2 : 0
                                width: parent.width - x - (historyStrip.visible ? earlierText.width + win.cw * 2 : 0)
                                horizontalAlignment: win.buildActive ? Text.AlignRight : Text.AlignLeft
                                text: historyStrip.hovered !== "" ? historyStrip.hovered : win.basis
                                color: theme.dim
                                elide: Text.ElideRight
                            }
                            Mono {
                                id: earlierText
                                visible: historyStrip.visible
                                anchors.right: parent.right
                                text: "earlier builds"
                                color: theme.faint
                            }
                        }
                        Item { width: 1; height: Math.round(win.ch / 4); visible: previousRow.visible }

                        // The build main had before this one, to compare with,
                        // and the way to it.
                        Rectangle {
                            id: previousRow
                            readonly property var was: backend.previous
                            visible: was.key !== undefined
                            width: parent.width
                            height: win.rowHeight
                            radius: Math.min(4, theme.radius)
                            color: previousMouse.containsMouse ? theme.hover : "transparent"

                            MouseArea {
                                id: previousMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.selectPrevious()
                            }
                            RowLayout {
                                anchors.fill: parent
                                spacing: win.cw
                                Mono { text: "before, " + (previousRow.was.branch || ""); color: theme.dim }
                                StatusIcon { state: previousRow.was.state || "queued" }
                                Mono {
                                    Layout.maximumWidth: previousRow.width * 0.3
                                    text: previousRow.was.title || ""
                                    elide: Text.ElideRight
                                }
                                Mono { text: previousRow.was.sha || ""; color: theme.faint }
                                Mono {
                                    color: theme.dim
                                    text: {
                                        const was = previousRow.was
                                        if (!was.key) return ""
                                        win.now
                                        const verb = ({ "success": "passed in ", "failure": "failed after ", "cancelled": "cancelled after " })[was.state] || "took "
                                        return verb + Format.duration(was.seconds) + " · " + win.ago(was.startedAt)
                                    }
                                }
                                // What changed since: the job that broke, the
                                // ones that got slower, the whole build.
                                Mono {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    readonly property var notes: previousRow.was.notes || []
                                    readonly property bool alarming: notes.some(note => note.endsWith(" broke"))
                                    color: alarming ? theme.bad : notes.some(note => note.endsWith(" slower")) ? theme.warn : theme.dim
                                    font.bold: alarming
                                    text: {
                                        const was = previousRow.was
                                        if (!was.key) return ""
                                        if (!was.ready) return "· fetching its jobs…"
                                        const parts = notes.slice(0, 3)
                                        // Two builds that ran to the end can be compared as a whole.
                                        if (was.notable && win.build.state === "success" && was.state === "success")
                                            parts.unshift("this build " + (was.delta > 0 ? "+" : "−") + Format.duration(Math.abs(was.delta)) + (was.delta > 0 ? " slower" : " faster"))
                                        if (parts.length === 0 && backend.jobs.length > 0) return "· no job stands out"
                                        return parts.length ? "· " + parts.join(", ") : ""
                                    }
                                }
                                Keycap {
                                    visible: backend.cameFrom !== ""
                                    Layout.maximumWidth: previousRow.width * 0.3
                                    clip: true
                                    key: "B"
                                    label: "back to " + backend.cameFrom
                                    onClicked: backend.selectCameFrom()
                                }
                                Keycap { key: "b"; label: "go to it"; onClicked: backend.selectPrevious() }
                            }
                        }
                    }
                }

                // The jobs, in time.
                Panel {
                    Layout.fillWidth: true
                    visible: !win.tall && backend.jobs.length > 0
                    title: "jobs"
                    note: win.jobsNote
                    focused: win.focusPanel === "jobs"
                    contentHeight: Math.min(jobsTimeline.implicitHeight, win.ch + 4 + 8.5 * jobsTimeline.rowHeight)

                    Flickable {
                        anchors.fill: parent
                        contentHeight: jobsTimeline.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: Scroll {}
                        JobsTimeline {
                            id: jobsTimeline
                            width: parent.width
                            jobs: backend.jobs
                            estimate: win.estimate
                            previous: backend.previous
                            selectedJobId: backend.selectedJobId
                            onPicked: id => { win.focusPanel = "jobs"; backend.selectJob(id) }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0

                    // The steps of the picked job. Each one that wrote
                    // something is a way into the log.
                    Panel {
                        Layout.preferredWidth: Math.round(Math.max(30, Math.min(38, win.width / win.cw * 0.2)) * win.cw)
                        Layout.fillHeight: true
                        title: "steps"
                        note: win.job.name || ""
                        corner: backend.steps.length ? backend.steps.length + "" : ""
                        focused: win.focusPanel === "steps"

                        Flickable {
                            anchors.fill: parent
                            contentHeight: stepColumn.implicitHeight
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: Scroll {}

                            Column {
                                id: stepColumn
                                width: parent.width

                                Repeater {
                                    model: backend.steps
                                    delegate: Rectangle {
                                        id: stepRow
                                        required property var modelData
                                        required property int index
                                        readonly property bool reachable: modelData.line >= 0 || modelData.pending
                                        readonly property bool current: backend.selectedStep >= 0 ? backend.selectedStep === index
                                                                                                  : logView.readingStep === index
                                        readonly property real textY: Math.round((height - win.ch) / 2)
                                        width: stepColumn.width
                                        height: win.rowHeight
                                        radius: Math.min(4, theme.radius)
                                        color: current ? theme.raised : stepMouse.containsMouse && reachable ? theme.hover : "transparent"

                                        Rectangle { width: Math.max(2, Math.round(win.cw / 3)); height: parent.height; color: theme.accent; visible: stepRow.current }
                                        StatusIcon { x: win.cw; y: stepRow.textY; state: modelData.state }
                                        Mono {
                                            x: win.cw * 3
                                            y: stepRow.textY
                                            width: parent.width - x - stepTime.implicitWidth - win.cw * 2
                                            text: modelData.name
                                            color: modelData.state === "failure" ? theme.bad
                                                 : modelData.state === "running" ? theme.accent
                                                 : modelData.state === "skipped" || modelData.state === "queued" ? theme.faint : theme.text
                                            font.bold: modelData.state === "failure"
                                            elide: Text.ElideRight
                                        }
                                        Mono {
                                            id: stepTime
                                            anchors.right: parent.right
                                            anchors.rightMargin: win.cw
                                            y: stepRow.textY
                                            color: modelData.state === "running" ? theme.accent : theme.dim
                                            text: {
                                                if (modelData.state === "running") {
                                                    win.now
                                                    return Format.duration(win.seconds(modelData.startedAt))
                                                         + (modelData.usual >= 2 ? "/~" + Format.duration(modelData.usual) : "")
                                                }
                                                return modelData.seconds >= 1 ? Format.duration(modelData.seconds) : ""
                                            }
                                        }
                                        MouseArea {
                                            id: stepMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: stepRow.reachable ? Qt.PointingHandCursor : Qt.ArrowCursor
                                            onClicked: win.jumpToStep(stepRow.index)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Panel {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        title: "log"
                        focused: win.focusPanel === "log"
                        note: {
                            if (!win.job.name) return ""
                            const step = backend.steps[backend.selectedStep >= 0 ? backend.selectedStep : logView.readingStep]
                            return win.job.name + (step ? " › " + step.name : "")
                        }
                        corner: logView.count ? logView.count + " lines" : ""

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 2

                            // What can be done with this job, each with its key.
                            Flow {
                                Layout.fillWidth: true
                                spacing: win.cw
                                Keycap { key: "c"; label: "copy link"; strong: true; onClicked: win.copyLink() }
                                Keycap {
                                    visible: backend.failures.length > 0
                                    key: "y"
                                    label: "copy failure"
                                    onClicked: win.copyFailure()
                                }
                                Keycap {
                                    visible: backend.failures.length > 0
                                    key: "e"
                                    label: backend.failures.length > 1 ? "next failure" : "go to failure"
                                    onClicked: win.jumpToFailure(win.failureIndex + 1)
                                }
                                Keycap { key: "o"; label: "open on github"; onClicked: win.openOnGitHub() }
                                Toggle { key: "w"; label: "wrap"; checked: win.wrap; onClicked: win.toggleWrap() }
                                Keycap { key: "z"; label: win.tall ? "show build" : "tall log"; onClicked: win.tall = !win.tall }
                                Keycap {
                                    visible: !logView.follow && win.job.state === "running"
                                    key: "end"
                                    label: "follow the log"
                                    strong: true
                                    onClicked: { logView.follow = true; backend.selectStep(-1); logView.toEnd(true) }
                                }
                            }

                            // What failed, by name, before any scrolling.
                            Rectangle {
                                Layout.fillWidth: true
                                visible: backend.failures.length > 0
                                implicitHeight: failureColumn.implicitHeight + 8
                                color: theme.badSoft
                                radius: Math.min(4, theme.radius)
                                Rectangle { width: Math.max(2, Math.round(win.cw / 3)); height: parent.height; color: theme.bad }
                                Column {
                                    id: failureColumn
                                    x: win.cw * 1.5
                                    y: 4
                                    width: parent.width - win.cw * 2.5
                                    Mono {
                                        text: {
                                            const count = backend.failures.length
                                            let step = ""
                                            for (const s of backend.steps) if (s.state === "failure" && !step) step = s.name
                                            return "✗ " + (count === 1 ? "1 failure" : count + " failures") + (step ? " in " + step : "")
                                        }
                                        color: theme.bad
                                        font.bold: true
                                    }
                                    Repeater {
                                        model: backend.failures.slice(0, 4)
                                        delegate: Rectangle {
                                            required property var modelData
                                            required property int index
                                            width: failureColumn.width
                                            height: win.ch + 4
                                            radius: Math.min(4, theme.radius)
                                            color: failureMouse.containsMouse || win.failureIndex === index
                                                   ? Qt.rgba(theme.bad.r, theme.bad.g, theme.bad.b, 0.18) : "transparent"
                                            Row {
                                                x: win.cw
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: win.cw * 2
                                                Mono {
                                                    visible: modelData.location !== ""
                                                    text: modelData.location
                                                    color: theme.bad
                                                }
                                                Mono {
                                                    width: Math.max(win.cw * 10, failureColumn.width - win.cw * 3 - (modelData.location !== "" ? x : 0))
                                                    text: modelData.title + (modelData.detail ? " · " + modelData.detail : "")
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            MouseArea {
                                                id: failureMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: win.jumpToFailure(index)
                                            }
                                        }
                                    }
                                    Mono {
                                        visible: backend.failures.length > 4
                                        x: win.cw
                                        text: "and " + (backend.failures.length - 4) + " more: [e] walks through all of them"
                                        color: theme.dim
                                    }
                                }
                            }

                            // The log, which follows the end while the job runs.
                            Item {
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                ListView {
                                    id: logView
                                    objectName: "log"
                                    // Following means staying at the bottom as lines arrive.
                                    // Only a scroll the user makes decides it, by wheel, drag
                                    // or scrollbar: scrolled to the end follows, scrolled up
                                    // stops. The view also moves by itself while it lays out
                                    // new lines, and that must not count as scrolling away.
                                    property bool follow: true
                                    property bool scripted: false
                                    property int highlighted: -1
                                    readonly property int lineHeight: Math.round(win.ch + 2)
                                    readonly property real timeWidth: win.cw * 10

                                    // The step whose lines are at the top of the view: the
                                    // one being read, which the step list marks.
                                    readonly property int readingStep: {
                                        backend.steps
                                        if (count === 0) return -1
                                        const top = indexAt(20, contentY + lineHeight * 1.6)
                                        return backend.log.stepAt(top < 0 ? 0 : top)
                                    }

                                    // Glides to the end rather than jumping, so new output
                                    // scrolls into view the way it would in a terminal. A
                                    // long way (a reload, a different job) goes at once.
                                    function toEnd(animated) {
                                        scripted = true
                                        const from = contentY
                                        positionViewAtEnd()
                                        const to = contentY
                                        scripted = false
                                        if (!animated || to <= from || to - from > height * 2) {
                                            followAnimation.stop()
                                            return
                                        }
                                        scripted = true
                                        contentY = from
                                        followAnimation.from = from
                                        followAnimation.to = to
                                        followAnimation.restart()
                                    }

                                    NumberAnimation {
                                        id: followAnimation
                                        target: logView
                                        property: "contentY"
                                        duration: 450
                                        easing.type: Easing.OutCubic
                                        onRunningChanged: logView.scripted = running
                                    }
                                    onMovementStarted: { followAnimation.stop(); backend.selectStep(-1); win.focusPanel = "log" }

                                    // New lines slide in from the right and fade up, and
                                    // the lines above them make room smoothly. A reload
                                    // or a different job appears at once.
                                    add: Transition {
                                        ParallelAnimation {
                                            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutQuad }
                                            NumberAnimation { property: "x"; from: 28; to: 0; duration: 420; easing.type: Easing.OutCubic }
                                        }
                                    }
                                    addDisplaced: Transition {
                                        NumberAnimation { property: "y"; duration: 300; easing.type: Easing.OutCubic }
                                    }
                                    // The step that is still running, as the next chapter:
                                    // GitHub publishes a step's log only once it is done,
                                    // and then it takes this place.
                                    footer: Item {
                                        width: logView.width
                                        visible: backend.logPending !== ""
                                        height: visible ? Math.round(logView.lineHeight * 2.5) + 4 : 0
                                        Item {
                                            width: parent.width
                                            height: Math.round(logView.lineHeight * 1.5)
                                            Rectangle {
                                                y: Math.round(parent.height / 2)
                                                width: parent.width - win.cw
                                                height: 1
                                                color: theme.line
                                            }
                                            Rectangle {
                                                x: logView.timeWidth - win.cw
                                                y: Math.round((parent.height - win.ch) / 2)
                                                width: pendingHeading.implicitWidth + win.cw * 2
                                                height: win.ch
                                                color: theme.background
                                                Row {
                                                    id: pendingHeading
                                                    x: win.cw
                                                    spacing: win.cw
                                                    StatusIcon { state: "running" }
                                                    Mono { text: backend.logPending; color: theme.accent; font.bold: true }
                                                }
                                            }
                                        }
                                        Mono {
                                            x: logView.timeWidth
                                            y: Math.round(logView.lineHeight * 1.5 + (logView.lineHeight - win.ch) / 2)
                                            width: logView.width - x - win.cw
                                            elide: Text.ElideRight
                                            text: "GitHub shows the log of a step once the step is done"
                                            color: theme.dim
                                        }
                                    }
                                    anchors.fill: parent
                                    clip: true
                                    model: backend.log
                                    boundsBehavior: Flickable.StopAtBounds
                                    reuseItems: true
                                    ScrollBar.vertical: Scroll { id: logBar }
                                    ScrollBar.horizontal: Scroll { quiet: true }
                                    flickableDirection: Flickable.AutoFlickIfNeeded
                                    contentWidth: win.wrap ? width : Math.max(width, win.cw * 300)

                                    readonly property bool nearEnd: contentY + height >= originY + contentHeight - 24
                                    onContentYChanged: if (!scripted && (moving || logBar.pressed)) follow = nearEnd
                                    onCountChanged: if (follow) Qt.callLater(toEnd, true)

                                    delegate: Item {
                                        id: logLine
                                        required property int index
                                        required property string time
                                        required property string text
                                        required property int kind
                                        required property string step
                                        required property int mark
                                        readonly property int headingHeight: step !== "" ? Math.round(logView.lineHeight * 1.5) : 0
                                        width: logView.contentWidth
                                        height: lineRow.height + headingHeight

                                        // Where a step begins: a rule with its name
                                        // in it, so the log reads as chapters.
                                        Item {
                                            visible: logLine.step !== ""
                                            width: logView.width
                                            height: logLine.headingHeight
                                            Rectangle {
                                                y: Math.round(parent.height / 2)
                                                width: parent.width - win.cw
                                                height: 1
                                                color: theme.line
                                            }
                                            Rectangle {
                                                x: logView.timeWidth - win.cw
                                                y: Math.round((parent.height - win.ch) / 2)
                                                width: headingText.implicitWidth + win.cw * 2
                                                height: win.ch
                                                color: theme.background
                                                Mono {
                                                    id: headingText
                                                    x: win.cw
                                                    text: logLine.step
                                                    color: theme.accent
                                                    font.bold: true
                                                }
                                            }
                                        }
                                        Rectangle {
                                            id: lineRow
                                            y: logLine.headingHeight
                                            width: parent.width
                                            height: win.wrap ? Math.max(logView.lineHeight, lineText.implicitHeight + logView.lineHeight - win.ch)
                                                             : logView.lineHeight
                                            color: logLine.index === logView.highlighted ? theme.accentSoft
                                                 : logLine.mark === 2 ? Qt.rgba(theme.bad.r, theme.bad.g, theme.bad.b, 0.22)
                                                 : logLine.mark === 1 ? Qt.rgba(theme.bad.r, theme.bad.g, theme.bad.b, 0.09)
                                                 : logLine.kind === 2 ? Qt.rgba(theme.bad.r, theme.bad.g, theme.bad.b, 0.12) : "transparent"
                                            Rectangle { width: Math.max(2, Math.round(win.cw / 3)); height: parent.height; color: theme.bad; visible: logLine.mark > 0 }
                                            Mono {
                                                x: win.cw
                                                y: Math.round((logView.lineHeight - win.ch) / 2)
                                                text: logLine.time
                                                color: theme.faint
                                            }
                                            Mono {
                                                id: lineText
                                                x: logView.timeWidth
                                                y: Math.round((logView.lineHeight - win.ch) / 2)
                                                width: win.wrap ? logView.width - x - win.cw : undefined
                                                wrapMode: win.wrap ? Text.WrapAtWordBoundaryOrAnywhere : Text.NoWrap
                                                text: (logLine.kind === 1 ? "▸ " : logLine.kind === 2 ? "error: " : logLine.kind === 3 ? "warning: " : "") + logLine.text
                                                color: logLine.mark === 2 ? theme.bad
                                                     : [theme.text, theme.accent, theme.bad, theme.warn, theme.info, theme.info][logLine.kind]
                                                font.bold: logLine.kind === 1 || logLine.kind === 2 || logLine.mark === 2
                                            }
                                        }
                                    }
                                }

                                Connections {
                                    target: backend.log
                                    function onModelAboutToBeReset() { logView.scripted = true }
                                    function onModelReset() {
                                        logView.scripted = false
                                        if (logView.follow) Qt.callLater(logView.toEnd)
                                    }
                                }

                                Connections {
                                    target: backend
                                    // The next step heading appears at the end, or moves on.
                                    function onLogStatusChanged() {
                                        if (logView.follow) Qt.callLater(logView.toEnd, true)
                                    }
                                    function onLogReset() {
                                        logView.highlighted = -1
                                        win.failureIndex = -1
                                        // A failed job opens on what failed; anything
                                        // else on its end, which is where a running
                                        // job is writing.
                                        if (backend.failures.length > 0 && win.job.state === "failure") {
                                            Qt.callLater(win.jumpToFailure, 0)
                                        } else {
                                            logView.follow = true
                                            Qt.callLater(logView.toEnd)
                                        }
                                    }
                                }

                                Mono {
                                    anchors.centerIn: parent
                                    width: parent.width * 0.7
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    visible: backend.logStatus !== "" && logView.count === 0 && backend.logPending === ""
                                    text: backend.logStatus
                                    color: theme.dim
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // A short confirmation for actions with no visible result, like copying.
    Rectangle {
        id: toast
        function show(message) { toastText.text = message; opacity = 1; toastTimer.restart() }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: win.ch * 2
        radius: theme.radius
        color: theme.background
        border.width: 1
        border.color: theme.accent
        opacity: 0
        visible: opacity > 0
        implicitWidth: Math.min(parent.width - win.cw * 6, toastText.implicitWidth + win.cw * 4)
        implicitHeight: win.ch * 2
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Mono {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - win.cw * 4)
            elide: Text.ElideMiddle
            color: theme.accent
            font.bold: true
        }
        Timer { id: toastTimer; interval: 2200; onTriggered: toast.opacity = 0 }
    }

    // Which repository, on g, and on its own when there is none yet.
    RepositoryMenu {
        open: win.repoMenuVisible || backend.repo === ""
        required: backend.repo === ""
        repositories: backend.repositories
        loading: backend.repositoriesLoading
        error: backend.repositoriesError
        current: backend.repo
        now: win.now
        onOpenChanged: if (open) backend.fetchRepositories()
        Component.onCompleted: if (open) backend.fetchRepositories()
        onPicked: name => {
            win.repoMenuVisible = false
            win.focusPanel = "builds"
            backend.show(name, "")
        }
        onClosed: win.repoMenuVisible = false
    }

    // Every shortcut, on ?.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.82)
        visible: win.helpVisible
        MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

        Rectangle {
            anchors.centerIn: parent
            width: helpFrame.width
            height: helpFrame.height
            color: theme.background
            radius: theme.radius
            Panel {
                id: helpFrame
                width: win.cw * 110
                title: "keys"
                corner: "esc closes"
                focused: true
                contentHeight: helpGrid.implicitHeight + win.ch
                Grid {
                    id: helpGrid
                    x: win.cw
                    y: Math.round(win.ch / 2)
                    columns: 2
                    columnSpacing: win.cw * 3
                    rowSpacing: Math.round(win.ch / 3)
                    flow: Grid.TopToBottom
                    rows: 12
                    Repeater {
                        model: [
                            ["j / ↓", "next build"], ["k / ↑", "previous build"],
                            ["l / → / tab", "next job"], ["h / ← / shift+tab", "previous job"],
                            ["n / p", "next, previous step"],
                            ["e / E", "next, previous failure"],
                            ["c", "copy the link of the job"],
                            ["y", "copy the failure and its link"],
                            ["C", "copy the whole log"],
                            ["o / enter", "open the job on github"],
                            ["b / B", "before on main, and back"],
                            ["z", "tall log, and back"],
                            ["w", "wrap long log lines, and back"],
                            ["m", "only main, and back"],
                            ["f", "only this branch, and back"],
                            ["a", "only running builds, and back"],
                            ["s", "show or hide skipped builds"],
                            ["pgup / pgdn", "scroll the log"],
                            ["home / end", "top of the log, follow the end"],
                            ["space / r", "refresh now"],
                            ["g", "watch another repository"],
                            ["esc", "close this, or the tall log"],
                            ["?", "show or hide this help"], ["q", "quit"]
                        ]
                        delegate: Row {
                            required property var modelData
                            Mono { width: win.cw * 19; text: modelData[0]; color: theme.accent }
                            Mono { width: win.cw * 32; text: modelData[1]; elide: Text.ElideRight }
                        }
                    }
                }
            }
        }
    }
}
