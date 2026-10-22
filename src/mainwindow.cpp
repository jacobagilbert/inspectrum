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

#include <QMessageBox>
#include <QtWidgets>
#include <QPixmapCache>
#include <QRubberBand>
#include <sstream>

#include "mainwindow.h"
#include "util.h"

MainWindow::MainWindow()
{
    setWindowTitle(tr("inspectrum - jacobagilbert edition"));

    QPixmapCache::setCacheLimit(40960);

    dock = new SpectrogramControls(tr("Controls"), this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    input = new InputSource();
    input->subscribe(this);

    plots = new PlotView(input);
    setCentralWidget(plots);

    // Connect dock inputs
    connect(dock, &SpectrogramControls::openFile, this, &MainWindow::openFile);
    connect(dock->sampleRate, static_cast<void (QLineEdit::*)(const QString&)>(&QLineEdit::textChanged), this, static_cast<void (MainWindow::*)(QString)>(&MainWindow::setSampleRate));
    connect(dock, static_cast<void (SpectrogramControls::*)(int, int)>(&SpectrogramControls::fftOrZoomChanged), plots, &PlotView::setFFTAndZoom);
    connect(dock->powerMaxSlider, &QSlider::valueChanged, plots, &PlotView::setPowerMax);
    connect(dock->powerMinSlider, &QSlider::valueChanged, plots, &PlotView::setPowerMin);
    connect(dock->cursorsCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableCursors);
    connect(dock->scalesCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableScales);
    connect(dock->crosshairCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableCrosshairs);
    connect(dock->cursorSymbolsSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), plots, &PlotView::setCursorSegments);

    // Connect dock outputs
    connect(plots, &PlotView::timeSelectionChanged, dock, &SpectrogramControls::timeSelectionChanged);
    connect(plots, &PlotView::pointerMoved, dock, &SpectrogramControls::pointerMoved);
    connect(plots, &PlotView::pointerLeft, dock, &SpectrogramControls::pointerLeft);
    connect(plots, &PlotView::zoomIn, dock, &SpectrogramControls::zoomIn);
    connect(plots, &PlotView::zoomOut, dock, &SpectrogramControls::zoomOut);
    connect(plots, &PlotView::spectrumPlotAdded, this, &MainWindow::addSpectrumPlot);

    // Keyboard shortcuts (Qt maps Ctrl to Command on macOS)
    auto openShortcut = new QShortcut(QKeySequence::Open, this);
    connect(openShortcut, &QShortcut::activated, dock->fileOpenButton, &QPushButton::click);
    auto reloadShortcut = new QShortcut(QKeySequence(tr("Ctrl+R")), this);
    connect(reloadShortcut, &QShortcut::activated, this, &MainWindow::reloadFile);

    // Set defaults after making connections so everything is in sync
    dock->setDefaults();

}

void MainWindow::openFile(QString fileName)
{
    QString title="%1 jacobagilbert edition: %2";
    this->setWindowTitle(title.arg(QApplication::applicationName(),fileName.section('/',-1,-1)));

    // Try to parse osmocom_fft filenames and extract the sample rate and center frequency.
    // Example file name: "name-f2.411200e+09-s5.000000e+06-t20160807180210.cfile"
    QRegularExpression rx(QRegularExpression::anchoredPattern("(.*)-f(.*)-s(.*)-.*\\.cfile"));
    QString basename = fileName.section('/',-1,-1);

    auto match = rx.match(basename);
    if (match.hasMatch()) {
        QString centerfreq = match.captured(2);
        QString samplerate = match.captured(3);

        std::stringstream ss(samplerate.toUtf8().constData());

        // Needs to be a double as the number is in scientific format
        double rate;
        ss >> rate;
        if (!ss.fail()) {
            setSampleRate(rate);
        }
    }

    try
    {
        input->openFile(fileName.toUtf8().constData());
        currentFile = fileName;
    }
    catch (const std::exception &ex)
    {
        QMessageBox msgBox(QMessageBox::Critical, "Inspectrum openFile error", QString("%1: %2").arg(fileName).arg(ex.what()));
        msgBox.exec();
    }
}

void MainWindow::reloadFile()
{
    if (currentFile.isEmpty())
        return;

    // Re-read the open file (e.g. to pick up samples appended to a capture that
    // is still being written), with any format override it was opened with.
    input->forceFormat(input->formatOverride());
    openFile(currentFile);
}

void MainWindow::invalidateEvent()
{
    plots->setSampleRate(input->rate());

    // Only update the text box if it is not already representing
    // the current value. Otherwise the cursor might jump or the
    // representation might change (e.g. to scientific).
    double currentValue = dock->sampleRate->text().toDouble();
    if(QString::number(input->rate()) != QString::number(currentValue)) {
        setSampleRate(input->rate());
    }

    // Show the center frequency parsed from SigMF metadata (read-only display).
    setCenterFrequency(input->getFrequency());
}

void MainWindow::setSampleRate(QString rate)
{
    auto sampleRate = rate.toDouble();
    input->setSampleRate(sampleRate);
    plots->setSampleRate(sampleRate);

    // Save the sample rate in settings as we're likely to be opening the same file across multiple runs
    QSettings settings;
    settings.setValue("SampleRate", sampleRate);
}

void MainWindow::setSampleRate(double rate)
{
    dock->sampleRate->setText(QString::number(rate));
}

void MainWindow::setCenterFrequency(double freq)
{
    // Format with SI prefixes (kHz/MHz/GHz...); show a plain "0 Hz" when unset.
    QString text = (freq == 0.0)
        ? QStringLiteral("0 Hz")
        : QString::fromStdString(formatFixedSI(freq, freq, "Hz"));
    dock->centerFrequency->setText(text);
}

void MainWindow::setFormat(QString fmt)
{
    input->setFormat(fmt.toUtf8().constData());
}

void MainWindow::forceFormat(QString fmt)
{
    input->forceFormat(fmt.toUtf8().constData());
}

void MainWindow::addSpectrumPlot(SpectrumView *plot)
{
    // Wrap the spectrum view in its own dock so it sits to the right of the
    // spectrogram but can be detached/floated like the Controls dock. The dock
    // deletes itself on close, which the spectrum plot's "Remove" action triggers.
    auto spectrumDock = new QDockWidget(tr("Spectrum"), this);
    spectrumDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    spectrumDock->setAttribute(Qt::WA_DeleteOnClose);
    spectrumDock->setWidget(plot);

    // While docked, hide the title bar so the spectrum's top edge lines up with
    // the spectrogram. When floated, restore the normal title bar so it behaves
    // like an ordinary window (move/close). Detach/dock is driven from the
    // spectrum plot's right-click menu.
    // The placeholder needs a (zero-margin) layout so its sizeHint is a valid
    // (0,0); a bare QWidget reports (-1,-1), which makes QDockWidget compute a
    // negative minimum height ("Negative sizes (160,-1)" warning).
    auto emptyTitle = new QWidget(spectrumDock);
    auto emptyTitleLayout = new QHBoxLayout(emptyTitle);
    emptyTitleLayout->setContentsMargins(0, 0, 0, 0);
    spectrumDock->setTitleBarWidget(emptyTitle);
    connect(spectrumDock, &QDockWidget::topLevelChanged, spectrumDock,
            [spectrumDock, emptyTitle](bool floating) {
        spectrumDock->setTitleBarWidget(floating ? nullptr : emptyTitle);
    });

    addDockWidget(Qt::RightDockWidgetArea, spectrumDock);
}
