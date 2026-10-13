import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import RemoteControlDemo

ApplicationWindow {
    id: root
    required property DemoApp app
    width: 1100
    height: 720
    minimumWidth: 860
    minimumHeight: 560
    visible: false // Shown by main() once the graphics device is set.
    title: root.app.viewing ? "远程控制 Demo — " + root.app.peerName : "远程控制 Demo"
    color: theme.background
    // Phones and tablets: no hovering pointer, a soft keyboard, the screen always full.
    readonly property bool touchUi: Qt.platform.os === "ios" || Qt.platform.os === "android"
    readonly property bool android: Qt.platform.os === "android"
    // A phone, or a tablet held upright: one panel at a time.
    readonly property bool narrow: width < 760

    QtObject {
        id: theme
        readonly property color background: "#0b1220"
        readonly property color card: "#131c2e"
        readonly property color line: "#22304a"
        readonly property color text: "#e8eef8"
        readonly property color muted: "#8a9bb8"
        readonly property color accent: "#3b82f6"
        readonly property color good: "#22c55e"
        readonly property color warn: "#f59e0b"
        readonly property color bad: "#ef4444"
    }

    palette {
        window: theme.background
        windowText: theme.text
        base: "#0f1829"
        text: theme.text
        button: "#1c2840"
        buttonText: theme.text
        highlight: theme.accent
        highlightedText: "white"
        placeholderText: theme.muted
        mid: theme.line
        dark: theme.line
    }

    component Card: Rectangle {
        color: theme.card
        radius: 12
        border.color: theme.line
    }
    component Caption: Label {
        color: theme.muted
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }
    // A check box whose text wraps within its column.
    // The page shown: its tab lit, with the accent below it.
    component PageTab: TabButton {
        id: tab
        background: Rectangle {
            color: tab.checked ? theme.card : "transparent"
            radius: 8
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 3
                radius: 1.5
                color: tab.checked ? theme.accent : "transparent"
            }
        }
        contentItem: Label {
            text: tab.text
            color: tab.checked ? theme.text : theme.muted
            font.bold: tab.checked
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    component WrapCheckBox: CheckBox {
        id: box
        Layout.fillWidth: true
        contentItem: Label {
            text: box.text
            wrapMode: Text.Wrap
            color: theme.text
            verticalAlignment: Text.AlignVCenter
            leftPadding: box.indicator.width + box.spacing
        }
    }
    component Badge: Rectangle {
        property alias text: badgeLabel.text
        property color tone: theme.accent
        implicitWidth: badgeLabel.implicitWidth + 14
        implicitHeight: 20
        radius: 10
        color: Qt.rgba(tone.r, tone.g, tone.b, 0.18)
        Label {
            id: badgeLabel
            anchors.centerIn: parent
            color: parent.tone
            font.pixelSize: 11
            font.bold: true
        }
    }

    // ---- Home ------------------------------------------------------------------

    Item {
        id: home
        anchors.fill: parent
        visible: !root.app.viewing

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.narrow ? 12 : 24
            spacing: root.narrow ? 10 : 18

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: "远程控制 Demo"
                    font.pixelSize: root.narrow ? 20 : 24
                    font.bold: true
                    color: theme.text
                }
                Caption {
                    Layout.fillWidth: true
                    text: "局域网内互相发现、配对码连接 · Remote Control SDK " + root.app.sdkVersion
                }
            }
            Rectangle {
                visible: root.app.viewerError.length > 0
                Layout.fillWidth: true
                color: Qt.rgba(0.94, 0.27, 0.27, 0.15)
                radius: 8
                implicitHeight: errorText.implicitHeight + 16
                Label {
                    id: errorText
                    anchors.fill: parent
                    anchors.margins: 8
                    text: root.app.viewerError
                    color: theme.bad
                    wrapMode: Text.Wrap
                }
            }

            TabBar {
                id: tabs
                visible: root.narrow
                Layout.fillWidth: true
                background: Item {}
                PageTab { text: "本机" }
                PageTab { text: "附近的设备" }
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: root.narrow ? 1 : 2
                columnSpacing: 18
                rowSpacing: 18

                // This machine
                Card {
                    visible: !root.narrow || tabs.currentIndex === 0
                    Layout.preferredWidth: root.narrow ? -1 : Math.min(460, Math.max(380, root.width * 0.36))
                    Layout.fillWidth: root.narrow
                    Layout.fillHeight: true

                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: root.narrow ? 14 : 20
                        contentWidth: availableWidth
                        clip: true

                        ColumnLayout {
                            width: parent.width
                            spacing: 12

                            Label { text: "本机"; font.pixelSize: 16; font.bold: true; color: theme.text }
                            ColumnLayout {
                                spacing: 2
                                Label { text: root.app.deviceName; color: theme.text; font.pixelSize: 15 }
                                Caption { text: root.app.platform }
                                Caption { text: root.app.addresses; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            }

                            Rectangle { Layout.fillWidth: true; height: 1; color: theme.line }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    spacing: 2
                                    Layout.fillWidth: true
                                    Label { text: "允许远程连接"; color: theme.text }
                                    Caption {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        text: root.app.appSharing
                                              ? "开启后，别人可以用配对码看到这个 App 的画面（iOS 只能共享 App 自己）"
                                              : root.android
                                              ? "开启后，别人可以用配对码看到这台设备的整个屏幕（系统会先问你）"
                                              : "开启后，别人可以用配对码看到并控制这台电脑"
                                    }
                                }
                                Switch {
                                    checked: root.app.accepting
                                    onToggled: root.app.accepting = checked
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                visible: root.app.accepting
                                color: "#0f1829"
                                radius: 10
                                implicitHeight: codeColumn.implicitHeight + 24
                                ColumnLayout {
                                    id: codeColumn
                                    anchors.centerIn: parent
                                    spacing: 6
                                    Caption { text: "配对码"; Layout.alignment: Qt.AlignHCenter }
                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: root.app.pairingCode.replace(/(\d{3})(\d{3})/, "$1 $2")
                                        font.pixelSize: 36
                                        font.bold: true
                                        font.letterSpacing: 4
                                        color: theme.text
                                    }
                                    Button {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "换一个"
                                        flat: true
                                        enabled: !root.app.hostConnected && !root.app.sharingScreen
                                        onClicked: root.app.newPairingCode()
                                    }
                                }
                            }

                            // Android: the user said no, or stopped sharing from the status bar.
                            Button {
                                visible: root.android && root.app.accepting && !root.app.sharingScreen
                                Layout.fillWidth: true
                                text: "共享屏幕"
                                highlighted: true
                                onClicked: root.app.shareWholeScreen()
                            }

                            // iOS: the whole screen, other apps too, through the broadcast extension.
                            ColumnLayout {
                                visible: root.app.appSharing && root.app.accepting
                                Layout.fillWidth: true
                                spacing: 4
                                Button {
                                    Layout.fillWidth: true
                                    text: root.app.sharingScreen ? "停止共享整个屏幕" : "共享整个屏幕"
                                    highlighted: !root.app.sharingScreen
                                    onClicked: root.app.shareWholeScreen()
                                }
                                Caption {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    text: "离开本 App 也在共享；对方用同一个配对码连接，只能看、不能控制"
                                }
                            }

                            RowLayout {
                                visible: root.app.accepting
                                spacing: 8
                                Rectangle {
                                    width: 8; height: 8; radius: 4
                                    color: root.app.hostConnected ? theme.good
                                         : root.app.hostStatus.startsWith("出错") ? theme.bad : theme.warn
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: root.app.hostStatus
                                    color: theme.text
                                    wrapMode: Text.Wrap
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    spacing: 2
                                    Layout.fillWidth: true
                                    Label {
                                        text: root.app.appSharing || root.android ? "允许对方点按和输入" : "允许对方控制键鼠"
                                        color: theme.text
                                    }
                                    Caption {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        text: root.app.appSharing ? "只作用于这个 App；关闭时对方只能观看"
                                              : root.android ? "经本 Demo 的无障碍服务执行；关闭时对方只能观看"
                                              : "关闭时对方只能观看"
                                    }
                                }
                                Switch {
                                    checked: root.app.allowControl
                                    onToggled: root.app.allowControl = checked
                                }
                            }

                            // macOS asks once per application for these.
                            Rectangle {
                                Layout.fillWidth: true
                                visible: !root.app.screenPermission || (root.app.allowControl && !root.app.inputPermission)
                                color: Qt.rgba(0.96, 0.62, 0.04, 0.12)
                                radius: 8
                                implicitHeight: permissionColumn.implicitHeight + 20
                                ColumnLayout {
                                    id: permissionColumn
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 6
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        color: theme.warn
                                        text: root.android
                                              ? "被控制需要在“设置 → 无障碍”里打开“远程控制 Demo：对方的操作”。"
                                              : (!root.app.screenPermission ? "共享屏幕需要“屏幕与系统音频录制”权限。" : "")
                                                + (root.app.allowControl && !root.app.inputPermission
                                                   ? "被控制需要“辅助功能”权限。" : "")
                                                + "授权后可能需要重新打开本程序。"
                                    }
                                    Button { text: root.android ? "去设置" : "去授权"; onClicked: root.app.requestPermissions() }
                                }
                            }

                            Rectangle { Layout.fillWidth: true; height: 1; color: theme.line }

                            Label { text: "本机分享出去的画面"; color: theme.text; font.bold: true }
                            Caption {
                                Layout.topMargin: -8
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: root.app.followViewer
                                      ? "由本机决定：对方请求了的分辨率、帧率、码率按对方的（不超过本机屏幕和这里的码率上限），对方选“默认”的按这里；连接中修改立即生效"
                                      : "由本机决定：一律按这里，对方的请求不生效；连接中修改立即生效"
                            }
                            Label { // What actually leaves, whoever asked for it.
                                visible: root.app.hostConnected && root.app.host.resolutionText.length > 0
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: theme.text
                                text: "正在发送：" + root.app.host.codecText + " " + root.app.host.resolutionText + " · "
                                      + root.app.host.framesPerSecondNow + " FPS · " + root.app.host.bitrateMbpsNow.toFixed(1) + " Mbit/s"
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 10
                                rowSpacing: 8
                                Caption { text: "分辨率" }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: root.app.resolutions()
                                    currentIndex: root.app.resolution
                                    onActivated: root.app.resolution = currentIndex
                                }
                                Caption { text: "帧率" }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: [30, 60, 120]
                                    currentIndex: model.indexOf(root.app.framesPerSecond)
                                    displayText: currentValue + " FPS"
                                    onActivated: root.app.framesPerSecond = currentValue
                                }
                                Caption { text: "码率上限" }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: [5, 10, 20, 40, 80]
                                    currentIndex: model.indexOf(root.app.bitrateMbps)
                                    displayText: currentValue + " Mbit/s"
                                    onActivated: root.app.bitrateMbps = currentValue
                                }
                                Caption { text: "画面编码" }
                                ComboBox { // Only what this machine's hardware encodes; the viewer must decode it too.
                                    Layout.fillWidth: true
                                    model: root.app.videoCodecs()
                                    currentIndex: root.app.videoCodec
                                    displayText: currentText + " 优先"
                                    onActivated: root.app.videoCodec = currentIndex
                                }
                                Caption { text: "声音编码" }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: root.app.audioCodecs()
                                    currentIndex: root.app.audioCodec
                                    displayText: currentText + " 优先"
                                    onActivated: root.app.audioCodec = currentIndex
                                }
                                Caption { text: "传输" }
                                ComboBox { // Viewers that find this machine use the same; by IP they choose.
                                    Layout.fillWidth: true
                                    model: ["UDP（自适应码率）", "TCP（固定码率）"]
                                    currentIndex: root.app.tcp ? 1 : 0
                                    onActivated: root.app.tcp = currentIndex === 1
                                }
                            }
                            WrapCheckBox {
                                text: "按观看端请求的画质发送（不超过本机屏幕）"
                                checked: root.app.followViewer
                                onToggled: root.app.followViewer = checked
                            }
                            WrapCheckBox {
                                text: root.app.appSharing ? "把麦克风的声音传给对方（回声消除）"
                                      : root.android ? "共享设备播放的声音" : "共享本机声音"
                                checked: root.app.shareSound
                                onToggled: root.app.shareSound = checked
                            }
                            WrapCheckBox {
                                text: "播放对方麦克风的声音"
                                checked: root.app.hearViewers
                                onToggled: root.app.hearViewers = checked
                            }
                            WrapCheckBox {
                                text: root.android ? "连接期间同步剪贴板（文字、HTML，两个方向）"
                                      : "连接期间同步剪贴板（文字、HTML、RTF、图片，两个方向）"
                                checked: root.app.syncClipboard
                                onToggled: root.app.syncClipboard = checked
                            }
                        }
                    }
                }

                // Machines on the network
                Card {
                    visible: !root.narrow || tabs.currentIndex === 1
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: root.narrow ? 14 : 20
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: "附近的设备"; font.pixelSize: 16; font.bold: true; color: theme.text }
                            Badge { text: root.app.devices.count; tone: theme.muted }
                            Item { Layout.fillWidth: true }
                            Button {
                                text: "按 IP 连接"
                                flat: true
                                onClicked: connectDialog.openFor("", "", 0, false)
                            }
                        }

                        ListView {
                            id: deviceList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 10
                            model: root.app.devices

                            delegate: Rectangle {
                                required property string name
                                required property string platform
                                required property string address
                                required property int port
                                required property bool accepting
                                required property string version
                                required property bool tcp
                                width: deviceList.width
                                height: 72
                                radius: 10
                                color: rowHover.hovered ? "#1a2740" : "#0f1829"
                                border.color: theme.line
                                HoverHandler { id: rowHover }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 14
                                    Rectangle {
                                        width: 44; height: 44; radius: 10
                                        color: "#1c2840"
                                        Label {
                                            anchors.centerIn: parent
                                            text: platform === "Windows" ? "Win" : platform === "macOS" ? "Mac"
                                                  : platform === "iOS" ? "iOS" : platform === "Android" ? "And" : "?"
                                            color: theme.text
                                            font.bold: true
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Label {
                                                Layout.fillWidth: true
                                                text: name
                                                color: theme.text
                                                font.pixelSize: 15
                                                elide: Text.ElideRight
                                            }
                                            Badge {
                                                text: accepting ? "可连接" : "未开放"
                                                tone: accepting ? theme.good : theme.muted
                                            }
                                        }
                                        Caption {
                                            Layout.fillWidth: true
                                            wrapMode: Text.NoWrap
                                            elide: Text.ElideRight
                                            text: platform + " · " + address + " · " + (tcp ? "TCP" : "UDP")
                                                  + (version.length ? " · SDK " + version : "")
                                        }
                                    }
                                    Button {
                                        text: "连接"
                                        enabled: accepting
                                        highlighted: accepting
                                        onClicked: connectDialog.openFor(name, address, port, tcp)
                                    }
                                }
                            }

                            ColumnLayout {
                                anchors.centerIn: parent
                                visible: deviceList.count === 0
                                spacing: 8
                                BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: visible }
                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "正在搜索局域网中的设备…"
                                    color: theme.text
                                }
                                Caption {
                                    Layout.preferredWidth: deviceList.width - 24
                                    horizontalAlignment: Text.AlignHCenter
                                    text: "对方也打开本 Demo 后会出现在这里；连接前对方需开启“允许远程连接”"
                                }
                                Caption { // UDP broadcast needs Apple's multicast entitlement there.
                                    Layout.preferredWidth: deviceList.width - 24
                                    horizontalAlignment: Text.AlignHCenter
                                    visible: Qt.platform.os === "ios"
                                    text: "iPhone / iPad 真机收不到局域网广播，请用“按 IP 连接”"
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: connectDialog
        property string targetName: ""
        property int targetPort: 0
        function openFor(name, address, port, tcp) {
            targetName = name
            targetPort = port > 0 ? port : 47990
            transportBox.currentIndex = tcp ? 1 : 0
            addressField.text = address
            codeField.text = ""
            open()
            (address.length ? codeField : addressField).forceActiveFocus()
        }
        anchors.centerIn: parent
        modal: true
        width: Math.min(380, root.width - 24)
        title: targetName.length ? "连接到 " + targetName : "按 IP 连接"
        standardButtons: Dialog.Cancel
        background: Rectangle { color: theme.card; radius: 12; border.color: theme.line }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Caption { text: "对方的 IP 地址"; visible: connectDialog.targetName.length === 0 }
            TextField {
                id: addressField
                Layout.fillWidth: true
                visible: connectDialog.targetName.length === 0
                placeholderText: "例如 192.168.1.20，或 192.168.1.20:47995"
            }
            Caption { text: "传输（与对方一致）"; visible: connectDialog.targetName.length === 0 }
            ComboBox { // A discovered host announces its transport.
                id: transportBox
                Layout.fillWidth: true
                visible: connectDialog.targetName.length === 0
                model: ["UDP", "TCP"]
            }
            Caption { text: "对方屏幕上显示的 6 位配对码" }
            TextField {
                id: codeField
                Layout.fillWidth: true
                placeholderText: "000000"
                font.pixelSize: 22
                font.letterSpacing: 3
                horizontalAlignment: Text.AlignHCenter
                inputMethodHints: Qt.ImhDigitsOnly
                validator: RegularExpressionValidator { regularExpression: /\d{0,6}/ }
                onAccepted: connectButton.clicked()
            }
            Caption {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "向对方请求的画质：对方开着“按观看端请求的画质发送”时生效，“默认”就是按对方的设置；观看中也可以改"
            }
            ViewQuality { Layout.fillWidth: true }
            WrapCheckBox {
                text: "接收对方的声音"
                checked: root.app.receiveSound
                onToggled: root.app.receiveSound = checked
            }
            Button {
                id: connectButton
                Layout.fillWidth: true
                text: "连接"
                highlighted: true
                enabled: codeField.text.length === 6 && addressField.text.length > 0
                onClicked: {
                    // "address:port" for a host on another port (IPv4: one colon).
                    const text = addressField.text.trim()
                    const colon = text.lastIndexOf(":")
                    const withPort = colon > 0 && text.indexOf(":") === colon && Number(text.slice(colon + 1)) > 0
                    root.app.connectTo(withPort ? text.slice(0, colon) : text,
                                       withPort ? Number(text.slice(colon + 1)) : connectDialog.targetPort,
                                       connectDialog.targetName, codeField.text, transportBox.currentIndex === 1)
                    connectDialog.close()
                }
            }
        }
    }

    // ---- Viewer ------------------------------------------------------------------

    Rectangle {
        id: viewerPage
        anchors.fill: parent
        visible: root.app.viewing
        color: "black"
        property bool showStats: false

        onVisibleChanged: if (visible) toolbar.reveal()

        Timer { // --log: what the screen shows, beside the decoder's rate printed by main().
            running: viewerPage.visible && Qt.application.arguments.indexOf("--log") >= 0
            interval: 1000
            repeat: true
            onTriggered: console.log("rendered " + video.renderedFps.toFixed(1) + " FPS")
        }

        RemoteVideo {
            id: video
            anchors.fill: parent
            session: root.app.viewer
            fillMode: RemoteVideo.Fit
            inputEnabled: viewerPage.visible && controlSwitch.checked && root.app.viewer.controlGranted
            focus: inputEnabled
            touchMode: trackpadSwitch.checked ? RemoteVideo.Trackpad : RemoteVideo.Direct
        }

        Rectangle { // The sharer paused: its last picture stays.
            visible: root.app.viewer.peerPaused
            anchors.centerIn: parent
            width: pausedText.implicitWidth + 40
            height: 52
            radius: 12
            color: "#e6131c2e"
            border.color: theme.line
            Label {
                id: pausedText
                anchors.centerIn: parent
                text: root.app.peerName + " 暂停了共享"
                color: theme.text
                font.pixelSize: 18
            }
        }

        Rectangle { // What the keyboard's input method is composing, before it types it there.
            visible: video.composing.length > 0
            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 70 }
            width: composingText.implicitWidth + 28
            height: 40
            radius: 8
            color: "#e6131c2e"
            border.color: theme.accent
            Label { id: composingText; anchors.centerIn: parent; text: video.composing; color: theme.text; font.pixelSize: 18 }
        }

        ColumnLayout {
            anchors.centerIn: parent
            visible: !video.hasFrame
            spacing: 10
            BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: parent.visible }
            Label {
                Layout.alignment: Qt.AlignHCenter
                color: theme.text
                text: video.frameError.length ? video.frameError
                      : root.app.viewer.reconnecting ? "连接中断，正在重新连接 " + root.app.peerName + " …"
                      : root.app.viewer.running ? "正在连接 " + root.app.peerName + " …" : root.app.viewer.stateText
            }
        }

        // Shown near the top edge, or briefly after connecting; on a touch
        // screen the handle at the top keeps it out.
        Item {
            id: toolbar
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 64
            property bool pinned: false
            property bool shown: hover.hovered || hideTimer.running || !video.hasFrame || pinned
            function reveal() { hideTimer.restart() }
            HoverHandler { id: hover }
            Timer { id: hideTimer; interval: 3000 }

            Rectangle {
                visible: root.touchUi && !toolbar.shown
                anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 4 }
                width: 64
                height: 24
                radius: 12
                color: "#99131c2e"
                border.color: theme.line
                Label { anchors.centerIn: parent; text: "︾"; color: theme.text }
                TapHandler { // Its own: the picture below takes no touch that starts here.
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: toolbar.pinned = true
                }
            }

            Rectangle {
                anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 10 }
                width: Math.min(bar.implicitWidth + 24, toolbar.width - 16) // Narrower than its buttons: they scroll.
                height: 44
                radius: 22
                color: "#e6131c2e"
                border.color: theme.line
                opacity: toolbar.shown ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 180 } }

                Flickable {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    contentWidth: bar.implicitWidth
                    contentHeight: height
                    flickableDirection: Flickable.HorizontalFlick
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                RowLayout {
                    id: bar
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Button { text: "断开"; onClicked: root.app.disconnectViewer() }
                    Label {
                        color: theme.text
                        visible: !root.touchUi || root.width > 1000
                        text: root.app.peerName + (root.app.viewer.resolutionText.length
                              ? " · " + root.app.viewer.codecText + " " + root.app.viewer.resolutionText : "")
                    }
                    Rectangle { width: 1; height: 22; color: theme.line }
                    Caption { text: "请求"; visible: !root.touchUi }
                    ViewQuality { compact: true; visible: !root.touchUi }
                    Button { // A touch screen has no room for the four lists in the bar.
                        visible: root.touchUi
                        text: "画质"
                        flat: true
                        onClicked: qualityPopup.open()
                    }
                    Rectangle { width: 1; height: 22; color: theme.line }
                    Switch {
                        id: controlSwitch
                        text: root.app.viewer.controlGranted ? "控制" : "对方未允许控制"
                        checked: true
                        enabled: root.app.viewer.controlGranted
                    }
                    Switch {
                        id: trackpadSwitch
                        visible: root.touchUi
                        text: "触控板"
                        enabled: video.inputEnabled
                    }
                    Button {
                        visible: root.touchUi
                        text: video.softKeyboard ? "收起键盘" : "键盘"
                        flat: true
                        enabled: video.inputEnabled
                        onClicked: video.softKeyboard = !video.softKeyboard
                    }
                    Switch {
                        text: "麦克风"
                        checked: root.app.sendMicrophone
                        onToggled: root.app.sendMicrophone = checked
                    }
                    Button {
                        text: viewerPage.showStats ? "隐藏统计" : "统计"
                        flat: true
                        onClicked: viewerPage.showStats = !viewerPage.showStats
                    }
                    Button {
                        visible: !root.touchUi
                        text: root.visibility === Window.FullScreen ? "退出全屏" : "全屏"
                        flat: true
                        onClicked: root.visibility = root.visibility === Window.FullScreen ? Window.Windowed
                                                                                            : Window.FullScreen
                    }
                    Button {
                        visible: root.touchUi
                        text: "收起"
                        flat: true
                        onClicked: { toolbar.pinned = false; hideTimer.stop() }
                    }
                }
                }
            }
        }

        Popup {
            id: qualityPopup
            anchors.centerIn: parent
            modal: true
            padding: 18
            background: Rectangle { color: theme.card; radius: 12; border.color: theme.line }
            ColumnLayout {
                spacing: 10
                Caption {
                    Layout.preferredWidth: Math.min(640, root.width - 80)
                    wrapMode: Text.Wrap
                    text: "向对方请求的画质：对方开着“按观看端请求的画质发送”时生效，“默认”就是按对方的设置"
                }
                ViewQuality { Layout.preferredWidth: Math.min(640, root.width - 80) }
                Label {
                    Layout.preferredWidth: Math.min(640, root.width - 80)
                    wrapMode: Text.Wrap
                    color: theme.text
                    text: "实际收到：" + root.app.viewer.codecText + " " + root.app.viewer.resolutionText + " · "
                          + root.app.viewer.framesPerSecondNow + " FPS · " + root.app.viewer.bitrateMbpsNow.toFixed(1) + " Mbit/s（对方决定）"
                }
            }
        }

        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom; margins: 14 }
            visible: viewerPage.showStats
            width: statsGrid.implicitWidth + 24
            height: statsGrid.implicitHeight + 20
            radius: 10
            color: "#d90b1220"
            border.color: theme.line
            GridLayout {
                id: statsGrid
                anchors.centerIn: parent
                columns: 2
                columnSpacing: 14
                rowSpacing: 4
                Caption { text: "画面" }
                Label {
                    color: theme.text
                    text: video.renderedFps.toFixed(0) + " FPS 显示 · " + root.app.viewer.framesPerSecondNow + " FPS 解码"
                }
                Caption { text: "编码" }
                Label { color: theme.text; text: root.app.viewer.codecText + " " + root.app.viewer.resolutionText }
                Caption { text: "往返" }
                Label { color: theme.text; text: root.app.viewer.roundTripMs.toFixed(1) + " ms" }
                Caption { text: "码率" }
                Label { color: theme.text; text: root.app.viewer.bitrateMbpsNow.toFixed(1) + " Mbit/s" }
                Caption { text: "声音" }
                Label {
                    color: theme.text
                    text: root.app.viewer.audioActive ? root.app.viewer.audioCodecText + " · 缓冲 " + root.app.viewer.audioBufferMs + " ms" : "关闭"
                }
                Caption { text: "路径" }
                Label { color: theme.text; text: root.app.viewer.pathText.length ? root.app.viewer.pathText : "TCP" }
            }
        }
    }

    // The stream the viewer asks for: resolution, frame rate and bitrate.
    // On a phone two to a row.
    component ViewQuality: GridLayout {
        property bool compact: false
        columns: !compact && root.narrow ? 2 : 4
        columnSpacing: 6
        rowSpacing: 6
        ComboBox {
            Layout.fillWidth: !parent.compact
            implicitWidth: parent.compact ? 150 : 0
            model: root.app.viewResolutions()
            currentIndex: root.app.viewResolution
            onActivated: root.app.viewResolution = currentIndex
        }
        ComboBox {
            Layout.fillWidth: !parent.compact && root.narrow
            implicitWidth: 104
            model: [0, 30, 60, 120, 144]
            currentIndex: Math.max(0, model.indexOf(root.app.viewFramesPerSecond))
            displayText: currentValue ? currentValue + " FPS" : "帧率默认"
            onActivated: root.app.viewFramesPerSecond = currentValue
        }
        ComboBox {
            Layout.fillWidth: !parent.compact && root.narrow
            implicitWidth: parent.compact ? 150 : 170
            model: root.app.viewAudioCodecs()
            currentIndex: root.app.viewAudioCodec
            onActivated: root.app.viewAudioCodec = currentIndex
        }
        ComboBox {
            Layout.fillWidth: !parent.compact && root.narrow
            implicitWidth: 120
            model: [0, 10, 20, 40, 80, 150]
            currentIndex: Math.max(0, model.indexOf(root.app.viewBitrateMbps))
            displayText: currentValue ? currentValue + " Mbit/s" : "码率默认"
            onActivated: root.app.viewBitrateMbps = currentValue
        }
    }
}
