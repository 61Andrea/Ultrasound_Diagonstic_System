#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox> // 用于弹出错误提示
#include <QtEndian>
#include <QVector>

// 引入你的子窗口头文件
#include "a_window.h"
#include "b_window.h"
#include "c_window.h"
#include "d_window.h"
#include "f_window.h"
#include "g_window.h"
#include "h_window.h"

// ==========================================
// 1. 构造函数 (不可删除：负责初始化界面)
// ==========================================








MainWindow::MainWindow(QWidget *parent)

    : QWidget(parent)
    , ui(new Ui::MainWindow)
    , m_deviceConnected(false)
    , dealFileThread(new QThread(this)) // 1. 实例化线程
    , dealFile(new DealFile())          // 2. 实例化通信对象
{

    ui->setupUi(this);
    this->setWindowTitle("专用检测设备");

    // 强制绑定动力轴系图片
    ui->image_label_1->setPixmap(QPixmap(":/images/images/axis.jpg"));
    ui->image_label_1->setScaledContents(true);

    // 强制绑定齿轮箱图片
    ui->image_label_2->setPixmap(QPixmap(":/images/images/gearbox.jpg"));
    ui->image_label_2->setScaledContents(true);

    // 强制绑定舰炮图片
    ui->image_label_3->setPixmap(QPixmap(":/images/images/gun.jpg"));
    ui->image_label_3->setScaledContents(true);

    // 强制绑定最上面的大标题背景 (假设你的标题 QLabel 叫 label_top_bg，请换成你实际的名字)
    ui->label_4->setPixmap(QPixmap(":/images/images/HIT.png"));
    ui->label_4->setScaledContents(true);


    // --- 网络通信多线程初始化 ---
    dealFile->moveToThread(dealFileThread); // 把通信扔到后台线程，防止界面卡死

    // 绑定主界面和子线程的信号槽
    connect(this, &MainWindow::startConnection, dealFile, &DealFile::connectToServer);
    connect(this, &MainWindow::disconnectFromServer, dealFile, &DealFile::disconnectFromServer);

    // 新增：绑定发送数据的信号槽
    connect(this, &MainWindow::sendDataToWorker, dealFile, &DealFile::sendData);

    // 🔴 修复：把你之前注释掉的这一行恢复并写对！(这是接收子线程连接状态的关键)
    connect(dealFile, &DealFile::connectionStatusChanged, this, &MainWindow::updateConnectionStatus);


    //发送超声配置参数
    connect(this, &MainWindow::sendFrameToWorker, dealFile, &DealFile::sendFrame);

    // 启动后台线程
    dealFileThread->start();


    // 初始化定时器/////////////////////////////////////////////////////////////////////////////////////////////主界面右上角的灯
    m_blinkTimer = new QTimer(this);
    connect(m_blinkTimer, &QTimer::timeout, this, &MainWindow::updateBlink);
    m_isLightOn = false;

    // 默认让它显示为暗绿色
    setConnectionState(State_Off);


    // ---  新增：60秒超时定时器 ---
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true); // 设定为只触发一次
    connect(m_timeoutTimer, &QTimer::timeout, this, &MainWindow::onConnectionTimeout);


    // ==========================================
    //5秒重连定时器
    // ==========================================
    m_retryTimer = new QTimer(this);
    connect(m_retryTimer, &QTimer::timeout, this, &MainWindow::onRetryConnection);

    // ==========================================
    //  核心操作：程序一运行，立刻自动发起连接！
        // ==========================================
    m_targetIP = "192.168.1.10";      // 请替换为你真实的下位机IP
    //m_targetIP = "127.0.0.1";      // 测试回环IP
    m_targetPort = 5001;             // 请替换为真实的端口号

    setConnectionState(State_Connecting); // 1. UI 绿灯开始闪烁
    m_timeoutTimer->start(60000);         // 2. 开启 60 秒 (60000毫秒) 倒计时
    m_retryTimer->start(5000);                  // 3. 每 5 秒自动重试一次
    emit startConnection(m_targetIP, m_targetPort); // 4. 命令后台子线程去连接！
    //灯////////////////////////////////////////////////////////////////////////////////////////////////////////灯



}




