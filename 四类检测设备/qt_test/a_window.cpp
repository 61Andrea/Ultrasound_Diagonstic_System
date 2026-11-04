#include "a_window.h"
#include "ui_a_window.h"
#include <QHBoxLayout>
#include <QtMath>
#include <QTime>
#include <QVector>
#include <QDir>
#include <QMessageBox>
// 引入头文件（加在 a_window.cpp 最上面）
#include <QCoreApplication>
#include <QCloseEvent>
#include <QPushButton> // 🔴 新增：为了使用 QPushButton

A_window::A_window(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::A_window)
{
    ui->setupUi(this);
    this->setWindowTitle("艉轴承磨损异常检测系统");

    // 1. 初始化角度
    currentAngle = 0;

    // 2. 初始化动画定时器
    m_dialAnimationTimer = new QTimer(this);
    connect(m_dialAnimationTimer, &QTimer::timeout, this, &A_window::updateDialAnimation);

    // 3. 初始化存盘路径 (🔴 必须先指定总目录，再拼接 csv 路径)
    saveDirectory = QCoreApplication::applicationDirPath() + "/Data/test_save";

    QDir dir(saveDirectory);
    if (!dir.exists()) {
        dir.mkpath(saveDirectory); // 现在绝对不会失败了，因为软件本身能运行，这块盘肯定存在
    }

    // 诊断记录也存在这个目录下
    m_historyFilePath = saveDirectory + "/diag_history.csv";


    currentSaveFile = new QFile(this);
    isAcquiring = false;
    lastPlotTime = 0;

    // 4. 统一的暗黑科技风表格样式表
    QString tableStyle =
        "QTableWidget {"
        "   background-color: #050a14;" /* 表格数据区：极深蓝底色 */
        "   color: #00e5ff;"            /* 表格数据区：亮青色文字 */
        "   gridline-color: #1e3a5f;"   /* 网格线：深钢蓝色 */
        "   border: 1px solid #1e3a5f;" /* 表格外边框 */
        "}"
        "QHeaderView::section {"
        "   background-color: #142542;" /* 表头底色改为深钢蓝 */
        "   color: #ffffff;"            /* 表头文字改为纯白 */
        "   font-weight: bold;"
        "   font-size: 16px;"
        "   border: 1px solid #1e3a5f;"
        "   padding: 4px;"
        "}"
        "QTableCornerButton::section {"
        "   background-color: #142542;"
        "   border: 1px solid #1e3a5f;"
        "}"
        "QTableWidget::item:selected {"
        "   background-color: #00e5ff;" /* 选中时：青底 */
        "   color: #050a14;"            /* 选中时：深字 */
        "}";

    // 5. 动态创建：诊断历史记录表格 (对应 CJ_A3)
    historyTable = new QTableWidget(this);
    historyTable->setColumnCount(3);
    historyTable->setHorizontalHeaderLabels({"检测时间", "检测结果", "诊断详情"});
    historyTable->horizontalHeader()->setStretchLastSection(true);
    historyTable->verticalHeader()->setVisible(false);
    historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyTable->setStyleSheet(tableStyle);
    ui->verticalLayout_display->addWidget(historyTable);
    historyTable->hide();

    // 6. 动态创建：本地文件查看表格 (对应 CJ_A4)
    fileTable = new QTableWidget(this);
    fileTable->setColumnCount(4); // 🔴 改为 4 列
    // 🔴 表头加上“存储位置”
    fileTable->setHorizontalHeaderLabels({"文件名", "创建时间", "文件大小", "存储位置"});
    fileTable->horizontalHeader()->setStretchLastSection(true);
    fileTable->verticalHeader()->setVisible(false);
    fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fileTable->setStyleSheet(tableStyle);
    ui->verticalLayout_display->addWidget(fileTable);
    fileTable->hide();

    // 7. 动态创建：下位机远程文件表格 (对应 JL_A3)
    isRemoteStoring = false;
    remoteFileTable = new QTableWidget(this);
    remoteFileTable->setColumnCount(3);
    remoteFileTable->setHorizontalHeaderLabels({"下位机文件名", "创建时间", "文件大小"});
    remoteFileTable->horizontalHeader()->setStretchLastSection(true);
    remoteFileTable->verticalHeader()->setVisible(false);
    remoteFileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    remoteFileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    remoteFileTable->setStyleSheet(tableStyle);
    if (ui->JL_screen->layout() == nullptr) {
        QVBoxLayout *jlLayout = new QVBoxLayout(ui->JL_screen);
        jlLayout->setContentsMargins(0, 0, 0, 0);
    }
    ui->JL_screen->layout()->addWidget(remoteFileTable);
    remoteFileTable->hide();

    // 8. 界面初始化完后，立刻从硬盘读取前 10 条历史记录！
    loadDiagHistory();

    // 9. 智能诊断表盘 UI 初始化
    ui->frame_dial_a->installEventFilter(this);
    ui->frame_dial_a->setParent(this);
    ui->label_diag_status_a->setParent(this);
    ui->frame_dial_a->setFixedSize(300, 300);

    // ===== 🔴 修改：调整提示框的大小与字体 =====
    ui->label_diag_status_a->setFixedSize(380, 110);
    ui->label_diag_status_a->setAlignment(Qt::AlignCenter);
    ui->label_diag_status_a->setStyleSheet(
        "QLabel {"
        "   background-color: #050a14;"
        "   border: 2px solid #1e3a5f;"
        "   border-radius: 4px;"
        "   color: #7b90ad;"
        "   font-size: 28px;"
        "   font-family: '华文仿宋';"
        "}"
    );
    ui->label_diag_status_a->setText("诊断进度展示");

    // ===== 🔴 修改：为“是”和“否”按钮创建一个垂直布局 =====
    QVBoxLayout *labelAndBtnLayout = new QVBoxLayout();
    labelAndBtnLayout->addWidget(ui->label_diag_status_a);
    labelAndBtnLayout->setAlignment(Qt::AlignCenter);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnYesDiag = new QPushButton("是", this);
    QPushButton *btnNoDiag = new QPushButton("否", this);
    btnYesDiag->setObjectName("btnYesDiag"); // 设置对象名以便后续查找
    btnNoDiag->setObjectName("btnNoDiag");
    btnYesDiag->setFixedSize(120, 45); // 调整按钮大小
    btnNoDiag->setFixedSize(120, 45);

    // 匹配暗黑科技风的按钮样式，放大字体
    QString btnStyle = "QPushButton { background-color: #142542; color: #00e5ff; border: 1px solid #1e3a5f; border-radius: 4px; font-weight: bold; font-size: 20px; }"
                       "QPushButton:hover { background-color: #00e5ff; color: #050a14; }";
    btnYesDiag->setStyleSheet(btnStyle);
    btnNoDiag->setStyleSheet(btnStyle);

    btnLayout->addStretch();
    btnLayout->addWidget(btnYesDiag);
    btnLayout->addSpacing(30);
    btnLayout->addWidget(btnNoDiag);
    btnLayout->addStretch();

    labelAndBtnLayout->addLayout(btnLayout);
    btnYesDiag->hide();
    btnNoDiag->hide();

    // 绑定按钮点击事件
        connect(btnYesDiag, &QPushButton::clicked, this, [this, btnYesDiag, btnNoDiag]() {
            btnYesDiag->hide();
            btnNoDiag->hide();
            ui->label_diag_status_a->setText("检测结果：异常\n正在进行诊断...");
            m_dialAnimationTimer->start(500);

            m_isDiagnosing = true;

            // ===== 🔴 1. 新增：开启详细诊断的拦截标记与独立缓存 =====
            this->setProperty("lockDetailedDiag", true);
            this->setProperty("cachedDetailedData", QByteArray());

            emit sendCommand(331); // A窗口的详细诊断开启指令

            // ===== 🔴 2. 新增：详细诊断的 10 分钟倒计时解锁机制 =====
            QTimer::singleShot(600000, this, [this]() {
                this->setProperty("lockDetailedDiag", false); // 10分钟到，解除详细诊断锁定
                QByteArray cachedData = this->property("cachedDetailedData").toByteArray();
                if (!cachedData.isEmpty()) {
                    this->onDiagDataReceived(cachedData);
                    this->setProperty("cachedDetailedData", QByteArray());
                }
            });
        });

        connect(btnNoDiag, &QPushButton::clicked, this, [this, btnYesDiag, btnNoDiag]() {
            btnYesDiag->hide();
            btnNoDiag->hide();
            ui->label_diag_status_a->setText("检测结果：异常");
        });

    QHBoxLayout *dialLayout = new QHBoxLayout();
    dialLayout->addStretch();
    dialLayout->addWidget(ui->frame_dial_a);
    dialLayout->addSpacing(60);
    dialLayout->addLayout(labelAndBtnLayout); // 用组合布局替换
    dialLayout->addStretch();

    ui->verticalLayout_display->addLayout(dialLayout);
    ui->frame_dial_a->hide();
    ui->label_diag_status_a->hide();

    // 10. 实时波形曲线图初始化
    ui->SJ_screen->setBackground(QColor("#050a14"));
    ui->SJ_screen->xAxis->setBasePen(QPen(Qt::white));
    ui->SJ_screen->xAxis->setTickPen(QPen(Qt::white));
    ui->SJ_screen->xAxis->setTickLabelColor(Qt::white);
    ui->SJ_screen->yAxis->setBasePen(QPen(Qt::white));
    ui->SJ_screen->yAxis->setTickPen(QPen(Qt::white));
    ui->SJ_screen->yAxis->setTickLabelColor(Qt::white);
    ui->SJ_screen->xAxis->setRange(0, 1024);
    ui->SJ_screen->yAxis->setRange(-5000, 5000);

    QVector<QColor> colors = {Qt::red, Qt::blue, Qt::green, Qt::darkCyan,
                              Qt::darkMagenta, Qt::darkYellow, Qt::black, Qt::gray};
    for (int i = 0; i < 8; ++i) {
        ui->SJ_screen->addGraph();
        QPen pen;
        pen.setColor(colors[i % colors.size()]);
        pen.setWidth(1);
        ui->SJ_screen->graph(i)->setPen(pen);
        ui->SJ_screen->graph(i)->setName(QString("通道 %1").arg(i));
    }
    ui->SJ_screen->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // 注意：把 tabWidget 换成你 UI 里真实的控件名字
    ui->A_window3->tabBar()->hide();
}

