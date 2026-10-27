/*
 * Copyright (c) 2013-2015 BlackBerry Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "service.hpp"

#include <bb/Application>

#include <QLocale>
#include <QTranslator>


#include <QDebug>
#include <QEvent>
#include <QAbstractSocket>
#include <QLocalSocket>
#include <exception>
#include <string.h>

#include "serviceguard.hpp"
#include "memoryguard.hpp"

#ifdef BB_FILE_LOG
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <stdio.h>

// Developer builds only (package.ps1 -DevTools): debug output also goes to a file in the app's data folder (slog2
// doesn't keep this process's lines): service_debug.log there, trimmed at 512 KB.
static void fileMessageHandler(QtMsgType type, const char *msg)
{
    // In the app's private data folder: in the shared one every line made the
    // media indexer (mmsynclite) re-index the file -- battery, in standby too.
    static const QString kPath = QDir::homePath() + "/service_debug.log";
    QFile f(kPath);
    if (f.size() > 512 * 1024) f.remove();
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        const char *level = type == QtDebugMsg ? "D" : type == QtWarningMsg ? "W" : type == QtCriticalMsg ? "C" : "F";
        QTextStream(&f) << QDateTime::currentDateTime().toString("MM-dd HH:mm:ss.zzz") << " " << level << " " << msg << "\n";
    }
    fprintf(stderr, "%s\n", msg);
}
#endif

using namespace bb;

volatile int g_memoryTrouble = 0;
MemoryGuard *g_memoryGuard = 0;

// An exception out of an event handler (Qt 4 throws std::bad_alloc, for one)
// used to abort the whole service -- calls and notifications gone until the
// app was opened again. It is logged with what was being delivered, and the
// service goes on.
class ServiceApplication : public Application
{
public:
    ServiceApplication(int &argc, char **argv) : Application(argc, argv), m_reported(0) {}
    bool notify(QObject *receiver, QEvent *event)
    {
        try {
            return Application::notify(receiver, event);
        } catch (const std::exception &e) {
            recover(receiver, event, e.what());
        } catch (...) {
            recover(receiver, event, "unknown exception");
        }
        return false;
    }

private:
    // A socket notifier that throws fires again at once (seen: bad_alloc
    // ~100 times a second for an hour, the push link never came back): close
    // that socket, its owner reconnects or drops it. Logged a few times only.
    void recover(QObject *receiver, QEvent *event, const char *what)
    {
        if (strstr(what, "bad_alloc") || strstr(what, "bad allocation")) {
            g_memoryTrouble = 1;
            if (g_memoryGuard) QTimer::singleShot(0, g_memoryGuard, SLOT(check()));
        }
        QObject *socket = 0;
        for (QObject *p = receiver; p && !socket; p = p->parent()) {
            if (qobject_cast<QAbstractSocket *>(p) || qobject_cast<QLocalSocket *>(p)) socket = p;
        }
        if (m_reported < 20) {
            ++m_reported;
            QString where = receiver ? receiver->metaObject()->className() : "?";
            if (QAbstractSocket *s = qobject_cast<QAbstractSocket *>(socket))
                where += QString(" of %1 to %2:%3").arg(s->metaObject()->className()).arg(s->peerName()).arg(s->peerPort());
            else if (QLocalSocket *s = qobject_cast<QLocalSocket *>(socket))
                where += QString(" of a local socket (%1)").arg(s->serverName());
            qWarning() << "[SERVICE] exception in an event handler:" << what << "event" << int(event->type()) << where
                       << (socket ? "- closing that socket" : "");
        }
        if (socket) {
            SocketCloser *closer = new SocketCloser(socket);
            closer->moveToThread(socket->thread());
            QMetaObject::invokeMethod(closer, "run", Qt::QueuedConnection);
        }
    }

    int m_reported;
};

int main(int argc, char **argv)
{
#ifdef BB_FILE_LOG
    qInstallMsgHandler(fileMessageHandler);
#endif
    ServiceApplication app(argc, argv);
    MemoryGuard memoryGuard(argv); // see memoryguard.hpp

    // Create the Application UI object, this is where the main.qml file
    // is loaded and the application scene is set.
    Service srv;

    // Enter the application main event loop.
    return Application::exec();
}
