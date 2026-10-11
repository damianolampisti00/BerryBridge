import bb.cascades 1.4

// Above the chat list while a call goes on and its screen is hidden
// ("Nascondi"): tap it to go back to the call, like the green bar of other
// phones. mainRef is main.qml's TabbedPane (openCallScreen()).
Container {
    id: banner
    property variant mainRef
    property bool inCall: callClient.state == "incoming" || callClient.state == "outgoing" || callClient.state == "active"

    visible: inCall
    horizontalAlignment: HorizontalAlignment.Fill
    background: Color.create("#1FA855")
    topPadding: ui.sdu(1.2)
    bottomPadding: ui.sdu(1.2)
    leftPadding: ui.sdu(2)
    rightPadding: ui.sdu(2)

    function who() {
        return callClient.name != "" ? callClient.name : callClient.phone;
    }

    Label {
        // Re-evaluated every second through callClient.tick (elapsed).
        text: "Chiamata in corso · " + banner.who()
              + (callClient.state == "active" && callClient.connected ? " · " + callClient.elapsed : "")
        textStyle.color: Color.White
        textStyle.fontWeight: FontWeight.Bold
    }
    Label {
        text: "Tocca per tornare alla chiamata"
        textStyle.color: Color.White
        textStyle.base: SystemDefaults.TextStyles.SubtitleText
    }

    gestureHandlers: TapHandler {
        onTapped: if (banner.mainRef) banner.mainRef.openCallScreen()
    }
}