A_window::~A_window() { delete ui; }


// ==============================================================
// 数据采集功能区
// ==============================================================
void A_window::on_CJ_A1_clicked()
{
    // 🔴 注入放行标志：标记已经点击过模式设置了
    this->setProperty("isModeSet", true);

    QString originalText = ui->CJ_A1->text();
    ui->CJ_A1->setEnabled(false);
    ui->CJ_A1->setText("配置中...");

    QTimer::singleShot(1000, this, [this, originalText]() {
        ui->CJ_A1->setText("配置完成");
        QByteArray hexData = QByteArray::fromHex(
            "8F 13 EA 07 05 00 10 00 0C 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
            "00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 "
            "00 00 00 00"
        );
        emit sendFrameSignal(hexData);

        QTimer::singleShot(1000, this, [this, originalText]() {
            ui->CJ_A1->setText(originalText);
            ui->CJ_A1->setEnabled(true);
        });
    });
}

void A_window::on_CJ_A2_clicked()
{
    if (!isAcquiring) {
        // 🔴 终极防呆：检查系统是否已经标记了“已配置”状态
        // 这样写的好处是不需要在 .h 文件里新增 bool 变量
        if (!this->property("isModeSet").toBool()) {
            QMessageBox::warning(this, "提示", "请先进行模式设置！");
            return; // 直接打断，不往下执行启动传输
        }

        // ================== 启动采集 ==================
        ui->CJ_A2->setText("停止传输");
        isAcquiring = true;
        currentAcqStartTime = QDateTime::currentDateTime();

        // 1. 生成带时间戳的唯一文件名
        QString fileName = "ZYNQ_DATA_" + currentAcqStartTime.toString("yyyyMMdd_HHmmss") + ".bin";
        currentSaveFile->setFileName(saveDirectory + "/" + fileName);

        // 2. 以只写、追加的模式打开文件
        if (!currentSaveFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
            QMessageBox::critical(this, "严重错误", "无法在本地创建数据文件！");
            return;
        }

        emit sendCommand(3001); // 通知下位机发数据
        switchRightPanel(0);    // 切回波形图

    } else {
        // ================== 停止采集 ==================
        ui->CJ_A2->setText("启动传输");
        emit sendCommand(3002);
        isAcquiring = false;

        // 3. 安全关闭文件，确保数据完全落盘不丢失
        if (currentSaveFile->isOpen()) {
            currentSaveFile->close();
        }

        // 记录到历史...
        qint64 secs = currentAcqStartTime.secsTo(QDateTime::currentDateTime());
        AcqHistory record;
        record.startTime = currentAcqStartTime;
        record.duration = QString::number(secs) + " 秒";
        historyRecords.prepend(record);
        if (historyRecords.size() > 10) { historyRecords.removeLast(); }
    }
}

