#include "h_window.h"
#include "ui_h_window.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFont>
#include <QHeaderView>
#include <QResizeEvent>
#include <QTabBar>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVector>
#include <QtGlobal>
#include <QDir>         // 新增：用于目录操作
#include <QMessageBox>  // 新增：用于弹窗提示

#include <cstring>
#include <algorithm>    // 新增：用于 std::sort

#include <QDesktopServices>
#include <QUrl>
#include <algorithm>
#include <cmath>
// =====================================================================
// H 窗口：信号监测功能
// 由 自测QT(YB/QT_zice) 的 MainWindow 迁移而来。
//
// 【布局策略】完全沿用原 自测QT 的做法：
//   · 控件位置全部交给 .ui 里的 QGridLayout / QVBoxLayout / QHBoxLayout
//   · 外观全部交给一份统一的 QSS（深色工业风）
//   · 不使用绝对定位，也不手工 setGeometry
//
// 【与原版最大的区别】本窗口不持有 QTcpSocket，收发通过信号交给 DealFile，
// 与 A~G 六个检测窗口共用同一条 TCP 连接。
// =====================================================================

namespace {

// ---------------- 指令码（与原 自测QT 完全一致，2 字节小端下发） ----------------
const quint16 kCmdVibrationStartBase  = 3050;  // 3050 + 通道号
const quint16 kCmdVibrationStop       = 3054;
const quint16 kCmdUltrasonicConfig    = 301;
const quint16 kCmdUltrasonicParam     = 302;
const quint16 kCmdUltrasonicStartBase = 304;   // 304 + 10 * 通道号
const quint16 kCmdUltrasonicStop      = 305;
const quint16 kCmdVoltageSnapshot     = 3030;
const quint16 kCmdCurrentSnapshot     = 3040;

// 过零测频使用的采样率，必须与 waveformwidget.cpp 中的标称值一致。
const double kVibrationSampleRate = 64000.0;

// 快照包：24 字节头 + 8 * float32 + FF FF
const int kSnapshotChannelCount = 8;

// 深色工业风配色（取自原 自测QT）
const char *kBgDeep     = "#080C17";
const char *kAccent     = "#00E5FF";
const char *kBtnBg      = "#383D48";
const char *kBtnBorder  = "#005577";
const char *kTableBg    = "#050810";
const char *kBgTab      = "#0A0F1C";
const char *kPaneBorder = "#1A2B4C";

// 以 1160 像素窗口宽度为 1.0 基准（与 h_window.ui 的初始 geometry 一致），
// 按窗口实际宽度平滑调整字号。位置由布局管理器负责，这里只微调可读性。
int fontScalePercent(int windowWidth)
{
    const double ratio = qBound(1.0, static_cast<double>(windowWidth) / 1160.0, 1.5);
    return qRound(ratio * 100.0);
}

// 按当前字号比例缩放一个基准点数
int scaledPointSize(int basePointSize, int scalePercent)
{
    return qMax(9, qRound(basePointSize * scalePercent / 100.0));
}

QFont scaledFont(const QString &family, int basePointSize, int scalePercent)
{
    QFont font(family);
    font.setPointSize(scaledPointSize(basePointSize, scalePercent));
    font.setBold(true);
    return font;
}

// 统一刷新两个快照表格的表头、行列尺寸与字体
void prepareSnapshotTable(QTableWidget *table, int scalePercent)
{
    if (table == nullptr) {
        return;
    }
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setVisible(false);
    table->verticalHeader()->setVisible(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFocusPolicy(Qt::NoFocus);
    table->setFont(scaledFont(QStringLiteral("Microsoft YaHei"), 10, scalePercent));
}

// 向表格单元格写入居中文本（覆盖创建与后续刷新）
void setCenteredItem(QTableWidget *table, int row, int column, const QString &text)
{
    QTableWidgetItem *item = table->item(row, column);
    if (item == nullptr) {
        item = new QTableWidgetItem(text);
        table->setItem(row, column, item);
    } else {
        item->setText(text);
    }
    item->setTextAlignment(Qt::AlignCenter);
}

} // namespace

H_window::H_window(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::H_window),
    activeVibrationChannel(-1),
    vibrationAcquiring(false),
    vibrationStartPending(false),
    voltageSnapshotPending(false),
    currentSnapshotPending(false),
    activeUltrasonicChannel(-1),
    displayUltrasonicUiChannel(-1),
    ultrasonicStartPending(false),
    ultrasonicAcquiring(false),
    ultrasonicPacketSequence(0),
    interfaceReady(false),
    m_statusToken(0),
    interfaceScale(0),
    deviceConnected(false),
    m_currentDownloadFile(nullptr) // 【新增】：下载引擎指针初始化
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("信号监测功能"));
    ui->mainwindow->tabBar()->setExpanding(true);
    ui->mainwindow->tabBar()->setElideMode(Qt::ElideNone);

    // ================================================================
    // 深色工业风样式表（作用域限于本对话框，不会污染主界面）
    // 与 自测QT 中的样式保持一致，仅把字体族统一为华文仿宋
    // ================================================================
    const QString darkTechStyle = QString(
        "QWidget { background-color: %1; color: %2; font-family: \"Times New Roman\", \"微软雅黑\", \"Microsoft YaHei\"; }\n"
        "QLabel { color: %2; font-weight: bold; }\n"
        "QTabWidget::pane { border: 1px solid %7; background-color: %6; }\n"
        "QTabBar::tab { background-color: #121A2F; color: #4B6E96; padding: 7px 16px;"
        " border: 1px solid %7; border-bottom: none; border-top-left-radius: 4px;"
        " border-top-right-radius: 4px; margin-right: 2px; font-weight: bold; }\n"
        "QTabBar::tab:selected { background-color: %6; color: %2;"
        " border-top: 2px solid %2; }\n"
        "QPushButton { color: %2; background-color: %3; border: 1px solid %4;"
        " border-top: 1px solid #5A6273; border-bottom: 2px solid #1D2229;"
        " border-radius: 4px; padding: 4px 14px; font-weight: bold; min-height: 25px; }\n"
        "QPushButton:hover { background-color: #4A5160; border: 1px solid %2;"
        " border-bottom: 2px solid #1D2229; }\n"
        "QPushButton:pressed { background-color: #2D313A; border: 1px solid #0088AA;"
        " border-top: 2px solid #1D2229; border-bottom: 1px solid %3; }\n"
        "QPushButton:disabled { color: #3A4A5A; background-color: #1A1F28;"
        " border: 1px solid #22303F; }\n"
        "QComboBox { color: %2; background-color: %3; border: 1px solid %4;"
        " border-radius: 4px; padding: 4px 10px; font-weight: bold; min-height: 25px; }\n"
        "QComboBox::drop-down { border: none; width: 22px; }\n"
        "QComboBox QAbstractItemView { background-color: %3; color: %2;"
        " selection-background-color: %4; }\n"
        "QTableWidget { background-color: %5; alternate-background-color: %6;"
        " color: %2; border: 1px solid %7; gridline-color: %7;"
        " selection-background-color: %2; selection-color: #000000; }\n"
        "QHeaderView::section { background-color: #0F1626; color: %2; font-weight: bold;"
        " border: none; border-bottom: 1px solid %2; border-right: 1px solid %7;"
        " padding: 4px; }\n")
        .arg(QString::fromLatin1(kBgDeep),
             QString::fromLatin1(kAccent),
             QString::fromLatin1(kBtnBg),
             QString::fromLatin1(kBtnBorder),
             QString::fromLatin1(kTableBg),
             QString::fromLatin1(kBgTab),
             QString::fromLatin1(kPaneBorder));

    setStyleSheet(darkTechStyle);

    // ---------------- 振动信号采集页 ----------------
    ui->vibrationChannelCombo->addItems(QStringList()
                                        << QStringLiteral("振动通道 0")
                                        << QStringLiteral("振动通道 1")
                                        << QStringLiteral("振动通道 2")
                                        << QStringLiteral("振动通道 3"));
    for (int channel = 0; channel < 8; ++channel) {
        ui->ultrasonicChannelCombo->addItem(
                    QStringLiteral("超声通道 %1").arg(channel + 1));
    }

    ui->vibrationWaveform->setAdaptiveValueRange(10.0);
    ui->vibrationWaveform->setContinuousLineMode(true);
    ui->vibrationWaveform->setSignalName(QStringLiteral("IEPE"));

    ui->ultrasonicWaveform->setSignalName(QStringLiteral("超声"));
    ui->ultrasonicWaveform->setContinuousLineMode(true);
    ui->ultrasonicWaveform->setWaveformLineWidth(2.0);

    // ---------------- 电压 / 电流快照表 ----------------
    ui->voltageTable->setAlternatingRowColors(true);
    ui->currentTable->setAlternatingRowColors(true);
    prepareSnapshotTable(ui->voltageTable, interfaceScale);
    prepareSnapshotTable(ui->currentTable, interfaceScale);

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 4; ++col) {
            setCenteredItem(ui->voltageTable, row, col, QString());
            setCenteredItem(ui->currentTable, row, col, QString());
        }
    }

    // ==============================================================
    // 【新增】：文件查看表格初始化
    // ==============================================================
    ui->fileTable->setColumnCount(4);
    ui->fileTable->setHorizontalHeaderLabels(QStringList() << "文件名" << "创建时间" << "文件大小" << "存储位置");
    ui->fileTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->fileTable->verticalHeader()->setVisible(false);
    ui->fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);      // 整行选中
    ui->fileTable->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 允许 Ctrl/Shift 多选
    ui->fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);       // 禁止编辑

    // 统一按钮样式
    QString actionBtnStyle =
            "QPushButton { color: #00e5ff; background-color: #383d48; border: 1px solid #005577; "
            "border-top: 1px solid #5a6273; border-bottom: 2px solid #1d2229; border-radius: 4px; "
            "padding: 4px 30px; font-weight: bold; min-height: 25px; min-width: 80px; }"
            "QPushButton:hover { background-color: #4a5160; border: 1px solid #00e5ff; }"
            "QPushButton:pressed { background-color: #2d313a; border: 1px solid #0088aa; }";
    ui->btnQueryFile->setStyleSheet(actionBtnStyle);
    ui->btnReadFile->setStyleSheet(actionBtnStyle);
    ui->btnDeleteFile->setStyleSheet(actionBtnStyle);

    interfaceReady = true;
    applyInterfaceScale();

    // 初始未连接：禁止启动，等待主界面通过 setDeviceConnected 通知
    setDeviceConnected(false);
}

