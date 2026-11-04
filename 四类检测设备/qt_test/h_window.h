#ifndef H_WINDOW_H
#define H_WINDOW_H

// =====================================================================
// H 窗口：信号监测功能
// 由 自测QT(YB/QT_zice) 的 MainWindow 整体迁移而来，降级为 qt_test 的
// 一个子界面（QDialog，与 A~G 六个检测窗口同级）。
//
// 【关键差异】本窗口不持有 QTcpSocket。收发全部通过信号交给后台线程的
// DealFile 统一处理，与 A~G 共用同一条 TCP 连接。
// =====================================================================

#include <QDialog>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QFile>

namespace Ui {
class H_window;
}

class H_window : public QDialog
{
    Q_OBJECT

public:
    explicit H_window(QWidget *parent = nullptr);
    ~H_window() override;

signals:
    // 交给 DealFile 发送：2 字节小端指令
    void sendCommand(unsigned short cmd);
    // 交给 DealFile 发送：整串字节流（超声配置帧）
    void sendFrameSignal(const QByteArray &frame);

public slots:
    // ---- 接收 DealFile 解析后的数据 ----
    void onVibrationDataReceived(quint8 channel, const QByteArray &payload);
    void onVoltageSnapshotReceived(const QByteArray &payload);
    void onCurrentSnapshotReceived(const QByteArray &payload);
    void onUltrasonicDataReceived(quint8 channelID, const QByteArray &payload);
    // 由主界面在收到 connectionStatusChanged 时转告本窗口，用于刷新状态提示
    void setDeviceConnected(bool connected);
    void onFileListReceived(const QStringList &fileList); // 接收查询到的文件列表
    void onFileDataReceived(const QByteArray &data);      // 接收文件内容写入本地
    void onFileTransferFinished();                        // 接收单文件下载结束标志

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void on_vibrationStartButton_clicked();
    void on_voltageStartButton_clicked();
    void on_currentStartButton_clicked();
    void on_ultrasonicStartButton_clicked();
    void on_btnQueryFile_clicked();
    void on_btnReadFile_clicked();
    void on_btnDeleteFile_clicked();

private:
    void applyInterfaceScale();
    void showStatus(const QString &message, int timeoutMs = 3000);

    // 解析辅助（原 自测QT::MainWindow 的私有静态函数）
    static quint32 readLittleEndianU32(const QByteArray &data, int offset);
    static quint16 readLittleEndianU16(const QByteArray &data, int offset);
    static QByteArray commandLittleEndianU16(quint16 command);
    static QByteArray build5007StatusPacket(int firstStatusIndex, int statusCount);
    QStringList m_downloadQueue;     // 待下载文件名的排队队列
    QFile *m_currentDownloadFile;    // 当前正在写入的下载文件指针
    QString m_currentFileName;       // 当前正在下载的文件名
    void processNextDownload();      // 处理下载队列的核心函数

    Ui::H_window *ui;

    // ---- 振动 ----
    int activeVibrationChannel;
    bool vibrationAcquiring;
    bool vibrationStartPending;

    // ---- 快照 ----
    bool voltageSnapshotPending;
    bool currentSnapshotPending;

    // ---- 超声 ----
    int activeUltrasonicChannel;
    int displayUltrasonicUiChannel;
    bool ultrasonicStartPending;
    bool ultrasonicAcquiring;
    quint32 ultrasonicPacketSequence;

    // ---- 字号自适应：存放当前的缩放百分比（100 = 基准） ----
    bool interfaceReady;
    int interfaceScale;

    // ---- 状态栏文案的定时清除令牌 ----
    qint64 m_statusToken;

    bool deviceConnected;
};

#endif // H_WINDOW_H
