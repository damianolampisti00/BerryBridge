#ifndef AUDIORING_HPP_
#define AUDIORING_HPP_

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QVector>
#include <QWaitCondition>

// A bounded byte FIFO between two real-time threads (capture -> network,
// network -> playback). The writer never blocks: when the ring is full the
// OLDEST bytes are dropped, so a stalled reader can only lose audio, never
// stall the writer. The reader may wait, with a timeout.
class AudioRing
{
public:
    explicit AudioRing(int capacityBytes) :
            m_buf(capacityBytes, 0), m_head(0), m_size(0), m_overflow(0), m_taken(0),
            m_target(0), m_hasPlay(false), m_playAtUs(0), m_playPos(0) {}

    void write(const char *data, int n)
    {
        QMutexLocker lock(&m_mutex);
        writeLocked(data, n);
    }

    // The gateway cancels the speaker's echo in the mic with what it sent as
    // the reference, so each mic block says how far playback had got when
    // its last sample was captured. Read when the network thread sent the
    // block, in whole device blocks, that was 0..60 ms late and changing from
    // block to block (2026-10-26): the canceller removed nothing. Even 1 ms
    // of wobble defeats it, so the position is counted, not timed: speaker
    // and mic run on one clock, mic sample k is heard with speaker sample
    // k + C (CallAudioInput keeps C), and the blocks written to the speaker
    // tell which ring audio, or silence, that sample was.
    //
    // Uplink: a mic block, with the speaker stream byte heard at its end.
    void write(const char *data, int n, qint64 heardAtEnd)
    {
        QMutexLocker lock(&m_mutex);
        writeLocked(data, n);
        m_heard.append(qMakePair(m_taken + m_size, heardAtEnd));
        if (m_heard.size() > 64) m_heard.remove(0, m_heard.size() - 64);
    }
    // The speaker stream byte heard when byte `pos` of this stream (counted
    // like taken()) was captured; false if no block written with one covers
    // it. The downlink ring's ringAtDev() turns it into audio sent.
    bool heardAt(qint64 pos, qint64 *heard) const
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_heard.size(); ++i) {
            if (m_heard.at(i).first >= pos) {
                *heard = qMax(Q_INT64_C(0), m_heard.at(i).second - (m_heard.at(i).first - pos));
                return true;
            }
        }
        return false;
    }
    // Downlink, from the playback thread: a block written to the device, the
    // speaker stream ending at devEnd (bytes since the thread started), len
    // long, made of `got` bytes of ring audio from ringStart then silence;
    // and, at atUs (monotonic), the speaker stream byte being heard.
    void addPlayed(qint64 devEnd, int len, qint64 ringStart, int got, qint64 atUs, qint64 devHeard)
    {
        QMutexLocker lock(&m_mutex);
        const PlayBlock b = { devEnd, len, ringStart, got };
        m_blocks.append(b);
        if (m_blocks.size() > 96) m_blocks.remove(0, m_blocks.size() - 96); // ~2 s
        m_hasPlay = true;
        m_playAtUs = atUs;
        m_playPos = devHeard;
    }
    // A new playback thread: its speaker stream starts again from 0.
    void resetPlayed()
    {
        QMutexLocker lock(&m_mutex);
        m_blocks.clear();
        m_hasPlay = false;
    }
    // The speaker stream byte heard at atUs, carried on in real time from
    // the last addPlayed. Only good to a few ms: used to find C, not as the
    // position itself.
    bool devHeardAt(qint64 atUs, int bytesPerSec, qint64 *dev) const
    {
        QMutexLocker lock(&m_mutex);
        if (!m_hasPlay) return false;
        *dev = m_playPos + (atUs - m_playAtUs) * bytesPerSec / 1000000;
        return true;
    }
    // The ring position of speaker stream byte `dev`: inside a block's ring
    // audio, that byte of it; in its silence, the end of that audio.
    bool ringAtDev(qint64 dev, qint64 *pos) const
    {
        QMutexLocker lock(&m_mutex);
        if (m_blocks.isEmpty() || dev < m_blocks.first().devEnd - m_blocks.first().len) return false;
        for (int i = m_blocks.size() - 1; i >= 0; --i) {
            const PlayBlock &b = m_blocks.at(i);
            const qint64 start = b.devEnd - b.len;
            if (dev >= start) {
                *pos = b.ringStart + qMin<qint64>(dev - start, b.got);
                return true;
            }
        }
        return false;
    }

private:
    void writeLocked(const char *data, int n)
    {
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
            m_taken += excess;
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

public:
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
        m_taken += n;
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

    // Downlink: how much audio playback should keep buffered, from the
    // network's jitter (CallLink); 0 = the configured default.
    void setTargetBytes(int bytes) { QMutexLocker lock(&m_mutex); m_target = bytes; }
    int targetBytes() const { QMutexLocker lock(&m_mutex); return m_target; }

    int level() const { QMutexLocker lock(&m_mutex); return m_size; }
    qint64 overflowBytes() const { QMutexLocker lock(&m_mutex); return m_overflow; }
    // Bytes that have left the ring (read, skipped, dropped, cleared): the
    // position in the written stream the reader has reached. For the
    // downlink, what the phone has played so far -- the gateway's echo
    // canceller lines up its reference with it.
    qint64 taken() const { QMutexLocker lock(&m_mutex); return m_taken; }
    void clear() { QMutexLocker lock(&m_mutex); m_taken += m_size; m_head = m_size = 0; }
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
        m_taken += n;
        return n;
    }

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    QByteArray m_buf;
    int m_head;
    int m_size;
    qint64 m_overflow;
    qint64 m_taken;
    int m_target;                            // downlink: see setTargetBytes
    QVector<QPair<qint64, qint64> > m_heard; // uplink: (stream end of a block, position heard there)
    struct PlayBlock { qint64 devEnd; int len; qint64 ringStart; int got; };
    QVector<PlayBlock> m_blocks;             // downlink: the last blocks written to the speaker
    bool m_hasPlay;                          // downlink: the last addPlayed's time and byte heard
    qint64 m_playAtUs;
    qint64 m_playPos;
};

#endif /* AUDIORING_HPP_ */