H_window::~H_window()
{
    delete ui;
}

// =====================================================================
// 字号自适应
// 位置与尺寸全部由布局管理器接管，这里只按窗口宽度调整字号，
// 避免原方案"手工 move + setFixedSize"带来的错位与拥挤。
// =====================================================================
void H_window::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (interfaceReady) {
        applyInterfaceScale();
    }
}

void H_window::applyInterfaceScale()
{
    const int scalePercent = fontScalePercent(width());
    if (scalePercent == interfaceScale) {
        return;
    }
    interfaceScale = scalePercent;

    // 标题：华文仿宋 16pt 加粗
    ui->label_title->setFont(scaledFont(QStringLiteral("华文仿宋"), 16, scalePercent));

    // 页签文字：华文仿宋 12pt 加粗
    ui->mainwindow->setFont(scaledFont(QStringLiteral("华文仿宋"), 12, scalePercent));

    // 表内文字：微软雅黑 10pt
    prepareSnapshotTable(ui->voltageTable, scalePercent);
    prepareSnapshotTable(ui->currentTable, scalePercent);

    // 控件文字：微软雅黑 10pt
    const QFont controlFont = scaledFont(QStringLiteral("Microsoft YaHei"), 10, scalePercent);
    ui->label_voltage->setFont(controlFont);
    ui->label_current->setFont(controlFont);
    ui->label_status->setFont(scaledFont(QStringLiteral("Microsoft YaHei"), 9, scalePercent));
    ui->vibrationChannelCombo->setFont(controlFont);
    ui->ultrasonicChannelCombo->setFont(controlFont);
    ui->vibrationStartButton->setFont(controlFont);
    ui->ultrasonicStartButton->setFont(controlFont);
    ui->voltageStartButton->setFont(controlFont);
    ui->currentStartButton->setFont(controlFont);
}

