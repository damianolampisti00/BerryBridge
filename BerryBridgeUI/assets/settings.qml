import bb.cascades 1.4
import bb.system 1.2

Page {
    id: settingsPage
    property TabbedPane mainRef
    
    titleBar: TitleBar {
        title: "Settings"
        scrollBehavior: TitleBarScrollBehavior.Sticky
    }
    
    ScrollView {
        content: Container {
            layout: StackLayout {
                orientation: LayoutOrientation.TopToBottom
            }
            leftPadding: ui.du(3.0)
            rightPadding: ui.du(3.0)
            topPadding: ui.du(3.0)
            bottomPadding: ui.du(3.0)
            
            
            
            // --- 2. DARK MODE SETTING ---
            Container {
                layout: StackLayout {
                    orientation: LayoutOrientation.LeftToRight
                }
                horizontalAlignment: HorizontalAlignment.Fill
                
                Container {
                    layoutProperties: StackLayoutProperties {
                        spaceQuota: 1.0
                    }
                    verticalAlignment: VerticalAlignment.Center
                    
                    Label {
                        text: "Dark Mode"
                        textStyle.fontWeight: FontWeight.W600
                        textStyle.fontSize: FontSize.Medium
                    }
                    Label {
                        text: "Enables dark theme across the application."
                        textStyle.fontSize: FontSize.XSmall
                        textStyle.color: Color.DarkGray
                        multiline: true
                    }
                }
                
                // Applied right away, no restart: see ApplicationUI::setDarkTheme.
                ToggleButton {
                    id: darkModeToggle
                    verticalAlignment: VerticalAlignment.Center
                    checked: app.darkMode
                    onCheckedChanged: app.darkMode = checked
                }

            }

            Divider {

            }

            // WhatsApp calls: the Berry Bridge gateway of WaCalls
            // (wss://.../ws) and its token. Saved, then the service reconnects.
            Label {
                text: "Calls server"
                textStyle.fontWeight: FontWeight.W600
                textStyle.fontSize: FontSize.Medium
            }
            TextField {
                id: callServerField
                hintText: "wss://calls.example.com/ws"
                text: app.getSetting("callServerUrl", "")
                inputMode: TextFieldInputMode.Url
                onTextChanged: {
                    app.updateSetting("callServerUrl", text);
                    callClient.reloadConfig();
                }
            }
            Label {
                text: "Calls token"
                textStyle.fontWeight: FontWeight.W600
                textStyle.fontSize: FontSize.Medium
            }
            TextField {
                id: callTokenField
                text: app.getSetting("callToken", "")
                inputMode: TextFieldInputMode.Password
                onTextChanged: {
                    app.updateSetting("callToken", text);
                    callClient.reloadConfig();
                }
            }
            Label {
                text: "Calls: " + callClient.link
                textStyle.color: Color.create(app.colors.muted)
                textStyle.fontSize: FontSize.Small
            }

            Divider {

            }
            
            // --- 1. TRACKPAD FOCUS OVERLAY SETTING ---
            Container {
                visible: app.isClassic
                layout: StackLayout {
                    orientation: LayoutOrientation.LeftToRight
                }
                horizontalAlignment: HorizontalAlignment.Fill
                bottomMargin: ui.du(4.0)
                
                Container {
                    layoutProperties: StackLayoutProperties {
                        spaceQuota: 1.0
                    }
                    verticalAlignment: VerticalAlignment.Center
                    
                    Label {
                        text: "Disable Trackpad Focus Overlay"
                        textStyle.fontWeight: FontWeight.W600
                        textStyle.fontSize: FontSize.Medium
                        multiline: true
                    }
                    Label {
                        text: "Disables the system highlight overlay when navigating messages on BlackBerry Classic devices."
                        textStyle.fontSize: FontSize.XSmall
                        textStyle.color: Color.DarkGray
                        multiline: true
                    }
                }
                
                ToggleButton {
                    verticalAlignment: VerticalAlignment.Center
                    checked: app.disableTrackpadFocus
                    onCheckedChanged: {
                        app.disableTrackpadFocus = checked;
                    }
                }
            }
            
        }
    }
    
    attachedObjects: [
        SystemToast {
            id: setToast
            body: ""
        }
    ]
    
    
}