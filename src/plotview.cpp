/*
 *  Copyright (C) 2015-2016, Mike Walters <mike@flomp.net>
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

#include "plotview.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <limits>
#include <QtGlobal>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSpinBox>
#include <QToolTip>
#include <QVBoxLayout>
#include "plots.h"
#include "util.h"

// Convert a normalised float sample (~[-1, 1]) to a signed 16-bit value.
static inline int16_t floatToS16(float v)
{
    return (int16_t)clamp((int32_t)lrintf(v * 32768.0f), -32768, 32767);
}

// Write a single output sample. Overloaded so the export template compiles for
// both complex<float> and float sources; sc16 only applies to complex sources.
static inline void writeSample(std::ofstream &os, const std::complex<float> &v, bool sc16)
{
    if (sc16) {
        int16_t iq[2] = { floatToS16(v.real()), floatToS16(v.imag()) };
        os.write(reinterpret_cast<const char*>(iq), sizeof(iq));
    } else {
        os.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
}

static inline void writeSample(std::ofstream &os, const float &v, bool /*sc16*/)
{
    os.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

// Apply a frequency-shift mix (e^{j*phase}) to a sample. Overloaded so the
// export template compiles for float sources, where shifting doesn't apply.
static inline std::complex<float> mixSample(const std::complex<float> &v, double phase)
{
    return v * std::complex<float>((float)std::cos(phase), (float)std::sin(phase));
}

static inline float mixSample(const float &v, double /*phase*/)
{
    return v;
}

// Round to a given number of significant figures.
static double roundToSigFigs(double v, int figs)
{
    if (v == 0.0)
        return 0.0;
    double mag = std::pow(10.0, figs - 1 - (int)std::floor(std::log10(std::fabs(v))));
    return std::round(v * mag) / mag;
}

PlotView::PlotView(InputSource *input) : cursors(this), viewRange({0, 0})
{
    mainSampleSource = input;
    setDragMode(QGraphicsView::ScrollHandDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setMouseTracking(true);
    enableCursors(false);
    connect(&cursors, &Cursors::cursorsMoved, this, &PlotView::cursorsMoved);

    // The spectrogram and the derived plots scroll vertically in their own
    // panes, each with its own scroll bar (laid out by layoutScrollBars()),
    // in place of the view's. They're children of the viewport, which also
    // gives them the overlay style on macOS (fading out after a scroll).
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    spectrogramScrollBar = new QScrollBar(Qt::Vertical, viewport());
    derivedScrollBar = new QScrollBar(Qt::Vertical, viewport());
    for (auto bar : {spectrogramScrollBar, derivedScrollBar}) {
        bar->setSingleStep(20);
        bar->hide();
        // Shown while there's somewhere to scroll to; one that has faded out
        // reappears when scrolled, as the view's own would.
        auto show = [bar] { bar->setVisible(bar->maximum() > 0); };
        connect(bar, &QScrollBar::rangeChanged, this, show);
        connect(bar, &QScrollBar::valueChanged, this, [this, show] { show(); updateView(); });
    }

    spectrogramPlot = new SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>>(mainSampleSource));
    auto tunerOutput = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(spectrogramPlot->output());

    enableScales(true);

    enableAnnotations(true);
    enableAnnoLabels(true);
    enableAnnoColors(false);
    enableAnnotationCommentsTooltips(false);

    addPlot(spectrogramPlot);

    mainSampleSource->subscribe(this);
}

void PlotView::addPlot(Plot *plot)
{
    plots.emplace_back(plot);
    connect(plot, &Plot::repaint, this, &PlotView::repaint);
}

void PlotView::addSpectrumPlot()
{
    if (spectrogramPlot == nullptr)
        return;

    auto plot = new SpectrumView(spectrogramPlot, this);
    plot->enableScales(timeScaleEnabled);
    spectrumPlots.push_back(plot);

    // Drop our reference when the widget (and its dock) is destroyed via the
    // spectrum plot's own "Remove" action.
    connect(plot, &QObject::destroyed, this, [this](QObject *obj) {
        auto it = std::find(spectrumPlots.begin(), spectrumPlots.end(), obj);
        if (it != spectrumPlots.end())
            spectrumPlots.erase(it);
    });

    // MainWindow wraps this in a detachable dock to the right of the spectrogram.
    emit spectrumPlotAdded(plot);

    // Seed the column under the pointer (alignment is resolved at paint time).
    updateSpectrumPlots();
}

void PlotView::updateSpectrumPlots()
{
    if (spectrumPlots.empty())
        return;

    size_t sample = columnToSample(horizontalScrollBar()->value() + pointerPos.x());
    for (auto plot : spectrumPlots) {
        plot->setSample(sample);
        // The crosshair's frequency line continues across the spectrum plots,
        // so they follow the pointer vertically too.
        if (crosshairsEnabled)
            plot->update();
    }
}

bool PlotView::spectrogramScreenBand(int &topGlobal, int &heightOut, int &paneBottomGlobal)
{
    if (spectrogramPlot == nullptr || spectrogramPlot->height() <= 0)
        return false;

    // The spectrogram is the first plot, drawn from this y in the viewport
    // (matches paintEvent / emitPointerPosition). Reported in global screen
    // coordinates so a SpectrumView can map it into its own (docked or floating)
    // widget regardless of its title bar.
    topGlobal = viewport()->mapToGlobal(QPoint(0, plotRect(0).top())).y();
    heightOut = spectrogramPlot->height();
    paneBottomGlobal = viewport()->mapToGlobal(QPoint(0, derivedPaneTop())).y();
    return true;
}

bool PlotView::crosshairScreenY(int &yGlobal)
{
    if (!crosshairsEnabled || !crosshairValid || spectrogramPlot == nullptr)
        return false;
    // Only while over the spectrogram, whose y-axis is frequency (as the
    // pointer readout).
    if (!paneRect(0).contains(pointerPos) || !plotRect(0).contains(pointerPos))
        return false;
    yGlobal = viewport()->mapToGlobal(pointerPos).y();
    return true;
}

void PlotView::mousePressEvent(QMouseEvent *event)
{
    // A hand-drag scrolls the pane it starts over vertically (the base class
    // only pans time, as the view's own vertical scroll bar is unused).
    dragScrollBar = nullptr;
    if (event->button() == Qt::LeftButton && dragMode() == QGraphicsView::ScrollHandDrag) {
        dragScrollBar = paneScrollBar(event->pos().y());
        dragY = event->pos().y();
        dragValue = dragScrollBar->value();
    }
    QGraphicsView::mousePressEvent(event);
}

void PlotView::mouseMoveEvent(QMouseEvent *event)
{
    updateAnnotationTooltip(event);
    // Report the pointer's time/frequency whether or not the crosshair overlay
    // is enabled; only the overlay itself is gated on crosshairsEnabled.
    pointerPos = event->pos();
    emitPointerPosition();
    updateSpectrumPlots();
    if (crosshairsEnabled) {
        crosshairValid = true;
        viewport()->update();
    }
    if (dragScrollBar != nullptr && (event->buttons() & Qt::LeftButton))
        dragScrollBar->setValue(dragValue - (event->pos().y() - dragY));
    QGraphicsView::mouseMoveEvent(event);
}

void PlotView::mouseReleaseEvent(QMouseEvent *event)
{
    dragScrollBar = nullptr;
    // This is used to show the tooltip again on drag release if the mouse is
    // hovering over an annotation.
    updateAnnotationTooltip(event);
    QGraphicsView::mouseReleaseEvent(event);
}

void PlotView::updateAnnotationTooltip(QMouseEvent *event)
{
    // If there are any mouse buttons pressed, we assume
    // that the plot is being dragged and hide the tooltip.
    bool isDrag = event->buttons() != Qt::NoButton;
    if (!annotationCommentsEnabled
        || !spectrogramPlot->isAnnotationsEnabled()
        || isDrag)  {
        QToolTip::hideText();
    } else {
        QString* comment = spectrogramPlot->mouseAnnotationComment(event);
        if (comment != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QToolTip::showText(event->globalPosition().toPoint(), *comment);
#else
            QToolTip::showText(event->globalPos(), *comment);
#endif
        } else {
            QToolTip::hideText();
        }
    }
}

void PlotView::contextMenuEvent(QContextMenuEvent * event)
{
    QMenu menu;

    // Get selected plot
    Plot *selectedPlot = nullptr;
    auto it = plots.begin();
    for (size_t i = 0; it != plots.end(); it++, i++) {
        if (paneRect(i).contains(event->pos()) && plotRect(i).contains(event->pos())) {
            selectedPlot = it->get();
            break;
        }
    }
    if (selectedPlot == nullptr)
        return;

    auto src = selectedPlot->output();

    // Add a standalone spectrum (PSD) window for the spectrogram. It tracks the
    // pointer and lives in its own detachable dock to the right.
    if (selectedPlot == spectrogramPlot) {
        auto spectrumAction = new QAction("Add spectrum plot", &menu);
        connect(
            spectrumAction, &QAction::triggered,
            this, [=]() {
                addSpectrumPlot();
            }
        );
        menu.addAction(spectrumAction);
    }

    // Add action to export the selected samples into a file
    auto save = new QAction("Export samples to file...", &menu);
    connect(
        save, &QAction::triggered,
        this, [=]() {
            if (selectedPlot == spectrogramPlot) {
                exportSamples(spectrogramPlot->tunerEnabled() ? spectrogramPlot->output() : spectrogramPlot->input());
            } else {
                exportSamples(src);
            }
        }
    );
    menu.addAction(save);

    // SigMF annotation display options, for SigMF recordings only: whether
    // annotations are shown at all, and how each box is drawn (only available
    // while they are shown).
    if (spectrogramPlot != nullptr && mainSampleSource->isSigMF()) {
        QMenu *sigmfMenu = menu.addMenu("SigMF");
        bool annotationsEnabled = spectrogramPlot->isAnnotationsEnabled();

        auto addToggle = [&](const QString &name, bool checked, void (PlotView::*setter)(bool)) {
            auto action = new QAction(name, sigmfMenu);
            action->setCheckable(true);
            action->setChecked(checked);
            connect(action, &QAction::triggered, this, [this, setter](bool on) { (this->*setter)(on); });
            sigmfMenu->addAction(action);
            return action;
        };
        addToggle("Annotations", annotationsEnabled, &PlotView::enableAnnotations);
        sigmfMenu->addSeparator();
        addToggle("Labels",   annotationLabelsEnabled,   &PlotView::enableAnnoLabels)->setEnabled(annotationsEnabled);
        addToggle("Comments", annotationCommentsEnabled, &PlotView::enableAnnotationCommentsTooltips)->setEnabled(annotationsEnabled);
        addToggle("Colors",   annotationColorsEnabled,   &PlotView::enableAnnoColors)->setEnabled(annotationsEnabled);
    }

    // Add actions to add derived plots
    // that are compatible with selectedPlot's output
    QMenu *plotsMenu = menu.addMenu("Add derived plot");
    auto compatiblePlots = as_range(Plots::plots.equal_range(src->sampleType()));
    for (auto p : compatiblePlots) {
        auto plotInfo = p.second;
        auto action = new QAction(QString("Add %1").arg(plotInfo.name), plotsMenu);
        auto plotCreator = plotInfo.creator;
        connect(
            action, &QAction::triggered,
            this, [=]() {
                addPlot(plotCreator(src));
                // Scroll the new plot (added at the bottom) into view.
                updateView();
                derivedScrollBar->setValue(derivedScrollBar->maximum());
            }
        );
        plotsMenu->addAction(action);
    }

    // Add submenu for extracting symbols
    QMenu *extractMenu = menu.addMenu("Extract symbols");
    // Add action to extract symbols from selected plot to stdout
    auto extract = new QAction("To stdout", extractMenu);
    connect(
        extract, &QAction::triggered,
        this, [=]() {
            extractSymbols(src, false);
        }
    );
    extract->setEnabled(cursorsEnabled && (src->sampleType() == typeid(float)));
    extractMenu->addAction(extract);

    // Add action to extract symbols from selected plot to clipboard
    auto extractClipboard = new QAction("Copy to clipboard", extractMenu);
    connect(
        extractClipboard, &QAction::triggered,
        this, [=]() {
            extractSymbols(src, true);
        }
    );
    extractClipboard->setEnabled(cursorsEnabled && (src->sampleType() == typeid(float)));
    extractMenu->addAction(extractClipboard);

    // Add action to remove the selected plot
    auto rem = new QAction("Remove plot", &menu);
    connect(
        rem, &QAction::triggered,
        this, [=]() {
            plots.erase(it);
        }
    );
    // Don't allow remove the first plot (the spectrogram)
    rem->setEnabled(it != plots.begin());
    menu.addAction(rem);

    updateViewRange(false);
    if(menu.exec(event->globalPos()))
        updateView(false);
}

void PlotView::cursorsMoved()
{
    selectedSamples = {
        columnToSample(horizontalScrollBar()->value() + cursors.selection().minimum),
        columnToSample(horizontalScrollBar()->value() + cursors.selection().maximum)
    };

    emitTimeSelection();
    viewport()->update();
}

void PlotView::emitTimeSelection()
{
    size_t sampleCount = selectedSamples.length();
    float selectionTime = sampleCount / (float)mainSampleSource->rate();
    emit timeSelectionChanged(selectionTime);
}

void PlotView::emitPointerPosition()
{
    if (sampleRate <= 0) {
        emit pointerMoved(QString(), QString());
        return;
    }

    // Time of the sample under the pointer (the x-axis is shared by every plot).
    // The unit is fixed from the recording's total duration so it doesn't change
    // as the pointer moves or the view zooms.
    size_t sample = columnToSample(horizontalScrollBar()->value() + pointerPos.x());
    double time = (double)sample / sampleRate;
    double duration = mainSampleSource->count() / sampleRate;
    QString timeStr = QString::fromStdString(formatFixedSI(time, duration, "s"));

    // Frequency is only meaningful while the pointer is over the spectrogram,
    // whose y-axis is frequency; derived plots use a different y-axis. The unit
    // is fixed from the sample rate (the full span shown).
    QString freqStr;
    // The spectrogram is the first plot, visible only within its own pane.
    if (paneRect(0).contains(pointerPos) && plotRect(0).contains(pointerPos)) {
        double frequency = spectrogramPlot->frequencyAt(pointerPos.y() - plotRect(0).top());
        freqStr = QString::fromStdString(formatFixedSI(frequency, sampleRate, "Hz"));
    }

    emit pointerMoved(timeStr, freqStr);
}

void PlotView::enableCursors(bool enabled)
{
    cursorsEnabled = enabled;
    if (enabled) {
        int margin = viewport()->rect().width() / 3;
        cursors.setSelection({viewport()->rect().left() + margin, viewport()->rect().right() - margin});
        cursorsMoved();
    }
    viewport()->update();
}

bool PlotView::viewportEvent(QEvent *event) {
    // Handle wheel events for zooming (before the parent's handler to stop normal scrolling)
    if (event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = (QWheelEvent*)event;
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            bool canZoomIn = zoomLevel < fftSize;
            bool canZoomOut = zoomLevel > 1;
            int delta = wheelEvent->angleDelta().y();
            if ((delta > 0 && canZoomIn) || (delta < 0 && canZoomOut)) {
                scrollZoomStepsAccumulated += delta;

                // `updateViewRange()` keeps the center sample in the same place after zoom. Apply
                // a scroll adjustment to keep the sample under the mouse cursor in the same place instead.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                zoomPos = wheelEvent->position().x();
#else
                zoomPos = wheelEvent->pos().x();
#endif
                zoomSample = columnToSample(horizontalScrollBar()->value() + zoomPos);
                if (scrollZoomStepsAccumulated >= 120) {
                    scrollZoomStepsAccumulated -= 120;
                    emit zoomIn();
                } else if (scrollZoomStepsAccumulated <= -120) {
                    scrollZoomStepsAccumulated += 120;
                    emit zoomOut();
                }
            }
            return true;
        }
    }

    // When the pointer leaves the plot, hide the crosshair overlay and flag the
    // readout as stale (it keeps its last value, shown de-emphasized).
    if (event->type() == QEvent::Leave) {
        if (crosshairValid) {
            crosshairValid = false;
            viewport()->update();
            updateSpectrumPlots();
        }
        emit pointerLeft();
    }

    // Pass mouse events to individual plot objects
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {

        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPoint pos = mouseEvent->position().toPoint();
#else
        QPoint pos = mouseEvent->pos();
#endif

        // Hovers and presses only reach a plot while the pointer is over its
        // pane, so e.g. the tuner can't be grabbed through the derived plots
        // covering the rest of the spectrogram. Other moves and releases always
        // reach it, so a drag carries on outside the pane.
        bool hoverOrPress = event->type() == QEvent::MouseButtonPress ||
            (event->type() == QEvent::MouseMove && mouseEvent->buttons() == Qt::NoButton);

        for (size_t i = 0; i < plots.size(); i++) {
            auto&& plot = plots[i];
            if (hoverOrPress && !paneRect(i).contains(pos)) {
                plot->leaveEvent();
                continue;
            }
            int plotY = plotRect(i).top();
            auto mouse_event = QMouseEvent(
                event->type(),
                QPoint(pos.x(), pos.y() - plotY),
                mouseEvent->button(),
                mouseEvent->buttons(),
                QApplication::keyboardModifiers()
            );
            bool result = plot->mouseEvent(
                event->type(),
                &mouse_event
            );
            if (result)
                return true;
        }

        if (cursorsEnabled)
            if (cursors.mouseEvent(event->type(), mouseEvent))
                return true;
    }

    if (event->type() == QEvent::Leave) {
        for (auto&& plot : plots) {
            plot->leaveEvent();
        }

        if (cursorsEnabled)
            cursors.leaveEvent();
    }

    // Handle parent eveents
    return QGraphicsView::viewportEvent(event);
}

