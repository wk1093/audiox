#include "audio/effects/apo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace audiox::effects {
namespace {

std::string lower(std::string text) {
    for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string &text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::vector<std::string> tokens(const std::string &text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        const size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i > start) out.push_back(text.substr(start, i - start));
    }
    return out;
}

// Parses a leading number (accepting ',' as the decimal mark) and returns the unit suffix.
bool number(const std::string &token, double *value, std::string *suffix = nullptr) {
    std::string text = token;
    std::replace(text.begin(), text.end(), ',', '.');
    char *end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed)) return false;
    *value = parsed;
    if (suffix) *suffix = lower(end);
    return true;
}

void warn(ApoImport *out, const std::string &message) {
    if (std::find(out->warnings.begin(), out->warnings.end(), message) == out->warnings.end() &&
        out->warnings.size() < 16)
        out->warnings.push_back(message);
}

double shelfQ(double slope, double gainDb) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double arg = (a + 1 / a) * (1 / slope - 1) + 2;
    return arg > 1e-6 ? 1 / std::sqrt(arg) : 40.0;
}

struct FilterType {
    const char *name;
    uint8_t type;
    bool corner;    // LS/HS: frequency is the corner rather than the centre.
    double defaultQ;
};

constexpr FilterType kTypes[] = {
    {"pk", AEQ_PEAK, false, 0.707}, {"peq", AEQ_PEAK, false, 0.707},
    {"modal", AEQ_PEAK, false, 0.707}, {"lp", AEQ_LOW_PASS, false, 0.70710678},
    {"lpq", AEQ_LOW_PASS, false, 0.70710678}, {"hp", AEQ_HIGH_PASS, false, 0.70710678},
    {"hpq", AEQ_HIGH_PASS, false, 0.70710678}, {"bp", AEQ_BAND_PASS, false, 0.70710678},
    {"no", AEQ_NOTCH, false, 30}, {"ap", AEQ_ALL_PASS, false, 0.70710678},
    {"ls", AEQ_LOW_SHELF, true, 0}, {"lsc", AEQ_LOW_SHELF, false, 0},
    {"hs", AEQ_HIGH_SHELF, true, 0}, {"hsc", AEQ_HIGH_SHELF, false, 0},
};

bool parseFilter(const std::vector<std::string> &words, ApoImport *out) {
    if (words.empty()) return false;
    const std::string state = lower(words[0]);
    if (state != "on" && state != "off") {
        ++out->skipped;
        warn(out, "Filter lines must start with ON or OFF");
        return false;
    }
    if (words.size() < 2) {
        ++out->skipped;
        warn(out, "Filter line is missing a filter type");
        return false;
    }
    const std::string typeName = lower(words[1]);
    const FilterType *type = nullptr;
    for (const auto &candidate : kTypes)
        if (typeName == candidate.name) type = &candidate;
    if (!type) {
        ++out->skipped;
        warn(out, "Unsupported filter type " + words[1] + " was skipped");
        return false;
    }
    double hz = NAN, gain = 0, q = NAN, bw = NAN, slope = NAN;
    for (size_t i = 2; i < words.size(); ++i) {
        const std::string word = lower(words[i]);
        double value = 0;
        std::string suffix;
        if (word == "fc" && i + 1 < words.size() && number(words[i + 1], &value)) {
            hz = value;
            ++i;
        } else if (word == "gain" && i + 1 < words.size() && number(words[i + 1], &value)) {
            gain = value;
            ++i;
        } else if (word == "q" && i + 1 < words.size() && number(words[i + 1], &value)) {
            q = value;
            ++i;
        } else if (word == "bw") {
            if (i + 1 < words.size() && lower(words[i + 1]) == "oct") ++i;
            if (i + 1 < words.size() && number(words[i + 1], &value)) {
                bw = value;
                ++i;
            }
        } else if (std::isnan(hz) && number(words[i], &value, &suffix)) {
            // Slope written before Fc, e.g. "LSC 12 dB Fc ..." or "LS 6dB Fc ...".
            if (suffix == "db" || (i + 1 < words.size() && lower(words[i + 1]) == "db")) slope = value;
        }
    }
    if (!std::isfinite(hz) || hz <= 0) {
        ++out->skipped;
        warn(out, "Filter without a valid Fc was skipped");
        return false;
    }
    if (out->bandCount >= kAdvancedEqMaxBands) {
        ++out->skipped;
        warn(out, "Only the first 32 filters were imported");
        return false;
    }
    if (std::isfinite(bw) && bw > 0 && !std::isfinite(q)) {
        const double ratio = std::pow(2.0, bw);
        q = std::sqrt(ratio) / (ratio - 1);
    }
    if (type->type == AEQ_LOW_SHELF || type->type == AEQ_HIGH_SHELF) {
        const double a = std::pow(10.0, gain / 40.0);
        double s = 0.9;
        if (std::isfinite(slope) && slope > 0) s = slope / 12.0;
        else if (std::isfinite(q) && q > 0) {
            const double denom = (1 / (q * q) - 2) / (a + 1 / a) + 1;
            s = denom > 1e-9 ? 1 / denom : 1.0;
        }
        if (type->corner) {
            const double shift = std::pow(10.0, std::fabs(gain) / 80.0 / s);
            hz = type->type == AEQ_LOW_SHELF ? hz * shift : hz / shift;
        }
        if (!std::isfinite(q) || q <= 0 || std::isfinite(slope)) q = shelfQ(s, gain);
    } else if (!std::isfinite(q) || q <= 0 || typeName == "lp" || typeName == "hp") {
        q = type->defaultQ;
    }
    ApoBand &band = out->bands[out->bandCount++];
    // OFF filters stay as disabled bands so exported files round-trip.
    band.type = state == "off" ? uint8_t(AEQ_OFF) : type->type;
    band.hz = float(std::clamp(hz, 10.0, 22000.0));
    band.gainDb = float(std::clamp(gain, -36.0, 36.0));
    band.q = float(std::clamp(q, 0.025, 40.0));
    if (band.hz != float(hz) || band.gainDb != float(gain) || band.q != float(q))
        warn(out, "Some filter values were outside the supported range and were clamped");
    return true;
}

} // namespace

