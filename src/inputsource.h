/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *  Copyright (C) 2015, Jared Boone <jared@sharebrained.com>
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

#include <complex>
#include <QFile>
#include "samplesource.h"

class SampleAdapter {
public:
    // Canonical format name, as accepted by -f
    virtual const char *name() = 0;
    virtual size_t sampleSize() = 0;
    virtual void copyRange(const void* const src, size_t start, size_t length, std::complex<float>* const dest) = 0;
    virtual ~SampleAdapter() { };
};

class InputSource : public SampleSource<std::complex<float>>
{
private:
    QFile *inputFile = nullptr;
    size_t sampleCount = 0;
    double sampleRate = 0.0;
    uchar *mmapData = nullptr;
    std::unique_ptr<SampleAdapter> sampleAdapter;
    std::string _fmt;
    std::string _forcedFmt;
    std::string _fileFmt;
    bool _realSignal = false;
    bool _sigmf = false;

    QJsonObject readMetaData(const QString &filename, std::unique_ptr<SampleAdapter> &adapter, bool &realSignal);

public:
    InputSource();
    ~InputSource();
    void cleanup();
    void openFile(const char *filename);
    std::unique_ptr<std::complex<float>[]> getSamples(size_t start, size_t length);
    size_t count() {
        return sampleCount;
    };
    void setSampleRate(double rate);
    // Format for files whose extension doesn't name one (SigMF never uses it)
    void setFormat(std::string fmt);
    // Decode the next file opened as fmt even if its extension names another format
    void forceFormat(std::string fmt);
    // The format override the open file was opened with, or empty if none
    std::string formatOverride() {
        return _fileFmt;
    };
    // The open file's sample format, or empty if no file is open
    std::string format() {
        return sampleAdapter ? sampleAdapter->name() : "";
    };
    // The format step places from the open file's among formats of the same
    // kind (complex or real), wrapping around
    std::string adjacentFormat(int step);
    double rate();
    bool realSignal() {
        return _realSignal;
    };
    bool isSigMF() {
        return _sigmf;
    };
    float relativeBandwidth() {
        return 1;
    }
};