void PlotView::extractSymbols(std::shared_ptr<AbstractSampleSource> src,
                              bool toClipboard)
{
    if (!cursorsEnabled)
        return;
    auto floatSrc = std::dynamic_pointer_cast<SampleSource<float>>(src);
    if (!floatSrc)
        return;
    auto samples = floatSrc->getSamples(selectedSamples.minimum, selectedSamples.length());
    auto step = (float)selectedSamples.length() / cursors.segments();
    auto symbols = std::vector<float>();
    for (auto i = step / 2; i < selectedSamples.length(); i += step)
    {
        symbols.push_back(samples[i]);
    }
    if (!toClipboard) {
        for (auto f : symbols)
            std::cout << f << ", ";
        std::cout << std::endl << std::flush;
    } else {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QString symbolText;
        QTextStream symbolStream(&symbolText);
        for (auto f : symbols)
            symbolStream << f << ", ";
        clipboard->setText(symbolText);
    }
}

void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    if (src->sampleType() == typeid(std::complex<float>)) {
        exportSamples<std::complex<float>>(src);
    } else {
        exportSamples<float>(src);
    }
}

template<typename SOURCETYPE>
void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    auto sampleSrc = std::dynamic_pointer_cast<SampleSource<SOURCETYPE>>(src);
    if (!sampleSrc) {
        return;
    }

    const bool isComplex = std::is_same<SOURCETYPE, std::complex<float>>::value;

    // The frequency offset field is absolute (relative to the recording centre /
    // spectrogram DC). When exporting the tuner output, the tuner has already
    // mixed its centre to DC, so we track that intrinsic offset and subtract it
    // from the requested offset to avoid double-counting the shift.
    double recordingCenter = sampleSrc->getFrequency();
    double tunerIntrinsicOffset = 0;
    if (spectrogramPlot != nullptr && src == spectrogramPlot->output()) {
        tunerIntrinsicOffset = spectrogramPlot->getTunerOffsetFrequency();
        recordingCenter = spectrogramPlot->getCenterFrequency() - tunerIntrinsicOffset;
    }

    QFileDialog dialog(this);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    // Format is chosen by the "Output Format" controls below; these filters only
    // affect which existing files are listed. "All files" stays the default so no
    // extension is forced onto the typed name.
    dialog.setNameFilters({
        "All files (*)",
        "SigMF files (*.sigmf-meta *.sigmf-data)",
        "All supported (*.fc32 *.sc16 *.sigmf-meta *.sigmf-data)"
    });
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);

    QGridLayout *l = dialog.findChild<QGridLayout*>();

    // --- Selection to export ---
    QGroupBox selectionBox("Selection to Export", &dialog);
    QRadioButton cursorSelection("Cursor selection", &selectionBox);
    QRadioButton currentView("Current view", &selectionBox);
    QRadioButton completeFile("Complete file (experimental)", &selectionBox);
    if (cursorsEnabled) {
        cursorSelection.setChecked(true);
    } else {
        currentView.setChecked(true);
        cursorSelection.setEnabled(false);
    }
    QVBoxLayout selectionLayout(&selectionBox);
    selectionLayout.addWidget(&cursorSelection);
    selectionLayout.addWidget(&currentView);
    selectionLayout.addWidget(&completeFile);
    selectionLayout.addStretch(1);

    // --- Output format (sc16 only offered for complex sources) ---
    QGroupBox formatBox("Output Format", &dialog);
    QRadioButton fmtNative(isComplex ? "Complex float32 (fc32)" : "Float32 (f32)", &formatBox);
    QRadioButton fmtSc16("Complex int16 (sc16)", &formatBox);
    fmtNative.setChecked(true);
    QCheckBox sigmfCheck("Use SigMF format", &formatBox);
    QVBoxLayout formatLayout(&formatBox);
    formatLayout.addWidget(&fmtNative);
    if (isComplex)
        formatLayout.addWidget(&fmtSc16);
    formatLayout.addWidget(&sigmfCheck);
    formatLayout.addStretch(1);

    // --- Resampling: decimation + frequency offset on one row ---
    // The frequency offset shifts the exported band up/down before decimation.
    // Disabled for real captures.
    bool offsetEnabled = isComplex && !mainSampleSource->realSignal();

    QGroupBox resampleBox("Resampling", &dialog);
    QLabel decimationLabel("Decimation:", &resampleBox);
    QSpinBox decimation(&resampleBox);
    decimation.setRange(1, std::numeric_limits<int>::max());
    decimation.setValue(1);
    decimation.setMaximumWidth(70);  // only needs 2-3 digits

    QLabel offsetLabel("Frequency offset (Hz):", &resampleBox);
    QSpinBox frequencyOffset(&resampleBox);
    int maxOffset = std::numeric_limits<int>::max();
    if (sampleSrc->rate() > 0)
        maxOffset = (int)std::min(sampleSrc->rate() / 2.0, (double)std::numeric_limits<int>::max());
    frequencyOffset.setRange(-maxOffset, maxOffset);
    frequencyOffset.setValue(0);
    frequencyOffset.setMinimumWidth(160);  // holds large Hz values comfortably
    frequencyOffset.setEnabled(offsetEnabled);
    offsetLabel.setEnabled(offsetEnabled);
    frequencyOffset.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // "Suggest" fills decimation + offset from the on-screen frequency filter
    // (tuner): decimation from its width, offset from its centre (4 sig figs).
    // Only available when a tuner is active on a complex source.
    bool suggestEnabled = offsetEnabled && spectrogramPlot != nullptr
                          && spectrogramPlot->tunerEnabled();
    QPushButton suggestButton("Suggest", &resampleBox);
    suggestButton.setEnabled(suggestEnabled);
    if (suggestEnabled) {
        connect(&suggestButton, &QPushButton::clicked, this, [&]() {
            double relBW = spectrogramPlot->getTunerRelativeBandwidth();
            if (relBW > 0)
                decimation.setValue(std::max(1, (int)std::ceil(1.0 / relBW)));
            frequencyOffset.setValue(
                (int)roundToSigFigs(spectrogramPlot->getTunerOffsetFrequency(), 4));
        });
    }

    QHBoxLayout resampleLayout(&resampleBox);
    resampleLayout.addWidget(&decimationLabel);
    resampleLayout.addWidget(&decimation);
    resampleLayout.addSpacing(20);
    resampleLayout.addWidget(&offsetLabel);
    resampleLayout.addWidget(&frequencyOffset, 1);
    resampleLayout.addWidget(&suggestButton);

    // Pack the option groups into their own grid spanning the full dialog width,
    // so they fill the space evenly instead of leaving the bottom-right empty.
    QGridLayout *optionsGrid = new QGridLayout();
    optionsGrid->addWidget(&selectionBox, 0, 0);
    optionsGrid->addWidget(&formatBox, 0, 1);
    optionsGrid->addWidget(&resampleBox, 1, 0, 1, 2);
    optionsGrid->setColumnStretch(0, 1);
    optionsGrid->setColumnStretch(1, 1);
    l->addLayout(optionsGrid, 4, 0, 1, l->columnCount());

    // Live-preview the bandwidth that decimation/offset will keep on the
    // spectrogram, but only when exporting the spectrogram's own samples.
    bool showDecimationPreview = (spectrogramPlot != nullptr) &&
        (src == spectrogramPlot->output() || src == spectrogramPlot->input());
    if (showDecimationPreview) {
        auto updatePreview = [this, &decimation, &frequencyOffset]() {
            spectrogramPlot->setDecimationPreview(decimation.value(), frequencyOffset.value());
        };
        updatePreview();
        connect(&decimation, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [updatePreview](int) { updatePreview(); });
        connect(&frequencyOffset, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [updatePreview](int) { updatePreview(); });
    }

    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();

        const bool sc16 = isComplex && fmtSc16.isChecked();
        QString datatype, defaultExt;
        if (!isComplex) {
            datatype = "rf32_le"; defaultExt = ".f32";
        } else if (sc16) {
            datatype = "ci16_le"; defaultExt = ".sc16";
        } else {
            datatype = "cf32_le"; defaultExt = ".fc32";
        }

        // When writing SigMF, the dataset uses the .sigmf-data extension so it
        // pairs with the .sigmf-meta sidecar; the radio still selects the binary
        // encoding (recorded as core:datatype). Otherwise use the format's
        // extension, appended only if the user didn't supply one.
        QString outPath;
        if (sigmfCheck.isChecked()) {
            QFileInfo fi(fileNames[0]);
            outPath = fi.path() + "/" + fi.completeBaseName() + ".sigmf-data";
        } else {
            outPath = fileNames[0];
            if (QFileInfo(outPath).suffix().isEmpty())
                outPath += defaultExt;
        }

        size_t start, end;
        if (cursorSelection.isChecked()) {
            start = selectedSamples.minimum;
            end = start + selectedSamples.length();
        } else if(currentView.isChecked()) {
            start = viewRange.minimum;
            end = start + viewRange.length();
        } else {
            start = 0;
            end = sampleSrc->count();
        }

        std::ofstream os (outPath.toStdString(), std::ios::binary);

        // The offset field is absolute; the data we read already sits at
        // tunerIntrinsicOffset, so the mix needed is the difference. (For real
        // captures the offset is disabled and the data is left where it is.)
        // Skip the mix entirely when there's no shift so the common path stays a
        // plain copy.
        const double rate = sampleSrc->rate();
        const double appliedOffset = offsetEnabled ? (double)frequencyOffset.value()
                                                   : tunerIntrinsicOffset;
        const double normOffset = (rate > 0) ? (appliedOffset - tunerIntrinsicOffset) / rate : 0.0;
        const bool applyMix = (normOffset != 0.0);
        const int decim = decimation.value();

        size_t index;
        // viewRange.length() is used as some less arbitrary step value
        size_t step = viewRange.length();

        QProgressDialog progress("Exporting samples...", "Cancel", start, end, this);
        progress.setWindowModality(Qt::WindowModal);
        for (index = start; index < end; index += step) {
            progress.setValue(index);
            if (progress.wasCanceled())
                break;

            size_t length = std::min(step, end - index);
            auto samples = sampleSrc->getSamples(index, length);
            if (samples != nullptr) {
                for (auto i = 0; i < length; i += decim) {
                    if (applyMix) {
                        // Mix down by the offset so the targeted band lands at DC.
                        double phase = -Tau * std::fmod((double)(index + i) * normOffset, 1.0);
                        writeSample(os, mixSample(samples[i], phase), sc16);
                    } else {
                        writeSample(os, samples[i], sc16);
                    }
                }
            }
        }
        os.close();

        if (!progress.wasCanceled() && sigmfCheck.isChecked()) {
            double outRate = sampleSrc->rate() / decimation.value();
            writeSigMFMeta(outPath, datatype, outRate, recordingCenter + appliedOffset);
        }
    }

    // Clear the bandwidth preview now the dialog has closed.
    if (showDecimationPreview)
        spectrogramPlot->setDecimationPreview(0, 0);
}

