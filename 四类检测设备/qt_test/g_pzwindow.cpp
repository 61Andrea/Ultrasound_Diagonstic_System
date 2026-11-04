#include "g_pzwindow.h"
#include "ui_g_pzwindow.h"
// g_pzwindow.cpp
#include <QTimer> // 必须包含定时器头文件以实现按钮闪烁

G_pzWindow::G_pzWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::G_pzWindow)
{
    ui->setupUi(this);
}


void G_pzWindow::on_btn_config_complete_clicked()
{
    QByteArray frame;

    // 1. 压入帧头 AA 55
    frame.append(static_cast<char>(0xAA));
    frame.append(static_cast<char>(0x55));

    // 2. 压入通道号 (ComboBox 直接取 index)
    frame.append(static_cast<char>(ui->PZ_G1_1->currentIndex()));

    // 3. 压入后面 9 个参数 (从文本框/微调框读取并转为整数发向网络)
    frame.append(static_cast<char>(ui->PZ_G1_2->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_3->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_4->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_5->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_6->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_7->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_8->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_9->text().toUInt()));
    frame.append(static_cast<char>(ui->PZ_G1_10->text().toUInt()));

    // 4. 压入帧尾 FF
    frame.append(static_cast<char>(0xFF));

    // 5. 将这串数据发往外层的 G_window
    emit sendConfigFrame(frame);

    // 6. UI 交互：给按钮加一个“下发成功”的短暂反馈，不关闭窗口
    QString originalText = ui->btn_config_complete->text();
    ui->btn_config_complete->setText("配置已下发！");
    ui->btn_config_complete->setEnabled(false);

    QTimer::singleShot(800, this, [this, originalText](){
        ui->btn_config_complete->setText(originalText);
        ui->btn_config_complete->setEnabled(true);
    });
}

G_pzWindow::~G_pzWindow()
{
    delete ui;
}