MainWindow::~MainWindow()
{
    // 安全退出线程并清理内存
    emit disconnectFromServer();
    dealFileThread->quit();
    dealFileThread->wait();
    delete dealFile;
    delete ui;
}

// ==========================================
// 灯
// ==========================================
void MainWindow::setConnectionState(ConnectionState state)
{
    //新增：如果状态是 State_Off，设为暗绿色并停止闪烁
    if (state == State_Off) {
        m_blinkTimer->stop();
        ui->label_status_light->setStyleSheet("background-color: #053305; border-radius: 15px; border: 2px solid #005500;");
        return;
    }

    if (state == State_Connecting) {
        if (!m_blinkTimer->isActive()) {
            m_blinkTimer->start(500);
        }
    } else {
        m_blinkTimer->stop();
        m_isLightOn = true;

        if (state == State_Connected) {
            ui->label_status_light->setStyleSheet("background-color: #00ff00; border-radius: 15px; border: 2px solid #005500;");
        } else if (state == State_Failed) {
            ui->label_status_light->setStyleSheet("background-color: #ff0000; border-radius: 15px; border: 2px solid #550000;");
        }
    }
}

// 核心逻辑：绿灯闪烁的切换动作
void MainWindow::updateBlink()
{
    if (m_isLightOn) {
        // 熄灭：变成很暗的绿色，模拟灭灯效果
        ui->label_status_light->setStyleSheet("background-color: #053305; border-radius: 15px; border: 2px solid #005500;");
    } else {
        // 点亮：变成亮绿色
        ui->label_status_light->setStyleSheet("background-color: #00ff00; border-radius: 15px; border: 2px solid #005500;");
    }
    m_isLightOn = !m_isLightOn; // 标志位取反
}
// ==========================================
// ==========================================
// 处理网络连接状态的改变
// ==========================================
void MainWindow::updateConnectionStatus(bool connected)
{
    m_deviceConnected = connected;

    if (connected) {
        // 连上了！赶紧让定时器停手
        m_timeoutTimer->stop();
        m_retryTimer->stop();

        setConnectionState(State_Connected); // 绿灯常亮
    } else {
        // 优化判断逻辑
        // 如果 60 秒定时器还在运行，说明我们还在“努力重试”的窗口期，此时不要亮红灯，继续让它闪绿灯！
        // 只有当 60 秒定时器不活跃了（说明真的彻底放弃了），或者用户自己拔了网线，才亮红灯。
        if (!m_timeoutTimer->isActive()) {
            setConnectionState(State_Failed); // 红灯常亮
        }
    }
}

// ==========================================
// 新增：处理 5 秒重试事件
// ==========================================
void MainWindow::onRetryConnection()
{
    // 只要这个定时器没被停掉，它每隔 5 秒就会给子线程下达一次连接命令
    emit startConnection(m_targetIP, m_targetPort);
}

// ==========================================
// 处理 60 秒超时事件
// ==========================================
void MainWindow::onConnectionTimeout()
{
    // 60 秒到了！
    m_retryTimer->stop(); // 🔴 强制命令 5 秒重试定时器停手，不要再试了

    setConnectionState(State_Failed); // 红灯常亮
    emit disconnectFromServer();      // 掐断底层 socket
}







// ==========================================
// 3. 按钮槽函数具体实现
// ==========================================

// ==============================================================
// 【动力轴系】大分组按钮控制
// ==============================================================
// 按钮 A：艉轴承磨损异常检测

