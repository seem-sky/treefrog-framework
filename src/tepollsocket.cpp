/* Copyright (c) 2013, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <sys/types.h>
#include <sys/epoll.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QBuffer>
#include <QDateTime>
#include <TSystemGlobal>
#include <THttpHeader>
#include <TAtomicQueue>
#include "tepollsocket.h"
#include "tepollhttpsocket.h"
#include "tepoll.h"
#include "tsendbuffer.h"
#include "tfcore_unix.h"

class SendData;

static int sendBufSize = 0;
static int recvBufSize = 0;
static QAtomicInt objectCounter(1);


TEpollSocket *TEpollSocket::accept(int listeningSocket)
{
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    int actfd = tf_accept4(listeningSocket, (sockaddr *)&addr, &addrlen, SOCK_CLOEXEC | SOCK_NONBLOCK);
    int err = errno;
    if (Q_UNLIKELY(actfd < 0)) {
        if (err != EAGAIN) {
            tSystemWarn("Failed accept.  errno:%d", err);
        }
        return NULL;
    }

    return create(actfd, QHostAddress((sockaddr *)&addr));
}


TEpollSocket *TEpollSocket::create(int socketDescriptor, const QHostAddress &address)
{
    TEpollSocket *sock = 0;

    if (Q_LIKELY(socketDescriptor > 0)) {
        sock  = new TEpollHttpSocket(socketDescriptor, address);
        sock->moveToThread(QCoreApplication::instance()->thread());

        initBuffer(socketDescriptor);
    }

    return sock;
}


TSendBuffer *TEpollSocket::createSendBuffer(const QByteArray &header, const QFileInfo &file, bool autoRemove, const TAccessLogger &logger)
{
    return new TSendBuffer(header, file, autoRemove, logger);
}


void TEpollSocket::initBuffer(int socketDescriptor)
{
    const int BUF_SIZE = 128 * 1024;

    if (Q_UNLIKELY(sendBufSize == 0)) {
        // Creates a common buffer
        int res;
        socklen_t optlen;

        optlen = sizeof(int);
        res = getsockopt(socketDescriptor, SOL_SOCKET, SO_SNDBUF, &sendBufSize, &optlen);
        if (res < 0)
            sendBufSize = BUF_SIZE;

        optlen = sizeof(int);
        res = getsockopt(socketDescriptor, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &optlen);
        if (res < 0)
            recvBufSize = BUF_SIZE;

    }
}


TEpollSocket::TEpollSocket(int socketDescriptor, const QHostAddress &address)
    : sd(socketDescriptor), identifier(0), clientAddr(address)
{
    quint64 h = QDateTime::currentDateTime().toTime_t();
    quint64 b = objectCounter.fetchAndAddOrdered(1);
    identifier = (h << 32) | (b & 0xffffffff);
    tSystemDebug("TEpollSocket  id:%llu", identifier);
}


TEpollSocket::~TEpollSocket()
{
    close();

    for (QListIterator<TSendBuffer*> it(sendBuf); it.hasNext(); ) {
        delete it.next();
    }
    sendBuf.clear();
}


/*!
  Receives data
  @return  0:success  -1:error
 */
int TEpollSocket::recv()
{
    int err;

    for (;;) {
        void *buf = getRecvBuffer(recvBufSize);
        errno = 0;
        int len = ::recv(sd, buf, recvBufSize, 0);
        err = errno;

        if (len <= 0) {
            break;
        }

        // Read successfully
        seekRecvBuffer(len);
    }

    int ret = 0;
    switch (err) {
    case EAGAIN:
        break;

    case 0:  // FALL THROUGH
    case ECONNRESET:
        tSystemDebug("Socket disconnected : errno:%d", err);
        ret = -1;
        break;

    default:
        tSystemError("Failed recv : errno:%d", err);
        ret = -1;
        break;
    }
    return ret;
}

/*!
  Sends data
  @return  0:success  -1:error
 */
int TEpollSocket::send()
{
    if (sendBuf.isEmpty()) {
        return 0;
    }

    int err = 0;
    int len;
    TSendBuffer *buf = sendBuf.first();
    TAccessLogger &logger = buf->accessLogger();

    for (;;) {
        len = sendBufSize;
        void *data = buf->getData(len);
        if (len == 0) {
            break;
        }

        errno = 0;
        len = ::send(sd, data, len, 0);
        err = errno;

        if (len > 0) {
            // Sent successfully
            buf->seekData(len);
            logger.setResponseBytes(logger.responseBytes() + len);
        } else {
            break;
        }
    }

    int ret = 0;
    switch (err) {
    case 0:  // FALL THROUGH
    case EAGAIN:
        break;

    case ECONNRESET:
        tSystemDebug("Socket disconnected : errno:%d", err);
        logger.setResponseBytes(-1);
        ret = -1;
        break;

    default:
        tSystemError("Failed send : errno:%d  len:%d", err, len);
        logger.setResponseBytes(-1);
        ret = -1;
        break;
    }

    if (err != EAGAIN && !sendBuf.isEmpty()) {
        TEpoll::instance()->modifyPoll(this, (EPOLLIN | EPOLLOUT | EPOLLET));  // reset
    }

    if (buf->atEnd() || ret < 0) {
        logger.write();  // Writes access log
        delete sendBuf.dequeue(); // delete send-buffer obj
    }

    return ret;
}


void TEpollSocket::enqueueSendData(TSendBuffer *buffer)
{
    sendBuf << buffer;
}


void TEpollSocket::enqueueSendData(const QByteArray &data)
{
    sendBuf << new TSendBuffer(data);
}


void TEpollSocket::setSocketDescpriter(int socketDescriptor)
{
    sd = socketDescriptor;
}


void TEpollSocket::close()
{
    if (sd > 0) {
        TF_CLOSE(sd);
        sd = 0;
    }
}