void PlotView::writeSigMFMeta(const QString &dataFilename, const QString &datatype,
                              double sampleRate, double centerFrequency)
{
    QFileInfo dataInfo(dataFilename);
    QString fname = dataInfo.fileName();

    // SigMF pairs <base>.sigmf-meta with its dataset. Derive <base> from the
    // data file, stripping a .sigmf-data suffix specially since it isn't a
    // normal extension.
    QString base;
    const QString sigmfDataSuffix = ".sigmf-data";
    if (fname.endsWith(sigmfDataSuffix))
        base = fname.left(fname.length() - sigmfDataSuffix.length());
    else
        base = dataInfo.completeBaseName();

    QString metaFilename = dataInfo.path() + "/" + base + ".sigmf-meta";

    QJsonObject global;
    global["core:datatype"] = datatype;
    global["core:version"] = "1.0.0";
    if (sampleRate > 0)
        global["core:sample_rate"] = sampleRate;
    // If the dataset isn't named <base>.sigmf-data, record it as a
    // non-conforming dataset so the metadata still points at the right file.
    if (!fname.endsWith(sigmfDataSuffix))
        global["core:dataset"] = fname;

    QJsonObject capture;
    capture["core:sample_start"] = 0;
    if (centerFrequency != 0)
        capture["core:frequency"] = centerFrequency;
    QJsonArray captures;
    captures.append(capture);

    QJsonObject root;
    root["global"] = global;
    root["captures"] = captures;
    root["annotations"] = QJsonArray();

    QFile metafile(metaFilename);
    if (!metafile.open(QFile::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export",
            "Samples were written, but the SigMF metadata file could not be created: "
            + metafile.errorString());
        return;
    }
    metafile.write(QJsonDocument(root).toJson());
    metafile.close();
}