void MainWindow::on_A_button_clicked() {

    // 🔴 1. 拦截判断：如果当前已经有窗口打开了，弹出提示并终止执行
    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有检测窗口正在运行，请先关闭该窗口！");
        return;
    }

    // 1. 发送底层指令
    //emit sendDataToWorker(101);

    // 2. 实例化并弹出 A_window 界面
    A_window *aWindow = new A_window();
    aWindow->setAttribute(Qt::WA_DeleteOnClose);

    // [接收]：把 DealFile 解析出的波形数据，传给 A_window 去画图
    connect(dealFile, &DealFile::waveDataReceived, aWindow, &A_window::onWaveDataReceived);

    // 🔴【新加这一行】：把 DealFile 解析出的【短包诊断结果】，传给 A_window 控制表盘
    connect(dealFile, &DealFile::diagDataReceived, aWindow, &A_window::onDiagDataReceived);

    // [发送]：把 A_window 里发出的整型指令，交给 DealFile 去发送
    connect(aWindow, &A_window::sendCommand, dealFile, &DealFile::sendData);

    // [发送2]：把 A_window 里发出的十六进制字节流，交给 DealFile 去发送
    connect(aWindow, &A_window::sendFrameSignal, dealFile, &DealFile::sendFrame);

    // 3. 显示窗口
    aWindow->show();

    // 🔴 2. 登记造册：记录当前活动的窗口就是 A 窗口
    m_activeChildWindow = aWindow;

}

// 按钮 B：艉轴密封装置泄露异常检测
void MainWindow::on_B_button_clicked() {

    // 🔴 1. 拦截判断
    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有检测窗口正在运行，请先关闭该窗口！");
        return;
    }

    // 1. 发送底层指令
    //emit sendDataToWorker(201);

    // 2. 实例化并弹出 B_window 界面
    B_window *bWindow = new B_window();
    bWindow->setAttribute(Qt::WA_DeleteOnClose);

    // [接收]：把 DealFile 解析出的波形数据，传给 A_window 去画图
    // 👇 改成下面这样：
    connect(dealFile, &DealFile::modeADataReceived, bWindow, &B_window::onModeADataReceived);

    // B窗口的诊断流程与A窗口一致，接收4字节诊断结果。
    connect(dealFile, &DealFile::diagDataReceived, bWindow, &B_window::onDiagDataReceived);

    // [发送]：把 A_window 里发出的整型指令，交给 DealFile 去发送
    connect(bWindow, &B_window::sendCommand, dealFile, &DealFile::sendData);

    // [发送2]：把 A_window 里发出的十六进制字节流，交给 DealFile 去发送
    connect(bWindow, &B_window::sendFrameSignal, dealFile, &DealFile::sendFrame);

    // 3. 显示窗口
    bWindow->show();

    // 🔴 2. 登记造册
    m_activeChildWindow = bWindow;

}
// ==============================================================
// 【齿轮箱】大分组按钮控制
// ==============================================================
// 按钮 C：齿轮箱体腐蚀异常检测
void MainWindow::on_C_button_clicked()
{

    // 🔴 1. 拦截判断
    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有检测窗口正在运行，请先关闭该窗口！");
        return;
    }

    //emit sendDataToWorker(301);


    // 2. 实例化并弹出 C_window 界面
    C_window *cWindow = new C_window();
    cWindow->setAttribute(Qt::WA_DeleteOnClose);

    // C窗口接收腐蚀板256点float上传波形。
    connect(dealFile, &DealFile::corrosionWaveDataReceived,
            cWindow, &C_window::onCorrosionWaveDataReceived);

    // C窗口接收4字节检测/诊断结果。
    connect(dealFile, &DealFile::diagDataReceived, cWindow, &C_window::onDiagDataReceived);

    // [发送]：把 C_window 里发出的整型指令，交给 DealFile 去发送
    connect(cWindow, &C_window::sendCommand, dealFile, &DealFile::sendData);

    // 🔴 [发送2]：把 C_window 里发出的十六进制字节流，交给 DealFile 去发送
    // =======================================================
    connect(cWindow, &C_window::sendFrameSignal, dealFile, &DealFile::sendFrame);


    // 3. 显示窗口
    cWindow->show();

    // 🔴 2. 登记造册
    m_activeChildWindow = cWindow;


}

