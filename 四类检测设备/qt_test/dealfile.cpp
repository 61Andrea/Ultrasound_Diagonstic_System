#include "dealfile.h"
#include <QHostAddress>
#include <QtEndian>
#include <QDebug>

DealFile::DealFile(QObject *parent)
    : QObject(parent),
      tcpSocket(nullptr)
{
    // 构造函数保持干净，等待子线程动态实例化
}

DealFile::~DealFile()
{
    if (tcpSocket && tcpSocket->isOpen()) {
        tcpSocket->close();
    }
}

// ---------------------------------------------------------
// 连接管理
// ---------------------------------------------------------
void DealFile::connectToServer(const QString &address, int port)
{
    qDebug() << "子线程收到连接指令，开始执行 connectToHost";

    if (tcpSocket == nullptr) {
        tcpSocket = new QTcpSocket(this);

        connect(tcpSocket, &QTcpSocket::connected, this, &DealFile::onConnected);
        connect(tcpSocket, &QTcpSocket::disconnected, this, &DealFile::onDisconnected);
        connect(tcpSocket, &QTcpSocket::readyRead, this, &DealFile::onReadyRead);
        connect(tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
                this, &DealFile::onError);
    }

    // 无论当前是什么状态（除了未连接），直接强制重置，防止重连定时器导致状态机卡死
    if (tcpSocket->state() != QAbstractSocket::UnconnectedState) {
        tcpSocket->abort();
    }

    tcpSocket->connectToHost(QHostAddress(address), port);
    qDebug() << "后台线程正在尝试异步连接..." << address << ":" << port;
}

void DealFile::disconnectFromServer()
{
    if (tcpSocket && tcpSocket->state() == QTcpSocket::ConnectedState) {
        tcpSocket->disconnectFromHost();
    }
}

void DealFile::onConnected()
{
    qDebug() << "子线程 Socket 握手成功，准备通知主界面！";
    emit connectionStatusChanged(true);
}

void DealFile::onDisconnected()
{
    qDebug() << "TCP 已断开连接";
    m_buffer.clear(); // 断开时清空残留的脏数据
    emit connectionStatusChanged(false);
}

void DealFile::onError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    emit errorOccurred(tcpSocket->errorString());
}

// ---------------------------------------------------------
// 数据发送与接收
// ---------------------------------------------------------
void DealFile::sendData(unsigned short cmd)
{
    if (tcpSocket && tcpSocket->state() == QTcpSocket::ConnectedState) {
        tcpSocket->write(reinterpret_cast<const char*>(&cmd), sizeof(unsigned short));
        tcpSocket->flush();
    } else {
        emit errorOccurred("未连接到设备，发送指令失败！");
    }
}

void DealFile::sendFrame(const QByteArray &frame)
{
    if (tcpSocket && tcpSocket->state() == QTcpSocket::ConnectedState) {
        tcpSocket->write(frame);
        tcpSocket->flush();
    } else {
        emit errorOccurred("未连接到设备，发送配置指令失败！");
    }
}

