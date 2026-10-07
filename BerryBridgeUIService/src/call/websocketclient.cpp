#include "websocketclient.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QStringList>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#include <QtEndian>

namespace {
const char *const kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const int kKeepAliveMs = 15000;   // ping this often...
const int kSilenceMs = 40000;     // ...and give up after this long without any data
const int kMaxMessage = 1 << 20;  // a call gateway never sends anything near this

QByteArray randomBytes(int n)
{
    QByteArray b(n, 0);
    for (int i = 0; i < n; ++i) b[i] = char(qrand() & 0xff);
    return b;
}

// Value of a header in a raw HTTP response head (case-insensitive name).
QByteArray headerValue(const QByteArray &head, const QByteArray &name)
{
    const QList<QByteArray> lines = head.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower()) return line.mid(colon + 1).trimmed();
    }
    return QByteArray();
}
}

WebSocketClient::WebSocketClient(QObject *parent) :
        QObject(parent),
        m_socket(0),
        m_state(Idle),
        m_verify(true),
        m_fragmentOpcode(0)
{
    qsrand(uint(QDateTime::currentMSecsSinceEpoch() & 0xffffffff) ^ uint(quintptr(this)));
    m_keepAlive.setInterval(kKeepAliveMs);
    connect(&m_keepAlive, SIGNAL(timeout()), this, SLOT(onKeepAlive()));
    m_lastReceived.invalidate();
}

WebSocketClient::~WebSocketClient()
{
    if (m_socket) m_socket->abort();
}

void WebSocketClient::open(const QUrl &url, const Headers &headers, bool verifyPeer)
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
    }
    m_url = url;
    m_headers = headers;
    m_verify = verifyPeer;
    m_buffer.clear();
    m_fragment.clear();
    m_fragmentOpcode = 0;
    m_tlsReport.clear();
    m_closeReason.clear();
    m_state = Connecting;

    m_socket = new QSslSocket(this);
    connect(m_socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_socket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onSocketError(QAbstractSocket::SocketError)));
    // Small, frequent audio frames: no Nagle delay.
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    const bool secure = url.scheme().compare("wss", Qt::CaseInsensitive) == 0;
    const int port = url.port(secure ? 443 : 80);
    if (secure) {
        QSslConfiguration cfg = m_socket->sslConfiguration();
        cfg.setProtocol(QSsl::SecureProtocols);
        cfg.setPeerVerifyMode(verifyPeer ? QSslSocket::VerifyPeer : QSslSocket::VerifyNone);
        m_socket->setSslConfiguration(cfg);
        connect(m_socket, SIGNAL(encrypted()), this, SLOT(onConnected()));
        connect(m_socket, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(onSslErrors(QList<QSslError>)));
        m_socket->connectToHostEncrypted(url.host(), quint16(port));
    } else {
        connect(m_socket, SIGNAL(connected()), this, SLOT(onConnected()));
        m_socket->connectToHost(url.host(), quint16(port));
    }
}

void WebSocketClient::onSslErrors(const QList<QSslError> &errors)
{
    QStringList list;
    for (int i = 0; i < errors.size(); ++i) list << errors.at(i).errorString();
    m_tlsReport = list.join("; ");
    if (!m_verify) m_socket->ignoreSslErrors(); // debugging only: never for real calls
    // With verification on, the handshake fails and onSocketError reports it.
}

void WebSocketClient::onConnected()
{
    m_state = Handshaking;
    m_key = randomBytes(16).toBase64();
    QByteArray path = m_url.encodedPath();
    if (path.isEmpty()) path = "/";
    if (m_url.hasQuery()) path += "?" + m_url.encodedQuery();
    QByteArray host = m_url.host().toLatin1();
    const bool secure = m_url.scheme().compare("wss", Qt::CaseInsensitive) == 0;
    if (m_url.port() > 0 && m_url.port() != (secure ? 443 : 80)) host += ":" + QByteArray::number(m_url.port());
    QByteArray req = "GET " + path + " HTTP/1.1\r\n"
                     "Host: " + host + "\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: " + m_key + "\r\n"
                     "Sec-WebSocket-Version: 13\r\n";
    for (int i = 0; i < m_headers.size(); ++i) req += m_headers.at(i).first + ": " + m_headers.at(i).second + "\r\n";
    req += "\r\n";
    m_socket->write(req);
    m_lastReceived.start();
}

void WebSocketClient::onReadyRead()
{
    m_buffer.append(m_socket->readAll());
    m_lastReceived.start();
    if (m_state == Handshaking) {
        const int end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buffer.size() > 16384) finish("upgrade answer too large");
            return;
        }
        const QByteArray head = m_buffer.left(end);
        m_buffer.remove(0, end + 4);
        const QByteArray status = head.left(head.indexOf('\r'));
        if (!status.startsWith("HTTP/1.1 101")) {
            finish("upgrade refused: " + QString::fromLatin1(status));
            return;
        }
        const QByteArray expected = QCryptographicHash::hash(m_key + kGuid, QCryptographicHash::Sha1).toBase64();
        if (headerValue(head, "Sec-WebSocket-Accept") != expected) {
            finish("bad Sec-WebSocket-Accept");
            return;
        }
        m_state = Open;
        m_keepAlive.start();
        emit opened();
    }
    if (m_state == Open || m_state == Closing) processFrames();
}

