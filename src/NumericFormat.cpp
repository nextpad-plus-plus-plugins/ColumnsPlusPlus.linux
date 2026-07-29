// Columns++ for Notepad++ — macOS port.
// NumericFormat.mm — formatNumber / parseNumber / getNumericAlignment.
//
// Ported essentially verbatim from the Windows NumericFormat.cpp. The only
// platform change is that the Windows std::wstring (UTF-16) work uses a
// std::u32string (UTF-32 code points) here via toWide32(); the currency/sign/
// space Unicode tables are unchanged. These routines power Align numeric and
// Add/Average numbers, and have no Win32 dependency.

#include "CppData.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

// Portable substitutes for the floating-point std::to_chars overloads, which are
// only available on macOS 13.3+ while this plugin deploys to 11.0. The integer
// std::to_chars is available and kept. These produce the same text the Windows
// build's std::to_chars(value, fixed, prec) / (value, scientific) calls did.
//
// fixedChars: write `value` in fixed notation with exactly `prec` fractional
//   digits into [p,q); returns pointer past the last char written, or nullptr on
//   overflow. Equivalent to std::to_chars(p,q,value,chars_format::fixed,prec).
static char* fixedChars(char* p, char* q, double value, int prec) {
    int need = std::snprintf(nullptr, 0, "%.*f", prec, value);
    if (need < 0 || p + need >= q) return nullptr;
    std::snprintf(p, (size_t)(q - p), "%.*f", prec, value);
    return p + need;
}

// scientificChars: shortest-ish scientific form into a std::string. Used only for
// magnitudes above 2^53 (where doubles are integers), so an exact shortest
// round-trip is not behaviour-critical; we emit %e and trim trailing mantissa
// zeros, normalising the exponent to lower-case 'e'.
static std::string scientificChars(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17e", value);
    std::string s(buf);
    size_t e = s.find_first_of("eE");
    if (e == std::string::npos) return s;
    std::string mant = s.substr(0, e);
    std::string exp  = s.substr(e);
    // Trim trailing zeros in the mantissa fraction.
    size_t dot = mant.find('.');
    if (dot != std::string::npos) {
        size_t last = mant.find_last_not_of('0');
        if (last == dot) last = dot - 1;     // drop the dot too if no fraction left
        mant = mant.substr(0, last + 1);
    }
    exp[0] = 'e';
    return mant + exp;
}

static constexpr double scientificNotationThreshold = static_cast<long long>(1) << std::numeric_limits<double>::digits;

static constexpr double factors[4][4] = { 1.                  , 24.          , 60. * 24., 60. * 60. * 24,
                                          1. / 24.            , 1            , 60.      , 60. * 60.     ,
                                          1. / 24. / 60.      , 1. / 60.     , 1.       , 60.           ,
                                          1. / 24. / 60. / 60., 1 / 60. / 60., 1. / 60. , 1.            };