void PlotView::invalidateEvent()
{
    // The underlying samples changed (e.g. file reload); drop memoised FFT lines.
    for (auto plot : spectrumPlots)
        plot->invalidateCache();

    // Recompute the view range, scrollbars and plot heights for the new file;
    // painting and "Current view" export both read viewRange.
    horizontalScrollBar()->setMinimum(0);
    updateView();
}

void PlotView::repaint()
{
    viewport()->update();
}

void PlotView::setCursorSegments(int segments)
{
    // Calculate number of samples per segment
    float sampPerSeg = (float)selectedSamples.length() / cursors.segments();

    // Alter selection to keep samples per segment the same
    selectedSamples.maximum = selectedSamples.minimum + (segments * sampPerSeg + 0.5f);

    cursors.setSegments(segments);

    updateView();
    emitTimeSelection();
}

void PlotView::setFFTAndZoom(int size, int zoom)
{
    auto oldSamplesPerColumn = samplesPerColumn();
    float oldPlotCenter = (spectrogramScrollBar->value() + derivedPaneTop() / 2.0) / spectrogramPlot->height();
    if (spectrogramScrollBar->maximum() == 0)
        oldPlotCenter = 0.5;

    // Set new FFT size
    fftSize = size;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setFFTSize(size);

    // Set new zoom level
    zoomLevel = std::max(1,zoom);
    nfftSkip = std::max(1,-1*zoom);
    if (spectrogramPlot != nullptr) {
        spectrogramPlot->setZoomLevel(zoomLevel);
        spectrogramPlot->setSkip(nfftSkip);
    }

    // Update horizontal (time) scrollbar
    horizontalScrollBar()->setSingleStep(10);
    horizontalScrollBar()->setPageStep(100);

    updateView(true, samplesPerColumn() < oldSamplesPerColumn);

    // maintain the relative position of the spectrogram's vertical scroll bar
    if (spectrogramScrollBar->maximum())
        spectrogramScrollBar->setValue((int )(oldPlotCenter * spectrogramPlot->height() - derivedPaneTop() / 2.0 + 0.5f));
}

