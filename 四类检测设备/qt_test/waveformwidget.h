#ifndef WAVEFORMWIDGET_H
#define WAVEFORMWIDGET_H

// =====================================================================
// 波形绘制控件（自测QT 移植）
// 无第三方依赖，纯 QPainter 手绘。原样搬入 qt_test 工程作为公共控件使用。
// =====================================================================

#include <QVector>
#include <QWidget>
#include <QString>

class WaveformWidget : public QWidget
{
public:
    explicit WaveformWidget(QWidget *parent = nullptr);

    void setWaveform(const QVector<quint32> &samples, quint32 sampleRateHz,
                     quint32 sequence, int channel);
    void setWaveform(const QVector<qint32> &samples, quint32 sampleRateHz,
                     quint32 sequence, int channel);

    // 振动数据接口（电压值 + 频率 + 幅度）
    void setVibrationData(const QVector<double> &voltageData, double frequency,
                          double amplitude, int channel);

    void setFixedValueRange(double minimum, double maximum);
    void clearFixedValueRange();
    void setAdaptiveValueRange(double minimumSpan);
    void setContinuousLineMode(bool enabled);
    void setWaveformLineWidth(qreal pixels);
    void setSignalName(const QString &name);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<double> m_samples;
    quint32 m_sampleRateHz;
    quint32 m_sequence;
    int m_channel;
    bool m_hasFixedValueRange;
    double m_fixedMinimum;
    double m_fixedMaximum;
    bool m_hasAdaptiveValueRange;
    bool m_adaptiveRangeInitialized;
    double m_adaptiveMinimum;
    double m_adaptiveMaximum;
    double m_adaptiveMinimumSpan;
    bool m_continuousLineMode;
    qreal m_waveformLineWidth;
    QString m_signalName;

    // 供 paintEvent 画标题栏用的频率/幅度
    double m_vibrationFrequency;
    double m_vibrationAmplitude;
};

#endif // WAVEFORMWIDGET_H