// 🔴 核心改造：CJ_A3 诊断历史记录查看
void A_window::on_CJ_A3_clicked()
{
    switchRightPanel(1); // 切换到 historyTable

    if (m_diagRecords.isEmpty()) {
        historyTable->setRowCount(1);
        QTableWidgetItem *emptyItem = new QTableWidgetItem("暂无诊断记录");
        emptyItem->setTextAlignment(Qt::AlignCenter);

        QTableWidgetItem *dash1 = new QTableWidgetItem("--");
        QTableWidgetItem *dash2 = new QTableWidgetItem("--");
        dash1->setTextAlignment(Qt::AlignCenter);
        dash2->setTextAlignment(Qt::AlignCenter);

        historyTable->setItem(0, 0, dash1);
        historyTable->setItem(0, 1, emptyItem);
        historyTable->setItem(0, 2, dash2);
        return;
    }

    historyTable->setRowCount(m_diagRecords.size());
    for (int i = 0; i < m_diagRecords.size(); ++i)
    {
        QTableWidgetItem *itemTime = new QTableWidgetItem(m_diagRecords[i].time);
        QTableWidgetItem *itemCheck = new QTableWidgetItem(m_diagRecords[i].checkResult);
        QTableWidgetItem *itemDetail = new QTableWidgetItem(m_diagRecords[i].diagDetail);

        itemTime->setTextAlignment(Qt::AlignCenter);
        itemCheck->setTextAlignment(Qt::AlignCenter);
        itemDetail->setTextAlignment(Qt::AlignCenter);

        if (m_diagRecords[i].checkResult == "异常") {
            itemCheck->setForeground(QBrush(QColor("#ff4d4d")));
            if (m_diagRecords[i].diagDetail == "轻度磨损") {
                itemDetail->setForeground(QBrush(QColor("#ffaa00")));
            } else if (m_diagRecords[i].diagDetail == "中度磨损") {
                itemDetail->setForeground(QBrush(QColor("#ff4d4d")));
            } else if (m_diagRecords[i].diagDetail == "重度磨损") {
                itemDetail->setForeground(QBrush(QColor("#cc0000")));
            }
        } else {
            itemCheck->setForeground(QBrush(QColor("#00ff00")));
        }

        historyTable->setItem(i, 0, itemTime);
        historyTable->setItem(i, 1, itemCheck);
        historyTable->setItem(i, 2, itemDetail);
    }
}