void PlotView::setPowerMin(int power)
{
    powerMin = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMin(power);
    updateView();
}

void PlotView::setPowerMax(int power)
{
    powerMax = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMax(power);
    updateView();
}

void PlotView::paintEvent(QPaintEvent *event)
{
    if (mainSampleSource == nullptr) return;

    QRect rect = QRect(0, 0, width(), height());
    QPainter painter(viewport());
    painter.fillRect(rect, Qt::black);


#define PLOT_LAYER(paintFunc)                                                   \
    for (size_t i = 0; i < plots.size(); i++) {                                 \
        QRect rect = plotRect(i);                                               \
        painter.save();                                                         \
        painter.setClipRect(paneRect(i));                                       \
        plots[i]->paintFunc(painter, rect, viewRange);                          \
        painter.restore();                                                      \
    }

    PLOT_LAYER(paintBack);
    PLOT_LAYER(paintMid);
    PLOT_LAYER(paintFront);
    if (cursorsEnabled)
        cursors.paintFront(painter, rect, viewRange);

    if (timeScaleEnabled) {
        paintTimeScale(painter, rect, viewRange);
    }

    if (crosshairsEnabled && crosshairValid) {
        paintCrosshair(painter, rect);
    }


#undef PLOT_LAYER
}

void PlotView::paintCrosshair(QPainter &painter, QRect &rect)
{
    if (!rect.contains(pointerPos))
        return;

    const int mx = pointerPos.x();
    const int my = pointerPos.y();
    const int cross = 2;          // half-length of the central "+" (5px tall/wide)
    const int gap = 8;            // blank space between the "+" and the long lines
    const int clear = cross + gap;

    painter.save();
    // Fine, single-pixel line; kept subtle so it doesn't obscure the spectrogram.
    painter.setPen(QPen(QColor(255, 255, 255, 160), 1, Qt::SolidLine));

    // Small central "+" marking the exact point.
    painter.drawLine(mx - cross, my, mx + cross, my);
    painter.drawLine(mx, my - cross, mx, my + cross);

    // Vertical line spans the full plot stack, linking the (shared) time axis of
    // the spectrogram and every derived plot.
    painter.drawLine(mx, rect.top(), mx, my - clear);
    painter.drawLine(mx, my + clear, mx, rect.bottom());

    // Horizontal line stays within the hovered plot's row (its y is meaningful
    // only there) and runs the full width.
    painter.drawLine(rect.left(), my, mx - clear, my);
    painter.drawLine(mx + clear, my, rect.right(), my);

    painter.restore();
}

