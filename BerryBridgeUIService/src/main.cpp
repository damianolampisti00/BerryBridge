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


#ifdef BB_FILE_LOG
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <stdio.h>

// Developer builds only (package.ps1 -DevTools): debug output also goes to a file in the shared Berry Bridge folder (slog2
// doesn't keep this process's lines): service_debug.log there, trimmed at 512 KB.
static void fileMessageHandler(QtMsgType type, const char *msg)
{
    static const char *const kPath = "/accounts/1000/shared/misc/BerryBridge/service_debug.log";
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

int main(int argc, char **argv)
{
#ifdef BB_FILE_LOG
    qInstallMsgHandler(fileMessageHandler);
#endif
    Application app(argc, argv);

    // Create the Application UI object, this is where the main.qml file
    // is loaded and the application scene is set.
    Service srv;

    // Enter the application main event loop.
    return Application::exec();
}
