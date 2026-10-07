APP_NAME = BerryBridgeUIService


CONFIG += qt warn_on

include(config.pri)

LIBS += -lbb -lbbsystem -lbbplatform
QT += network
QT += cascades sql
LIBS += -lbbdata

# --- WhatsApp calls, step 1: real-time call audio inside the service (src/call).
SOURCES += src/call/callaudio.cpp src/call/callaudiotest.cpp src/call/websocketclient.cpp src/call/calllink.cpp src/call/callservice.cpp
HEADERS += src/call/audioring.hpp src/call/callaudio.hpp src/call/callaudiotest.hpp src/call/websocketclient.hpp src/call/calllink.hpp src/call/callservice.hpp
LIBS += -lasound -laudio_manager