void PlotView::paintTimeScale(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    float startTime = (float)sampleRange.minimum / sampleRate;
    float stopTime = (float)sampleRange.maximum / sampleRate;
    float duration = stopTime - startTime;

    if (duration <= 0)
        return;

    painter.save();

    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    int tickWidth = 80;
    int maxTicks = rect.width() / tickWidth;

    double durationPerTick = 10 * pow(10, floor(log(duration / maxTicks) / log(10)));

    double firstTick = int(startTime / durationPerTick) * durationPerTick;

    double tick = firstTick;

    while (tick <= stopTime) {

        size_t tickSample = tick * sampleRate;
        int tickLine = sampleToColumn(tickSample - sampleRange.minimum);

        char buf[128];
        snprintf(buf, sizeof(buf), "%.06f", tick);
        painter.drawLine(tickLine, 0, tickLine, 30);
        painter.drawText(tickLine + 2, 25, buf);

        tick += durationPerTick;
    }

    // Draw small ticks
    durationPerTick /= 10;
    firstTick = int(startTime / durationPerTick) * durationPerTick;
    tick = firstTick;
    while (tick <= stopTime) {

        size_t tickSample = tick * sampleRate;
        int tickLine = sampleToColumn(tickSample - sampleRange.minimum);

        painter.drawLine(tickLine, 0, tickLine, 10);
        tick += durationPerTick;
    }

    painter.restore();
}