// 文件查看
void A_window::on_CJ_A4_clicked()
{
    switchRightPanel(2);

    QDir dir(saveDirectory);
    QStringList filters;
    filters << "*.bin";
    dir.setNameFilters(filters);

    // 按照时间降序排列（最新鲜的文件排在最前面）
    dir.setSorting(QDir::Time | QDir::Reversed);

    QFileInfoList fileList = dir.entryInfoList();

    if (fileList.isEmpty()) {
        fileTable->setRowCount(1);
        fileTable->setItem(0, 0, new QTableWidgetItem("暂无本地采集数据"));
        fileTable->setItem(0, 1, new QTableWidgetItem("--"));
        fileTable->setItem(0, 2, new QTableWidgetItem("--"));
        fileTable->setItem(0, 3, new QTableWidgetItem("--"));
        return;
    }

    // 🔴 核心逻辑：取文件总数和 20 之间的最小值，确保最多只显示前 20 个
    int displayCount = qMin(fileList.size(), 20);

    fileTable->setRowCount(displayCount);

    for (int i = 0; i < displayCount; ++i) {
        QFileInfo fileInfo = fileList.at(i);

        QTableWidgetItem *itemName = new QTableWidgetItem(fileInfo.fileName());
        QTableWidgetItem *itemTime = new QTableWidgetItem(fileInfo.created().toString("yyyy-MM-dd HH:mm:ss"));

        // 文件大小智能换算 (大于1MB显示MB，否则显示KB)
        double sizeKB = fileInfo.size() / 1024.0;
        QString sizeStr = (sizeKB > 1024)
                            ? QString::number(sizeKB / 1024.0, 'f', 2) + " MB"
                            : QString::number(sizeKB, 'f', 2) + " KB";
        QTableWidgetItem *itemSize = new QTableWidgetItem(sizeStr);

        // 增加第4列：绝对路径展示
        QTableWidgetItem *itemPath = new QTableWidgetItem(fileInfo.absoluteFilePath());

        // 文字居中处理
        itemName->setTextAlignment(Qt::AlignCenter);
        itemTime->setTextAlignment(Qt::AlignCenter);
        itemSize->setTextAlignment(Qt::AlignCenter);
        itemPath->setTextAlignment(Qt::AlignCenter);

        fileTable->setItem(i, 0, itemName);
        fileTable->setItem(i, 1, itemTime);
        fileTable->setItem(i, 2, itemSize);
        fileTable->setItem(i, 3, itemPath);
    }
}

