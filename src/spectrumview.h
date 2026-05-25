/*
 *  Copyright (C) 2026, Jacob Gilbert
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

#include <QWidget>
#include <cstddef>
#include <vector>

class SpectrogramPlot;
class PlotView;
class QDockWidget;

// A standalone power-spectral-density (PSD) view: power (dB) on the horizontal
// axis, frequency on the vertical axis so it lines up with the spectrogram it's
// docked beside. It renders the FFT column under the mouse pointer, fed live by
// PlotView via setSample(). Note this is a plain QWidget, NOT part of the Plot
// pull-graph family -- it sits in its own (detachable) dock rather than the
// stacked plot view.
class SpectrumView : public QWidget
{
    Q_OBJECT

public:
    SpectrumView(SpectrogramPlot *spectrogram, PlotView *plotView, QWidget *parent = nullptr);

    QSize sizeHint() const override;

public slots:
    // Select which spectrogram column (absolute sample index) to display.
    void setSample(size_t sample);
    // Show/hide the power (dB) scale (driven by the Scales checkbox).
    void enableScales(bool enabled);
    // Drop the memoised FFT lines (e.g. after the input file is reloaded).
    void invalidateCache();

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    SpectrogramPlot *spectrogram;
    PlotView *plotView;
    size_t currentSample = 0;
    bool hasSample = false;
    bool scalesEnabled = true;
    bool persistenceEnabled = false;  // overlay neighbouring-column FFTs (right-click toggle)

    // Memo of the FFT lines drawn last paint so pure repaints (scroll, resize,
    // power-range) don't recompute them; rebuilt when any keyed field changes.
    std::vector<std::vector<float>> cachedLines;
    size_t cacheSample = 0;
    int cacheFFTSize = 0;
    int cacheStride = 0;
    bool cachePersistence = false;
    bool cacheValid = false;

    // The QDockWidget MainWindow wraps this view in, or nullptr if not docked.
    QDockWidget *dock() const;
    void saveToFile();
};
