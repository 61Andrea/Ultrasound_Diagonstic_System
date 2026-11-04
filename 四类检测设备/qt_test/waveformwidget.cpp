#include "waveformwidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>

// =====================================================================
// 波形绘制控件（自测QT 移植，逻辑保持不变）
// =====================================================================

namespace {
// 振动通道标称采样率。
// 注意：振动包头偏移 10 处上报的采样率为 4096 Hz（0x1000），但原工程注释明确指出
// “忽略硬件上报的假采样率，强行写入真实的 ADC 物理采样率”，故此处沿用 64000。
// 若日后确认硬件真实采样率不同，只需修改这一处常量（以及 h_window.cpp 中的
// REAL_SAMPLE_RATE，两者用于过零测频，必须保持一致）。
const quint32 kVibrationSampleRateHz = 64000;
}

WaveformWidget::WaveformWidget(QWidget *parent) :
    QWidget(parent),
    m_sampleRateHz(0),
    m_sequence(0),
    m_channel(-1),
    m_hasFixedValueRange(false),
    m_fixedMinimum(0.0),
    m_fixedMaximum(0.0),
    m_hasAdaptiveValueRange(false),
    m_adaptiveRangeInitialized(false),
    m_adaptiveMinimum(0.0),
    m_adaptiveMaximum(0.0),
    m_adaptiveMinimumSpan(0.0),
    m_continuousLineMode(false),
    m_waveformLineWidth(1.0),
    m_signalName("IEPE"),
    m_vibrationFrequency(0.0),
    m_vibrationAmplitude(0.0)
{
    setMinimumSize(300, 180);
}

void WaveformWidget::setWaveform(const QVector<quint32> &samples,
                                 quint32 sampleRateHz,
                                 quint32 sequence,
                                 int channel)
{
    m_samples.clear();
    m_samples.reserve(samples.size());
    for (quint32 sample : samples) {
        m_samples.append(sample);
    }
    m_sampleRateHz = sampleRateHz;
    m_sequence = sequence;
    m_channel = channel;
    update();
}

void WaveformWidget::setWaveform(const QVector<qint32> &samples,
                                 quint32 sampleRateHz,
                                 quint32 sequence,
                                 int channel)
{
    m_samples.clear();
    m_samples.reserve(samples.size());
    for (qint32 sample : samples) {
        m_samples.append(sample);
    }
    m_sampleRateHz = sampleRateHz;
    m_sequence = sequence;
    m_channel = channel;
    update();
}

void WaveformWidget::setVibrationData(const QVector<double> &voltageData,
                                     double frequency, double amplitude,
                                     int channel)
{
    m_samples.clear();
    m_samples.reserve(voltageData.size());

    // 🔴 1. 核心修复：将电压采样点乘以 2.0，让波峰正确顶到 +5V，波谷踩到 -5V
    for (double sample : voltageData) {
        m_samples.append(sample);
    }

    m_channel = channel;
    m_vibrationFrequency = frequency;

    // 🔴 2. 核心修复：文字幅度也保持对应除以 2.0（对应真实的 5V 幅度）
    // （注：取消之前的除以 2）
    m_vibrationAmplitude = amplitude / 2.0;

    // 🔴 3. 量程锁定为 -10V ~ +10V，波峰 +5V 正好位于上半区正中心
    if (m_signalName == "IEPE") {
        m_hasFixedValueRange = true;
        m_fixedMinimum = -10.0;
        m_fixedMaximum = 10.0;
    }

    // 固定使用标称采样率，让 X 轴换算出真实时间
    m_sampleRateHz = kVibrationSampleRateHz;

    // 强制触发界面重绘
    update();
}

void WaveformWidget::setFixedValueRange(double minimum, double maximum)
{
    m_hasFixedValueRange = minimum < maximum;
    m_fixedMinimum = minimum;
    m_fixedMaximum = maximum;
    update();
}

void WaveformWidget::clearFixedValueRange()
{
    m_hasFixedValueRange = false;
    update();
}

void WaveformWidget::setAdaptiveValueRange(double minimumSpan)
{
    m_hasFixedValueRange = false;
    m_hasAdaptiveValueRange = minimumSpan > 0.0;
    m_adaptiveRangeInitialized = false;
    m_adaptiveMinimumSpan = minimumSpan;
    update();
}

void WaveformWidget::setContinuousLineMode(bool enabled)
{
    m_continuousLineMode = enabled;
    update();
}

void WaveformWidget::setWaveformLineWidth(qreal pixels)
{
    m_waveformLineWidth = qMax<qreal>(1.0, pixels);
    update();
}

void WaveformWidget::setSignalName(const QString &name)
{
    m_signalName = name;
    update();
}

void WaveformWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), QColor("#050810"));
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRect plotRect(58, 36, qMax(1, width() - 78), qMax(1, height() - 72));
    painter.setPen(QPen(QColor("#1A2B4C"), 1));
    for (int row = 0; row <= 4; ++row) {
        const int y = plotRect.top() + row * plotRect.height() / 4;
        painter.drawLine(plotRect.left(), y, plotRect.right(), y);
    }
    for (int column = 0; column <= 4; ++column) {
        const int x = plotRect.left() + column * plotRect.width() / 4;
        painter.drawLine(x, plotRect.top(), x, plotRect.bottom());
    }

    painter.setPen(QColor("#00E5FF"));
    if (m_samples.isEmpty()) {
        painter.drawText(plotRect, Qt::AlignCenter,
                         "等待下位机上传波形数据...");
        return;
    }

    double minimum = m_samples.first();
    double maximum = minimum;
    for (double sample : m_samples) {
        minimum = qMin(minimum, sample);
        maximum = qMax(maximum, sample);
    }

    double displayMinimum = minimum;
    double displayMaximum = maximum;
    if (m_hasFixedValueRange) {
        displayMinimum = m_fixedMinimum;
        displayMaximum = m_fixedMaximum;
    } else if (m_hasAdaptiveValueRange) {
        const double margin = qMax((maximum - minimum) * 0.08,
                                   m_adaptiveMinimumSpan * 0.04);
        const double targetMinimum = minimum - margin;
        const double targetMaximum = maximum + margin;
        const double targetCenter = (targetMinimum + targetMaximum) * 0.5;
        const double targetSpan = qMax(targetMaximum - targetMinimum,
                                       m_adaptiveMinimumSpan);

        if (!m_adaptiveRangeInitialized) {
            m_adaptiveMinimum = targetCenter - targetSpan * 0.5;
            m_adaptiveMaximum = targetCenter + targetSpan * 0.5;
            m_adaptiveRangeInitialized = true;
        } else {
            const double previousCenter =
                    (m_adaptiveMinimum + m_adaptiveMaximum) * 0.5;
            const double previousSpan = m_adaptiveMaximum - m_adaptiveMinimum;
            const double nextCenter = previousCenter * 0.80 + targetCenter * 0.20;
            double nextSpan = qMax(targetSpan, previousSpan * 0.90);
            double nextMinimum = nextCenter - nextSpan * 0.5;
            double nextMaximum = nextCenter + nextSpan * 0.5;

            nextMinimum = qMin(nextMinimum, targetMinimum);
            nextMaximum = qMax(nextMaximum, targetMaximum);
            m_adaptiveMinimum = nextMinimum;
            m_adaptiveMaximum = nextMaximum;
        }
        displayMinimum = m_adaptiveMinimum;
        displayMaximum = m_adaptiveMaximum;
    }

    if (qFuzzyCompare(displayMinimum + 1.0, displayMaximum + 1.0)) {
        displayMinimum -= 1.0;
        displayMaximum += 1.0;
    }

    // Y 轴纵坐标：IEPE 通道显示带 2 位小数的电压值，超声保留无小数的 AD 值
    if (m_signalName == "IEPE") {
        painter.drawText(8, plotRect.top() + 5, QString::number(displayMaximum, 'f', 2) + " V");
        painter.drawText(8, plotRect.bottom(), QString::number(displayMinimum, 'f', 2) + " V");
    } else {
        painter.drawText(8, plotRect.top() + 5, QString::number(displayMaximum, 'f', 0));
        painter.drawText(8, plotRect.bottom(), QString::number(displayMinimum, 'f', 0));
    }

    const double durationSeconds = m_sampleRateHz == 0
            ? 0.0
            : static_cast<double>(m_samples.size()) / m_sampleRateHz;

    // 🔴 3. 恢复“幅度”字样，并显示除以2之后的真正单峰值
    if (m_signalName == "IEPE") {
        painter.drawText(8, 20,
                         QString("IEPE%1  |  频率 %2 Hz  |  幅度 %3 V")
                         .arg(m_channel)
                         .arg(m_vibrationFrequency, 0, 'f', 1)
                         .arg(m_vibrationAmplitude, 0, 'f', 3));
    } else {
        painter.drawText(8, 20,
                         QString("%1%2")
                         .arg(m_signalName)
                         .arg(m_channel + 1));
    }

    painter.drawText(plotRect.left(), height() - 10, "0 s");
    painter.drawText(plotRect.right() - 48, height() - 10,
                     QString::number(durationSeconds, 'f', 3) + " s");

    painter.setPen(QPen(QColor("#00E5FF"), m_waveformLineWidth));
    if (m_continuousLineMode) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath waveformPath;
        for (int index = 0; index < m_samples.size(); ++index) {
            const double x = plotRect.left() + (m_samples.size() == 1
                    ? 0.0
                    : static_cast<double>(index) * plotRect.width()
                    / static_cast<double>(m_samples.size() - 1));
            int y = plotRect.bottom() - static_cast<int>(
                        (m_samples.at(index) - displayMinimum) /
                        (displayMaximum - displayMinimum) * plotRect.height());
            y = qBound(plotRect.top(), y, plotRect.bottom());
            if (index == 0) {
                waveformPath.moveTo(x, y);
            } else {
                waveformPath.lineTo(x, y);
            }
        }
        painter.drawPath(waveformPath);
        return;
    }

    // 按像素列做 min-max 抽稀（点远多于像素时使用）
    const int pixelCount = plotRect.width() + 1;
    for (int pixel = 0; pixel < pixelCount; ++pixel) {
        const int begin = pixel * m_samples.size() / pixelCount;
        const int end = qMax(begin + 1,
                             (pixel + 1) * m_samples.size() / pixelCount);
        double localMinimum = m_samples.at(begin);
        double localMaximum = localMinimum;
        for (int index = begin + 1; index < end && index < m_samples.size(); ++index) {
            localMinimum = qMin(localMinimum, m_samples.at(index));
            localMaximum = qMax(localMaximum, m_samples.at(index));
        }

        int yMinimum = plotRect.bottom() - static_cast<int>(
                    (localMinimum - displayMinimum) /
                    (displayMaximum - displayMinimum) * plotRect.height());
        int yMaximum = plotRect.bottom() - static_cast<int>(
                    (localMaximum - displayMinimum) /
                    (displayMaximum - displayMinimum) * plotRect.height());
        yMinimum = qBound(plotRect.top(), yMinimum, plotRect.bottom());
        yMaximum = qBound(plotRect.top(), yMaximum, plotRect.bottom());
        painter.drawLine(plotRect.left() + pixel, yMinimum,
                         plotRect.left() + pixel, yMaximum);
    }
}