void A_window::on_CJ_A5_clicked()
{
    switchRightPanel(3);
    ui->label_diag_status_a->setText("检测中，请稍候...");
    ui->label_diag_status_a->setStyleSheet("color: #00e5ff; border: 2px solid #1e3a5f;");

    QPushButton *btnYes = this->findChild<QPushButton*>("btnYesDiag");
    QPushButton *btnNo = this->findChild<QPushButton*>("btnNoDiag");
    if(btnYes) btnYes->hide();
    if(btnNo) btnNo->hide();

    m_isDiagnosing = false;

    // 初始化初步检测的 10 分钟拦截标记与缓存
    this->setProperty("lockDiagResult", true);
    this->setProperty("cachedDiagData", QByteArray());

    emit sendCommand(321);
    m_dialAnimationTimer->start(500);

    QTimer::singleShot(600000, this, [this]() {
        this->setProperty("lockDiagResult", false);
        QByteArray cachedData = this->property("cachedDiagData").toByteArray();
        if (!cachedData.isEmpty()) {
            this->onDiagDataReceived(cachedData);
            this->setProperty("cachedDiagData", QByteArray());
        }
    });
}


// ==============================================================
// 设备记录功能区
// ==============================================================
void A_window::on_JL_A1_clicked()
{
    emit sendCommand(301);
    QTimer::singleShot(1000, this, [this]() {
        emit sendCommand(302);
    });

    QTimer::singleShot(1000, this, [this]() {
        QByteArray hexData = QByteArray::fromHex("AA5503016402036480002864FF");
        // emit sendByteCommand(hexData);
    });
}

void A_window::on_JL_A2_clicked()
{
    emit sendCommand(304);

    if (!isRemoteStoring) {
        isRemoteStoring = true;
        ui->JL_A2->setText("存储中...");
        ui->JL_A2->setEnabled(false);

        QTimer::singleShot(1000, this, [this]() {
            ui->JL_A2->setText("停止存储");
            ui->JL_A2->setEnabled(true);
        });
    } else {
        isRemoteStoring = false;
        ui->JL_A2->setText("启动存储");
    }
}

// 🔴 JL_A3 恢复为远程文件表格
void A_window::on_JL_A3_clicked()
{
    remoteFileTable->show();

    // 预留接口，这里先写死假数据
    remoteFileTable->setRowCount(2);
    remoteFileTable->setItem(0, 0, new QTableWidgetItem("ZYNQ_DATA_001.bin"));
    remoteFileTable->setItem(0, 1, new QTableWidgetItem("2026-05-26 15:30:00"));
    remoteFileTable->setItem(0, 2, new QTableWidgetItem("10.0 MB"));

    remoteFileTable->setItem(1, 0, new QTableWidgetItem("ZYNQ_DATA_002.bin"));
    remoteFileTable->setItem(1, 1, new QTableWidgetItem("2026-05-26 15:45:00"));
    remoteFileTable->setItem(1, 2, new QTableWidgetItem("5.2 MB"));
}

void A_window::on_JL_A4_clicked()
{
    int currentRow = remoteFileTable->currentRow();
    if (currentRow < 0) {
        QMessageBox::warning(this, "提示", "请先在右侧列表中选中一个要删除的文件！");
        return;
    }
    QString fileName = remoteFileTable->item(currentRow, 0)->text();
    QMessageBox::StandardButton reply = QMessageBox::question(this, "确认删除", "确定要通知下位机删除文件: " + fileName + " 吗？",
                                  QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        remoteFileTable->removeRow(currentRow);
    }
}

void A_window::on_JL_A5_clicked()
{
    int currentRow = remoteFileTable->currentRow();
    if (currentRow < 0) {
        QMessageBox::warning(this, "提示", "请先在右侧列表中选中一个要上传的文件！");
        return;
    }
    QString fileName = remoteFileTable->item(currentRow, 0)->text();
    QMessageBox::information(this, "上传请求", "已向下位机发送上传请求，准备接收文件: " + fileName);
}