// ---------------------------------------------------------
// 核心：多协议网络流环形解包状态机
// ---------------------------------------------------------
void DealFile::onReadyRead()
{
    m_buffer.append(tcpSocket->readAll());

    // 【重要修复】：用一个大 while 循环包裹两种模式，完美解决 TCP 粘包导致的卡死
    while (!m_buffer.isEmpty()) {

        // ========================================================
        // 模式 1：文件流式下载接收
        // ========================================================
        if (m_isFileDownloading) {
            if (m_expectedFileBlockSize == -1) {
                if (m_buffer.size() < 4) break; // 等待 4 字节的长度信息

                quint32 netSize;
                memcpy(&netSize, m_buffer.constData(), 4);
                quint32 blockSize = qFromBigEndian(netSize);

                if (blockSize == 0xFFFFFFFF) {
                    // 收到 EOF 标志，将残留数据刷入硬盘
                    if (!m_fileDataCache.isEmpty()) {
                        emit fileDataReceived(m_fileDataCache);
                        m_fileDataCache.clear();
                    }
                    m_isFileDownloading = false;
                    m_expectedFileBlockSize = -1;
                    m_buffer.remove(0, 4);
                    emit fileTransferFinished();
                    continue; // 【关键】：跳回循环开头，继续处理后续可能的常规指令
                } else {
                    m_expectedFileBlockSize = blockSize;
                    m_buffer.remove(0, 4);
                }
            }

            if (m_expectedFileBlockSize > 0) {
                if (m_buffer.size() >= m_expectedFileBlockSize) {
                    m_fileDataCache.append(m_buffer.left(m_expectedFileBlockSize));
                    m_buffer.remove(0, m_expectedFileBlockSize);
                    m_expectedFileBlockSize = -1;

                    // 防卡死设计：数据攒够 128KB 统一抛给主界面一次
                    if (m_fileDataCache.size() >= 128 * 1024) {
                        emit fileDataReceived(m_fileDataCache);
                        m_fileDataCache.clear();
                    }
                    continue; // 【关键】：跳回循环开头，连贯处理后续块
                } else {
                    break; // 数据不够，退出大循环等待网络收信
                }
            }
            continue;
        }

        // ========================================================
        // 模式 2：常规控制包解析 (同时搜索 AA 55 和 55 00)
        // ========================================================
        if (m_buffer.size() < 4) break;

        int headIndexAA55 = m_buffer.indexOf(QByteArray::fromHex("AA55"));
        int headIndex5500 = m_buffer.indexOf(QByteArray::fromHex("5500"));

        int headIndex = -1;
        if (headIndexAA55 != -1 && headIndex5500 != -1) {
            headIndex = qMin(headIndexAA55, headIndex5500);
        } else if (headIndexAA55 != -1) {
            headIndex = headIndexAA55;
        } else if (headIndex5500 != -1) {
            headIndex = headIndex5500;
        }

        if (headIndex == -1) {
            m_buffer.clear();
            break;
        }

        if (headIndex > 0) {
            m_buffer.remove(0, headIndex);
            continue;
        }

        // ------------------ 处理 55 00 ------------------
        if (static_cast<quint8>(m_buffer.at(0)) == 0x55 && static_cast<quint8>(m_buffer.at(1)) == 0x00) {
            if (m_buffer.size() < 6) break;

            quint16 cmd = static_cast<quint8>(m_buffer.at(2)) | (static_cast<quint8>(m_buffer.at(3)) << 8);
            quint8 status = m_buffer.at(4);

            if (cmd == 306) {
                quint8 count = m_buffer.at(5);
                int totalLen = 6 + count * 64;
                if (m_buffer.size() < totalLen) break;

                QStringList fileList;
                for (int i = 0; i < count; i++) {
                    QByteArray nameBytes = m_buffer.mid(6 + i * 64, 64);
                    int nullPos = nameBytes.indexOf('\0');
                    if (nullPos != -1) nameBytes.truncate(nullPos);
                    QString name = QString::fromUtf8(nameBytes);
                    if (!name.isEmpty()) fileList.append(name);
                }

                emit fileListReceived(fileList);
                m_buffer.remove(0, totalLen);
                continue;
            }
            else if (cmd == 307) {
                if (status == 0x00) {
                    m_isFileDownloading = true;
                    m_expectedFileBlockSize = -1;
                    m_fileDataCache.clear();
                } else {
                    emit fileTransferFinished();
                }
                m_buffer.remove(0, 6);
                continue; // 【关键】：进入流接收模式，跳回开头由模式1接管
            }
            else if (cmd == 308) {
                m_buffer.remove(0, 6);
                continue;
            }
            else {
                m_buffer.remove(0, 2);
                continue;
            }
        }

        // ------------------ 处理超声短包头 ------------------
        if (m_buffer.size() >= 5) {
            const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
            const quint8 possibleChannel = frame[2];

            if (possibleChannel < 8) {
                const quint16 payloadBytes = qFromLittleEndian<quint16>(frame + 3);
                if (payloadBytes >= 4 && payloadBytes <= 4096 * 4 && payloadBytes % 4 == 0) {
                    const int frameSize = 5 + payloadBytes + 2;
                    if (m_buffer.size() < frameSize) break;

                    if (frame[frameSize - 2] == 0xFF && frame[frameSize - 1] == 0xFF) {
                        QByteArray payload = m_buffer.mid(5, payloadBytes);
                        emit ultrasonicDataReceived(possibleChannel, payload);
                        m_buffer.remove(0, frameSize);
                        continue;
                    }
                }
            }
        }

        // ------------------ 处理诊断包与 0x01 包 ------------------
        quint8 byte2 = (quint8)m_buffer.at(2);

        if (byte2 == 0x10 || byte2 == 0x11) {
            QByteArray diagData = m_buffer.left(4);
            emit diagDataReceived(diagData);
            m_buffer.remove(0, 4);
            continue;
        }
        else if (byte2 == 0x01) {
            if (m_buffer.size() < 4) break;
            quint8 frameType = (quint8)m_buffer.at(3);

            if (frameType == 0x01) {
                if (m_buffer.size() < 4122) break;
                if ((quint8)m_buffer.at(4120) == 0xFF && (quint8)m_buffer.at(4121) == 0xFF) {
                    quint8 channelID = (quint8)m_buffer.at(4);
                    QByteArray payload = m_buffer.mid(24, 4096);
                    emit waveDataReceived(channelID, payload);
                    m_buffer.remove(0, 4122);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x02) {
                if (m_buffer.size() < 58) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                const quint16 valueCount = qFromLittleEndian<quint16>(frame + 10);
                const quint32 payloadLength = qFromLittleEndian<quint32>(frame + 12);
                if (frame[4] == 0xFF && valueCount == 8 && payloadLength == 32 &&
                    frame[56] == 0xFF && frame[57] == 0xFF) {
                    emit modeADataReceived(m_buffer.mid(24, 32));
                    m_buffer.remove(0, 58);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x03) {
                const int frameBytes = 1050;
                if (m_buffer.size() < frameBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                const quint16 valueCount = qFromLittleEndian<quint16>(frame + 10);
                const quint32 payloadLength = qFromLittleEndian<quint32>(frame + 12);
                if (frame[4] == 0x04 && valueCount == 256 && payloadLength == 1024 &&
                    frame[1048] == 0xFF && frame[1049] == 0xFF) {
                    emit corrosionWaveDataReceived(frame[4], m_buffer.mid(24, 1024));
                    m_buffer.remove(0, frameBytes);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x04) {
                const int frameBytes = 74;
                if (m_buffer.size() < frameBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                const quint16 valueCount = qFromLittleEndian<quint16>(frame + 10);
                const quint32 payloadLength = qFromLittleEndian<quint32>(frame + 12);
                if (frame[4] == 0x05 && valueCount == 12 && payloadLength == 48 &&
                    frame[72] == 0xFF && frame[73] == 0xFF) {
                    emit leakWaveDataReceived(frame[4], m_buffer.mid(24, 48));
                    m_buffer.remove(0, frameBytes);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x05) {
                const int frameBytes = 2074;
                if (m_buffer.size() < frameBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                const quint16 valueCount = qFromLittleEndian<quint16>(frame + 10);
                const quint32 payloadLength = qFromLittleEndian<quint32>(frame + 12);
                if (frame[4] == 0x04 && valueCount == 512 && payloadLength == 2048 &&
                    frame[2072] == 0xFF && frame[2073] == 0xFF) {
                    emit algo67WaveDataReceived(frame[4], m_buffer.mid(24, 2048));
                    m_buffer.remove(0, frameBytes);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x06) {
                const int frameBytes = 1050;
                if (m_buffer.size() < frameBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                const quint16 valueCount = qFromLittleEndian<quint16>(frame + 10);
                const quint32 payloadLength = qFromLittleEndian<quint32>(frame + 12);
                if (frame[4] == 0x04 && valueCount == 256 && payloadLength == 1024 &&
                    frame[1048] == 0xFF && frame[1049] == 0xFF) {
                    emit algo1WaveDataReceived(frame[4], m_buffer.mid(24, 1024));
                    m_buffer.remove(0, frameBytes);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x30) {
                const int frameBytes = 58;
                if (m_buffer.size() < frameBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                if (qFromLittleEndian<quint16>(frame + 10) == 8 && qFromLittleEndian<quint32>(frame + 12) == 32 &&
                    frame[56] == 0xFF && frame[57] == 0xFF) {
                    emit voltageSnapshotReceived(m_buffer.mid(24, 32));
                    m_buffer.remove(0, frameBytes);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x33) {
                const int frameBytes = 58;
                if (m_buffer.size() < frameBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                if (qFromLittleEndian<quint16>(frame + 10) == 8 && qFromLittleEndian<quint32>(frame + 12) == 32 &&
                    frame[56] == 0xFF && frame[57] == 0xFF) {
                    emit currentSnapshotReceived(m_buffer.mid(24, 32));
                    m_buffer.remove(0, frameBytes);
                } else m_buffer.remove(0, 1);
            }
            else if (frameType == 0x32) {
                const int headerBytes = 30;
                if (m_buffer.size() < headerBytes) break;
                const uchar *frame = reinterpret_cast<const uchar *>(m_buffer.constData());
                const quint32 sampleCount = qFromLittleEndian<quint32>(frame + 14);
                const quint32 payloadLength = qFromLittleEndian<quint32>(frame + 18);
                const quint64 expectedPayloadBytes = static_cast<quint64>(sampleCount) * 4;

                if (frame[5] != 0x00 || frame[4] > 3 || sampleCount == 0 ||
                    sampleCount > 1048576 || payloadLength != expectedPayloadBytes) {
                    m_buffer.remove(0, 1);
                    continue;
                }

                const quint64 frameBytes64 = headerBytes + expectedPayloadBytes + 2;
                if (static_cast<quint64>(m_buffer.size()) < frameBytes64) break;

                const int frameBytes = static_cast<int>(frameBytes64);
                if ((quint8)m_buffer.at(frameBytes - 2) != 0xFF ||
                    (quint8)m_buffer.at(frameBytes - 1) != 0xFF) {
                    m_buffer.remove(0, 1);
                    continue;
                }

                emit vibrationDataReceived(frame[4], m_buffer.mid(headerBytes, static_cast<int>(expectedPayloadBytes)));
                m_buffer.remove(0, frameBytes);
            }
            else {
                m_buffer.remove(0, 1);
            }
        }
        else {
            m_buffer.remove(0, 1);
        }
    }
}