void H_window::showStatus(const QString &message, int timeoutMs)
{
    ui->label_status->setText(message);

    if (timeoutMs <= 0) {
        return;
    }

    // 用递增令牌实现"定时清除"，避免多次调用互相覆盖导致文案提前消失
    const qint64 token = ++m_statusToken;
    QTimer::singleShot(timeoutMs, this, [this, token]() {
        if (m_statusToken == token) {
            ui->label_status->setText(QStringLiteral("就绪"));
        }
    });
}

void H_window::setDeviceConnected(bool connected)
{
    deviceConnected = connected;

    if (!connected) {
        // 断线时复位所有采集状态，避免状态机卡在"采集中"
        vibrationAcquiring = false;
        vibrationStartPending = false;
        ultrasonicAcquiring = false;
        ultrasonicStartPending = false;
        voltageSnapshotPending = false;
        currentSnapshotPending = false;
        activeVibrationChannel = -1;
        activeUltrasonicChannel = -1;
        displayUltrasonicUiChannel = -1;

        ui->vibrationStartButton->setText(QStringLiteral("启动采集"));
        ui->ultrasonicStartButton->setText(QStringLiteral("启动采集"));
        ui->vibrationStartButton->setEnabled(false);
        ui->ultrasonicStartButton->setEnabled(false);
        ui->voltageStartButton->setEnabled(false);
        ui->currentStartButton->setEnabled(false);
        ui->vibrationChannelCombo->setEnabled(false);
        ui->ultrasonicChannelCombo->setEnabled(false);
        ui->btnQueryFile->setEnabled(false);
        ui->btnReadFile->setEnabled(false);
        ui->btnDeleteFile->setEnabled(false);
        showStatus(QStringLiteral("下位机未连接，等待重连…"));
        return;
    }

    ui->vibrationStartButton->setEnabled(true);
    ui->ultrasonicStartButton->setEnabled(true);
    ui->voltageStartButton->setEnabled(true);
    ui->currentStartButton->setEnabled(true);
    ui->vibrationChannelCombo->setEnabled(true);
    ui->ultrasonicChannelCombo->setEnabled(true);
    ui->btnQueryFile->setEnabled(true);
    ui->btnReadFile->setEnabled(true);
    ui->btnDeleteFile->setEnabled(true);
    showStatus(QStringLiteral("下位机已连接，可以启动采集。"));
}

// =====================================================================
// 发送侧：全部改为发信号给 DealFile，本窗口不再直接操作 socket
// =====================================================================