// ==============================================================
// 核心：数据接收与处理
// ==============================================================
void A_window::onWaveDataReceived(quint8 channelID, const QByteArray& payload)
{
    if (payload.size() != 4096) return;

    // 🔴 核心落盘逻辑：如果正在采集，且文件处于打开状态，直接把 4096 字节写入硬盘
    if (isAcquiring && currentSaveFile->isOpen()) {
        currentSaveFile->write(payload);
    }

    qint64 currentMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_lastPlotTimes.contains(channelID)) {
        m_lastPlotTimes[channelID] = 0;
    }

    if (currentMs - m_lastPlotTimes[channelID] >= 33) {
        m_lastPlotTimes[channelID] = currentMs;
        int pointsCount = 1024;
        const float* dataPtr = reinterpret_cast<const float*>(payload.constData());
        QVector<double> x(pointsCount);
        QVector<double> y(pointsCount);

        for (int i = 0; i < pointsCount; ++i) {
            x[i] = i;
            y[i] = dataPtr[i];
        }

        if (channelID < ui->SJ_screen->graphCount()) {
            ui->SJ_screen->graph(channelID)->setData(x, y);
            ui->SJ_screen->graph(channelID)->rescaleAxes();
            ui->SJ_screen->replot();
        }
    }
}

bool A_window::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->frame_dial_a && event->type() == QEvent::Paint) {
        QPainter painter(ui->frame_dial_a);
        painter.setRenderHint(QPainter::Antialiasing);
        int centerX = ui->frame_dial_a->width() / 2;
        int centerY = ui->frame_dial_a->height() / 2;
        int radius = ui->frame_dial_a->width() / 2 - 10;

        painter.setPen(QPen(QColor("#1e3a5f"), 4));
        painter.setBrush(QColor("#050a14"));
        painter.drawEllipse(centerX - radius, centerY - radius, radius * 2, radius * 2);

        painter.setPen(QPen(QColor("#00e5ff"), 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        int innerRadius = radius - 20;
        painter.drawEllipse(centerX - innerRadius, centerY - innerRadius, innerRadius * 2, innerRadius * 2);

        painter.save();
        painter.translate(centerX, centerY);
        QPen tickPen(Qt::white, 2);
        painter.setPen(tickPen);
        int tickStart = innerRadius;
        int tickEnd = innerRadius - 10;
        for (int i = 0; i < 12; ++i) {
            painter.drawLine(0, -tickStart, 0, -tickEnd);
            painter.rotate(30);
        }
        painter.restore();

        QPen pointerPen(QColor("#00e5ff"), 3);
        painter.setPen(pointerPen);
        painter.setBrush(QColor("#00e5ff"));

        painter.translate(centerX, centerY);
        painter.rotate(currentAngle - 90);
        painter.drawLine(0, 0, radius - 35, 0);
        QPolygon arrowHead;
        arrowHead << QPoint(radius - 20, 0)
                  << QPoint(radius - 35, -6)
                  << QPoint(radius - 35, 6);
        painter.drawPolygon(arrowHead);

        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(-6, -6, 12, 12);

        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void A_window::switchRightPanel(int mode)
{
    ui->SJ_screen->setVisible(mode == 0);
    historyTable->setVisible(mode == 1);
    fileTable->setVisible(mode == 2);
    ui->frame_dial_a->setVisible(mode == 3);
    ui->label_diag_status_a->setVisible(mode == 3);
}

void A_window::updateDialAnimation()
{
    currentAngle += 5;
    if (currentAngle >= 360) {
        currentAngle -= 360;
    }
    ui->frame_dial_a->update();
}

void A_window::onDiagDataReceived(const QByteArray& data)
{
    // ==========================================
    // 阶段二：当前处于“详细诊断”阶段
    // ==========================================
    if (m_isDiagnosing) {
        // 🔴 拦截详细诊断的 10 分钟数据
        if (this->property("lockDetailedDiag").toBool()) {
            this->setProperty("cachedDetailedData", data);
            return;
        }

        if (data == QByteArray::fromHex("AA551100")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_a->setText("检测结果：异常\n诊断结果：正常");
            ui->label_diag_status_a->setStyleSheet("color: #ffaa00; border: 2px solid #ffaa00;");
            saveDiagResult("异常", "正常");
            emit sendCommand(333);
        }
        else if (data == QByteArray::fromHex("AA551101")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_a->setText("检测结果：异常\n诊断结果：轻度磨损");
            ui->label_diag_status_a->setStyleSheet("color: #ff4d4d; border: 2px solid #ff4d4d;");
            saveDiagResult("异常", "轻度磨损");
            emit sendCommand(333);
        }
        else if (data == QByteArray::fromHex("AA551102")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_a->setText("检测结果：异常\n诊断结果：中度磨损");
            ui->label_diag_status_a->setStyleSheet("color: #cc0000; font-weight: bold; border: 2px solid #cc0000;");
            saveDiagResult("异常", "中度磨损");
            emit sendCommand(333);
        }
        else if (data == QByteArray::fromHex("AA551103")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_a->setText("检测结果：异常\n诊断结果：重度磨损");
            ui->label_diag_status_a->setStyleSheet("color: #cc0000; font-weight: bold; border: 2px solid #cc0000;");
            saveDiagResult("异常", "重度磨损");
            emit sendCommand(333);
        }
        m_isDiagnosing = false;
        return;
    }

    // ==========================================
    // 阶段一：当前处于“初步检测”阶段
    // ==========================================
    if (this->property("lockDiagResult").toBool()) {
        this->setProperty("cachedDiagData", data);
        return;
    }

    if (data == QByteArray::fromHex("AA551000")) {
        m_dialAnimationTimer->stop();
        ui->label_diag_status_a->setText("检测结果：正常");
        ui->label_diag_status_a->setStyleSheet("color: #00ff00; border: 2px solid #00ff00;");
        saveDiagResult("正常", "--");
    }
    else if (data == QByteArray::fromHex("AA551001")) {
        m_dialAnimationTimer->stop();
        ui->label_diag_status_a->setText("检测结果：异常");
        ui->label_diag_status_a->setStyleSheet("color: #ff0000; border: 2px solid #ff0000;");
        emit sendCommand(323);

        QTimer::singleShot(1500, this, [this]() {
            ui->label_diag_status_a->setText("检测结果：异常\n是否进行诊断？");
            QPushButton *btnYes = this->findChild<QPushButton*>("btnYesDiag");
            QPushButton *btnNo = this->findChild<QPushButton*>("btnNoDiag");
            if(btnYes && btnNo) {
                btnYes->show();
                btnNo->show();
            }
        });
    }
}

// ==============================================================
// 核心：CSV 持久化存储
// ==============================================================
void A_window::loadDiagHistory()
{
    QFile file(m_historyFilePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QTextStream in(&file);
    in.setCodec("UTF-8");
    m_diagRecords.clear();

    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList parts = line.split(",");

        if (parts.size() >= 3) {
            DiagRecord record;
            record.time = parts[0];
            record.checkResult = parts[1];
            record.diagDetail = parts[2];
            m_diagRecords.append(record);
        }
    }
    file.close();
}

void A_window::updateHistoryFile()
{
    QFile file(m_historyFilePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out.setCodec("UTF-8");

        for (int i = 0; i < m_diagRecords.size(); ++i) {
            out << m_diagRecords[i].time << ","
                << m_diagRecords[i].checkResult << ","
                << m_diagRecords[i].diagDetail << "\n";
        }
        file.close();
    }
}

void A_window::saveDiagResult(const QString &check, const QString &detail)
{
    DiagRecord record;
    record.time = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    record.checkResult = check;
    record.diagDetail = detail;

    m_diagRecords.prepend(record);

    if (m_diagRecords.size() > 10) {
        m_diagRecords.removeLast();
    }

    updateHistoryFile();
}


// ==============================================================
// 🔴 终极防护：拦截右上角 X 号关闭动作
// ==============================================================
void A_window::closeEvent(QCloseEvent *event)
{
    // 检查当前是否正在采集（传输）
    if (isAcquiring) {
        // 弹出严重警告
        QMessageBox::warning(this, "警告", "当前正在高速传输数据！\n为了保护数据完整性，请先点击左侧的【停止传输】按钮，然后再关闭窗口。");

        // 忽略关闭指令，窗口将继续保持打开状态
        event->ignore();
    } else {
        emit sendCommand(323);
        emit sendCommand(333);
        // 如果没有在采集，允许正常关闭
        event->accept();
    }
}
