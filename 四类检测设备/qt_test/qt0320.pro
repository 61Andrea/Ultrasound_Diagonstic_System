QT       += core gui network printsupport # 确保加上 network 模块


greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11
CONFIG += console

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    a_window.cpp \
    b_window.cpp \
    c_window.cpp \
    d_window.cpp \
    f_window.cpp \
    g_window.cpp \
    g_pzwindow.cpp \
    dealfile.cpp \
    qcustomplot.cpp \
    c_pzwindow.cpp \
    h_window.cpp \
    waveformwidget.cpp

HEADERS += \
    mainwindow.h \
    a_window.h \
    b_window.h \
    c_window.h \
    d_window.h \
    f_window.h \
    g_window.h \
    g_pzwindow.h \
    dealfile.h \
    qcustomplot.h \
    c_pzwindow.h \
    h_window.h \
    waveformwidget.h

FORMS += \
    mainwindow.ui \
    a_window.ui \
    b_window.ui \
    c_window.ui \
    d_window.ui \
    f_window.ui \
    g_window.ui \
    g_pzwindow.ui \
    c_pzwindow.ui \
    h_window.ui

# 信号监测协议的抓包样例，仅作协议对照参考，不参与编译
DISTFILES += \
    docs/vibration_packet_ch0.txt \
    docs/ultrasonic_packet_ch0.txt \
    docs/voltage_snapshot_packet.txt \
    docs/current_snapshot_packet.txt

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    picture.qrc