// ---- 振动 ----
void H_window::on_vibrationStartButton_clicked()
{
    if (!deviceConnected) {
        showStatus(QStringLiteral("未连接下位机，无法启动振动采集。"));
        return;
    }
    if (vibrationStartPending) {
        showStatus(QStringLiteral("正在发送振动启动命令，请稍候。"), 2000);
        return;
    }

    if (vibrationAcquiring) {
        // 1. 发送停止 TCP 上传指令 (3054)
        emit sendCommand(kCmdVibrationStop);

        // ==========================================
        // 【新增】：2. 拼装并发送 5008 停止本地存储指令
        // 格式: 5008(2字节小端) + 通道号(2字节小端)
        // ==========================================
        QByteArray endStoreFrame = commandLittleEndianU16(5008);
        endStoreFrame.append(commandLittleEndianU16(static_cast<quint16>(activeVibrationChannel)));
        emit sendFrameSignal(endStoreFrame);

        vibrationAcquiring = false;
        activeVibrationChannel = -1;
        ui->vibrationStartButton->setText(QStringLiteral("启动采集"));
        showStatus(QStringLiteral("已停止振动数据上传与本地存储。"));
        return;
    }

    const int channel = ui->vibrationChannelCombo->currentIndex();
    activeVibrationChannel = -1;
    vibrationStartPending = true;

    // 先下发状态包告知下位机取哪些状态，500ms 后再下发启动指令
    emit sendFrameSignal(build5007StatusPacket(channel, 1));

    QTimer::singleShot(500, this, [this, channel]() {
        if (!deviceConnected) {
            vibrationStartPending = false;
            showStatus(QStringLiteral("下位机已断开，振动通道命令未发送。"));
            return;
        }
        emit sendCommand(static_cast<quint16>(kCmdVibrationStartBase + channel));
        activeVibrationChannel = channel;
        vibrationStartPending = false;
        vibrationAcquiring = true;
        ui->vibrationStartButton->setText(QStringLiteral("停止采集"));
        showStatus(QStringLiteral("已启动振动通道 %1 数据上传与本地存储。").arg(channel));
    });
}
// ---- 电压快照 ----
void H_window::on_voltageStartButton_clicked()
{
    if (!deviceConnected) {
        showStatus(QStringLiteral("未连接下位机，无法采集电压。"));
        return;
    }
    if (voltageSnapshotPending) {
        showStatus(QStringLiteral("正在等待八通道电压快照，请稍候。"), 2000);
        return;
    }
    voltageSnapshotPending = true;

    // 发送 5007 开启 12~19 通道采集与存盘
    emit sendFrameSignal(build5007StatusPacket(12, 8));

    QTimer::singleShot(500, this, [this]() {
        if (!deviceConnected) {
            voltageSnapshotPending = false;
            showStatus(QStringLiteral("下位机已断开，电压快照命令未发送。"));
            return;
        }
        // 发送快照上传请求 (3030)
        emit sendCommand(kCmdVoltageSnapshot);
        showStatus(QStringLiteral("已请求八通道电压快照。"));

        // ==========================================
        // 【新增】：延时 1.5 秒后，自动发送 8 条 5008 指令
        // 彻底关闭 12~19 这 8 个电压通道的本地存储。
        // ==========================================
        QTimer::singleShot(1500, this, [this]() {
            for (int i = 12; i <= 19; ++i) {
                QByteArray endStoreFrame = commandLittleEndianU16(5008);
                endStoreFrame.append(commandLittleEndianU16(static_cast<quint16>(i)));
                emit sendFrameSignal(endStoreFrame);
            }
            showStatus(QStringLiteral("已自动关闭八通道电压本地存储。"), 2000);
        });
    });
}

// ---- 电流快照 ----
void H_window::on_currentStartButton_clicked()
{
    if (!deviceConnected) {
        showStatus(QStringLiteral("未连接下位机，无法采集电流。"));
        return;
    }
    if (currentSnapshotPending) {
        showStatus(QStringLiteral("正在等待八通道电流快照，请稍候。"), 2000);
        return;
    }
    currentSnapshotPending = true;

    // 发送 5007 开启 4~11 通道采集与存盘
    emit sendFrameSignal(build5007StatusPacket(4, 8));

    QTimer::singleShot(500, this, [this]() {
        if (!deviceConnected) {
            currentSnapshotPending = false;
            showStatus(QStringLiteral("下位机已断开，电流快照命令未发送。"));
            return;
        }
        // 发送快照上传请求 (3040)
        emit sendCommand(kCmdCurrentSnapshot);
        showStatus(QStringLiteral("已请求八通道电流快照。"));

        // ==========================================
        // 【新增】：延时 1.5 秒后，自动发送 8 条 5008 指令
        // 彻底关闭 4~11 这 8 个电流通道的本地存储。
        // ==========================================
        QTimer::singleShot(1500, this, [this]() {
            for (int i = 4; i <= 11; ++i) {
                QByteArray endStoreFrame = commandLittleEndianU16(5008);
                endStoreFrame.append(commandLittleEndianU16(static_cast<quint16>(i)));
                emit sendFrameSignal(endStoreFrame);
            }
            showStatus(QStringLiteral("已自动关闭八通道电流本地存储。"), 2000);
        });
    });
}

