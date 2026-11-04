#ifndef G_PZWINDOW_H
#define G_PZWINDOW_H

// g_pzwindow.h
#include <QByteArray>

#include <QDialog>

namespace Ui {
class G_pzWindow;
}

class G_pzWindow : public QDialog
{
    Q_OBJECT

public:
    explicit G_pzWindow(QWidget *parent = nullptr);
    ~G_pzWindow();


signals:
    // 往外发完整数据的信号
    void sendConfigFrame(const QByteArray &frame);

private slots:
    // 配置完成按钮的槽函数
    void on_btn_config_complete_clicked();

private:
    Ui::G_pzWindow *ui;
};

#endif // G_PZWINDOW_H