// 按钮 D：齿轮箱漏油异常检测
void MainWindow::on_D_button_clicked() {

    // 🔴 1. 拦截判断
    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有检测窗口正在运行，请先关闭该窗口！");
        return;
    }

    // 1. 发送底层指令
    //emit sendDataToWorker(401);

    // 2. 实例化并弹出 D_window 界面
    D_window *dWindow = new D_window();
    dWindow->setAttribute(Qt::WA_DeleteOnClose);

    // D窗口接收漏油12点float上传波形。
    connect(dealFile, &DealFile::leakWaveDataReceived,
            dWindow, &D_window::onLeakWaveDataReceived);

    // D窗口接收4字节检测/诊断结果。
    connect(dealFile, &DealFile::diagDataReceived, dWindow, &D_window::onDiagDataReceived);

    // [发送]：把 D_window 里发出的整型指令，交给 DealFile 去发送
    connect(dWindow, &D_window::sendCommand, dealFile, &DealFile::sendData);

    // [发送2]：把 D_window 里发出的十六进制字节流，交给 DealFile 去发送
    connect(dWindow, &D_window::sendFrameSignal, dealFile, &DealFile::sendFrame);

    // 3. 显示窗口
    dWindow->show();

    // 🔴 2. 登记造册
    m_activeChildWindow = dWindow;

}


// ==============================================================
// 【舰炮】大分组按钮控制
// ==============================================================
// 按钮 F：舰炮撞针断裂异常检测
void MainWindow::on_F_button_clicked() {

    // 🔴 1. 拦截判断
    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有检测窗口正在运行，请先关闭该窗口！");
        return;
    }


    // 1. 发送底层指令 (F窗口对应的是 501)
    //emit sendDataToWorker(501);

    // 2. 实例化并弹出 F_window 界面
    F_window *fWindow = new F_window();
    fWindow->setAttribute(Qt::WA_DeleteOnClose);

    // F窗口接收算法6/7 512点float上传波形。
    connect(dealFile, &DealFile::algo67WaveDataReceived,
            fWindow, &F_window::onAlgo67WaveDataReceived);

    // F窗口接收4字节检测/诊断结果。
    connect(dealFile, &DealFile::diagDataReceived, fWindow, &F_window::onDiagDataReceived);

    // [发送]：把 F_window 里发出的整型指令，交给 DealFile 去发送
    connect(fWindow, &F_window::sendCommand, dealFile, &DealFile::sendData);

    // [发送2]：把 F_window 里发出的十六进制字节流，交给 DealFile 去发送
    connect(fWindow, &F_window::sendFrameSignal, dealFile, &DealFile::sendFrame);

    // 3. 显示窗口
    fWindow->show();

    // 🔴 2. 登记造册
    m_activeChildWindow = fWindow;

}


// 按钮 G：舰炮密封装置漏水异常检测

void MainWindow::on_G_button_clicked() {

    // 🔴 1. 拦截判断
    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有检测窗口正在运行，请先关闭该窗口！");
        return;
    }

    // 1. 发送底层指令 (G窗口对应的是 601)
    //emit sendDataToWorker(601);

    // 2. 实例化并弹出 G_window 界面
    G_window *gWindow = new G_window();
    gWindow->setAttribute(Qt::WA_DeleteOnClose);

    // G窗口接收算法1 256点float上传波形。
    connect(dealFile, &DealFile::algo1WaveDataReceived,
            gWindow, &G_window::onAlgo1WaveDataReceived);

    // G窗口接收4字节检测/诊断结果。
    connect(dealFile, &DealFile::diagDataReceived, gWindow, &G_window::onDiagDataReceived);

    // [发送]：把 G_window 里发出的整型指令，交给 DealFile 去发送
    connect(gWindow, &G_window::sendCommand, dealFile, &DealFile::sendData);

    // [发送2]：把 G_window 里发出的十六进制字节流，交给 DealFile 去发送
    connect(gWindow, &G_window::sendFrameSignal, dealFile, &DealFile::sendFrame);

    // 3. 显示窗口
    gWindow->show();

    // 🔴 2. 登记造册
    m_activeChildWindow = gWindow;

}


