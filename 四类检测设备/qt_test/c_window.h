#ifndef C_WINDOW_H
#define C_WINDOW_H

#include <QDialog>
#include <QByteArray>
#include <QTimer>
#include <QLabel>
#include <QVector>
#include <QPaintEvent>
#include <QCloseEvent>
#include <QPainter>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QTableWidget>
#include <QList>
#include <QTextStream>
#include <QStringList>
#include <QtGlobal>

namespace Ui {
class C_window;
}

class C_window : public QDialog
{
    Q_OBJECT

private:
    // 将单条记录结构体移入类内部，防止冲突
    struct DiagRecord {
        QString time;        // 检测时间
        QString checkResult; // 检测结果 (正常/异常)
        QString diagDetail;  // 诊断详情
    };

    struct AcqHistory {
        QDateTime startTime;
        QString duration;
    };

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

public slots:
    void updateDialAnimation(); // 专门负责让表针转圈的函数
    void onDiagDataReceived(const QByteArray& data); // 专门接收 4字节 诊断结果
    void onCorrosionWaveDataReceived(quint8 channelID, const QByteArray& payload); // 接收腐蚀板256点float波形

public:
    explicit C_window(QWidget *parent = nullptr);
    ~C_window() override;

signals:
    void sendCommand(unsigned short cmd);
    void sendFrame(const QByteArray &frame);
    void sendFrameSignal(const QByteArray &frame); // 发AA55配置包用的

private slots:
    // 数据采集
    void on_CJ_C1_clicked(); // 模式设置
    void on_CJ_C2_clicked(); // 开始采集
    void on_CJ_C3_clicked(); // 历史记录
    void on_CJ_C4_clicked(); // 文件查看
    void on_CJ_C5_clicked(); // 启动检测

    // 设备记录
    void on_JL_C1_clicked(); // 模式设置
    void on_JL_C2_clicked(); // 启动存储
    void on_JL_C3_clicked(); // 文件查看
    void on_JL_C4_clicked(); // 文件删除
    void on_JL_C5_clicked(); // 文件上传

private:
    Ui::C_window *ui;

    QTimer *m_dialAnimationTimer; // 旋转动画定时器

    // ==== 实时采集界面专属变量 ====
    QList<DiagRecord> m_diagRecords;    // 存储最近10条记录的链表
    QString m_historyFilePath;          // 保存本地 csv 文件的路径
    void loadDiagHistory();             // 开机读取历史
    void updateHistoryFile();           // 覆盖保存历史
    void saveDiagResult(const QString &check, const QString &detail); // 存入新结果

    // ============ 1. 抽帧显示与存盘控制 ============
    bool isAcquiring;          // 是否正在采集
    qint64 m_lastCorrosionPlotMs; // 腐蚀板波形UI刷新节流时间戳
    QFile *currentSaveFile;    // 当前正在写入的文件指针
    QString saveDirectory;     // 保存文件夹路径

    // ============ 2. 历史记录列表 ============
    QList<AcqHistory> historyRecords; // 存储最近的记录
    QDateTime currentAcqStartTime;    // 当前这次采集的开始时间

    // ============ 3. 动态生成的表格UI ============
    QTableWidget *historyTable; // 历史记录表格
    QTableWidget *fileTable;    // 文件查看表格

    // ============ 辅助函数 ============
    void switchRightPanel(int mode); // 切换右侧面板显示

    // ==== 设备记录界面专属变量 ====
    bool isRemoteStoring;               // 记录下位机是否正在存储
    QTableWidget *remoteFileTable;      // 用于显示下位机文件的表格

    // 新增：记录当前指针角度的成员变量
    int currentAngle;

    // ✅ 修复：在这里添加状态机标志位声明并赋初值
    bool m_isDiagnosing ;

};

#endif // C_WINDOW_H