void WebSocketClient::processFrames()
{
    while (m_buffer.size() >= 2) {
        const uchar b0 = uchar(m_buffer.at(0)), b1 = uchar(m_buffer.at(1));
        const bool fin = b0 & 0x80;
        const int opcode = b0 & 0x0f;
        const bool masked = b1 & 0x80;
        quint64 len = b1 & 0x7f;
        int header = 2;
        if (len == 126) {
            if (m_buffer.size() < 4) return;
            len = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(m_buffer.constData() + 2));
            header = 4;
        } else if (len == 127) {
            if (m_buffer.size() < 10) return;
            len = qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(m_buffer.constData() + 2));
            header = 10;
        }
        if (len > quint64(kMaxMessage)) {
            finish("frame too large");
            return;
        }
        const int maskLen = masked ? 4 : 0;
        if (quint64(m_buffer.size()) < quint64(header + maskLen) + len) return;
        QByteArray payload = m_buffer.mid(header + maskLen, int(len));
        if (masked) { // servers must not mask, but tolerate it
            const char *mask = m_buffer.constData() + header;
            for (int i = 0; i < payload.size(); ++i) payload[i] = payload[i] ^ mask[i % 4];
        }
        m_buffer.remove(0, header + maskLen + int(len));

        switch (opcode) {
        case 0x0: // continuation
            m_fragment.append(payload);
            if (m_fragment.size() > kMaxMessage) { finish("message too large"); return; }
            if (fin) {
                if (m_fragmentOpcode == 0x1) emit textMessage(m_fragment);
                else if (m_fragmentOpcode == 0x2) emit binaryMessage(m_fragment);
                m_fragment.clear();
                m_fragmentOpcode = 0;
            }
            break;
        case 0x1:
        case 0x2:
            if (fin) {
                if (opcode == 0x1) emit textMessage(payload);
                else emit binaryMessage(payload);
            } else {
                m_fragment = payload;
                m_fragmentOpcode = opcode;
            }
            break;
        case 0x8: { // close: answer it (once), then the server ends the TCP connection
            QString reason = "closed by server";
            if (payload.size() >= 2) {
                reason += QString(" (%1 %2)").arg(qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(payload.constData())))
                          .arg(QString::fromUtf8(payload.mid(2)));
            }
            m_closeReason = reason;
            if (m_state == Open) {
                sendFrame(0x8, payload.left(2));
                m_state = Closing;
            }
            m_socket->disconnectFromHost();
            return;
        }
        case 0x9: // ping
            sendFrame(0xA, payload);
            break;
        case 0xA: // pong
            break;
        default:
            finish(QString("unknown opcode %1").arg(opcode));
            return;
        }
        if (!m_socket) return; // a slot connected to a message signal closed us
    }
}

bool WebSocketClient::sendFrame(int opcode, const QByteArray &payload)
{
    if (!m_socket || (m_state != Open && !(m_state == Closing && opcode == 0x8))) return false;
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(char(0x80 | opcode));
    const int len = payload.size();
    if (len < 126) {
        frame.append(char(0x80 | len));
    } else if (len < 65536) {
        frame.append(char(0x80 | 126));
        frame.append(char((len >> 8) & 0xff));
        frame.append(char(len & 0xff));
    } else {
        frame.append(char(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) frame.append(char((quint64(len) >> shift) & 0xff));
    }
    const QByteArray mask = randomBytes(4);
    frame.append(mask);
    const int start = frame.size();
    frame.append(payload);
    char *p = frame.data() + start;
    for (int i = 0; i < len; ++i) p[i] = p[i] ^ mask.at(i % 4);
    return m_socket->write(frame) == frame.size();
}

bool WebSocketClient::sendText(const QByteArray &utf8) { return sendFrame(0x1, utf8); }
bool WebSocketClient::sendBinary(const QByteArray &data) { return sendFrame(0x2, data); }

qint64 WebSocketClient::bytesToWrite() const
{
    return m_socket ? m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite() : 0;
}

void WebSocketClient::close(quint16 code, const QByteArray &reason)
{
    if (m_state == Open) {
        QByteArray payload(2, 0);
        payload[0] = char(code >> 8);
        payload[1] = char(code & 0xff);
        payload.append(reason);
        m_state = Closing;
        sendFrame(0x8, payload);
        m_socket->disconnectFromHost();
        m_closeReason = "closed by us";
    } else if (m_socket && m_state != Idle) {
        finish("closed by us");
    }
}

void WebSocketClient::onKeepAlive()
{
    if (m_state != Open) return;
    if (m_lastReceived.isValid() && m_lastReceived.elapsed() > kSilenceMs) {
        finish("no data from the server for 40 s");
        return;
    }
    sendFrame(0x9, QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
}

void WebSocketClient::onSocketError(QAbstractSocket::SocketError)
{
    if (!m_socket) return;
    QString why = m_socket->errorString();
    if (!m_tlsReport.isEmpty()) why += " [TLS: " + m_tlsReport + "]";
    finish(why);
}

void WebSocketClient::onDisconnected()
{
    finish(m_closeReason.isEmpty() ? QString("connection lost") : m_closeReason);
}

void WebSocketClient::finish(const QString &reason)
{
    if (m_state == Idle) return;
    m_state = Idle;
    m_keepAlive.stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = 0;
    }
    emit closed(reason);
}
