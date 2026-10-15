#ifndef CACHEPATHS_HPP_
#define CACHEPATHS_HPP_

#include <QDir>
#include <QString>

// Where Berry Bridge keeps what it fetched from Beeper: the chat and message
// databases, profile pictures, the app <-> service refresh file. In the app's
// PRIVATE data folder (the app and its service share it) -- the original kept
// them in shared/misc/BerryBridge, readable by any app with access to shared
// files. It is only a cache: Beeper Desktop on the server has everything, the
// app fills it when it opens and it is deleted when the app closes (see
// Service::setUiPresent). Files sent in chats (photos, videos, voice notes,
// documents) stay in shared/misc/BerryBridge, as the user's own files.
// Shared by the app (BerryBridgeUI/src includes it from here) and the service.
inline QString bbCacheDir() { return QDir::homePath() + "/cache"; }
inline QString bbCachePath(const QString &name) { return bbCacheDir() + "/" + name; }

#endif /* CACHEPATHS_HPP_ */