// ---- 超声（四步时序：301 → 302 → 配置帧 → 304+10*ch） ----
void H_window::on_ultrasonicStartButton_clicked()
{
    if (!deviceConnected) {
        showStatus(QStringLiteral("未连接下位机，无法启动超声采集。"));
        return;
    }
    if (ultrasonicStartPending) {
        showStatus(QStringLiteral("正在启动超声采集，请稍候。"), 2000);
        return;
    }

    if (ultrasonicAcquiring) {
        emit sendFrameSignal(commandLittleEndianU16(kCmdUltrasonicStop));
        ultrasonicAcquiring = false;
        activeUltrasonicChannel = -1;
        displayUltrasonicUiChannel = -1;
        ui->ultrasonicStartButton->setText(QStringLiteral("启动采集"));
        showStatus(QStringLiteral("已停止超声采集与数据上传。"));
        return;
    }

    const int uiChannel = ui->ultrasonicChannelCombo->currentIndex();
    const int physicalChannel = uiChannel;
    ultrasonicStartPending = true;
    activeUltrasonicChannel = -1;
    displayUltrasonicUiChannel = uiChannel;

    emit sendFrameSignal(commandLittleEndianU16(kCmdUltrasonicConfig));

    QTimer::singleShot(200, this, [this, uiChannel, physicalChannel]() {
        if (!deviceConnected) {
            ultrasonicStartPending = false;
            showStatus(QStringLiteral("下位机已断开，超声配置命令未发送。"));
            return;
        }

        emit sendFrameSignal(commandLittleEndianU16(kCmdUltrasonicParam));

        QTimer::singleShot(200, this, [this, uiChannel, physicalChannel]() {
            if (!deviceConnected) {
                ultrasonicStartPending = false;
                showStatus(QStringLiteral("下位机已断开，超声参数帧未发送。"));
                return;
            }

            QByteArray configFrame = QByteArray::fromHex("AA55");
            configFrame.append(static_cast<char>(physicalChannel));
            configFrame.append(QByteArray::fromHex("016402036480002864FF"));
            emit sendFrameSignal(configFrame);

            QTimer::singleShot(200, this, [this, uiChannel, physicalChannel]() {
                if (!deviceConnected) {
                    ultrasonicStartPending = false;
                    showStatus(QStringLiteral("下位机已断开，超声上传命令未发送。"));
                    return;
                }

                emit sendFrameSignal(commandLittleEndianU16(
                    static_cast<quint16>(kCmdUltrasonicStartBase + 10 * physicalChannel)));

                activeUltrasonicChannel = physicalChannel;
                ultrasonicStartPending = false;
                ultrasonicAcquiring = true;
                ui->ultrasonicStartButton->setText(QStringLiteral("停止采集"));
                showStatus(QStringLiteral("已启动界面通道 %1 的波形上传。").arg(uiChannel + 1));
            });
        });
    });
}

// =====================================================================
// 接收侧：由 DealFile 在后台线程解析后通过信号送达（队列连接）
// =====================================================================

// ---- 振动波形 ----
void H_window::onVibrationDataReceived(quint8 channel, const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() % 4 != 0) {
        return;
    }

    const int sampleCount = payload.size() / 4;
    if (channel > 3 || sampleCount <= 0) {
        return;
    }
    if (activeVibrationChannel != static_cast<int>(channel)) {
        return;
    }

    const double V_REF = 5.0;
    const double ADC_MAX_VAL = 8388608.0;

    QVector<double> voltageSamples;
    voltageSamples.reserve(sampleCount);

    double maxVoltage = -99999.0;
    double minVoltage = 99999.0;

    const uchar *raw = reinterpret_cast<const uchar *>(payload.constData());
    for (int index = 0; index < sampleCount; ++index) {
        const quint32 word = static_cast<quint32>(raw[index * 4])
                | (static_cast<quint32>(raw[index * 4 + 1]) << 8)
                | (static_cast<quint32>(raw[index * 4 + 2]) << 16)
                | (static_cast<quint32>(raw[index * 4 + 3]) << 24);
        const quint32 code24 = word & 0x00FFFFFFU;
        const qint32 signedSample = (code24 & 0x00800000U)
                ? static_cast<qint32>(code24 | 0xFF000000U)
                : static_cast<qint32>(code24);

        const double currentV =
                (static_cast<double>(signedSample) / ADC_MAX_VAL) * V_REF;
        voltageSamples.append(currentV);

        if (currentV > maxVoltage) maxVoltage = currentV;
        if (currentV < minVoltage) minVoltage = currentV;
    }

    const double amplitude = maxVoltage - minVoltage;

    int firstCrossingIndex = -1;
    int lastCrossingIndex = -1;
    int crossingCount = 0;
    for (int i = 1; i < voltageSamples.size(); ++i) {
        if (voltageSamples[i - 1] < 0 && voltageSamples[i] >= 0) {
            if (firstCrossingIndex == -1) {
                firstCrossingIndex = i;
            }
            lastCrossingIndex = i;
            crossingCount++;
        }
    }

    double frequency = 0.0;
    if (crossingCount >= 2 && lastCrossingIndex > firstCrossingIndex) {
        const int fullPeriods = crossingCount - 1;
        const int pointsBetween = lastCrossingIndex - firstCrossingIndex;
        frequency = (static_cast<double>(fullPeriods) * kVibrationSampleRate)
                / pointsBetween;
    }

    ui->vibrationWaveform->setVibrationData(voltageSamples, frequency,
                                            amplitude, channel);
}

