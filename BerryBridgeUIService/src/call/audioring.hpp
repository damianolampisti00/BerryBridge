#ifndef AUDIORING_HPP_
#define AUDIORING_HPP_

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

// A bounded byte FIFO between two real-time threads (capture -> network,
// network -> playback). The writer never blocks: when the ring is full the
// OLDEST bytes are dropped, so a stalled reader can only lose audio, never
// stall the writer. The reader may wait, with a timeout.
class AudioRing
{
public:
    explicit AudioRing(int capacityBytes) :
            m_buf(capacityBytes, 0), m_head(0), m_size(0), m_overflow(0) {}

    void write(const char *data, int n)
    {
        QMutexLocker lock(&m_mutex);
        const int cap = m_buf.size();
        if (n > cap) {               // keep only the newest `cap` bytes
            m_overflow += n - cap;
            data += n - cap;
            n = cap;
        }
        const int excess = m_size + n - cap;
        if (excess > 0) {            // make room: drop the oldest bytes
            m_head = (m_head + excess) % cap;
            m_size -= excess;
            m_overflow += excess;
        }
        int tail = (m_head + m_size) % cap;
        for (int done = 0; done < n;) {
            const int chunk = qMin(n - done, cap - tail);
            memcpy(m_buf.data() + tail, data + done, chunk);
            done += chunk;
            tail = (tail + chunk) % cap;
        }
        m_size += n;
        m_cond.wakeAll();
    }

    // Up to n bytes; returns how many were read.
    int read(char *out, int n)
    {
        QMutexLocker lock(&m_mutex);
        return readLocked(out, n);
    }

    // Discards up to n bytes (playback drift control).
    int skip(int n)
    {
        QMutexLocker lock(&m_mutex);
        n = qMin(n, m_size);
        m_head = (m_head + n) % m_buf.size();
        m_size -= n;
        return n;
    }

    // Waits until at least `bytes` are buffered, or the timeout passes.
    bool waitFor(int bytes, unsigned long timeoutMs)
    {
        QMutexLocker lock(&m_mutex);
        if (m_size >= bytes) return true;
        m_cond.wait(&m_mutex, timeoutMs);
        return m_size >= bytes;
    }

    int level() const { QMutexLocker lock(&m_mutex); return m_size; }
    qint64 overflowBytes() const { QMutexLocker lock(&m_mutex); return m_overflow; }
    void clear() { QMutexLocker lock(&m_mutex); m_head = m_size = 0; }
    void wakeReaders() { QMutexLocker lock(&m_mutex); m_cond.wakeAll(); }

private:
    int readLocked(char *out, int n)
    {
        n = qMin(n, m_size);
        const int cap = m_buf.size();
        for (int done = 0; done < n;) {
            const int chunk = qMin(n - done, cap - m_head);
            memcpy(out + done, m_buf.constData() + m_head, chunk);
            done += chunk;
            m_head = (m_head + chunk) % cap;
        }
        m_size -= n;
        return n;
    }

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    QByteArray m_buf;
    int m_head;
    int m_size;
    qint64 m_overflow;
};

#endif /* AUDIORING_HPP_ */
