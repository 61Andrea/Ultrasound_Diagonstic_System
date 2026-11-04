#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QThread>
#include "dealfile.h"
#include <QTimer> // 1. 引入定时器头文件
// 在顶部 include 区域添加
#include <QPointer>
#include <QWidget>


QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 2. 定义三种连接状态
    enum ConnectionState
    {

        State_Off,        // 初始熄灭状态（暗绿色）
        State_Connecting, // 连接中 (绿灯闪)
        State_Connected,  // 已连接 (绿灯常亮)
        State_Failed      // 失败/断开 (红灯常亮)
    };


    // 3. 添加一个可以被外部或内部调用的状态切换函数
    void setConnectionState(ConnectionState state);

signals:
    // 告诉子线程去连接服务器的信号
    void startConnection(const QString &ip, int port);
    // 告诉子线程断开连接的信号
    void disconnectFromServer();

    // 新增：发送数据给后台线程的信号
    void sendDataToWorker(unsigned short cmd);

    void sendFrameToWorker(const QByteArray &frame);


private slots:

    // 4. 用于控制闪烁的槽函数
    void updateBlink();


    // ==========================================
    // 导航槽函数：点击按钮弹出对应的子界面
    // ==========================================

    // 1. 动力轴系
    void on_A_button_clicked(); // 艉轴承磨损异常检测
    void on_B_button_clicked(); // 艉轴密封装置泄露异常检测

    // 2. 齿轮箱
    void on_C_button_clicked(); // 齿轮箱体腐蚀异常检测
    void on_D_button_clicked(); // 齿轮箱漏油异常检测

    // 3. 舰炮
    void on_F_button_clicked(); // 舰炮撞针断裂异常检测
    void on_G_button_clicked(); // 舰炮密封装置漏水异常检测

    // 4. 信号监测（H窗口，由 自测QT 迁移而来，与检测协议共用同一条 TCP 连接）
    void on_H_button_clicked();

    //  新增：用来接收 DealFile 传来的连接成功/失败状态
        void updateConnectionStatus(bool connected);

    //  新增：用来处理 60 秒连接超时的函数
    void onConnectionTimeout();
    //  新增：5秒重试定时器的槽函数
    void onRetryConnection();


private:
    Ui::MainWindow *ui;

    // 5. 定时器和灯光状态标志
    QTimer *m_blinkTimer;
    bool m_isLightOn;
    // 新增：超时定时器指针
    QTimer *m_timeoutTimer;
    // 2. 新增：5秒重试定时器 和 目标IP/端口
    QTimer *m_retryTimer;
    QString m_targetIP;
    int m_targetPort;

    // 新增：用来记录当前唯一打开的子窗口
    QPointer<QWidget> m_activeChildWindow;

    // 新增：记录当前 TCP 连接状态（用于新建子窗口时同步初始状态）
    bool m_deviceConnected;



    QThread *dealFileThread;
    DealFile *dealFile;

};

#endif // MAINWINDOW_H