// ---- 电压快照 ----
void H_window::onVoltageSnapshotReceived(const QByteArray &payload)
{
    if (payload.size() != kSnapshotChannelCount * static_cast<int>(sizeof(float))) {
        return;
    }
    if (!voltageSnapshotPending) {
        return;
    }

    const uchar *raw = reinterpret_cast<const uchar *>(payload.constData());
    for (int channel = 0; channel < kSnapshotChannelCount; ++channel) {
        const quint32 bits = static_cast<quint32>(raw[channel * 4])
                | (static_cast<quint32>(raw[channel * 4 + 1]) << 8)
                | (static_cast<quint32>(raw[channel * 4 + 2]) << 16)
                | (static_cast<quint32>(raw[channel * 4 + 3]) << 24);
        float voltage = 0.0f;
        std::memcpy(&voltage, &bits, sizeof(voltage));

        setCenteredItem(ui->voltageTable, channel / 4, channel % 4,
                        QStringLiteral("通道%1：%2 V")
                        .arg(channel).arg(voltage, 0, 'f', 4));
    }
    voltageSnapshotPending = false;
    showStatus(QStringLiteral("已接收八通道实际电压值。"));
}

// ---- 电流快照 ----
void H_window::onCurrentSnapshotReceived(const QByteArray &payload)
{
    if (payload.size() != kSnapshotChannelCount * static_cast<int>(sizeof(float))) {
        return;
    }
    if (!currentSnapshotPending) {
        return;
    }

    const uchar *raw = reinterpret_cast<const uchar *>(payload.constData());
    for (int channel = 0; channel < kSnapshotChannelCount; ++channel) {
        const quint32 bits = static_cast<quint32>(raw[channel * 4])
                | (static_cast<quint32>(raw[channel * 4 + 1]) << 8)
                | (static_cast<quint32>(raw[channel * 4 + 2]) << 16)
                | (static_cast<quint32>(raw[channel * 4 + 3]) << 24);
        float currentAmpere = 0.0f;
        std::memcpy(&currentAmpere, &bits, sizeof(currentAmpere));
        const float currentMilliampere = currentAmpere * 1000.0f;

        setCenteredItem(ui->currentTable, channel / 4, channel % 4,
                        QStringLiteral("通道%1：%2 mA")
                        .arg(channel).arg(currentMilliampere, 0, 'f', 3));
    }
    currentSnapshotPending = false;
    showStatus(QStringLiteral("已接收八通道实际电流值（单位：mA）。"));
}

// ---- 超声波形 ----
void H_window::onUltrasonicDataReceived(quint8 channelID, const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() % sizeof(qint32) != 0) {
        return;
    }

    int pointCount = payload.size() / sizeof(qint32);
    if (pointCount <= 0) return;

    QVector<qint32> originalSamples;
    originalSamples.reserve(pointCount);

    const uchar *raw = reinterpret_cast<const uchar *>(payload.constData());
    for (int point = 0; point < pointCount; ++point) {
        const quint32 bits = static_cast<quint32>(raw[point * 4])
                | (static_cast<quint32>(raw[point * 4 + 1]) << 8)
                | (static_cast<quint32>(raw[point * 4 + 2]) << 16)
                | (static_cast<quint32>(raw[point * 4 + 3]) << 24);
        const qint64 signedValue = (bits & 0x80000000U)
                ? static_cast<qint64>(bits) - 0x100000000LL
                : static_cast<qint64>(bits);
        originalSamples.append(static_cast<qint32>(signedValue));
    }

    if (originalSamples.isEmpty()) return;

    // ==========================================================
    // 【完美的对数平滑压缩算法 (Logarithmic Compression)】
    // 特点：
    // 1. 最高峰值原汁原味，不加任何限制，自动显示真实具体数值。
    // 2. 曲线 100% 顺滑，没有任何折角。
    // 3. 底部波动会被平滑放大 10~20 倍，完美填满下方 3 个网格！
    // ==========================================================
    QVector<qint32> displaySamples;
    displaySamples.reserve(pointCount);

    // 1. 寻找当前帧的全局真实最高峰值 (例如 4723)
    qint32 maxAbsRaw = 0;
    for (int i = 0; i < pointCount; ++i) {
        maxAbsRaw = qMax(maxAbsRaw, qAbs(originalSamples[i]));
    }

    // 2. 控制放大倍数的核心系数 C。C=80 是一个极佳的黄金平衡点。
    // 真实值为 100 左右的小波动，会被平滑放大到 1000 多，占领下方网格。
    // 真实值 4000 以上的大震波，会被柔和地安排在最上面一层网格。
    double C = 80.0;
    double log1pC = std::log(1.0 + C);

    for (int i = 0; i < pointCount; ++i) {
        double realVal = static_cast<double>(originalSamples[i]);
        double absVal = std::abs(realVal);
        double sign = (realVal < 0) ? -1.0 : 1.0;

        qint32 mappedVal = 0;
        if (maxAbsRaw > 0) {
            // 核心对数映射公式: y = V_max * ln(1 + C * (x / V_max)) / ln(1 + C)
            // 该公式能保证 0 永远是 0，最大值永远是最大值，中间过程平滑拱起
            double normalized = absVal / static_cast<double>(maxAbsRaw);
            double mappedDouble = static_cast<double>(maxAbsRaw) * (std::log(1.0 + C * normalized) / log1pC);
            mappedVal = static_cast<qint32>(sign * mappedDouble);
        }
        displaySamples.append(mappedVal);
    }

    // 【恢复真实峰值显示】：清除强制锁死的 Y 轴，让它随着真实的 maxAbsRaw 自动生成最高刻度标签
    ui->ultrasonicWaveform->clearFixedValueRange();
    // ==========================================================

    ui->ultrasonicWaveform->setWaveform(
                displaySamples,
                static_cast<quint32>(pointCount),
                ultrasonicPacketSequence++,
                static_cast<int>(channelID));
}
// =====================================================================
// 协议辅助函数（与原 自测QT 完全一致）
// =====================================================================
quint32 H_window::readLittleEndianU32(const QByteArray &data, int offset)
{
    const uchar *bytes = reinterpret_cast<const uchar *>(data.constData() + offset);
    return static_cast<quint32>(bytes[0])
            | (static_cast<quint32>(bytes[1]) << 8)
            | (static_cast<quint32>(bytes[2]) << 16)
            | (static_cast<quint32>(bytes[3]) << 24);
}

