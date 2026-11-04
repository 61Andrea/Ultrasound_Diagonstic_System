#ifndef C_PZWINDOW_H
#define C_PZWINDOW_H

#include <QDialog>

namespace Ui {
class C_pzwindow;
}

class C_pzwindow : public QDialog
{
    Q_OBJECT

private slots:

        //void on_btn_config_complete_clicked();


public:
    explicit C_pzwindow(QWidget *parent = nullptr);
    ~C_pzwindow();

private:
    Ui::C_pzwindow *ui;
};

#endif // C_PZWINDOW_H
