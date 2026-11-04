#ifndef DEALFILE_H
#define DEALFILE_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>

// =====================================================================
// 后台通信对象（运行在独立 QThread 中）
//
// 负责全系统唯一的一条 TCP 连接，并把两套下位机协议统一解包：
//   1) qt_test 检测协议   —— AA55 10/11 短包、AA55 01 01~06 业务包
//   2) 信号监测协议       —— AA55 00/01 40 超声、AA55 01 30 电压快照、
//                            AA55 01 32 振动、AA55 01 33 电流快照
// 两类帧的 (第3字节, 第4字节) 组合互不重叠，因此可以安全地合并分流。
// =====================================================================

class DealFile : public QObject
{
    Q_OBJECT
public:
    explicit DealFile(QObject *parent = nullptr);
    ~DealFile();

signals:
    // --- 连接与错误 ---
    void connectionStatusChanged(bool connected);    // 连接状态改变信号
    void errorOccurred(const QString &error);        // 错误报警信号

    // --- qt_test 检测协议 ---
    void dataReceived(const QByteArray &data);       // 接收到完整数据包的信号
    void waveDataReceived(quint8 channelID, const QByteArray &payload);//(波形专用)

    // B窗口模式A：完整58字节帧校验通过后，转发其中32字节载荷
    void modeADataReceived(const QByteArray& payload);

    // C窗口腐蚀板上传：完整1050字节帧校验通过后，转发其中1024字节载荷
    void corrosionWaveDataReceived(quint8 channelID, const QByteArray& payload);

    // D窗口漏油上传：完整74字节帧校验通过后，转发其中48字节载荷
    void leakWaveDataReceived(quint8 channelID, const QByteArray& payload);

    // F窗口算法6/7上传：完整2074字节帧校验通过后，转发其中2048字节载荷
    void algo67WaveDataReceived(quint8 channelID, const QByteArray& payload);

    // G窗口算法1上传：完整1050字节帧校验通过后，转发其中1024字节载荷
    void algo1WaveDataReceived(quint8 channelID, const QByteArray& payload);

    void diagDataReceived(const QByteArray &data); // 发射诊断结果信号  CJ_A5

    // --- 信号监测（H窗口）协议 ---
    // 振动波形：AA 55 01 32 <ch> ... FF FF，载荷为 采样点数 * 4 字节
    void vibrationDataReceived(quint8 channelID, const QByteArray &payload);
    // 八通道电压快照：AA 55 01 30，载荷 8 * float32
    void voltageSnapshotReceived(const QByteArray &payload);
    // 八通道电流快照：AA 55 01 33，载荷 8 * float32
    void currentSnapshotReceived(const QByteArray &payload);
    // 超声变长包：AA 55 <ch> 40 <字节数 u16> ... FF FF，载荷为 qint32 序列
    void ultrasonicDataReceived(quint8 channelID, const QByteArray &payload);

    void fileListReceived(const QStringList &fileList); // 抛出文件列表
    void fileDataReceived(const QByteArray &data);      // 抛出文件数据块
    void fileTransferFinished();                        // 抛出文件传输结束标志


public slots:
    // --- 接收外部指令的槽函数 ---
    void connectToServer(const QString &address, int port); // 发起连接
    void disconnectFromServer();                            // 断开连接
    void sendData(unsigned short cmd);                      // 发送数据指令
    void sendFrame(const QByteArray &frame);                // 发送一整串字节流

private slots:
    // --- 内部处理 Socket 状态的槽函数 ---
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError socketError);
    void onReadyRead(); // 核心：读取并解析网络数据

private:
    QTcpSocket *tcpSocket;
    QByteArray m_buffer; // 作为类的私有成员，避免危险的 static 缓存
    bool m_isFileDownloading = false; // 是否处于文件流接收模式
    int m_expectedFileBlockSize = -1; // 期待的下一个文件块大小
    QByteArray m_fileDataCache;
};

#endif // DEALFILE_H
