/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QCache>
#include <QString>
#include <QWidget>
#include "fft.h"
#include "inputsource.h"
#include "plot.h"
#include "tuner.h"
#include "tunertransform.h"

#include <memory>
#include <array>
#include <math.h>
#include <vector>

class AnnotationLocation;


class TileCacheKey
{

public:
    TileCacheKey(int fftSize, int zoomLevel, int nfftSkip, size_t sample) {
        this->fftSize = fftSize;
        this->zoomLevel = zoomLevel;
        this->nfftSkip = nfftSkip;
        this->sample = sample;
    }

    bool operator==(const TileCacheKey &k2) const {
        return (this->fftSize == k2.fftSize) &&
               (this->zoomLevel == k2.zoomLevel) &&
               (this->nfftSkip == k2.nfftSkip) &&
               (this->sample == k2.sample);
    }

    int fftSize;
    int zoomLevel;
    int nfftSkip;
    size_t sample;
};

class SpectrogramPlot : public Plot
{
    Q_OBJECT

public:
    SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src);
    void invalidateEvent() override;
    std::shared_ptr<AbstractSampleSource> output() override;
    void paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    bool mouseEvent(QEvent::Type type, QMouseEvent *event) override;
    void leaveEvent();
    std::shared_ptr<SampleSource<std::complex<float>>> input() { return inputSource; };
    double getCenterFrequency();
    double frequencyAt(int y);

    // Accessors used by the standalone spectrum (PSD) window. getSpectrumLine()
    // returns the same per-column FFT power (dB) the spectrogram renders, one
    // value per bin from -Fs/2 (index 0) to +Fs/2 (index fftSize-1).
    std::vector<float> getSpectrumLine(size_t sample);
    int getColumnStride() { return getStride(); }  // samples between adjacent columns
    int getFFTSize() { return fftSize; }
    float getPowerMin() { return powerMin; }
    float getPowerMax() { return powerMax; }
    double getSampleRate() { return sampleRate; }
    bool isRealSignal() { return inputSource->realSignal(); }
    double getTunerOffsetFrequency();
    double getTunerRelativeBandwidth();
    void setDecimationPreview(int decimation, double frequencyOffset = 0);
    void setSampleRate(double sampleRate);
    bool tunerEnabled();
    void enableScales(bool enabled);
    void enableAnnotations(bool enabled);
    void enableAnnoLabels(bool enabled);
    bool isAnnotationsEnabled();
    void enableAnnoColors(bool enabled);
    QString *mouseAnnotationComment(const QMouseEvent *event);

public slots:
    void setFFTSize(int size);
    void setPowerMax(int power);
    void setPowerMin(int power);
    void setZoomLevel(int zoom);
    void setSkip(int skip);
    void tunerMoved();

private:
    const int linesPerGraduation = 50;
    static const int tileSize = 65536; // This must be a multiple of the maximum FFT size

    std::shared_ptr<SampleSource<std::complex<float>>> inputSource;
    std::vector<AnnotationLocation> visibleAnnotationLocations;
    std::unique_ptr<FFT> fft;
    std::unique_ptr<float[]> window;
    QCache<TileCacheKey, QPixmap> pixmapCache;
    QCache<TileCacheKey, std::array<float, tileSize>> fftCache;
    uint colormap[256];

    int fftSize;
    int zoomLevel;
    int nfftSkip;
    int decimationPreview = 0;
    double decimationOffset = 0;
    float powerMax;
    float powerMin;
    double sampleRate;
    bool frequencyScaleEnabled;
    bool sigmfAnnotationsEnabled;
    bool sigmfAnnotationLabels;
    bool sigmfAnnotationColors;

    Tuner tuner;
    std::shared_ptr<TunerTransform> tunerTransform;

    QPixmap* getPixmapTile(size_t tile);
    float* getFFTTile(size_t tile);
    void getLine(float *dest, size_t sample);
    int getStride();
    int spectrumHeight();
    float getTunerPhaseInc();
    std::vector<float> getTunerTaps();
    int linesPerTile();
    void paintFrequencyScale(QPainter &painter, QRect &rect);
    void paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    void paintDecimationOverlay(QPainter &painter, QRect &rect);
};

class AnnotationLocation
{
public:
    Annotation annotation;

    AnnotationLocation(Annotation annotation, int x, int y, int width, int height)
        : annotation(annotation), x(x), y(y), width(width), height(height) {}

    bool isInside(int pos_x, int pos_y) {
        return (x <= pos_x) && (pos_x <= x + width)
            && (y <= pos_y) && (pos_y <= y + height);
    }

private:
    int x;
    int y;
    int width;
    int height;
};
