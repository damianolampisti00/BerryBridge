#ifndef MEMORYGUARD_HPP_
#define MEMORYGUARD_HPP_

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QTimer>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

// Set wherever a std::bad_alloc is caught; MemoryGuard acts on it.
extern volatile int g_memoryTrouble;

// The navigator caps a headless service's data at ~2.9 MB (RLIMIT_DATA)
// while it runs in the background partition, and lifts the cap while the
// app's UI is in front (the service is moved into the UI's partition). A call
// brings the UI up; the service grows past 2.9 MB meanwhile (call audio,
// ringing, notifications), and once the UI is gone it's over its cap: every
// allocation fails from then on -- DNS lookups, new connections, the TLS
// layer, each push message (bad_alloc) -- until the process restarts.
// Seen 2026-10-14/15: messages and calls dead for 20 min to 1h45 after a call,
// reproduced with an injected call. RLIMIT_DATA is 3 MB; /proc/<pid>/vmstat's
// rlimit_data is what counts against it: 2.90 MB when broken, 1.59 MB at a
// fresh start.
//
// MemoryGuard: at start, raises the soft limit to the hard one where it can,
// and every 30 s compares what counts against the limit with the limit. Close
// to it (or after a bad_alloc), outside a call, the service re-executes
// itself: same pid (the navigator keeps tracking it), a fresh small heap,
// everything reconnects as at a normal start. Not more than once in 10 minutes.
class MemoryGuard : public QObject
{
    Q_OBJECT
public:
    explicit MemoryGuard(char **argv, QObject *parent = 0) :
            QObject(parent), m_argv(argv), m_waitingForCall(false)
    {
        raiseLimit();
        connect(&m_timer, SIGNAL(timeout()), this, SLOT(check()));
        m_timer.start(30000);
        QTimer::singleShot(10000, this, SLOT(report()));
    }

    // Bytes counted against RLIMIT_DATA (/proc/self/vmstat's rlimit_data;
    // the file gives pages in hex: "as_stats.rlimit_data=0x197 (1.589MB)") and
    // the limit itself (getrlimit, which the navigator may change).
    static bool usage(qint64 *used, qint64 *limit)
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_DATA, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return false;
        *limit = qint64(rl.rlim_cur);
        qint64 anon = -1, counted = -1;
        QFile f("/proc/self/vmstat");
        if (!f.open(QIODevice::ReadOnly)) return false;
        const long page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
        foreach (const QByteArray &line, f.readAll().split('\n')) {
            const int eq = line.indexOf('=');
            if (eq < 0) continue;
            const QByteArray key = line.left(eq).trimmed();
            QByteArray val = line.mid(eq + 1).trimmed();
            val = val.left(val.indexOf(' ') < 0 ? val.size() : val.indexOf(' '));
            bool ok = false;
            const qint64 pages = val.startsWith("0x") ? val.mid(2).toLongLong(&ok, 16) : val.toLongLong(&ok);
            if (!ok) continue;
            if (key == "as_stats.anon_rsv") anon = pages * page;
            else if (key == "as_stats.rlimit_data") counted = pages * page;
        }
        *used = counted >= 0 ? counted : anon;
        return *used >= 0 && *limit > 0;
    }

private slots:
    void report()
    {
        qint64 anon, limit;
        if (usage(&anon, &limit))
            qDebug() << "[MEMORY] data" << anon / 1024 << "KB of" << limit / 1024 << "KB allowed";
    }

    void check()
    {
        qint64 anon = 0, limit = 0;
        const bool known = usage(&anon, &limit);
        const bool over = known && anon > limit - 128 * 1024;
        if (!over && !g_memoryTrouble) return;
        if (QFile::exists(QDir::homePath() + "/call_active")) { // never in a call
            if (!m_waitingForCall) qWarning() << "[MEMORY] over the limit during a call: restart after it";
            m_waitingForCall = true;
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch() / 1000;
        const qint64 last = qgetenv("BB_MEMGUARD_LAST").toLongLong();
        if (last > 0 && now - last < 600) return; // just restarted: don't loop
        qWarning() << "[MEMORY] data" << anon / 1024 << "KB, allowed" << limit / 1024 << "KB, bad_alloc"
                   << int(g_memoryTrouble) << "- restarting the service";
        restart(now);
    }

private:
    void raiseLimit()
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_DATA, &rl) != 0) return;
        qDebug() << "[MEMORY] RLIMIT_DATA soft" << qint64(rl.rlim_cur) << "hard" << qint64(rl.rlim_max);
        if (rl.rlim_max != rl.rlim_cur) {
            rl.rlim_cur = rl.rlim_max;
            const int r = setrlimit(RLIMIT_DATA, &rl);
            qDebug() << "[MEMORY] raise the soft limit to the hard one:" << (r == 0 ? "ok" : strerror(errno));
        }
    }

    void restart(qint64 now)
    {
        // The executable, as the system records it (a path without the
        // leading slash, e.g. "accounts/1000/appdata/.../BerryBridgeUIService").
        QByteArray exe;
        QFile f("/proc/self/exefile");
        if (f.open(QIODevice::ReadOnly)) exe = f.readAll().trimmed();
        if (exe.endsWith('\0')) exe.chop(1);
        if (!exe.isEmpty() && !exe.startsWith('/')) exe.prepend('/');
        if (exe.isEmpty()) return;
        setenv("BB_MEMGUARD_LAST", QByteArray::number(now).constData(), 1);
        // Nothing of this image is carried over: sockets, PPS objects, the
        // databases and the call control socket are all opened again.
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);
        execv(exe.constData(), m_argv);
        _exit(1); // exec failed: the navigator starts the service again
    }

    char **m_argv;
    QTimer m_timer;
    bool m_waitingForCall;
};

#endif /* MEMORYGUARD_HPP_ */