std::string ColumnsPlusPlusData::formatNumber(double value, const NumericFormat& format) const {

    if (!std::isfinite(value)) return "";

    int segments[4];
    int minSeg;
    int maxSeg;

    if (format.timeEnable & 14) {
        segments[0] = !(format.timeEnable & 1) ? -1 : timeScalarUnit;
        segments[1] = !(format.timeEnable & 2) ? -1 : timePartialRule == 0 ? 1 : timePartialRule == 3 ? 3 : 2;
        segments[2] = !(format.timeEnable & 4) ? -1 : timePartialRule < 2 ? 2 : 3;
        segments[3] = !(format.timeEnable & 8) ? -1 : 3;
        maxSeg = std::max(std::max(segments[0], segments[1]), std::max(segments[2], segments[3]));
        minSeg = std::min(std::min(segments[0] < 0 ? 3 : segments[0], segments[1] < 0 ? 3 : segments[1] - 1),
                          std::min(segments[2] < 0 ? 3 : segments[2] - 2, segments[3] < 0 ? 3 : 0));
    }
    else {
        segments[0] = minSeg = maxSeg = timeScalarUnit;
        segments[1] = segments[2] = segments[3] = -1;
    }

    if (timeScalarUnit != maxSeg) value *= factors[timeScalarUnit][maxSeg];
    value = std::round(value * pow(10, format.maxDec)) / pow(10, format.maxDec);

    std::string s(31 + format.maxDec, 0);

    if (std::abs(value) > scientificNotationThreshold) {
        s = scientificChars(value);
        size_t d = s.find('.');
        if (d != std::string::npos) {
            if (settings.decimalSeparatorIsComma) s[d] = ',';
            if (!format.thousands.empty()) {
                size_t e = s.find_first_of("eE");
                if (e != std::string::npos && e > d + 4)
                    for (size_t q = e - (e - d - 2) % 3 - 1; q > d + 1; q -= 3) s = s.substr(0, q) + format.thousands + s.substr(q);
            }
        }
        return s;
    }

    int best;
    long long segFinal[4];
    long long segValue[4];

    if (minSeg == maxSeg) best = 0;
    else {
        double whole = std::trunc(std::abs(value));
        int needRight = whole == std::abs(value) ? minSeg : maxSeg;
        int wantLeft = maxSeg;
        segFinal[maxSeg] = segValue[maxSeg] = static_cast<long long>(whole);
        for (int i = maxSeg; i > minSeg; --i) {
            segFinal[i - 1] = segValue[i - 1] = segValue[i] / (i == 1 ? 24 : 60);
            segValue[i] = segValue[i] % (i == 1 ? 24 : 60);
            if (segValue[i] != 0 && needRight < i) needRight = i;
            if (segValue[i - 1] != 0) wantLeft = i - 1;
        }
        best = -1;
        for (int i = 0; i < 4; ++i) {
            if (segments[i] < needRight) continue;
            if (best < 0 || (segments[best] - best > wantLeft && segments[i] - i < segments[best] - best)) best = i;
        }
    }

    if (best == 0 && segments[best] == maxSeg) {
        char* end = fixedChars(s.data(), s.data() + s.length(), value, format.maxDec);
        if (!end) return "";
        s.resize(end - s.data());
    }

    else {
        char* p = s.data();
        char* q = p + s.length();
        if (value < 0) *p++ = '-';
        int fromSeg = segments[best] - best;
        std::to_chars_result tcr = std::to_chars(p, q, segFinal[segments[best] - best]);
        if (tcr.ec != std::errc()) return "";
        p = tcr.ptr;
        for (int i = fromSeg + 1; i <= segments[best]; ++i) {
            *p++ = ':';
            if (segValue[i] < 10) *p++ = '0';
            tcr = std::to_chars(p, q, segValue[i]);
            if (tcr.ec != std::errc()) return "";
            p = tcr.ptr;
        }
        if (segments[best] == maxSeg) {
            char onesDigit = *--p;
            char* end = fixedChars(p, q, std::abs(value) - std::trunc(std::abs(value)), format.maxDec);
            if (!end) return "";
            *p = onesDigit;
            p = end;
        }
        s.resize(p - s.data());
    }

    if (format.leftPad > 1) {
        size_t left = s.find_first_of(".:");
        if (left == std::string::npos) left = s.length();
        if (s[0] == '-') --left;
        if (static_cast<int>(left) < format.leftPad)
            s = s[0] == '-' ? '-' + std::string(format.leftPad - left, '0') + s.substr(1) : std::string(format.leftPad - left, '0') + s;
    }

    if (format.maxDec && segments[best] == maxSeg) {
        if (format.minDec < format.maxDec) {
            size_t dMin = format.minDec < 0 ? 0 : static_cast<size_t>(format.minDec);
            size_t dMax = static_cast<size_t>(format.maxDec);
            s = s.substr(0, std::max(s.find_last_not_of('0') + 1, s.length() - (dMax - dMin)));
            if (format.minDec < 0 && !std::isdigit((unsigned char)s.back())) s.pop_back();
        }
    }
    else if (format.minDec == 0) s += '.';

    if (settings.decimalSeparatorIsComma) {
        size_t d = s.find('.');
        if (d != std::string::npos) s[d] = ',';
    }

    if (!format.thousands.empty()) {
        const char decimalSeparator = settings.decimalSeparatorIsComma ? ',' : '.';
        bool negative = false;
        if (s[0] == '-') {
            s = s.substr(1);
            negative = true;
        }
        size_t j = s.find_first_not_of("0123456789");
        if (j == std::string::npos) j = s.length();
        else {
            size_t k = s.find_last_not_of("0123456789");
            if (s[k] == decimalSeparator) {
                size_t p = s.length() - k - 1;
                if (p > 3) {
                    for (size_t q = s.length() - (p - 1) % 3 - 1; q > k + 1; q -= 3)
                        s = s.substr(0, q) + format.thousands + s.substr(q);
                }
            }
        }
        if (j > 3) for (ptrdiff_t q = j - 3; q > 0; q -= 3)
            s = s.substr(0, q) + format.thousands + s.substr(q);
        if (negative) s = '-' + s;
    }

    return s;
}