quint16 H_window::readLittleEndianU16(const QByteArray &data, int offset)
{
    const uchar *bytes = reinterpret_cast<const uchar *>(data.constData() + offset);
    return static_cast<quint16>(bytes[0])
            | (static_cast<quint16>(bytes[1]) << 8);
}

QByteArray H_window::commandLittleEndianU16(quint16 command)
{
    QByteArray bytes;
    bytes.append(static_cast<char>(command & 0xFF));
    bytes.append(static_cast<char>((command >> 8) & 0xFF));
    return bytes;
}

QByteArray H_window::build5007StatusPacket(int firstStatusIndex, int statusCount)
{
    QByteArray packet = QByteArray::fromHex("8F13EA07050010000C0000000000");
    for (int statusIndex = 0; statusIndex < 20; ++statusIndex) {
        const bool enabled = statusIndex >= firstStatusIndex
                && statusIndex < firstStatusIndex + statusCount;
        packet.append(static_cast<char>(enabled ? 0x01 : 0x00));
        packet.append(static_cast<char>(0x00));
    }
    return packet;
}

// ==============================================================
// 文件查看功能区：查询、读取、删除
// ==============================================================

void H_window::on_btnQueryFile_clicked()
{
    if (!deviceConnected) {
        showStatus("未连接下位机，无法查询文件。");
        return;
    }
    ui->fileTable->setRowCount(0); // 清空旧列表
    showStatus("正在查询下位机文件，请稍候...");

    // 向下位机发送 306 指令请求文件列表
    emit sendFrameSignal(commandLittleEndianU16(306));
}

void H_window::on_btnReadFile_clicked()
{
    QList<QTableWidgetItem*> selectedItems = ui->fileTable->selectedItems();
    if (selectedItems.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先在列表中选择要读取的文件！");
        return;
    }

    // ==========================================
    // 【修改】：使用绝对路径指向 exe 所在目录
    // ==========================================
    QString dataDirPath = QCoreApplication::applicationDirPath() + "/Data";
    QDir dir;
    if (!dir.exists(dataDirPath)) {
        dir.mkpath(dataDirPath);
    }

    // 收集选中的文件名并加入队列 (去重)
    QList<int> selectedRows;
    for (auto item : selectedItems) {
        if (!selectedRows.contains(item->row())) {
            selectedRows.append(item->row());
        }
    }

    int addedCount = 0;
    for (int row : selectedRows) {
        QString fileName = ui->fileTable->item(row, 0)->text();
        if (!m_downloadQueue.contains(fileName) && !fileName.isEmpty()) {
            m_downloadQueue.append(fileName);
            addedCount++;
        }
    }

    if (addedCount == 0) return;

    // 如果当前没有正在下载的文件，则启动队列引擎
    if (m_currentDownloadFile == nullptr && !m_downloadQueue.isEmpty()) {
        processNextDownload();
    } else {
        QMessageBox::information(this, "提示", QString("已将 %1 个文件追加到下载队列，请等待...").arg(addedCount));
    }
}

void H_window::processNextDownload()
{
    QString dataDirPath = QCoreApplication::applicationDirPath() + "/Data";

    if (m_downloadQueue.isEmpty()) {
        QMessageBox::information(this, "下载完成", "所有选中的文件已全部下载完毕！");
        showStatus("文件批量下载完成。");

        // ==========================================
        // 【大招】：下载完成后，自动打开电脑上的 Data 文件夹！
        // ==========================================
        QDesktopServices::openUrl(QUrl::fromLocalFile(dataDirPath));
        return;
    }

    m_currentFileName = m_downloadQueue.takeFirst();

    // 【修改】：使用绝对路径保存文件
    QString savePath = dataDirPath + "/" + m_currentFileName;

    m_currentDownloadFile = new QFile(savePath);
    if (!m_currentDownloadFile->open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "错误", "无法在 Data 文件夹创建文件：" + m_currentFileName);
        delete m_currentDownloadFile;
        m_currentDownloadFile = nullptr;
        processNextDownload();
        return;
    }

    showStatus(QString("正在下载文件: %1 ...").arg(m_currentFileName), 0); // 0表示不自动清除提示

    // 【拼装 307 下载请求帧】：指令码 307(2字节) + 文件名长度(1字节) + 文件名文本
    QByteArray requestData = commandLittleEndianU16(307);
    QByteArray nameBytes = m_currentFileName.toUtf8();
    requestData.append(static_cast<char>(nameBytes.size()));
    requestData.append(nameBytes);

    emit sendFrameSignal(requestData);
}

