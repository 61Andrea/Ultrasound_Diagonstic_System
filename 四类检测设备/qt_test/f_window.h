#ifndef F_WINDOW_H
#define F_WINDOW_H

#include <QDialog>
#include <QByteArray>
#include <QTimer>
#include <QLabel>
#include <QVector> // 【新增】确保包含 QVector
#include <QPaintEvent> // 🔴绘图事件头文件
#include <QPainter>    // 🔴画家类头文件
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QTableWidget>
#include <QList>
#include <QTextStream>
#include <QStringList>
#include <QtGlobal>

namespace Ui {
class F_window;
}

class F_window : public QDialog // 【关键修复】：继承 QDialog
{
    Q_OBJECT

private:
    // --- 🔴 关键修复1：将单条记录结构体移入类内部，防止和A窗口冲突 ---
    struct DiagRecord {
        QString time;        // 检测时间
        QString checkResult; // 检测结果 (正常/异常)
        QString diagDetail;  // 诊断详情 (-- / 轻度磨损 / 中度磨损 / 重度磨损)
    };

    struct AcqHistory {
        QDateTime startTime;
        QString duration;
    };

protected:
// 🔴 2. 换成这个：事件过滤器
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;


public slots:
    void updateDialAnimation(); // 专门负责让表针转圈的函数
    void onDiagDataReceived(const QByteArray& data); // 专门接收 4字节 诊断结果
    void onAlgo67WaveDataReceived(quint8 channelID, const QByteArray& payload); // 接收算法6/7 512点float波形

public:
    explicit F_window(QWidget *parent = nullptr);
    ~F_window() override; // 🔴 关键修复2：加上 override，消除警告

signals:
    void sendCommand(unsigned short cmd);
    void sendFrame(const QByteArray &frame);
    void sendFrameSignal(const QByteArray &frame); // 【新增这行】发AA55配置包用的

private slots:

    //数据采集
    void on_CJ_F1_clicked(); // 模式设置
    void on_CJ_F2_clicked(); // 开始采集
    void on_CJ_F3_clicked(); // 历史记录
    void on_CJ_F4_clicked(); // 文件查看
    void on_CJ_F5_clicked(); // 启动检测

    //设备记录
    void on_JL_F1_clicked(); // 模式设置
    void on_JL_F2_clicked(); // 启动存储
    void on_JL_F3_clicked(); // 文件查看
    void on_JL_F4_clicked(); // 文件删除
    void on_JL_F5_clicked(); // 文件上传





private:
    Ui::F_window *ui;

    QTimer *m_dialAnimationTimer; // 旋转动画定时器  0628

    // ==== 实时采集界面专属变量 ====  0628
    QList<DiagRecord> m_diagRecords;    // 存储最近10条记录的链表
    QString m_historyFilePath;          // 保存本地 csv 文件的路径
    void loadDiagHistory();                                      // 开机读取历史
    void updateHistoryFile();                                    // 覆盖保存历史
    void saveDiagResult(const QString &check, const QString &detail); // 存入新结果
    // ==== 实时采集界面专属变量 ====  0628

    // ==== 实时采集界面专属变量 ====
    // ============ 1. 抽帧显示与存盘控制 ============
    bool isAcquiring;          // 是否正在采集
    qint64 m_lastAlgo67PlotMs; // 算法6/7波形UI刷新节流时间戳
    QFile *currentSaveFile;    // 当前正在写入的文件指针
    QString saveDirectory;     // 保存文件夹路径

    // ============ 2. 历史记录列表 ============
    QList<AcqHistory> historyRecords; // 存储最近的记录
    QDateTime currentAcqStartTime;    // 当前这次采集的开始时间

    // ============ 3. 动态生成的表格UI ============
    QTableWidget *historyTable; // 历史记录表格
    QTableWidget *fileTable;    // 文件查看表格

    // ============ 辅助函数 ============
    void switchRightPanel(int mode); // 切换右侧面板显示 (0:波形, 1:历史, 2:文件)


// ==== 设备记录界面专属变量 ====
    bool isRemoteStoring;               // 记录下位机是否正在存储
    QTableWidget *remoteFileTable;      // 用于显示下位机文件的表格

// 🔴 新增：记录当前指针角度的成员变量
    int currentAngle;

    bool m_isDiagnosing;
};

#endif // F_WINDOW_H