NumericParse ColumnsPlusPlusData::parseNumber(const std::string& text) {

    NumericParse numericParse;

    static const std::u32string currency =
        U"$¢£¤¥֏؋߾߿৲৳৻૱௹฿៛"
        U"₠₡₢₣₤₥₦₧₨₩₪₫€₭₮₯"
        U"₰₱₲₳₴₵₶₷₸₹₺₻₼₽₾₿"
        U"꠸﷼﹩＄￠￡￥￦";
    static const std::u32string plus   = U"+＋";
    static const std::u32string minus  = U"-‒–−﹣－";
    static const std::u32string spaces = U"               ";
    static const std::u32string inside = U".,:'0123456789" + spaces;

    const char32_t decimal = settings.decimalSeparatorIsComma ? U',' : U'.';
    const std::u32string s = toWide32(text);

    size_t left = s.find_first_of(U"0123456789");
    if (left == std::u32string::npos) return numericParse;
    size_t right = s.find_last_of(U"0123456789");

    size_t decimalPosition = (left > 0 && s[left - 1] == decimal) ? --left : s.find_first_of(decimal, left);
    if (decimalPosition == right + 1) ++right;
    else if (decimalPosition > right) decimalPosition = right + 1;
    else if (decimalPosition != s.find_last_of(decimal, right)) return numericParse;

    size_t colonPosition = s.find_last_of(U':', right);
    if (colonPosition < left) colonPosition = std::u32string::npos;
    if (colonPosition != std::u32string::npos) {
        if (colonPosition > decimalPosition) return numericParse;
        if (std::count(std::next(s.begin(), left), std::next(s.begin(), right), U':') > 3) return numericParse;
    }

    const std::u32string_view prefix = std::u32string_view(s).substr(0, left);
    const std::u32string_view number = std::u32string_view(s).substr(left, right - left + 1);
    const std::u32string_view suffix = std::u32string_view(s).substr(right + 1);

    if (number.find_first_not_of(inside) != std::u32string::npos) return numericParse;

    bool negative = false;
    bool foundSign = false;

    if (prefix.find_first_not_of(spaces) != std::u32string::npos) {
        const size_t lastArbitrary = prefix.find_last_not_of(spaces + currency + plus + minus);
        const size_t signAt = prefix.find_first_of(plus + minus);
        const size_t cncyAt = prefix.find_first_of(currency);
        if (lastArbitrary == std::u32string::npos
            && (signAt == std::u32string::npos || signAt == prefix.find_last_of(plus + minus))
            && (cncyAt == std::u32string::npos || cncyAt == prefix.find_last_of(currency))) {
            if (signAt != std::u32string::npos) {
                foundSign = true;
                negative = minus.find_first_of(prefix[signAt]) != std::u32string::npos;
            }
        }
        else {
            const size_t lastSpace = prefix.find_last_of(spaces);
            if (lastSpace == std::u32string::npos) return numericParse;
            switch (prefix.length() - lastSpace) {
            case 1:
                break;
            case 2:
                if (currency.find_first_of(prefix[lastSpace + 1]) != std::u32string::npos) break;
                foundSign = true;
                if (plus.find_first_of(prefix[lastSpace + 1]) != std::u32string::npos) break;
                negative = true;
                if (minus.find_first_of(prefix[lastSpace + 1]) != std::u32string::npos) break;
                return numericParse;
            case 3:
                if (plus.find_first_of(prefix[lastSpace + 2]) != std::u32string::npos)   foundSign = true;
                else if (minus.find_first_of(prefix[lastSpace + 2]) != std::u32string::npos) { foundSign = true; negative = true; }
                else if (currency.find_first_of(prefix[lastSpace + 2]) == std::u32string::npos) return numericParse;
                if (currency.find_first_of(prefix[lastSpace + 1]) != std::u32string::npos) {
                    if (foundSign) break;
                    else return numericParse;
                }
                foundSign = true;
                if (plus.find_first_of(prefix[lastSpace + 1]) != std::u32string::npos) break;
                negative = true;
                if (minus.find_first_of(prefix[lastSpace + 1]) != std::u32string::npos) break;
                return numericParse;
            default:
                return numericParse;
            }
        }
    }

    if (suffix.find_first_not_of(spaces) != std::u32string::npos) {
        const size_t firstArbitrary = suffix.find_first_not_of(spaces + currency + plus + minus);
        const size_t signAt = suffix.find_first_of(plus + minus);
        const size_t cncyAt = suffix.find_first_of(currency);
        if (firstArbitrary == std::u32string::npos
            && (signAt == std::u32string::npos || signAt == suffix.find_last_of(plus + minus))
            && (cncyAt == std::u32string::npos || cncyAt == suffix.find_last_of(currency))) {
            if (signAt != std::u32string::npos) {
                if (foundSign) return numericParse;
                foundSign = true;
                if (minus.find_first_of(suffix[signAt]) != std::u32string::npos) negative = true;
            }
        }
        else if (firstArbitrary != 0) {
            size_t firstSpace = suffix.find_first_of(spaces);
            if (firstSpace != 0) {
                if (cncyAt == 0) {
                    bool isMinus = minus.find_first_of(suffix[1]) != std::u32string::npos;
                    bool isPlus = !isMinus && plus.find_first_of(suffix[1]) != std::u32string::npos;
                    if (isPlus || isMinus) {
                        if (foundSign || firstSpace != 2) return numericParse;
                        foundSign = true;
                        if (isMinus) negative = true;
                    }
                }
                else {
                    if (foundSign || !(firstSpace == 1 || (cncyAt == 1 && firstSpace == 2))) return numericParse;
                    foundSign = true;
                    negative = minus.find_first_of(suffix[0]) != std::u32string::npos;
                }
            }
        }
    }

    std::string n[4];
    int nLevel = 0;
    bool separatorOK = true;
    for (size_t i = 0; i < number.length(); ++i) {
        if (number[i] >= U'0' && number[i] <= U'9') {
            n[nLevel] += static_cast<char>(number[i]);
            separatorOK = true;
            continue;
        }
        else if (!separatorOK) return numericParse;
        separatorOK = false;
        if (number[i] == decimal) n[nLevel] += '.';
        else if (number[i] == U':') ++nLevel;
    }

    if (n[nLevel].empty()) return numericParse;
    double v = std::stod(n[nLevel]);

    switch (nLevel) {
    case 1:
        v += (timePartialRule == 0 ? 24 : 60) * std::stod(n[0]);
        v *= factors[timePartialRule == 0 ? 1 : timePartialRule == 3 ? 3 : 2][timeScalarUnit];
        break;
    case 2:
        v += (timePartialRule < 2 ? 24 * 60 : 60 * 60) * std::stod(n[0]) + 60 * std::stod(n[1]);
        v *= factors[timePartialRule < 2 ? 2 : 3][timeScalarUnit];
        break;
    case 3:
        v += 24 * 60 * 60 * std::stod(n[0]) + 60 * 60 * std::stod(n[1]) + 60 * std::stod(n[2]);
        v *= factors[3][timeScalarUnit];
    }

    numericParse.value         = negative ? -v : v;
    numericParse.decimalPlaces = right > decimalPosition ? static_cast<int>(right - decimalPosition) : 0;
    numericParse.timeSegments  = nLevel;
    return numericParse;
}