bool parseApo(const char *text, size_t length, ApoImport *out) {
    if (!out) return false;
    *out = ApoImport{};
    if (!text) return false;
    bool selected = true, found = false;
    size_t start = 0;
    while (start <= length) {
        size_t end = start;
        while (end < length && text[end] != '\n') ++end;
        std::string line(text + start, end - start);
        start = end + 1;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) {
            if (end >= length) break;
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            warn(out, "Lines without a command were ignored");
            continue;
        }
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::vector<std::string> words = tokens(line.substr(colon + 1));
        if (key == "channel") {
            selected = false;
            for (const auto &word : words) {
                const std::string channel = lower(word);
                if (channel == "all" || channel == "l" || channel == "1") selected = true;
            }
            if (!selected || words.size() > 1)
                warn(out, "Channel sections are merged: only filters for all channels or the left channel are used");
            continue;
        }
        if (key == "preamp") {
            double value = 0;
            if (!words.empty() && number(words[0], &value)) {
                if (selected) out->preampDb += float(value);
                found = true;
            } else {
                warn(out, "Invalid Preamp line was ignored");
            }
            continue;
        }
        if (key.rfind("filter", 0) == 0 &&
            key.find_first_not_of("0123456789 \t", 6) == std::string::npos) {
            if (!selected) {
                ++out->skipped;
                continue;
            }
            found = parseFilter(words, out) || found;
            continue;
        }
        if (key == "device" || key == "stage") continue;
        warn(out, "Unsupported command \"" + trim(line.substr(0, colon)) + "\" was ignored");
    }
    if (out->preampDb < -36 || out->preampDb > 36) {
        out->preampDb = std::clamp(out->preampDb, -36.0f, 36.0f);
        warn(out, "Preamp was clamped to the supported range");
    }
    return found;
}

void applyApo(const ApoImport &import, float *values) {
    values[0] = import.preampDb;
    values[1] = float(import.bandCount);
    for (unsigned i = 0; i < import.bandCount && i < kAdvancedEqMaxBands; ++i) {
        float *band = values + kAdvancedEqFirstBandParam + i * kAdvancedEqParamsPerBand;
        band[0] = import.bands[i].type;
        band[1] = import.bands[i].hz;
        band[2] = import.bands[i].gainDb;
        band[3] = import.bands[i].q;
    }
}

std::string exportApo(const float *values) {
    // Off bands keep their settings and are written as disabled peaking filters.
    static const char *const names[] = {"PK", "PK", "LSC", "HSC", "LPQ", "HPQ", "BP", "NO", "AP"};
    char line[160];
    snprintf(line, sizeof(line), "Preamp: %.6g dB\n", double(values[0]));
    std::string out = line;
    const unsigned count = std::min<unsigned>(kAdvancedEqMaxBands, unsigned(std::max(0.0f, values[1]) + 0.5f));
    for (unsigned i = 0; i < count; ++i) {
        const float *band = values + kAdvancedEqFirstBandParam + i * kAdvancedEqParamsPerBand;
        const unsigned type = std::min<unsigned>(AEQ_FILTER_LAST, unsigned(std::max(0.0f, band[0]) + 0.5f));
        const bool gain = type <= AEQ_HIGH_SHELF;
        int n = snprintf(line, sizeof(line), "Filter %u: %s %s Fc %.6g Hz", i + 1,
                         type == AEQ_OFF ? "OFF" : "ON", names[type], double(band[1]));
        if (gain && n > 0 && size_t(n) < sizeof(line))
            n += snprintf(line + n, sizeof(line) - size_t(n), " Gain %.6g dB", double(band[2]));
        if (n > 0 && size_t(n) < sizeof(line))
            snprintf(line + n, sizeof(line) - size_t(n), " Q %.6g\n", double(band[3]));
        out += line;
    }
    return out;
}

} // namespace audiox::effects