void H_window::onFileDataReceived(const QByteArray &data)
{
    if (m_currentDownloadFile && m_currentDownloadFile->isOpen()) {
        m_currentDownloadFile->write(data);
    }
}

void H_window::onFileTransferFinished()
{
    if (m_currentDownloadFile) {
        m_currentDownloadFile->close();
        delete m_currentDownloadFile;
        m_currentDownloadFile = nullptr;
    }
    // 当前文件下载完，立刻去拿队列里的下一个文件
    processNextDownload();
}

void H_window::on_btnDeleteFile_clicked()
{
    QList<QTableWidgetItem*> selectedItems = ui->fileTable->selectedItems();
    if (selectedItems.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先在列表中选择要删除的文件！");
        return;
    }

    if (QMessageBox::question(this, "确认", "确定要删除选中的文件吗？下位机中的源文件也将被永久删除！") != QMessageBox::Yes) {
        return;
    }

    // 收集要删除的行号（从大到小排序，防止删除前面行导致后面行号错位）
    QList<int> selectedRows;
    for (auto item : selectedItems) {
        if (!selectedRows.contains(item->row())) {
            selectedRows.append(item->row());
        }
    }
    std::sort(selectedRows.begin(), selectedRows.end(), std::greater<int>());

    for (int row : selectedRows) {
        QString fileName = ui->fileTable->item(row, 0)->text();

        // 【拼装 308 删除请求帧】：指令码 308(2字节) + 文件名长度(1字节) + 文件名文本
        QByteArray requestData = commandLittleEndianU16(308);
        QByteArray nameBytes = fileName.toUtf8();
        requestData.append(static_cast<char>(nameBytes.size()));
        requestData.append(nameBytes);

        emit sendFrameSignal(requestData);

        // 界面上直接移除这一行
        ui->fileTable->removeRow(row);
    }
    showStatus("删除指令已发送。");
}

void H_window::onFileListReceived(const QStringList &fileList)
{
    // 1. 界面更新：去掉文件大小，恢复3列
    ui->fileTable->setColumnCount(3);
    ui->fileTable->setHorizontalHeaderLabels(QStringList() << "文件名" << "创建时间" << "存储位置");

    // 2. 将数据转存并进行自定义排序
    QStringList sortedList = fileList;
    std::sort(sortedList.begin(), sortedList.end(), [](const QString &a, const QString &b) {
        // 提取前缀的 Lambda 工具函数
        auto getPrefix = [](const QString &str, bool &isNum, int &num) {
            int idx = str.indexOf('_');
            if (idx != -1) {
                QString pre = str.left(idx);
                bool ok;
                num = pre.toInt(&ok);
                isNum = ok;
            } else {
                isNum = false;
                num = 0;
            }
        };

        bool isNumA, isNumB;
        int numA, numB;
        getPrefix(a, isNumA, numA);
        getPrefix(b, isNumB, numB);

        // 规则 1：纯数字开头的排在最前面 (例如 1_ 优先于 S0_)
        if (isNumA && !isNumB) return true;
        if (!isNumA && isNumB) return false;

        // 规则 2：如果都是纯数字，按照数字从小到大升序 (0, 1, 2, ... 19)
        if (isNumA && isNumB) {
            if (numA != numB) {
                return numA < numB;
            }
        }

        // 规则 3：如果都是字母，或者数字前缀相同，则按时间降序（最新的排上面）
        // 因为时间格式固定，直接比较字符串即可实现时间降序
        return a > b;
    });

    // 3. 填入表格
    ui->fileTable->setRowCount(sortedList.size());
    for (int i = 0; i < sortedList.size(); ++i) {
        QString filename = sortedList[i];
        setCenteredItem(ui->fileTable, i, 0, filename);

        // 4. 智能正则提取时间（完美兼容 .bin 和 .txt 的不同命名风格）
        QString timeStr = "--";
        QRegExp rxBin("(\\d{4})_(\\d{2})_(\\d{2})_(\\d{2})_(\\d{2})_(\\d{2})"); // 匹配 2026_05_16_12_00_00
        QRegExp rxTxt("(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})"); // 匹配 20260516_124559

        if (rxBin.indexIn(filename) != -1) {
            timeStr = QString("%1-%2-%3 %4:%5:%6")
                      .arg(rxBin.cap(1)).arg(rxBin.cap(2)).arg(rxBin.cap(3))
                      .arg(rxBin.cap(4)).arg(rxBin.cap(5)).arg(rxBin.cap(6));
        } else if (rxTxt.indexIn(filename) != -1) {
            timeStr = QString("%1-%2-%3 %4:%5:%6")
                      .arg(rxTxt.cap(1)).arg(rxTxt.cap(2)).arg(rxTxt.cap(3))
                      .arg(rxTxt.cap(4)).arg(rxTxt.cap(5)).arg(rxTxt.cap(6));
        }

        setCenteredItem(ui->fileTable, i, 1, timeStr);
        setCenteredItem(ui->fileTable, i, 2, "/mnt/file/"); // 第 3 列直接显示存储位置
    }
    showStatus(QString("文件查询完成，共 %1 个文件。").arg(sortedList.size()));
}