int PlotView::derivedPlotsHeight()
{
    int height = 0;
    for (size_t i = 1; i < plots.size(); i++)
        height += plots[i]->height();
    return height;
}

// The view is split into two panes, each scrolling vertically on its own: the
// spectrogram on top and the derived plots below. The derived pane always has
// room for at least one plot, so a tall spectrogram (large FFT) can't push
// them all out of view.
int PlotView::derivedPaneHeight()
{
    int tallest = 0;
    for (size_t i = 1; i < plots.size(); i++)
        tallest = std::max(tallest, plots[i]->height());

    int viewHeight = viewport()->height();
    int roomBelowSpectrogram = viewHeight - spectrogramPlot->height();
    return std::min({derivedPlotsHeight(), std::max(tallest, roomBelowSpectrogram), viewHeight});
}

int PlotView::derivedPaneTop()
{
    // Without derived plots the spectrogram has the whole view. Otherwise their
    // pane sits directly below the spectrogram while that fits, and is pushed
    // up over it as far as needed when it doesn't.
    int viewHeight = viewport()->height();
    if (plots.size() < 2)
        return viewHeight;
    return std::max(0, std::min(spectrogramPlot->height(), viewHeight - derivedPaneHeight()));
}

// Where a plot is drawn in the viewport, scrolled within its pane.
QRect PlotView::plotRect(size_t index)
{
    int y = -spectrogramScrollBar->value();
    if (index > 0) {
        y = derivedPaneTop() - derivedScrollBar->value();
        for (size_t i = 1; i < index; i++)
            y += plots[i]->height();
    }
    return QRect(0, y, width(), plots[index]->height());
}

// The part of the viewport a plot is visible in.
QRect PlotView::paneRect(size_t index)
{
    int top = derivedPaneTop();
    if (index == 0)
        return QRect(0, 0, width(), top);
    return QRect(0, top, width(), derivedPaneHeight());
}

// The scroll bar of the pane at a viewport y.
QScrollBar *PlotView::paneScrollBar(int y)
{
    return (plots.size() > 1 && y >= derivedPaneTop()) ? derivedScrollBar : spectrogramScrollBar;
}

void PlotView::layoutScrollBars()
{
    // Each pane's scroll bar overlays the right edge of its pane, kept clear
    // of the view's horizontal bar where that overlays the viewport's bottom
    // edge (overlay style).
    int barWidth = spectrogramScrollBar->sizeHint().width();
    int x = viewport()->width() - barWidth;
    int bottom = viewport()->height();
    QWidget *hbarBox = horizontalScrollBar()->parentWidget();
    if (hbarBox->isVisible())
        bottom = std::min(bottom, viewport()->mapFromParent(hbarBox->geometry().topLeft()).y());
    int top = std::min(derivedPaneTop(), bottom);
    spectrogramScrollBar->setGeometry(x, 0, barWidth, top);
    derivedScrollBar->setGeometry(x, top, barWidth, std::max(0, std::min(derivedPaneHeight(), bottom - top)));
}

void PlotView::resizeEvent(QResizeEvent * event)
{
    updateView();
}

size_t PlotView::samplesPerColumn()
{
    return fftSize * nfftSkip / zoomLevel;
}

