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

#include "spectrumview.h"

#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QTextStream>
#include <cmath>
#include <fstream>
#include <vector>

#include "plotview.h"
#include "spectrogramplot.h"
#include "util.h"

SpectrumView::SpectrumView(SpectrogramPlot *spectrogram, PlotView *plotView, QWidget *parent)
    : QWidget(parent), spectrogram(spectrogram), plotView(plotView)
{
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setMinimumWidth(160);
}

QSize SpectrumView::sizeHint() const
{
    return QSize(240, 512);
}

QDockWidget *SpectrumView::dock() const
{
    return qobject_cast<QDockWidget *>(parentWidget());
}

void SpectrumView::setSample(size_t sample)
{
    // Skip redundant FFT + repaint when the column hasn't moved (e.g. the pointer
    // moved vertically within the same spectrogram column).
    if (hasSample && sample == currentSample)
        return;
    currentSample = sample;
    hasSample = true;
    update();
}

void SpectrumView::enableScales(bool enabled)
{
    scalesEnabled = enabled;
    update();
}

void SpectrumView::invalidateCache()
{
    cacheValid = false;
    update();
}

void SpectrumView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (spectrogram == nullptr)
        return;

    const int n = spectrogram->getFFTSize();
    const float powerMin = spectrogram->getPowerMin();
    const float powerMax = spectrogram->getPowerMax();
    const bool realSignal = spectrogram->isRealSignal();

    // Vertical (frequency) band: while docked, pin to the spectrogram's on-screen
    // extent so the frequency axes line up. The band is resolved fresh here (not
    // cached) so it can never go stale. When floated into its own window, fill the
    // whole widget instead.
    int bandTop = 0;
    int bandH = height();
    auto d = dock();
    bool floating = d && d->isFloating();
    if (!floating && plotView != nullptr) {
        int topGlobal, specHeight;
        if (plotView->spectrogramScreenBand(topGlobal, specHeight)) {
            bandTop = mapFromGlobal(QPoint(0, topGlobal)).y();
            bandH = specHeight;
        }
    }

    // The PSD spans the full width (power on X). The frequency axis is omitted
    // here: it duplicates the spectrogram's own scale and the pointer readout.
    QRect plotArea(0, bandTop, width(), bandH);

    if (n <= 0 || powerMax <= powerMin || plotArea.width() <= 0 || plotArea.height() <= 0)
        return;

    painter.setPen(Qt::white);
    QFontMetrics fm(painter.font());

    if (!hasSample) {
        painter.drawText(plotArea, Qt::AlignCenter | Qt::TextWordWrap,
                         "Move the pointer over the spectrogram");
        return;
    }

    // Bins span -Fs/2 (index 0) to +Fs/2 (index n-1). For real signals the
    // spectrogram shows only the positive half; mirror that so the trace matches
    // the spectrogram's layout (DC at the bottom, +Fs/2 at the top).
    const int binLo = realSignal ? n / 2 : 0;
    const int binHi = n - 1;
    if (binHi <= binLo)
        return;

    auto powerToX = [&](float power) {
        float t = (power - powerMin) / (powerMax - powerMin);
        t = clamp(t, 0.0f, 1.0f);
        return plotArea.left() + t * plotArea.width();
    };
    // Lower bins (more negative frequency) sit at the bottom, matching the
    // spectrogram's vertical layout.
    auto fracToY = [&](float frac) {
        return plotArea.bottom() - frac * plotArea.height();
    };

    if (scalesEnabled) {
        // Keep the dB axis labels inside the widget even when the band reaches
        // the bottom edge.
        int dbLabelY = std::min(plotArea.bottom() + fm.ascent() + 2, height() - 2);

        // --- Power grid + bottom scale (dB) ---
        double dbStep = 10.0;
        while ((powerMax - powerMin) / dbStep > 8.0)
            dbStep *= 2.0;
        for (double db = std::ceil(powerMin / dbStep) * dbStep; db <= powerMax; db += dbStep) {
            int x = (int)powerToX((float)db);
            painter.setPen(QPen(QColor(60, 60, 60), 1));
            painter.drawLine(x, plotArea.top(), x, plotArea.bottom());
            painter.setPen(Qt::white);
            QString label = QString::number((int)std::lround(db));
            int lx = clamp(x - fm.boundingRect(label).width() / 2, 0,
                           width() - fm.boundingRect(label).width());
            painter.drawText(lx, dbLabelY, label);
        }
        QString unit = "dB";
        painter.setPen(Qt::white);
        painter.drawText(plotArea.right() - fm.boundingRect(unit).width(), dbLabelY, unit);
    }

    // --- PSD trace(s) ---
    auto drawTrace = [&](const std::vector<float> &line, qreal opacity) {
        QPainterPath path;
        bool started = false;
        for (int bin = binLo; bin <= binHi; bin++) {
            float power = line[bin];
            if (!std::isfinite(power))
                power = powerMin;
            float x = powerToX(power);
            float frac = (float)(bin - binLo) / (float)(binHi - binLo);
            float y = fracToY(frac);
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }
        painter.setOpacity(opacity);
        painter.drawPath(path);
    };

    // Build the list of (column sample, opacity) traces to draw. The centre column
    // is drawn last at full opacity; with persistence on, neighbouring columns on
    // each side are overlaid starting at firstNeighbourOpacity and rolling off
    // gently outward, so the surrounding spectrum forms a faint persistence fan.
    // Outermost (faintest) is listed first so nearer columns, then the centre, end
    // up on top.
    std::vector<std::pair<size_t, qreal>> traces;
    const int stride = spectrogram->getColumnStride();
    const size_t count = spectrogram->input() ? spectrogram->input()->count() : 0;
    if (persistenceEnabled) {
        const int neighbourCount = 20;
        const qreal firstNeighbourOpacity = 0.4;  // nearest neighbour vs the centre
        const qreal falloff = 0.85;               // gentle roll-off across the span
        for (int k = neighbourCount; k >= 1 && stride > 0; k--) {
            qreal opacity = firstNeighbourOpacity * std::pow(falloff, k - 1);
            if (opacity * 255.0 < 1.0)   // rounds to fully transparent; skip the FFT
                continue;
            size_t off = (size_t)k * stride;
            if (off <= currentSample)
                traces.emplace_back(currentSample - off, opacity);
            if (count == 0 || currentSample + off < count)
                traces.emplace_back(currentSample + off, opacity);
        }
    }
    traces.emplace_back(currentSample, 1.0);

    // Memoise the FFT lines: pure repaints (scroll, resize, power-range changes) at
    // the same column reuse them. Only a column / FFT-size / stride / persistence
    // change -- or a file reload (invalidateCache()) -- recomputes them.
    if (!cacheValid || cacheSample != currentSample || cacheFFTSize != n
            || cacheStride != stride || cachePersistence != persistenceEnabled
            || cachedLines.size() != traces.size()) {
        cachedLines.clear();
        cachedLines.reserve(traces.size());
        for (auto &t : traces)
            cachedLines.push_back(spectrogram->getSpectrumLine(t.first));
        cacheValid = true;
        cacheSample = currentSample;
        cacheFFTSize = n;
        cacheStride = stride;
        cachePersistence = persistenceEnabled;
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 220, 0), 0.5));  // thin line; neighbours overlay faintly
    for (size_t i = 0; i < traces.size(); i++)
        drawTrace(cachedLines[i], traces[i].second);
    painter.setOpacity(1.0);

    // Frame the plot area (only when scales are shown).
    if (scalesEnabled) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(120, 120, 120), 1));
        painter.drawRect(plotArea);
    }
}

