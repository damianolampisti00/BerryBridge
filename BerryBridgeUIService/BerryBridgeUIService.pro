APP_NAME = BerryBridgeUIService


CONFIG += qt warn_on

include(config.pri)

LIBS += -lbb -lbbsystem -lbbplatform
QT += network
QT += cascades sql
LIBS += -lbbdata

# --- WhatsApp calls, step 1: real-time call audio inside the service (src/call).
SOURCES += src/call/callaudio.cpp src/call/callaudiotest.cpp src/call/websocketclient.cpp src/call/calllink.cpp src/call/callservice.cpp src/call/callringer.cpp
HEADERS += src/serviceguard.hpp src/freshresolver.hpp src/memoryguard.hpp src/call/audioring.hpp src/call/callaudio.hpp src/call/callaudiotest.hpp src/call/websocketclient.hpp src/call/calllink.hpp src/call/callservice.hpp src/call/callringer.hpp
LIBS += -lasound -laudio_manager
# Opus for the call audio (the same vendored libopus as the UI's voice notes).
INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/../third_party/opus/include)
LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/opus/lib/armv7/libopus.a)
# package.ps1 -DevTools: call test harness + file logs (never in public builds).
calltest: DEFINES += BB_CALL_TEST BB_FILE_LOG