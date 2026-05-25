/*
 *  Copyright (C) 2016, Mike Walters <mike@flomp.net>
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

#include "util.h"

#include <cmath>
#include <iomanip>

std::string formatSIValue(float value)
{
    std::map<int, std::string> prefixes = {
        {  9,   "G" },
        {  6,   "M" },
        {  3,   "k" },
        {  0,   ""  },
        { -3,   "m" },
        { -6,   "µ" },
        { -9,   "n" },
    };

    int power = 0;
    while (value < 1.0f && power > -9) {
        value *= 1e3;
        power -= 3;
    }
    while (value >= 1e3 && power < 9) {
        value *= 1e-3;
        power += 3;
    }
    std::stringstream ss;
    ss << value << prefixes[power];
    return ss.str();
}

std::string formatFixedSI(double value, double reference, const std::string &unit)
{
    // SI prefix chosen from the magnitude of `reference` (not `value`), so the
    // displayed unit stays fixed as the value changes, with fixed decimal
    // precision. e.g. a >= 2 MHz reference always reads in MHz, and a value near
    // zero reads "0.000 MHz" rather than switching unit (which also avoids the
    // negative-value pitfall in formatSIValue()).
    static const struct { double threshold; double scale; const char *prefix; } steps[] = {
        { 2e9,  1e9,  "G" },
        { 2e6,  1e6,  "M" },
        { 2e3,  1e3,  "k" },
        { 2e0,  1e0,  ""  },
        { 2e-3, 1e-3, "m" },
        { 2e-6, 1e-6, "µ" },
        { 2e-9, 1e-9, "n" },
    };

    const int decimals = 3;
    double absRef = std::fabs(reference);
    double scale = 1e-9;          // used when reference is below the smallest threshold
    const char *prefix = "n";
    for (auto &s : steps) {
        if (absRef >= s.threshold) {
            scale = s.scale;
            prefix = s.prefix;
            break;
        }
    }

    double scaled = value / scale;
    // Snap values that round to zero so we never display a negative zero.
    if (std::fabs(scaled) < 0.5 * std::pow(10.0, -decimals))
        scaled = 0.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimals) << scaled << " " << prefix << unit;
    return ss.str();
}