void PlotView::scrollContentsBy(int dx, int dy)
{
    updateView();
}

void PlotView::showEvent(QShowEvent *event)
{
    // Intentionally left blank. See #171
}

void PlotView::keyPressEvent(QKeyEvent *event)
{
    // Vertical keys scroll the spectrogram's pane (the base class would scroll
    // the view's unused bar); the rest, e.g. Left/Right, pan time as before.
    switch (event->key()) {
    case Qt::Key_Up:       spectrogramScrollBar->triggerAction(QAbstractSlider::SliderSingleStepSub); break;
    case Qt::Key_Down:     spectrogramScrollBar->triggerAction(QAbstractSlider::SliderSingleStepAdd); break;
    case Qt::Key_PageUp:   spectrogramScrollBar->triggerAction(QAbstractSlider::SliderPageStepSub); break;
    case Qt::Key_PageDown: spectrogramScrollBar->triggerAction(QAbstractSlider::SliderPageStepAdd); break;
    default:
        QGraphicsView::keyPressEvent(event);
    }
}

void PlotView::wheelEvent(QWheelEvent *event)
{
    // Vertical scrolling goes to the pane under the pointer; horizontal still
    // pans time through the base class.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    int y = event->position().y();
#else
    int y = event->pos().y();
#endif
    if (qAbs(event->angleDelta().y()) >= qAbs(event->angleDelta().x())) {
        QApplication::sendEvent(paneScrollBar(y), event);
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void PlotView::updateViewRange(bool reCenter)
{
    // Update current view
    auto start = columnToSample(horizontalScrollBar()->value());
    viewRange = {start, std::min(start + columnToSample(width()), mainSampleSource->count())};

    // Adjust time offset to zoom around central sample
    if (reCenter) {
        horizontalScrollBar()->setValue(
            sampleToColumn(zoomSample) - zoomPos
        );
    }
    zoomSample = viewRange.minimum + viewRange.length() / 2;
    zoomPos = width() / 2;
}

void PlotView::updateView(bool reCenter, bool expanding)
{
    if (!expanding) {
        updateViewRange(reCenter);
    }
    horizontalScrollBar()->setMaximum(std::max(0, sampleToColumn(mainSampleSource->count()) - width()));

    // Each pane scrolls vertically over what it holds.
    int paneTop = derivedPaneTop();
    int paneHeight = derivedPaneHeight();
    spectrogramScrollBar->setMaximum(std::max(0, spectrogramPlot->height() - paneTop));
    spectrogramScrollBar->setPageStep(paneTop);
    derivedScrollBar->setMaximum(std::max(0, derivedPlotsHeight() - paneHeight));
    derivedScrollBar->setPageStep(paneHeight);
    layoutScrollBars();

    if (expanding) {
        updateViewRange(reCenter);
    }

    // Update cursors
    range_t<int> newSelection = {
        sampleToColumn(selectedSamples.minimum) - horizontalScrollBar()->value(),
        sampleToColumn(selectedSamples.maximum) - horizontalScrollBar()->value()
    };
    cursors.setSelection(newSelection);

    // Track the column under the (possibly stationary) pointer; horizontal scroll
    // changes it. setSample() skips the repaint when the column is unchanged.
    updateSpectrumPlots();
    // Force a repaint regardless so the alignment band and power scaling follow
    // vertical scroll, resize and power-range changes even when the column (and
    // thus setSample) didn't change. The view recomputes its alignment in paint.
    for (auto plot : spectrumPlots)
        plot->update();

    // Keep the pointer readout current when the view changes under a stationary
    // pointer (e.g. zoom or scroll).
    if (viewport()->underMouse())
        emitPointerPosition();

    // Re-paint
    viewport()->update();
}

void PlotView::setSampleRate(double rate)
{
    sampleRate = rate;

    if (spectrogramPlot != nullptr)
        spectrogramPlot->setSampleRate(rate);

    emitTimeSelection();
}

void PlotView::enableScales(bool enabled)
{
    timeScaleEnabled = enabled;

    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableScales(enabled);

    for (auto plot : spectrumPlots)
        plot->enableScales(enabled);

    viewport()->update();
}

void PlotView::enableAnnotations(bool enabled)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnotations(enabled);

    viewport()->update();
}

void PlotView::enableAnnoLabels(bool enabled)
{
    annotationLabelsEnabled = enabled;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnoLabels(enabled);

    viewport()->update();
}

void PlotView::enableAnnotationCommentsTooltips(bool enabled)
{
    annotationCommentsEnabled = enabled;

    viewport()->update();
}

void PlotView::enableAnnoColors(bool enabled)
{
    annotationColorsEnabled = enabled;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnoColors(enabled);

    viewport()->update();
}

void PlotView::enableCrosshairs(bool enabled)
{
    crosshairsEnabled = enabled;
    if (enabled) {
        // Hide the pointer entirely; the drawn "+" marks the exact point. NoDrag
        // is required because ScrollHandDrag forces its own hand cursor on every
        // press/release.
        setDragMode(QGraphicsView::NoDrag);
        viewport()->setCursor(Qt::BlankCursor);
        // If the pointer is already over the plot, show the crosshair at once
        // instead of waiting for the first move.
        if (viewport()->underMouse()) {
            pointerPos = viewport()->mapFromGlobal(QCursor::pos());
            crosshairValid = true;
        }
    } else {
        crosshairValid = false;
        // Restore the default pan-hand drag behaviour (resets the cursor too).
        setDragMode(QGraphicsView::ScrollHandDrag);
    }

    for (auto plot : spectrumPlots)
        plot->update();
    viewport()->update();
}

int PlotView::sampleToColumn(size_t sample)
{
    return sample / samplesPerColumn();
}

size_t PlotView::columnToSample(int col)
{
    return col * samplesPerColumn();
}
