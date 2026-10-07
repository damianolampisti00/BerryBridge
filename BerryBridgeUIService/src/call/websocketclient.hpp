#ifndef WEBSOCKETCLIENT_HPP_
#define WEBSOCKETCLIENT_HPP_

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QtNetwork/QAbstractSocket>

class QSslSocket;

// A complete RFC 6455 client over QSslSocket (Qt 4.8), for the call gateway.
// Unlike the Beeper push socket in Service (send-only text under 126 bytes,
// no close handshake, unchecked upgrade), this one: verifies the server
// certificate by default, checks the 101 + Sec-WebSocket-Accept answer,
// masks every frame with a random key, sends text and binary of any length
// (7/16/64-bit lengths), reassembles fragmented messages, answers ping and
// close, pings on its own and drops a silent connection. One message = one
// signal. Reconnecting is up to the owner (closed() then open() again).
//
// Lives in the thread it is created in; all calls from that thread.
class WebSocketClient : public QObject
{
    Q_OBJECT
public:
    typedef QList<QPair<QByteArray, QByteArray> > Headers;

    explicit WebSocketClient(QObject *parent = 0);
    ~WebSocketClient();

    // ws:// or wss://; extra headers go on the upgrade request (Authorization...).
    // verifyPeer false is for debugging only: the certificate is then ignored.
    void open(const QUrl &url, const Headers &headers = Headers(), bool verifyPeer = true);
    void close(quint16 code = 1000, const QByteArray &reason = QByteArray());
    bool isOpen() const { return m_state == Open; }

    bool sendText(const QByteArray &utf8);
    bool sendBinary(const QByteArray &data);
    qint64 bytesToWrite() const; // backlog not yet handed to the network

    // What TLS said about the server certificate (empty = verified fine).
    QString tlsReport() const { return m_tlsReport; }

signals:
    void opened();
    void closed(const QString &reason);
    void textMessage(const QByteArray &utf8);
    void binaryMessage(const QByteArray &data);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void onSslErrors(const QList<QSslError> &errors);
    void onKeepAlive();

private:
    enum State { Idle, Connecting, Handshaking, Open, Closing };
    bool sendFrame(int opcode, const QByteArray &payload);
    void processFrames();
    void finish(const QString &reason);

    QSslSocket *m_socket;
    State m_state;
    QUrl m_url;
    Headers m_headers;
    bool m_verify;
    QByteArray m_key;
    QByteArray m_buffer;
    QByteArray m_fragment;
    int m_fragmentOpcode;
    QTimer m_keepAlive;
    QElapsedTimer m_lastReceived;
    QString m_tlsReport;
    QString m_closeReason;
};

#endif /* WEBSOCKETCLIENT_HPP_ */
