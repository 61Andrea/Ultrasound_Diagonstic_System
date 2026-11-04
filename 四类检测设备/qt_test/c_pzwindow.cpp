#include "c_pzwindow.h"
#include "ui_c_pzwindow.h"

C_pzwindow::C_pzwindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::C_pzwindow)
{
    ui->setupUi(this);
}

C_pzwindow::~C_pzwindow()
{
    delete ui;
}
