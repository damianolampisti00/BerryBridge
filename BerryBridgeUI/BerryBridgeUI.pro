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

# --- Stickers (src/sticker): WhatsApp sends them as WebP, which BB10 can't
# decode; libwebp 1.4.0 decoder + demuxer, vendored (third_party/libwebp,
# rebuilt by its build-libwebp.sh, armv7 only).
SOURCES += src/sticker/webpimage.cpp
HEADERS += src/sticker/webpimage.hpp
INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/../third_party/libwebp/include)
LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/libwebp/lib/armv7/libwebpdemux.a)
LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/libwebp/lib/armv7/libwebpdecoder.a)

# --- Instagram view-once media through the igview service (src/viewonce).
SOURCES += src/viewonce/viewonceclient.cpp
HEADERS += src/viewonce/viewonceclient.hpp
LIBS += -lbbutility

# --- WhatsApp calls: UI client of the service's CallService (src/call).
SOURCES += src/call/callclient.cpp
HEADERS += src/call/callclient.hpp
# package.ps1 -DevTools: file logs (never in public builds).
calltest: DEFINES += BB_FILE_LOG
