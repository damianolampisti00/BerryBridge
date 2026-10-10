import bb.cascades 1.4

// Active Frame: one section per enabled account with its unread messages
// (dat.getUnreadSummary: archived chats left out; muted ones counted, and
// said). Refreshed on every incoming message (app.dbUpdateTrigger), when a
// chat is read (dat.dataRefreshRequested) and when the app is minimized.
Container {
    id: cover
    background: Color.create(app.colors.chatBg)
    leftPadding: 16
    rightPadding: 16
    topPadding: 12
    // The system's own bar (app name + close) covers the bottom of the frame.
    bottomPadding: 72

    function iconFor(accountID) {
        var net = accountID ? accountID.toLowerCase() : "";
        var known = ["whatsapp", "telegram", "instagram", "signal", "facebook", "discord", "slack",
                     "linkedin", "matrix", "line", "tumblr", "googlechat"];
        for (var i = 0; i < known.length; i++) {
            if (net.indexOf(known[i]) !== -1) return "asset:///images/" + known[i] + ".png";
        }
        if (net.indexOf("twitter") !== -1) return "asset:///images/x.png";
        if (net.indexOf("gmessages") !== -1 || net.indexOf("googlemessages") !== -1 || net.indexOf("sms") !== -1)
            return "asset:///images/googlemessages.png";
        return "asset:///images/icon.png";
    }

    function refresh() {
        var list = dat.getUnreadSummary();
        while (rows.count() > 0) {
            var old = rows.at(0);
            rows.remove(old);
            old.destroy();
        }
        for (var i = 0; i < list.length; i++) {
            var a = list[i];
            var row = rowDef.createObject();
            row.icon = iconFor(a.accountID);
            row.name = a.network;
            row.unread = a.unread;
            row.chats = a.chats;
            row.muted = a.muted;
            rows.add(row);
        }
    }

    Container {
        id: rows
    }

    attachedObjects: [
        ComponentDefinition {
            id: rowDef
            Container {
                property string icon
                property string name
                property int unread
                property int chats
                property int muted
                layout: StackLayout { orientation: LayoutOrientation.LeftToRight }
                topPadding: 6
                bottomPadding: 6
                ImageView {
                    imageSource: icon
                    preferredWidth: 48
                    preferredHeight: 48
                    scalingMethod: ScalingMethod.AspectFit
                    verticalAlignment: VerticalAlignment.Center
                }
                Container {
                    leftPadding: 12
                    verticalAlignment: VerticalAlignment.Center
                    layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                    Label {
                        text: name
                        textStyle.fontSize: FontSize.XSmall
                        textStyle.color: Color.create(app.colors.text)
                    }
                    Label {
                        text: unread == 0 ? "No unread messages"
                              : (chats == 1 ? "in 1 chat" : "in " + chats + " chats")
                                + (muted > 0 ? ", " + muted + " muted" : "")
                        textStyle.fontSize: FontSize.XXSmall
                        textStyle.color: Color.create(app.colors.muted)
                    }
                }
                Label {
                    text: String(unread)
                    verticalAlignment: VerticalAlignment.Center
                    textStyle.fontSize: FontSize.Large
                    textStyle.fontWeight: unread > 0 ? FontWeight.Bold : FontWeight.Normal
                    textStyle.color: unread > 0 ? Color.create(app.colors.text) : Color.create(app.colors.muted)
                }
            }
        }
    ]

    onCreationCompleted: {
        refresh();
        app.dbUpdateTriggerChanged.connect(cover.refresh);
        app.darkThemeChanged.connect(cover.refresh);
        dat.dataRefreshRequested.connect(cover.refresh);
        Application.thumbnail.connect(cover.refresh);
    }
}