void SpectrumView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;

    auto save = new QAction("Save spectrum to file...", &menu);
    connect(save, &QAction::triggered, this, [this]() { saveToFile(); });
    save->setEnabled(hasSample);
    menu.addAction(save);

    // Toggle the neighbouring-column overlay (persistence fan).
    auto persistence = new QAction("Persistence", &menu);
    persistence->setCheckable(true);
    persistence->setChecked(persistenceEnabled);
    connect(persistence, &QAction::toggled, this, [this](bool on) {
        persistenceEnabled = on;
        update();
    });
    menu.addAction(persistence);

    // Detach into a floating window (with a normal title bar) or re-dock. While
    // docked the title bar is hidden so the spectrum lines up with the spectrogram.
    if (auto d = dock()) {
        bool floating = d->isFloating();
        auto detach = new QAction(floating ? "Dock" : "Detach to window", &menu);
        connect(detach, &QAction::triggered, this, [d, floating]() {
            d->setFloating(!floating);
        });
        menu.addAction(detach);
    }

    menu.addSeparator();

    auto remove = new QAction("Remove spectrum plot", &menu);
    connect(remove, &QAction::triggered, this, [this]() {
        // The widget lives inside a QDockWidget (set up by MainWindow); closing
        // it removes and deletes the dock (WA_DeleteOnClose), which deletes us.
        if (auto d = dock())
            d->close();
        else
            deleteLater();
    });
    menu.addAction(remove);

    menu.exec(event->globalPos());
}

void SpectrumView::saveToFile()
{
    if (spectrogram == nullptr || !hasSample)
        return;

    QString fileName = QFileDialog::getSaveFileName(
        this, "Save spectrum", QString(), "CSV files (*.csv);;All files (*)");
    if (fileName.isEmpty())
        return;
    if (QFileInfo(fileName).suffix().isEmpty())
        fileName += ".csv";

    const int n = spectrogram->getFFTSize();
    const double sampleRate = spectrogram->getSampleRate();
    const bool realSignal = spectrogram->isRealSignal();
    std::vector<float> line = spectrogram->getSpectrumLine(currentSample);

    const int binLo = realSignal ? n / 2 : 0;

    std::ofstream os(fileName.toStdString());
    if (!os) {
        QMessageBox::warning(this, "Save spectrum",
                             "Could not open the file for writing.");
        return;
    }

    os << "# frequency_offset_hz,power_db\n";
    for (int bin = binLo; bin < n; bin++) {
        double freqOffset = ((double)bin - n / 2.0) / n * sampleRate;
        os << freqOffset << "," << line[bin] << "\n";
    }
    os.close();
}