bool ColumnsPlusPlusData::getNumericAlignment(const std::string& text, size_t& colonPosition, size_t& decimalPosition) {

    static const std::u32string inside = U".,:'0123456789               ";

    const char decimal = settings.decimalSeparatorIsComma ? ',' : '.';

    size_t left = text.find_first_of("0123456789");
    if (left == std::string::npos) return false;
    size_t right = text.find_last_of("0123456789");

    decimalPosition = (left > 0 && text[left - 1] == decimal) ? --left : text.find_first_of(decimal, left);
    if (decimalPosition > right) decimalPosition = right + 1;
    else if (decimalPosition != text.find_last_of(decimal, right)) return false;

    colonPosition = text.find_last_of(':', right);
    if (colonPosition != std::string::npos) {
        if (colonPosition > decimalPosition) return false;
        size_t colonCount = std::count(std::next(text.begin(), left), std::next(text.begin(), right), ':');
        if (colonCount > 3) return false;
        if (colonCount == 2) {
            if (timePartialRule == 0 || timePartialRule == 2) colonPosition = text.find_first_of(':', left);
        }
        else if (colonCount == 3) {
            if (timePartialRule == 0) colonPosition = text.find_first_of(':', left);
            else if (timePartialRule != 3) colonPosition = text.find_last_of(':', colonPosition - 1);
        }
    }

    std::u32string s = toWide32(text);
    left = s.find_first_of(U"0123456789");
    right = s.find_last_of(U"0123456789");
    if (s.find_first_not_of(inside, left) < right) return false;

    return true;
}
