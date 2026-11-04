#include "c_window.h"
#include "ui_c_window.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QtMath>
#include <QTime>
#include <QVector>
#include <QDir>
#include <QMessageBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QtEndian>
#include <cstring>
#include <QPushButton> // 🔴 新增：为了使用 QPushButton

C_window::C_window(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::C_window)
{
    ui->setupUi(this);
    this->setWindowTitle("齿轮箱体腐蚀异常检测系统");

    // 1. 初始化角度
    currentAngle = 0;

    // 2. 初始化动画定时器
    m_dialAnimationTimer = new QTimer(this);
    connect(m_dialAnimationTimer, &QTimer::timeout, this, &C_window::updateDialAnimation);

    // 3. 初始化C窗口独立存盘路径
    saveDirectory = QCoreApplication::applicationDirPath() + "/Data/test_save_C";

    QDir dir(saveDirectory);
    if (!dir.exists()) {
        dir.mkpath(saveDirectory);
    }

    // 诊断记录也存在这个目录下
    m_historyFilePath = saveDirectory + "/diag_history.csv";

    currentSaveFile = new QFile(this);
    isAcquiring = false;
    m_lastCorrosionPlotMs = 0;

    // 4. 统一的暗黑科技风表格样式表
    QString tableStyle =
        "QTableWidget {"
        "   background-color: #050a14;"
        "   color: #00e5ff;"
        "   gridline-color: #1e3a5f;"
        "   border: 1px solid #1e3a5f;"
        "}"
        "QHeaderView::section {"
        "   background-color: #142542;"
        "   color: #ffffff;"
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
        "   background-color: #00e5ff;"
        "   color: #050a14;"
        "}";

    // 5. 动态创建：诊断历史记录表格
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

    // 6. 动态创建：本地文件查看表格
    fileTable = new QTableWidget(this);
    fileTable->setColumnCount(4);
    fileTable->setHorizontalHeaderLabels({"文件名", "创建时间", "文件大小", "存储位置"});
    fileTable->horizontalHeader()->setStretchLastSection(true);
    fileTable->verticalHeader()->setVisible(false);
    fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fileTable->setStyleSheet(tableStyle);
    ui->verticalLayout_display->addWidget(fileTable);
    fileTable->hide();

    // 7. 动态创建：下位机远程文件表格
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

    // 8. 界面初始化完后，立刻从硬盘读取前 10 条历史记录
    loadDiagHistory();

    // 9. 智能诊断表盘 UI 初始化
    ui->frame_dial_3->installEventFilter(this);
    ui->frame_dial_3->setFixedSize(300, 300);

    // ===== 🔴 修改：调整提示框的大小与字体 =====
    ui->label_diag_status_3->setFixedSize(380, 110);
    ui->label_diag_status_3->setAlignment(Qt::AlignCenter);
    ui->label_diag_status_3->setStyleSheet(
        "QLabel {"
        "   background-color: #050a14;"
        "   border: 2px solid #1e3a5f;"
        "   border-radius: 4px;"
        "   color: #7b90ad;"
        "   font-size: 28px;"
        "   font-family: '华文仿宋';"
        "}"
    );
    ui->label_diag_status_3->setText("诊断进度展示");

    // ===== 🔴 修改：为“是”和“否”按钮创建一个垂直布局 =====
    QVBoxLayout *labelAndBtnLayout = new QVBoxLayout();
    labelAndBtnLayout->addWidget(ui->label_diag_status_3);
    labelAndBtnLayout->setAlignment(Qt::AlignCenter);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnYesDiag = new QPushButton("是", this);
    QPushButton *btnNoDiag = new QPushButton("否", this);
    btnYesDiag->setObjectName("btnYesDiag");
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

    connect(btnYesDiag, &QPushButton::clicked, this, [this, btnYesDiag, btnNoDiag]() {
            btnYesDiag->hide();
            btnNoDiag->hide();
            ui->label_diag_status_3->setText("检测结果：异常\n正在进行诊断...");
            m_dialAnimationTimer->start(500);

            m_isDiagnosing = true;
            this->setProperty("lockDetailedDiag", true);
            this->setProperty("cachedDetailedData", QByteArray());

            emit sendCommand(351); // C窗口详细诊断开始指令

            QTimer::singleShot(600000, this, [this]() {
                this->setProperty("lockDetailedDiag", false);
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
            ui->label_diag_status_3->setText("检测结果：异常");
        });

    QHBoxLayout *dialLayout = new QHBoxLayout();
    dialLayout->addStretch();
    dialLayout->addWidget(ui->frame_dial_3);
    dialLayout->addSpacing(60);
    dialLayout->addLayout(labelAndBtnLayout); // 用组合布局替换原来的单标签
    dialLayout->addStretch();

    ui->verticalLayout_display->addLayout(dialLayout);
    ui->frame_dial_3->hide();
    ui->label_diag_status_3->hide();

    // 10. 实时波形曲线图初始化
    ui->SJ_screen->setBackground(QColor("#050a14"));
    ui->SJ_screen->xAxis->setBasePen(QPen(Qt::white));
    ui->SJ_screen->xAxis->setTickPen(QPen(Qt::white));
    ui->SJ_screen->xAxis->setTickLabelColor(Qt::white);
    ui->SJ_screen->yAxis->setBasePen(QPen(Qt::white));
    ui->SJ_screen->yAxis->setTickPen(QPen(Qt::white));
    ui->SJ_screen->yAxis->setTickLabelColor(Qt::white);
    ui->SJ_screen->xAxis->setRange(0, 255);
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

    // 隐藏对应的 tab 控件头部
    ui->C_window3->tabBar()->hide();
}

C_window::~C_window() { delete ui; }

// ==============================================================
// 🔴 终极防护：拦截右上角 X 号关闭动作
// ==============================================================
void C_window::closeEvent(QCloseEvent *event)
{
    if (isAcquiring) {
        QMessageBox::warning(this, "警告", "当前正在高速传输数据！\n为了保护数据完整性，请先点击左侧的【停止传输】按钮，然后再关闭窗口。");
        event->ignore();
    } else {
        emit sendCommand(353);
        event->accept();
    }
}

// ==============================================================
// 数据采集功能区
// ==============================================================
void C_window::on_CJ_C1_clicked()
{
    this->setProperty("isModeSet", true);
    QString originalText = ui->CJ_C1->text();
    ui->CJ_C1->setEnabled(false);
    ui->CJ_C1->setText("配置中...");

    QTimer::singleShot(1000, this, [this, originalText]() {
        ui->CJ_C1->setText("配置完成");
        QByteArray hexData = QByteArray::fromHex(
            "8F 13 EA 07 05 00 10 00 0C 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
            "00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 "
            "00 00 00 00"
        );
        emit sendFrameSignal(hexData);

        QTimer::singleShot(1000, this, [this, originalText]() {
            ui->CJ_C1->setText(originalText);
            ui->CJ_C1->setEnabled(true);
        });
    });
}

void C_window::on_CJ_C2_clicked()
{
    if (!isAcquiring) {
        if (!this->property("isModeSet").toBool()) {
            QMessageBox::warning(this, "提示", "请先进行模式设置！");
            return;
        }

        currentAcqStartTime = QDateTime::currentDateTime();
        QString fileName = "ZYNQ_DATA_" + currentAcqStartTime.toString("yyyyMMdd_HHmmss") + ".bin";

        // 确保之前的文件句柄被正确关闭
        if (currentSaveFile->isOpen()) {
            currentSaveFile->close();
        }

        currentSaveFile->setFileName(saveDirectory + "/" + fileName);

        if (!currentSaveFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
            QMessageBox::critical(this, "严重错误", "无法在本地创建数据文件！");
            ui->CJ_C2->setText("启动传输");
            isAcquiring = false;
            return;
        }

        ui->CJ_C2->setText("停止传输");
        isAcquiring = true;
        m_lastCorrosionPlotMs = 0;
        ui->SJ_screen->xAxis->setRange(0, 255);
        switchRightPanel(0);

        // C窗口腐蚀板256点float上传：3005开启
        emit sendCommand(3005);

    } else {
        ui->CJ_C2->setText("启动传输");

        // C窗口腐蚀板256点float上传：3006停止
        emit sendCommand(3006);
        isAcquiring = false;

        if (currentSaveFile->isOpen()) {
            currentSaveFile->close();
        }

        qint64 secs = currentAcqStartTime.secsTo(QDateTime::currentDateTime());
        AcqHistory record;
        record.startTime = currentAcqStartTime;
        record.duration = QString::number(secs) + " 秒";
        historyRecords.prepend(record);
        if (historyRecords.size() > 10) { historyRecords.removeLast(); }
    }
}

void C_window::on_CJ_C3_clicked()
{
    switchRightPanel(1);

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
            if (m_diagRecords[i].diagDetail == "轻度腐蚀") { // 注意这里改成了腐蚀
                itemDetail->setForeground(QBrush(QColor("#ffaa00")));
            } else if (m_diagRecords[i].diagDetail == "中度腐蚀") {
                itemDetail->setForeground(QBrush(QColor("#ff4d4d")));
            } else if (m_diagRecords[i].diagDetail == "重度腐蚀") {
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

void C_window::on_CJ_C4_clicked()
{
    switchRightPanel(2);

    QDir dir(saveDirectory);
    QStringList filters;
    filters << "*.bin";
    dir.setNameFilters(filters);
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

    int displayCount = qMin(fileList.size(), 20);
    fileTable->setRowCount(displayCount);

    for (int i = 0; i < displayCount; ++i) {
        QFileInfo fileInfo = fileList.at(i);

        QTableWidgetItem *itemName = new QTableWidgetItem(fileInfo.fileName());
        QTableWidgetItem *itemTime = new QTableWidgetItem(fileInfo.created().toString("yyyy-MM-dd HH:mm:ss"));

        double sizeKB = fileInfo.size() / 1024.0;
        QString sizeStr = (sizeKB > 1024)
                            ? QString::number(sizeKB / 1024.0, 'f', 2) + " MB"
                            : QString::number(sizeKB, 'f', 2) + " KB";
        QTableWidgetItem *itemSize = new QTableWidgetItem(sizeStr);
        QTableWidgetItem *itemPath = new QTableWidgetItem(fileInfo.absoluteFilePath());

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

void C_window::on_CJ_C5_clicked()
{
    switchRightPanel(3);
    ui->label_diag_status_3->setText("检测中，请稍候...");
    ui->label_diag_status_3->setStyleSheet("color: #00e5ff; border: 2px solid #1e3a5f;");

    QPushButton *btnYes = this->findChild<QPushButton*>("btnYesDiag");
    QPushButton *btnNo = this->findChild<QPushButton*>("btnNoDiag");
    if(btnYes) btnYes->hide();
    if(btnNo) btnNo->hide();

    m_isDiagnosing = false;
    this->setProperty("lockDiagResult", true);
    this->setProperty("cachedDiagData", QByteArray());

    emit sendCommand(351);
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
void C_window::on_JL_C1_clicked()
{
    emit sendCommand(301);
    QTimer::singleShot(1000, this, [this]() {
        emit sendCommand(302);
    });

    QTimer::singleShot(1000, this, []() {
        QByteArray hexData = QByteArray::fromHex("AA5503016402036480002864FF");
    });
}

void C_window::on_JL_C2_clicked()
{
    emit sendCommand(304);

    if (!isRemoteStoring) {
        isRemoteStoring = true;
        ui->JL_C2->setText("存储中...");
        ui->JL_C2->setEnabled(false);

        QTimer::singleShot(1000, this, [this]() {
            ui->JL_C2->setText("停止存储");
            ui->JL_C2->setEnabled(true);
        });
    } else {
        isRemoteStoring = false;
        ui->JL_C2->setText("启动存储");
    }
}

void C_window::on_JL_C3_clicked()
{
    remoteFileTable->show();

    remoteFileTable->setRowCount(2);
    remoteFileTable->setItem(0, 0, new QTableWidgetItem("ZYNQ_DATA_001.bin"));
    remoteFileTable->setItem(0, 1, new QTableWidgetItem("2026-05-26 15:30:00"));
    remoteFileTable->setItem(0, 2, new QTableWidgetItem("10.0 MB"));

    remoteFileTable->setItem(1, 0, new QTableWidgetItem("ZYNQ_DATA_002.bin"));
    remoteFileTable->setItem(1, 1, new QTableWidgetItem("2026-05-26 15:45:00"));
    remoteFileTable->setItem(1, 2, new QTableWidgetItem("5.2 MB"));
}

void C_window::on_JL_C4_clicked()
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

void C_window::on_JL_C5_clicked()
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
// C窗口腐蚀板256点float上传波形显示
// 设备帧格式：AA 55 01 03 ... 24..1047 为 256 个小端 float32
// ==============================================================
void C_window::onCorrosionWaveDataReceived(quint8 channelID, const QByteArray& payload)
{
    Q_UNUSED(channelID);

    if (payload.size() != 256 * static_cast<int>(sizeof(float))) {
        return;
    }

    // 保持原有本地文件功能：采集中时保存每帧1024字节载荷
    if (isAcquiring && currentSaveFile && currentSaveFile->isOpen()) {
        currentSaveFile->write(payload);
    }

    // UI刷新节流，避免设备高频上传时界面卡顿；数据保存不受节流影响
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastCorrosionPlotMs != 0 && nowMs - m_lastCorrosionPlotMs < 50) {
        return;
    }
    m_lastCorrosionPlotMs = nowMs;

    const uchar *raw = reinterpret_cast<const uchar *>(payload.constData());
    QVector<double> x(256);
    QVector<double> y(256);

    double minY = 0.0;
    double maxY = 0.0;

    for (int i = 0; i < 256; ++i) {
        quint32 bits = qFromLittleEndian<quint32>(raw + i * 4);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));

        x[i] = i;
        y[i] = static_cast<double>(value);

        if (i == 0) {
            minY = maxY = y[i];
        } else {
            if (y[i] < minY) minY = y[i];
            if (y[i] > maxY) maxY = y[i];
        }
    }

    if (ui->SJ_screen->graphCount() <= 0) {
        ui->SJ_screen->addGraph();
    }

    ui->SJ_screen->graph(0)->setData(x, y);
    ui->SJ_screen->xAxis->setRange(0, 255);

    double margin = (maxY - minY) * 0.1;
    if (margin < 1.0) {
        margin = 1.0;
    }
    ui->SJ_screen->yAxis->setRange(minY - margin, maxY + margin);
    ui->SJ_screen->replot();
}

bool C_window::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->frame_dial_3 && event->type() == QEvent::Paint) {
        QPainter painter(ui->frame_dial_3);
        painter.setRenderHint(QPainter::Antialiasing);
        int centerX = ui->frame_dial_3->width() / 2;
        int centerY = ui->frame_dial_3->height() / 2;
        int radius = ui->frame_dial_3->width() / 2 - 10;

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

void C_window::switchRightPanel(int mode)
{
    ui->SJ_screen->setVisible(mode == 0);
    historyTable->setVisible(mode == 1);
    fileTable->setVisible(mode == 2);
    ui->frame_dial_3->setVisible(mode == 3);
    ui->label_diag_status_3->setVisible(mode == 3);
}

void C_window::updateDialAnimation()
{
    currentAngle += 5;
    if (currentAngle >= 360) {
        currentAngle -= 360;
    }
    ui->frame_dial_3->update();
}

void C_window::onDiagDataReceived(const QByteArray& data)
{
    if (m_isDiagnosing) {
        if (this->property("lockDetailedDiag").toBool()) {
            this->setProperty("cachedDetailedData", data);
            return;
        }

        if (data == QByteArray::fromHex("AA551100")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_3->setText("检测结果：异常\n诊断结果：无腐蚀");
            ui->label_diag_status_3->setStyleSheet("color: #ffaa00; border: 2px solid #ffaa00;");
            saveDiagResult("异常", "无腐蚀");
            emit sendCommand(353);
        }
        else if (data == QByteArray::fromHex("AA551101")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_3->setText("检测结果：异常\n诊断结果：轻度腐蚀");
            ui->label_diag_status_3->setStyleSheet("color: #ff4d4d; border: 2px solid #ff4d4d;");
            saveDiagResult("异常", "轻度腐蚀");
            emit sendCommand(353);
        }
        else if (data == QByteArray::fromHex("AA551102")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_3->setText("检测结果：异常\n诊断结果：中度腐蚀");
            ui->label_diag_status_3->setStyleSheet("color: #cc0000; font-weight: bold; border: 2px solid #cc0000;");
            saveDiagResult("异常", "中度腐蚀");
            emit sendCommand(353);
        }
        else if (data == QByteArray::fromHex("AA551103")) {
            m_dialAnimationTimer->stop();
            ui->label_diag_status_3->setText("检测结果：异常\n诊断结果：重度腐蚀");
            ui->label_diag_status_3->setStyleSheet("color: #cc0000; font-weight: bold; border: 2px solid #cc0000;");
            saveDiagResult("异常", "重度腐蚀");
            emit sendCommand(353);
        }
        m_isDiagnosing = false;
        return;
    }

    if (this->property("lockDiagResult").toBool()) {
        this->setProperty("cachedDiagData", data);
        return;
    }

    if (data == QByteArray::fromHex("AA551100")) {
        m_dialAnimationTimer->stop();
        ui->label_diag_status_3->setText("检测结果：正常");
        ui->label_diag_status_3->setStyleSheet("color: #00ff00; border: 2px solid #00ff00;");
        saveDiagResult("正常", "--");
    }
    else if (data == QByteArray::fromHex("AA551101") ||
             data == QByteArray::fromHex("AA551102") ||
             data == QByteArray::fromHex("AA551103")) {
        m_dialAnimationTimer->stop();
        ui->label_diag_status_3->setText("检测结果：异常");
        ui->label_diag_status_3->setStyleSheet("color: #ff0000; border: 2px solid #ff0000;");
        emit sendCommand(353);

        QTimer::singleShot(1500, this, [this]() {
            ui->label_diag_status_3->setText("检测结果：异常\n是否进行诊断？");
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
void C_window::loadDiagHistory()
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

void C_window::updateHistoryFile()
{
    QFile file(m_historyFilePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out (&file);
        out.setCodec("UTF-8");

        for (int i = 0; i < m_diagRecords.size(); ++i) {
            out << m_diagRecords[i].time << ","
                << m_diagRecords[i].checkResult << ","
                << m_diagRecords[i].diagDetail << "\n";
        }
        file.close();
    }
}

void C_window::saveDiagResult(const QString &check, const QString &detail)
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
