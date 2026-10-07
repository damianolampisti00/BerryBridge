APP_NAME = BerryBridgeUI

CONFIG += qt warn_on cascades10

include(config.pri)

LIBS += -lbb -lbbsystem
LIBS += -lbbdata -lbbdevice
LIBS += -lbbpim
LIBS += -lbbplatform
QT += network
LIBS += -lQtLocationSubset
LIBS += -lcurl
LIBS += -lbbcascadespickers
QT += gui
LIBS += -lscreen

# --- Voice messages (src/audio): QNX PCM capture (libasound + audio manager),
# Ogg/Opus encode/decode (libopus, vendored from BBport, armv7 only), playback
# through bb::multimedia::MediaPlayer.
SOURCES += src/audio/oggopusencoder.cpp src/audio/oggopusdecoder.cpp \
           src/audio/voicerecorder.cpp src/audio/voiceplayer.cpp
HEADERS += src/audio/oggopusencoder.hpp src/audio/oggopusdecoder.hpp \
           src/audio/voicerecorder.hpp src/audio/voiceplayer.hpp
INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/../third_party/opus/include)
LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/opus/lib/armv7/libopus.a)
LIBS += -lasound -laudio_manager -lbbmultimedia

# --- WhatsApp calls: UI client of the service's CallService (src/call).
SOURCES += src/call/callclient.cpp
HEADERS += src/call/callclient.hpp