// ==============================================================
// 【信号监测功能】按钮 H —— 由 自测QT(YB/QT_zice) 迁移而来的子界面
// 与 A~G 六个检测窗口同级：同样受 m_activeChildWindow 互斥保护，
// 同样共用后台 DealFile 的那一条 TCP 连接（不再自建 socket）。
// ==============================================================
void MainWindow::on_H_button_clicked() {

    if (m_activeChildWindow) {
        QMessageBox::warning(this, "提示", "已有窗口正在运行，请先关闭该窗口！");
        return;
    }

    H_window *hWindow = new H_window();
    hWindow->setAttribute(Qt::WA_DeleteOnClose);

    // 1. 振动 (注意：你原来代码里漏了绑振动，我在这里帮你补上，不然振动也没数据！)
    connect(dealFile, &DealFile::vibrationDataReceived, this,
            [this, hWindow](quint8 channelID, const QByteArray &payload) {
        if (m_activeChildWindow == hWindow) {
            hWindow->onVibrationDataReceived(channelID, payload);
        }
    });

    // 2. 超声 (只保留这个带有 channelID 的双参数版本)
    connect(dealFile, &DealFile::ultrasonicDataReceived, this,
            [this, hWindow](quint8 channelID, const QByteArray &payload) {
        if (m_activeChildWindow == hWindow) {
            hWindow->onUltrasonicDataReceived(channelID, payload);
        }
    });

    // 3. 电压
    connect(dealFile, &DealFile::voltageSnapshotReceived, this,
            [this, hWindow](const QByteArray &payload) {
        if (m_activeChildWindow == hWindow) {
            hWindow->onVoltageSnapshotReceived(payload);
        }
    });

    // 4. 电流
    connect(dealFile, &DealFile::currentSnapshotReceived, this,
            [this, hWindow](const QByteArray &payload) {
        if (m_activeChildWindow == hWindow) {
            hWindow->onCurrentSnapshotReceived(payload);
        }
    });

    // [连接状态]
    connect(dealFile, &DealFile::connectionStatusChanged,
            hWindow, &H_window::setDeviceConnected);
    hWindow->setDeviceConnected(m_deviceConnected);

    // [发送]
    connect(hWindow, &H_window::sendCommand, dealFile, &DealFile::sendData);
    connect(hWindow, &H_window::sendFrameSignal, dealFile, &DealFile::sendFrame);

    // ==========================================
        // 【新增】：文件管理系统的三个关键信号连接
        // ==========================================
        // 1. 接收查询到的文件列表并抛给 H 窗口显示
        connect(dealFile, &DealFile::fileListReceived, this,
                [this, hWindow](const QStringList &fileList) {
            if (m_activeChildWindow == hWindow) {
                hWindow->onFileListReceived(fileList);
            }
        });

        // 2. 接收流式下载的数据块并抛给 H 窗口存盘
        connect(dealFile, &DealFile::fileDataReceived, this,
                [this, hWindow](const QByteArray &data) {
            if (m_activeChildWindow == hWindow) {
                hWindow->onFileDataReceived(data);
            }
        });

        // 3. 接收单个文件下载完成标志，让 H 窗口触发队列里的下一个文件
        connect(dealFile, &DealFile::fileTransferFinished, this,
                [this, hWindow]() {
            if (m_activeChildWindow == hWindow) {
                hWindow->onFileTransferFinished();
            }
        });

    hWindow->show();
    m_activeChildWindow = hWindow;
}


