import bb.cascades 1.4

// The WhatsApp call screen (callClient, src/call/callclient.hpp). The call
// itself lives in the headless service, so hiding this sheet doesn't end it.
// Always dark, like a phone's call screen.
Sheet {
    id: callSheet
    peekEnabled: false

    function stateText() {
        var s = callClient.state;
        if (s == "incoming") return "Chiamata WhatsApp in arrivo";
        if (s == "outgoing") return "Chiamata WhatsApp…";
        if (s == "active") return callClient.connected ? callClient.elapsed : "Connessione…";
        if (s == "ended") return endedText(callClient.reason);
        return "";
    }
    function endedText(r) {
        if (r == "" || r == "user_ended") return "Chiamata terminata";
        if (r == "declined") return "Chiamata rifiutata";
        if (r == "busy") return "Occupato";
        if (r == "timeout") return "Nessuna risposta";
        if (r == "cancelled") return "Chiamata annullata";
        if (r == "do_not_disturb") return "Non disturbare";
        return "Chiamata terminata (" + r + ")";
    }

    content: Page {
        titleBar: TitleBar {
            title: "WhatsApp"
            dismissAction: ActionItem {
                title: "Nascondi"
                onTriggered: callSheet.close()
            }
        }
        // Stacked, not docked: on the Q10's 720x720 screen the buttons used to
        // cover the timer. The caller block takes whatever space is left.
        Container {
            background: Color.create("#0B141A")
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            layout: StackLayout { orientation: LayoutOrientation.TopToBottom }

            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                layout: DockLayout {}
                Container {
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                Container {
                    horizontalAlignment: HorizontalAlignment.Center
                    preferredWidth: ui.du(13)
                    preferredHeight: ui.du(13)
                    background: Color.create("#202C33")
                    layout: DockLayout {}
                    Label {
                        text: (callClient.name.length > 0 ? callClient.name : "?").charAt(0).toUpperCase()
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.fontSize: FontSize.XXLarge
                        textStyle.color: Color.create("#AEBAC1")
                    }
                }
                Label {
                    text: callClient.name.length > 0 ? callClient.name : callClient.phone
                    horizontalAlignment: HorizontalAlignment.Center
                    topMargin: ui.du(2)
                    textStyle.fontSize: FontSize.XLarge
                    textStyle.color: Color.White
                }
                Label {
                    text: callClient.name.length > 0 ? callClient.phone : ""
                    visible: text.length > 0
                    horizontalAlignment: HorizontalAlignment.Center
                    textStyle.color: Color.create("#8696A0")
                }
                Label {
                    // Re-evaluated every second through callClient.tick (elapsed).
                    text: callClient.elapsed.length >= 0 ? callSheet.stateText() : ""
                    horizontalAlignment: HorizontalAlignment.Center
                    topMargin: ui.du(1)
                    textStyle.fontSize: FontSize.Large
                    textStyle.color: Color.create("#8696A0")
                }
                }
            }

            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                leftPadding: ui.du(3)
                rightPadding: ui.du(3)
                bottomPadding: ui.du(3)

                // Ringing: decline / answer.
                Container {
                    visible: callClient.state == "incoming"
                    horizontalAlignment: HorizontalAlignment.Fill
                    layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                    Button {
                        text: "Rifiuta"
                        color: Color.create("#D32F2F")
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        onClicked: callClient.reject()
                    }
                    Button {
                        text: "Rispondi"
                        color: Color.create("#21BD5C")
                        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                        onClicked: callClient.accept()
                    }
                }

                // In a call (or calling): mute, speaker, hang up.
                Container {
                    visible: callClient.state == "outgoing" || callClient.state == "active"
                    horizontalAlignment: HorizontalAlignment.Fill
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                        Button {
                            text: callClient.muted ? "Riattiva microfono" : "Muto"
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            onClicked: callClient.setMuted(! callClient.muted)
                        }
                        Button {
                            text: callClient.speaker ? "Auricolare" : "Vivavoce"
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            onClicked: callClient.setSpeaker(! callClient.speaker)
                        }
                    }
                    Button {
                        text: "Termina"
                        color: Color.create("#D32F2F")
                        horizontalAlignment: HorizontalAlignment.Fill
                        topMargin: ui.du(2)
                        onClicked: callClient.hangup()
                    }
                }
            }
        }
    }
}
