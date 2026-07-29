// Columns++ for Notepad++ — macOS port.
// Timestamps.mm — Timestamps command (M_TIMESTAMPS): counter ⇄ datetime.
// Faithful port of Windows Timestamps.cpp + TimestampsHelpers.cpp.
//
// Adaptations:
//   * C++20 chrono tzdb/utc_clock are absent on the CommandLineTools toolchain,
//     so time_zone / zoned_time / utc_clock / clock_cast / from_stream come from
//     the vendored Howard Hinnant date/tz library (date::), reading macOS's
//     /usr/share/zoneinfo (USE_OS_TZDB). The calendar (year/month/day/sys_days)
//     is available natively but we use date:: uniformly.
//   * Windows system_clock::duration is 100 ns; macOS libc++ is microseconds — so
//     counters use an explicit 100 ns `tick` type, NOT system_clock::duration.
//   * Windows locale API (GetLocaleInfoEx / GetDateFormatEx / CompareStringEx /
//     EnumSystemLocalesEx) → NSDateFormatter / NSLocale.
//   * std::wstring/toWide/fromWide dropped — everything is UTF-8 std::string;
//     token scanning uses UTF-32.
//   * The dialogs live in Dialogs.mm (runTimestampsDialog / runTimeFormatsDialog).

#include "CppData.h"

#include <unicode/coll.h>
#include <unicode/datefmt.h>
#include <unicode/dtfmtsym.h>
#include <unicode/gregocal.h>
#include <unicode/locid.h>
#include <unicode/smpdtfmt.h>
#include <unicode/unistr.h>
#include "RegularExpression.h"

#define USE_OS_TZDB 1
#define HAS_REMOTE_API 0
#define AUTO_DOWNLOAD 0
#include "date/date.h"
#include "date/tz.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// 100-nanosecond tick (the plugin's internal counter unit), independent of the
// platform's system_clock period.
using tick      = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
using sys_tick  = date::sys_time<tick>;
using utc_tick  = date::utc_time<tick>;

// Dialog action results (Dialogs.mm returns one of these).
enum { TS_CANCEL = 0, TS_TO_DATETIME = 1, TS_TO_COUNTER = 2 };

// ---- the macOS Timestamps types (Windows Timestamps.h, adapted) -------------
struct LocaleWords {
    std::vector<std::string> abbrMonth, fullMonth, geniMonth, dayAbbrev, dayOfWeek, ampm;
    std::string locale;
    size_t fullMax = 0, geniMax = 0, weekMax = 0;
    LocaleWords(const std::string& locale = "");
    std::string formatTimePoint(int64_t counter, bool leap, const date::time_zone* tz,
                                TimestampSettings::DateFormat format, const std::string& customPicture) const;
};

struct TimestampsCommon {
    ColumnsPlusPlusData& data;
    TimestampsCommon(ColumnsPlusPlusData& d) : data(d) {}
    const date::time_zone* zoneNamed(const std::string& name) const;
};

struct TimestampsParse {
    struct ParsedValues {
        date::year_month_day ymd{};
        int64_t ticks = 0;
        int hour = 0, minute = 0, offset = 0;
        bool offsetFound = false;
    };
    ColumnsPlusPlusData& data;
    RegularExpression    rx;
    const intptr_t       action;
    LocaleWords          localeWords;
    unsigned int         codepage = 0;
    TimestampsParse(ColumnsPlusPlusData& data, intptr_t action, const std::string& locale = "");
    bool parseDateText(std::string_view source, int64_t& counter, const date::time_zone* tz);
    bool parseGenericDateText(std::string_view source, ParsedValues& pv) const;
    bool parsePatternDateText(std::string_view source, ParsedValues& pv);
};

// dialog (Dialogs.mm)
int  runTimestampsDialog(ColumnsPlusPlusData& data,
                         const std::vector<std::string>& locales, const std::vector<std::string>& zones);
void runTimeFormatsDialog(ColumnsPlusPlusData& data);

// Lists for the dialog pickers (used by convertTimestamps + the dialog).
std::vector<std::string> availableLocaleNames();
std::vector<std::string> availableZoneNames();

namespace {

// ---- UTF helpers ------------------------------------------------------------
std::u32string toU32(std::string_view s) {
    std::u32string r;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
        else { cp = c; n = 1; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
        r += cp; i += n;
    }
    return r;
}
std::string toU8(std::u32string_view s) {
    std::string r;
    for (char32_t cp : s) {
        if (cp < 0x80) r += (char)cp;
        else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
        else { r += (char)(0xF0 | (cp >> 18)); r += (char)(0x80 | ((cp >> 12) & 0x3F)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
    }
    return r;
}
size_t cpLen(const std::string& s) { size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n; return n; }
std::string padRight(const std::string& s, size_t n) {
    size_t l = cpLen(s);
    return l >= n ? s : s + std::string(n - l, ' ');
}

// locale-aware, case- and diacritic-insensitive equality (Windows CompareStringEx).
// If firstCharOnly, compare only the first code point of each (for single-letter AM/PM).
bool ciEqual(const std::string& a, const std::string& b, bool firstCharOnly = false) {
    if (a.empty() || b.empty()) return false;
    icu::UnicodeString ua = icu::UnicodeString::fromUTF8(a);
    icu::UnicodeString ub = icu::UnicodeString::fromUTF8(b);
    if (firstCharOnly) {
        // first CODE POINT of each (macOS took the first UTF-16 unit; a code
        // point is the safer reading of the same intent for astral chars)
        if (ua.length()) ua = ua.tempSubString(0, U16_LENGTH(ua.char32At(0)));
        if (ub.length()) ub = ub.tempSubString(0, U16_LENGTH(ub.char32At(0)));
    }
    UErrorCode status = U_ZERO_ERROR;
    static std::unique_ptr<icu::Collator> coll([]() {
        UErrorCode st = U_ZERO_ERROR;
        icu::Collator* c = icu::Collator::createInstance(st);
        if (U_FAILURE(st)) return (icu::Collator*)nullptr;
        c->setStrength(icu::Collator::PRIMARY);   // case- + diacritic-insensitive
        return c;
    }());
    if (!coll) return ua.caseCompare(ub, U_FOLD_CASE_DEFAULT) == 0;
    return coll->compare(ua, ub, status) == UCOL_EQUAL && U_SUCCESS(status);
}

// ---- number helpers (Timestamps.cpp) ---------------------------------------
bool ratioToDecimal(int64_t numerator, int64_t denominator, int64_t& integer, int64_t& decimal, size_t& places) {
    if (denominator == 0) return false;
    integer = numerator / denominator;
    int64_t remainder = numerator % denominator;
    if (remainder < 0) { remainder += denominator; --integer; }
    decimal = 0; places = 0;
    while (remainder && places < 18) {
        remainder *= 10;
        decimal = decimal * 10 + remainder / denominator;
        remainder %= denominator;
        ++places;
    }
    return true;
}
bool ratioToDecimal(int64_t numerator, int64_t denominator, std::string& result, char dsep = '.') {
    int64_t integer, decimal; size_t places;
    if (!ratioToDecimal(numerator, denominator, integer, decimal, places)) return false;
    result = std::to_string(integer);
    if (integer == 0 && numerator < 0) result = "-" + result;
    if (places) {
        std::string d = std::to_string(decimal);
        if (d.size() < places) d = std::string(places - d.size(), '0') + d;
        result += dsep + d;
    }
    return true;
}
bool stringToCounter(const std::string& source, int64_t& counter, int64_t unit, char dsep) {
    try {
        std::string integer, decimal;
        bool negative = false;
        size_t pos = source.find_first_not_of(' ');
        if (pos == std::string::npos) return false;
        if (source[pos] == '-') { negative = true; ++pos; }
        else if (source[pos] == '+') ++pos;
        for (; pos < source.length(); ++pos) {
            const char ch = source[pos];
            if (ch == dsep) break;
            if (ch >= '0' && ch <= '9') { integer += ch; continue; }
            if (ch == ' ' || ch == '.' || ch == ',' || ch == '\'') continue;
            return false;
        }
        if (integer.empty()) return false;
        if (pos >= source.length()) counter = std::stoll(integer) * unit;
        else {
            ++pos;
            double power = 1;
            for (; pos < source.length(); ++pos) {
                const char ch = source[pos];
                if (ch == dsep) return false;
                if (ch >= '0' && ch <= '9') { decimal += ch; power *= 10; continue; }
                if (ch == ' ' || ch == '.' || ch == ',' || ch == '\'') continue;
                return false;
            }
            counter = std::stoll(integer) * unit
                    + (int64_t)std::llround((decimal.empty() ? 0.0 : std::stod(decimal)) * (double)unit / power);
        }
        if (negative) counter = -counter;
        return true;
    }
    catch (...) { return false; }
}

// naive local wall-clock (ymd + seconds-of-day) → locale long/short date+time via NSDateFormatter.
std::string macDateTimeFormat(int64_t counter, bool leap, bool longForm, const std::string& localeName) {
    date::year_month_day ymd; int secs;
    if (leap) {
        auto sctp = date::clock_cast<std::chrono::system_clock>(date::floor<std::chrono::seconds>(utc_tick(tick(counter))));
        auto dp = date::floor<date::days>(sctp);
        ymd = date::year_month_day(dp);
        secs = (int)std::chrono::duration_cast<std::chrono::seconds>(sctp - dp).count();
    }
    else {
        auto sctp = date::floor<std::chrono::seconds>(sys_tick(tick(counter)));
        auto dp = date::floor<date::days>(sctp);
        ymd = date::year_month_day(dp);
        secs = (int)std::chrono::duration_cast<std::chrono::seconds>(sctp - dp).count();
    }
    if (secs < 0) secs += 86400;
    // ICU: same engine NSDateFormatter wraps; identical style constants.
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::TimeZone> utc(icu::TimeZone::createTimeZone(u"GMT"));
    icu::Locale icuLoc = localeName.empty() ? icu::Locale::getDefault()
                                            : icu::Locale(localeName.c_str());
    icu::GregorianCalendar cal(*utc, icuLoc, status);
    if (U_FAILURE(status)) return "";
    cal.clear();
    cal.set((int32_t)(int)ymd.year(), (int32_t)(unsigned)ymd.month() - 1,
            (int32_t)(unsigned)ymd.day(), secs / 3600, (secs % 3600) / 60, secs % 60);
    UDate date = cal.getTime(status);
    if (U_FAILURE(status)) return "";
    std::unique_ptr<icu::DateFormat> df(icu::DateFormat::createDateTimeInstance(
        longForm ? icu::DateFormat::kLong  : icu::DateFormat::kShort,
        longForm ? icu::DateFormat::kMedium : icu::DateFormat::kShort,
        icuLoc));
    if (!df) return "";
    df->setTimeZone(*utc);
    icu::UnicodeString us;
    df->format(date, us);
    std::string out;
    us.toUTF8String(out);
    return out;
}

} // namespace

// ===========================================================================
// LocaleWords — month / day / AM-PM names via NSDateFormatter.
// ===========================================================================
LocaleWords::LocaleWords(const std::string& localeName) : locale(localeName) {
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale icuLoc = localeName.empty() ? icu::Locale::getDefault()
                                            : icu::Locale(localeName.c_str());
    icu::DateFormatSymbols sym(icuLoc, status);
    if (U_FAILURE(status)) { ampm = { "AM", "PM" }; return; }
    auto pull = [](const icu::UnicodeString* a, int32_t n, int32_t from = 0) {
        std::vector<std::string> v;
        for (int32_t k = from; k < n; ++k) {
            std::string u; a[k].toUTF8String(u); v.push_back(u);
        }
        return v;
    };
    int32_t n = 0;
    const icu::UnicodeString* p;
    p = sym.getShortMonths(n);                                    // 12  Jan..Dec
    abbrMonth = pull(p, n);
    p = sym.getMonths(n, icu::DateFormatSymbols::FORMAT,
                         icu::DateFormatSymbols::WIDE);           // 12  format (genitive)
    geniMonth = pull(p, n);
    p = sym.getMonths(n, icu::DateFormatSymbols::STANDALONE,
                         icu::DateFormatSymbols::WIDE);           // 12  standalone (nominative)
    fullMonth = pull(p, n);
    // ICU weekday arrays are 1-based (index 1 = Sunday, index 0 unused);
    // NSDateFormatter's are 0-based from Sunday — shift to match.
    p = sym.getShortWeekdays(n);
    dayAbbrev = pull(p, n, 1);                                    // 7  index 0 = Sunday
    p = sym.getWeekdays(n);
    dayOfWeek = pull(p, n, 1);                                    // 7  index 0 = Sunday
    p = sym.getAmPmStrings(n);
    if (n >= 2) {
        std::string am, pm;
        p[0].toUTF8String(am);
        p[1].toUTF8String(pm);
        ampm = { am.empty() ? "AM" : am, pm.empty() ? "PM" : pm };
    }
    else ampm = { "AM", "PM" };
    // fall back to a full 12/7/2 set if a locale returns nothing.
    if (abbrMonth.size() < 12 || fullMonth.size() < 12 || geniMonth.size() < 12
        || dayAbbrev.size() < 7 || dayOfWeek.size() < 7) { LocaleWords en; *this = en; locale = localeName; return; }
    for (const auto& s : fullMonth) fullMax = std::max(fullMax, cpLen(s));
    for (const auto& s : geniMonth) geniMax = std::max(geniMax, cpLen(s));
    for (const auto& s : dayOfWeek) weekMax = std::max(weekMax, cpLen(s));
}

std::string LocaleWords::formatTimePoint(int64_t counter, bool leap, const date::time_zone* tz,
                                         TimestampSettings::DateFormat format, const std::string& customPicture) const {
    struct MainInfo {
        std::string data;
        std::string zj2(size_t i, bool zs, bool rj) const {
            return (!zs && !rj) || data[i] != '0' ? data.substr(i, 2) : rj ? " " + data.substr(i + 1, 1) : data.substr(i + 1, 1);
        }
        std::string zj3(size_t i, bool zs, bool rj) const {
            return (!zs && !rj) || data[i] != '0' ? data.substr(i, 3)
                : rj ? (data[i + 1] != '0' ? " " + data.substr(i + 1, 2) : "  " + data.substr(i + 2, 1))
                     : (data[i + 1] != '0' ?       data.substr(i + 1, 2) :        data.substr(i + 2, 1));
        }
        std::string month     (bool z = false, bool rj = false) const { return zj2( 0, z, rj); }
        std::string dayOfMonth(bool z = false, bool rj = false) const { return zj2( 2, z, rj); }
        std::string dayOfYear (bool z = false, bool rj = false) const { return zj3( 4, z, rj); }
        int         dayOfWeek ()                                const { return data[7] - '0'; }
        std::string hour24    (bool z = false, bool rj = false) const { return zj2( 8, z, rj); }
        std::string hour12    (bool z = false, bool rj = false) const { return zj2(10, z, rj); }
        std::string minute    (bool z = false, bool rj = false) const { return zj2(12, z, rj); }
        std::string second    (bool z = false, bool rj = false) const { return zj2(14, z, rj); }
        std::string subSecond (size_t n = std::string::npos)    const { return data.substr(17, n); }
    } info;

    std::string yearInfo, zoneInfo;

    if (tz) {
        sys_tick systp;
        std::string seconds;
        if (leap) {
            auto utctp = utc_tick(tick(counter));
            systp = date::clock_cast<std::chrono::system_clock>(utctp);
            seconds = date::format("%S", utctp);
        }
        else {
            systp = sys_tick(tick(counter));
            seconds = date::format("%S", systp);
        }
        auto zt = date::make_zoned(tz, systp);
        if (format == TimestampSettings::DateFormat::localeShort)
            return macDateTimeFormat(zt.get_local_time().time_since_epoch().count(), false, false, locale);
        if (format == TimestampSettings::DateFormat::localeLong)
            return macDateTimeFormat(zt.get_local_time().time_since_epoch().count(), false, true, locale);
        info.data = date::format("%m%d%j%w%H%I%M", zt) + seconds;
        yearInfo  = date::format("%Y", zt);
        zoneInfo  = date::format("%z%Z", zt);
    }
    else if (format == TimestampSettings::DateFormat::localeShort) return macDateTimeFormat(counter, leap, false, locale);
    else if (format == TimestampSettings::DateFormat::localeLong ) return macDateTimeFormat(counter, leap, true , locale);
    else if (leap) {
        auto tp = utc_tick(tick(counter));
        info.data = date::format("%m%d%j%w%H%I%M%S", tp);
        yearInfo  = date::format("%Y", tp);
        zoneInfo  = "+0000UTC";
    }
    else {
        auto tp = sys_tick(tick(counter));
        info.data = date::format("%m%d%j%w%H%I%M%S", tp);
        yearInfo  = date::format("%Y", tp);
        zoneInfo  = "+0000UTC";
    }

    const std::string picture = format == TimestampSettings::DateFormat::custom ? customPicture : std::string("yyyy-MM-dd'T'HH:mm:ss.sssZ");

    std::string s;
    for (size_t i = 0; i < picture.length();) {
        switch (picture[i]) {
        case 'y': { size_t j = std::min(picture.find_first_not_of('y', i), picture.length());
            s += j - i < 3 ? yearInfo.substr(yearInfo.length() - 2) : yearInfo; i = j; break; }
        case 'M': { size_t j = std::min(picture.find_first_not_of('M', i), picture.length());
            s += j - i <  3 ? info.month(j - i == 1)
               : j - i == 3 ? abbrMonth[std::stoi(info.month()) - 1]
               : j - i == 4 ? geniMonth[std::stoi(info.month()) - 1]
                            : padRight(geniMonth[std::stoi(info.month()) - 1], geniMax);
            i = j; break; }
        case 'N': { size_t j = std::min(picture.find_first_not_of('N', i), picture.length());
            s += j - i < 5 ? fullMonth[std::stoi(info.month()) - 1] : padRight(fullMonth[std::stoi(info.month()) - 1], fullMax);
            i = j; break; }
        case 'd': { size_t j = std::min(picture.find_first_not_of('d', i), picture.length());
            s += j - i <  3 ? info.dayOfMonth(j - i == 1)
               : j - i == 3 ? dayAbbrev[info.dayOfWeek()]
               : j - i == 4 ? dayOfWeek[info.dayOfWeek()]
                            : padRight(dayOfWeek[info.dayOfWeek()], weekMax);
            i = j; break; }
        case 'D': { size_t j = std::min(picture.find_first_not_of('D', i), picture.length()); s += info.dayOfYear(j - i == 1); i = j; break; }
        case 'H': { size_t j = std::min(picture.find_first_not_of('H', i), picture.length()); s += info.hour24(j - i == 1); i = j; break; }
        case 'h': { size_t j = std::min(picture.find_first_not_of('h', i), picture.length()); s += info.hour12(j - i == 1); i = j; break; }
        case 'm': { size_t j = std::min(picture.find_first_not_of('m', i), picture.length()); s += info.minute(j - i == 1); i = j; break; }
        case 's': { size_t j = std::min(picture.find_first_not_of('s', i), picture.length());
            s += info.second(j - i == 1); i = j;
            if (picture.substr(i, 2) == ".s" || picture.substr(i, 2) == ",s") {
                s += picture[i]; j = std::min(picture.find_first_not_of('s', i + 1), picture.length());
                s += info.subSecond(j - i - 1); i = j;
            }
            break; }
        case 'T': { size_t j = std::min(picture.find_first_not_of('T', i), picture.length());
            size_t k = info.hour24() < "12" ? 0 : 1;
            if (j - i == 1) s += ampm[k].substr(0, 1); else s += ampm[k]; i = j; break; }
        case 't': { size_t j = std::min(picture.find_first_not_of('t', i), picture.length());
            size_t k = info.hour24() < "12" ? 0 : 1;
            {
                icu::UnicodeString a = icu::UnicodeString::fromUTF8(ampm[k]);
                if (j - i == 1 && a.length()) a = a.tempSubString(0, U16_LENGTH(a.char32At(0)));
                a.toLower();
                std::string c; a.toUTF8String(c); s += c;
            }
            i = j; break; }
        case '\'':
            for (;;) {
                size_t j = std::min(picture.find_first_of('\'', i + 1), picture.length());
                s += picture.substr(i + 1, j - i - 1); i = j + 1;
                if (i >= picture.length() || picture[i] != '\'') break;
                s += '\'';
            }
            break;
        case 'Z': case 'z': {
            char pc = picture[i];
            size_t j = std::min(picture.find_first_not_of(pc, i), picture.length());
            if (pc == 'Z' && j - i == 3)                s += zoneInfo.substr(5);
            else if (j - i == 1)                        s += zoneInfo.substr(0, 3) + ":" + zoneInfo.substr(3, 2);
            else if (j - i != 2)                        s += (pc == 'Z' && zoneInfo.substr(1, 4) == "0000") ? std::string("Z") : zoneInfo.substr(0, 5);
            else if (zoneInfo.substr(3, 2) == "00")     s += zoneInfo.substr(0, 3);
            else                                        s += zoneInfo.substr(0, 3) + ":" + zoneInfo.substr(3, 2);
            i = j; break; }
        case 'x':
            if (i + 1 >= picture.length()) { s += 'x'; ++i; break; }
            switch (picture[i + 1]) {
            case 'M': s += info.month(false, true); i += 2; break;
            case 'd': s += info.dayOfMonth(false, true); i += 2; break;
            case 'D': s += info.dayOfYear(false, true); i += 2; break;
            case 'H': s += info.hour24(false, true); i += 2; break;
            case 'h': s += info.hour12(false, true); i += 2; break;
            case 'm': s += info.minute(false, true); i += 2; break;
            case 's': { s += info.second(false, true); i += 2;
                if (picture.substr(i, 2) == ".s" || picture.substr(i, 2) == ",s") {
                    s += picture[i]; size_t j = std::min(picture.find_first_not_of('s', i + 1), picture.length());
                    s += info.subSecond(j - i - 1); i = j;
                }
                break; }
            case 'x': s += picture[i + 1]; i += 2; break;
            default: s += 'x'; ++i;
            }
            break;
        default: s += picture[i]; ++i;
        }
    }
    return s;
}

// ===========================================================================
// TimestampsCommon / lists
// ===========================================================================
const date::time_zone* TimestampsCommon::zoneNamed(const std::string& name) const {
    if (name.empty()) return nullptr;
    try { return date::locate_zone(name); } catch (...) { return nullptr; }
}

std::vector<std::string> availableZoneNames() {
    std::vector<std::string> v;
    try { for (const auto& z : date::get_tzdb().zones) v.push_back(z.name()); }
    catch (...) {}
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::string> availableLocaleNames() {
    std::vector<std::string> v;
    int32_t count = 0;
    const icu::Locale* locs = icu::Locale::getAvailableLocales(count);
    for (int32_t i = 0; i < count; ++i) {
        std::string s = locs[i].getName();
        icu::UnicodeString ud;
        locs[i].getDisplayName(icu::Locale::getDefault(), ud);
        std::string disp;
        ud.toUTF8String(disp);
        if (!disp.empty()) s += "  -  " + disp;
        if (!s.empty()) v.push_back(s);
    }
    std::sort(v.begin(), v.end());
    return v;
}

// ===========================================================================
// TimestampsParse
// ===========================================================================
TimestampsParse::TimestampsParse(ColumnsPlusPlusData& data, intptr_t action, const std::string& locale)
    : data(data), action(action), localeWords(locale) {
    codepage = (unsigned)data.CodePage();
    if (data.timestamps.datePriority == TimestampSettings::DatePriority::custom
        && !data.timestamps.dateParse.empty() && !data.timestamps.dateParse.back().empty())
        rx.find(data.timestamps.dateParse.back(), false);
}

bool TimestampsParse::parseDateText(std::string_view source, int64_t& counter, const date::time_zone* tz) {
    if (!data.timestamps.enableFromDatetime) return false;
    ParsedValues pv;
    if ( !(data.timestamps.datePriority == TimestampSettings::DatePriority::custom ? parsePatternDateText(source, pv)
                                                                                   : parseGenericDateText(source, pv)) ) return false;
    utc_tick tp;
    if (tz && !pv.offsetFound) {
        date::local_seconds lt = date::local_days(pv.ymd) + std::chrono::hours(pv.hour) + std::chrono::minutes(pv.minute);
        tp = date::clock_cast<date::utc_clock>(date::make_zoned(tz, lt, date::choose::earliest).get_sys_time());
    }
    else {
        tp = date::clock_cast<date::utc_clock>(date::sys_days(pv.ymd))
           + std::chrono::hours(pv.hour) + std::chrono::minutes(pv.minute - pv.offset);
    }
    counter = tp.time_since_epoch().count() + pv.ticks;
    return true;
}

namespace {
bool isParseDivider(char32_t c) {
    static const std::u32string nonAsciiDividers = U"‒–−﹣－              ";
    if (c < 128) return !(std::isalnum((int)c));
    return nonAsciiDividers.find(c) != std::u32string::npos;
}
}

bool TimestampsParse::parseGenericDateText(std::string_view source, ParsedValues& pv) const {
    const TimestampSettings& ts = data.timestamps;
    const std::u32string s = toU32(source);

    struct Token {
        std::u32string_view sv;
        enum TokenType { unknown, number, divider, ampm, month } tokenType = unknown;
        int tokenValue = -1;
        char32_t at(size_t i) const { return sv[i]; }
        char32_t back() const { return sv.empty() ? 0 : sv.back(); }
        size_t length() const { return sv.size(); }
        bool operator==(const char* a) const { std::u32string u = toU32(a); return sv == std::u32string_view(u); }
        int iValue() const {
            if (tokenType == ampm || tokenType == month) return tokenValue;
            if (tokenType != number || length() > 9) return -1;
            int v = 0; for (size_t i = 0;;) { v += (int)(at(i) - U'0'); if (++i >= length()) break; v *= 10; } return v;
        }
    };

    std::vector<Token>  tokens;
    std::vector<size_t> numberTokens, dividerTokens, alphaTokens;
    size_t ampmToken = std::u32string::npos, monthToken = std::u32string::npos;

    auto u8 = [](std::u32string_view v) { return toU8(v); };

    for (size_t i = 0; i < s.length();) {
        if (s[i] >= U'0' && s[i] <= U'9') {
            size_t j = i + 1; while (j < s.length() && s[j] >= U'0' && s[j] <= U'9') ++j;
            numberTokens.push_back(tokens.size());
            tokens.push_back({ std::u32string_view(s).substr(i, j - i), Token::number }); i = j;
        }
        else if (isParseDivider(s[i])) {
            size_t j = i + 1; while (j < s.length() && isParseDivider(s[j])) ++j;
            dividerTokens.push_back(tokens.size());
            tokens.push_back({ std::u32string_view(s).substr(i, j - i), Token::divider }); i = j;
        }
        else {
            size_t j = i + 1; while (j < s.length() && !(s[j] >= U'0' && s[j] <= U'9') && !isParseDivider(s[j])) ++j;
            alphaTokens.push_back(tokens.size());
            tokens.push_back({ std::u32string_view(s).substr(i, j - i) });
            Token& t = tokens.back();
            std::string ts8 = u8(t.sv);
            if (t == "T" || t == "t" || t == "Z" || t == "z") { /* reserved for ISO 8601 */ }
            else if (ciEqual(ts8, localeWords.ampm[0], t.length() == 1)) {
                if (ampmToken != std::u32string::npos) return false;
                t.tokenType = Token::ampm; t.tokenValue = 0; ampmToken = tokens.size() - 1;
            }
            else if (ciEqual(ts8, localeWords.ampm[1], t.length() == 1)) {
                if (ampmToken != std::u32string::npos) return false;
                t.tokenType = Token::ampm; t.tokenValue = 12; ampmToken = tokens.size() - 1;
            }
            else for (int k = 0; k < 12; ++k) {
                if (ciEqual(ts8, localeWords.abbrMonth[k]) || ciEqual(ts8, localeWords.fullMonth[k]) || ciEqual(ts8, localeWords.geniMonth[k])) {
                    if (monthToken != std::u32string::npos) return false;
                    t.tokenType = Token::month; t.tokenValue = k + 1; monthToken = tokens.size() - 1; break;
                }
            }
            i = j;
        }
    }

    if (numberTokens.empty()) return false;
    const Token& num1 = tokens[numberTokens[0]];
    if (num1.length() == 6 || num1.length() > 8) return false;

    size_t timeToken, timeNumber;

    if (num1.length() > 6) {
        if (monthToken != std::u32string::npos) return false;
        int y = (int)(num1.at(0) - U'0') * 1000 + (int)(num1.at(1) - U'0') * 100 + (int)(num1.at(2) - U'0') * 10 + (int)(num1.at(3) - U'0');
        if (num1.length() == 8) {
            int m = (int)(num1.at(4) - U'0') * 10 + (int)(num1.at(5) - U'0');
            int d = (int)(num1.at(6) - U'0') * 10 + (int)(num1.at(7) - U'0');
            pv.ymd = date::year(y) / m / d;
            if (!pv.ymd.ok()) return false;
        }
        else {
            int d = (int)(num1.at(4) - U'0') * 100 + (int)(num1.at(5) - U'0') * 10 + (int)(num1.at(6) - U'0');
            if (d > (date::year(y).is_leap() ? 366 : 365)) return false;
            pv.ymd = date::year_month_day(date::sys_days(date::year(y) / 1 / 1) + date::days(d - 1));
        }
        timeNumber = 1; timeToken = numberTokens[0] + 1;
    }
    else if (monthToken != std::u32string::npos) {
        if (numberTokens.size() < 2) return false;
        if (numberTokens.size() > 2 && numberTokens[2] < monthToken) return false;
        const Token& num2 = tokens[numberTokens[1]];
        if (num1.length() > 2 || (num2.length() < 3 && ts.datePriority == TimestampSettings::DatePriority::ymd)) {
            if (num2.length() > 2) return false;
            pv.ymd = date::year(num1.iValue()) / tokens[monthToken].tokenValue / num2.iValue();
        }
        else pv.ymd = date::year(num2.iValue()) / tokens[monthToken].tokenValue / num1.iValue();
        if (!pv.ymd.ok()) return false;
        timeNumber = 2; timeToken = std::max(numberTokens[1], monthToken) + 1;
    }
    else if (numberTokens.size() > 1 && num1.length() == 4 && tokens.size() > 2 && tokens[2].length() == 3) {
        pv.ymd = date::year_month_day(date::sys_days(date::year(num1.iValue()) / 1 / 1) + date::days(tokens[2].iValue() - 1));
        timeNumber = 2; timeToken = numberTokens[1] + 1;
    }
    else {
        if (numberTokens.size() < 3) return false;
        const Token& num2 = tokens[numberTokens[1]];
        const Token& num3 = tokens[numberTokens[2]];
        const int val1 = num1.iValue(), val2 = num2.iValue(), val3 = num3.iValue();
        int year = 0, yearPosition = 0;
        if (num1.length() > 2) { yearPosition = 1; year = val1; }
        if (num2.length() > 2) { if (yearPosition != 0) return false; yearPosition = 2; year = val2; }
        if (num3.length() > 2) { if (yearPosition != 0) return false; yearPosition = 3; year = val3; }
        if (yearPosition == 0) {
            if (ts.datePriority == TimestampSettings::DatePriority::ymd) { yearPosition = 1; year = val1; }
            else                                                         { yearPosition = 3; year = val3; }
            year += ts.twoDigitYearLimit - (ts.twoDigitYearLimit % 100) - (year > ts.twoDigitYearLimit % 100 ? 100 : 0);
        }
        if (yearPosition == 1) pv.ymd = date::year(year) / val2 / val3;
        else if (yearPosition == 2)
            pv.ymd = ts.datePriority == TimestampSettings::DatePriority::dmy ? date::year(year) / val3 / val1 : date::year(year) / val1 / val3;
        else
            pv.ymd = ts.datePriority == TimestampSettings::DatePriority::dmy ? date::year(year) / val2 / val1 : date::year(year) / val1 / val2;
        if (!pv.ymd.ok()) return false;
        timeNumber = 3; timeToken = numberTokens[2] + 1;
    }
    (void)timeToken;

    if (timeNumber >= numberTokens.size()) return ampmToken == std::u32string::npos;

    if (tokens[numberTokens[timeNumber]].length() > 2) {
        const Token& time = tokens[numberTokens[timeNumber]];
        switch (time.length()) {
        case 3: pv.hour = (int)(time.at(0) - U'0'); pv.minute = (int)(time.at(1) - U'0') * 10 + (int)(time.at(2) - U'0'); break;
        case 4: pv.hour = (int)(time.at(0) - U'0') * 10 + (int)(time.at(1) - U'0'); pv.minute = (int)(time.at(2) - U'0') * 10 + (int)(time.at(3) - U'0'); break;
        case 5: pv.hour = (int)(time.at(0) - U'0'); pv.minute = (int)(time.at(1) - U'0') * 10 + (int)(time.at(2) - U'0');
                pv.ticks = (int64_t)(time.at(3) - U'0') * 100000000LL + (int64_t)(time.at(4) - U'0') * 10000000LL; break;
        case 6: pv.hour = (int)(time.at(0) - U'0') * 10 + (int)(time.at(1) - U'0'); pv.minute = (int)(time.at(2) - U'0') * 10 + (int)(time.at(3) - U'0');
                pv.ticks = (int64_t)(time.at(4) - U'0') * 100000000LL + (int64_t)(time.at(5) - U'0') * 10000000LL; break;
        default: return false;
        }
        if (pv.hour > 23 || pv.minute > 59 || pv.ticks > 600000000LL) return false;
    }
    else {
        pv.hour = tokens[numberTokens[timeNumber]].iValue();
        if (ampmToken != std::u32string::npos) {
            if (ampmToken < timeToken) return false;
            if (pv.hour < 1 || pv.hour > 12) return false;
            pv.hour = (pv.hour % 12) + tokens[ampmToken].tokenValue;
        }
        else if (pv.hour < 0 || pv.hour > 23) return false;
    }

    pv.offsetFound = tokens.back() == "Z" || tokens.back() == "z";
    if (timeNumber + 1 == numberTokens.size()) return true;

    size_t zoneNumber = numberTokens.size();
    if (!pv.offsetFound && numberTokens.back() >= 3) {
        const Token& tz1 = tokens[numberTokens.back()];
        const Token& tz2 = tokens[numberTokens.back() - 1];
        const Token& tz3 = tokens[numberTokens.back() - 2];
        const Token& tz4 = tokens[numberTokens.back() - 3];
        if (tz1.length() == 4 && (tz2.back() == U'+' || tz2.back() == U'-' || tz2.back() == U'−')) {
            zoneNumber = numberTokens.size() - 1;
            pv.offset = (int)(tz1.at(0) - U'0') * 600 + (int)(tz1.at(1) - U'0') * 60 + (int)(tz1.at(2) - U'0') * 10 + (int)(tz1.at(3) - U'0');
            if (tz2.back() != U'+') pv.offset = -pv.offset; pv.offsetFound = true;
        }
        else if (tz1.length() < 3 && (tz2.back() == U'+' || tz2.back() == U'-' || tz2.back() == U'−')) {
            if ( tokens[numberTokens[timeNumber]].length() > 2 || timeNumber + 3 < numberTokens.size() || tz2.length() > 1
              || ( (tokens[numberTokens[timeNumber] - 1] == "T" || tokens[numberTokens[timeNumber] - 1] == "t")
                && ( timeNumber == numberTokens.size() - 2
                  || tokens[numberTokens[numberTokens.size() - 2] - 1] == ":"
                  || tokens[numberTokens[numberTokens.size() - 2] - 1] == ".") ) ) {
                zoneNumber = numberTokens.size() - 1;
                pv.offset = tz1.iValue() * 60;
                if (tz2.back() != U'+') pv.offset = -pv.offset; pv.offsetFound = true;
            }
        }
        else if (tz3.tokenType == Token::number && tz1.length() == 2 && tz3.length() < 3 && tz2 == ":"
              && (tz4.back() == U'+' || tz4.back() == U'-' || tz4.back() == U'−')) {
            zoneNumber = numberTokens.size() - 2;
            pv.offset = tz3.iValue() * 60 + tz1.iValue();
            if (tz4.back() != U'+') pv.offset = -pv.offset; pv.offsetFound = true;
        }
    }

    if (timeNumber + 1 == zoneNumber) return true;

    auto decimalOf = [&](size_t idx) { int64_t v = 0; const auto& d = tokens[idx].sv; for (size_t i = 0;;) { if (i < d.size()) v += (int64_t)(d[i] - U'0'); if (++i >= 9) break; v *= 10; } return v; };

    if (tokens[numberTokens[timeNumber]].length() > 2) {
        if ( timeNumber + 2 != zoneNumber || numberTokens[timeNumber + 1] != numberTokens[timeNumber] + 2
          || (!(tokens[numberTokens[timeNumber] + 1] == ".") && !(tokens[numberTokens[timeNumber] + 1] == ",")) ) return false;
        int64_t v = decimalOf(numberTokens[timeNumber + 1]);
        if (tokens[numberTokens[timeNumber]].length() > 4) pv.ticks += (v + 50) / 100;
        else pv.ticks = (v * 6 + 5) / 10;
        return true;
    }

    if ( timeNumber + 2 == zoneNumber && numberTokens[timeNumber + 1] == numberTokens[timeNumber] + 2
      && (tokens[numberTokens[timeNumber] + 1] == "." || tokens[numberTokens[timeNumber] + 1] == ",") ) {
        int64_t v = decimalOf(numberTokens[timeNumber + 1]) * 36;
        pv.minute = (int)(v / 600000000); pv.ticks = v % 600000000; return true;
    }

    pv.minute = tokens[numberTokens[timeNumber + 1]].iValue();
    if (pv.minute < 0 || pv.minute > 59) return false;
    if (timeNumber + 2 == zoneNumber) return true;

    if ( timeNumber + 3 == zoneNumber && numberTokens[timeNumber + 2] == numberTokens[timeNumber + 1] + 2
      && (tokens[numberTokens[timeNumber + 1] + 1] == "." || tokens[numberTokens[timeNumber + 1] + 1] == ",")
      && ( numberTokens[timeNumber + 1] != numberTokens[timeNumber] + 2
        || !(tokens[numberTokens[timeNumber] + 1].sv == tokens[numberTokens[timeNumber + 1] + 1].sv) ) ) {
        pv.ticks = (decimalOf(numberTokens[timeNumber + 2]) * 6 + 5) / 10; return true;
    }

    pv.ticks = tokens[numberTokens[timeNumber + 2]].iValue();
    if (pv.ticks < 0 || pv.ticks > 60) return false;
    pv.ticks *= 10000000;
    if (timeNumber + 3 == zoneNumber) return true;

    if ( timeNumber + 4 == zoneNumber && numberTokens[timeNumber + 3] == numberTokens[timeNumber + 2] + 2
      && (tokens[numberTokens[timeNumber + 2] + 1] == "." || tokens[numberTokens[timeNumber + 2] + 1] == ",") ) {
        pv.ticks += (decimalOf(numberTokens[timeNumber + 3]) + 50) / 100; return true;
    }
    return false;
}

bool TimestampsParse::parsePatternDateText(std::string_view source, ParsedValues& pv) {
    if (data.timestamps.dateParse.empty()) return false;
    if (!rx.can_search()) return false;
    if (!rx.search(source)) return false;
    std::string year = rx.str("y"), doy = rx.str("D"), month = rx.str("M"), dom = rx.str("d");
    std::string hour = rx.str("H"), minute = rx.str("m"), seconds = rx.str("s"), ampm = rx.str("t"), offset = rx.str("Z");
    if (year.empty()) return false;
    if (doy.empty() && (month.empty() || dom.empty())) return false;
    if (!doy.empty() && !(month.empty() && dom.empty())) return false;
    try {
        int nYear = std::stoi(year);
        if (year.length() == 2) nYear += data.timestamps.twoDigitYearLimit - (data.timestamps.twoDigitYearLimit % 100)
                                       - (nYear > data.timestamps.twoDigitYearLimit % 100 ? 100 : 0);
        if (doy.empty()) {
            int nMonth = 0;
            if (month.find_first_not_of("0123456789 ") == std::string::npos) nMonth = std::stoi(month);
            else while (++nMonth < 13) {
                if (ciEqual(month, localeWords.abbrMonth[nMonth - 1]) || ciEqual(month, localeWords.fullMonth[nMonth - 1]) || ciEqual(month, localeWords.geniMonth[nMonth - 1])) break;
            }
            if (nMonth < 1 || nMonth > 12) return false;
            int nDom = std::stoi(dom);
            if (nDom < 1 || nDom > 31) return false;
            pv.ymd = date::year(nYear) / nMonth / nDom;
            if (!pv.ymd.ok()) return false;
        }
        else pv.ymd = date::year_month_day(date::sys_days(date::year(nYear) / 1 / 1) + date::days(std::stoi(doy) - 1));

        if (!offset.empty()) {
            if (offset == "Z" || offset == "z") pv.offset = 0;
            else {
                bool west = false;
                std::u32string s = toU32(offset);
                if (s[0] == U'-' || s[0] == U'−') west = true; else if (s[0] != U'+') return false;
                std::string digits = toU8(std::u32string_view(s).substr(1));
                switch (s.length()) {
                case 2: pv.offset = 60 * std::stoi(digits); break;
                case 3: pv.offset = 60 * std::stoi(digits); break;
                case 5: if (s[2] == U':') pv.offset = 60 * (int)(s[1] - U'0') + std::stoi(toU8(std::u32string_view(s).substr(3, 2)));
                        else pv.offset = 60 * std::stoi(toU8(std::u32string_view(s).substr(1, 2))) + std::stoi(toU8(std::u32string_view(s).substr(3, 2))); break;
                case 6: pv.offset = 60 * std::stoi(toU8(std::u32string_view(s).substr(1, 2))) + std::stoi(toU8(std::u32string_view(s).substr(4, 2))); break;
                default: return false;
                }
                if (west) pv.offset = -pv.offset;
            }
            pv.offsetFound = true;
        }
        pv.hour = hour.empty() ? 0 : std::stoi(hour);
        if (!ampm.empty()) {
            pv.hour %= 12;
            if (ciEqual(ampm, localeWords.ampm[1], ampm.length() == 1)) pv.hour += 12;
            else if (!ciEqual(ampm, localeWords.ampm[0], ampm.length() == 1)) return false;
        }
        pv.minute = minute.empty() ? 0 : std::stoi(minute);
        if (seconds.empty()) pv.ticks = 0;
        else {
            size_t p = seconds.find_first_not_of("0123456789 ");
            if (p == std::string::npos || (seconds[p] != '.' && seconds[p] != ',')) pv.ticks = 10000000LL * std::stoi(seconds);
            else {
                pv.ticks = 10000000LL * std::stoi(seconds.substr(0, p));
                try { pv.ticks += std::stoi((seconds.substr(p + 1) + "0000000").substr(0, 7)); } catch (...) {}
            }
        }
        return true;
    }
    catch (...) { return false; }
}

// ===========================================================================
// convertTimestamps command
// ===========================================================================
void ColumnsPlusPlusData::convertTimestamps() {
    auto rs = getRectangularSelection();
    if (!rs.size()) return;

    TimestampsCommon tsc(*this);
    int action = runTimestampsDialog(*this, availableLocaleNames(), availableZoneNames());
    if (!action) return;

    const date::time_zone* tzFrom = timestamps.enableTzAndLocale ? tsc.zoneNamed(timestamps.fromZone) : nullptr;
    const date::time_zone* tzTo   = timestamps.enableTzAndLocale ? tsc.zoneNamed(timestamps.toZone)   : nullptr;

    TimestampSettings::CounterSpec fromCounter = timestamps.fromCounter.spec();
    TimestampSettings::CounterSpec toCounter   = timestamps.toCounter.spec();

    // Non-leap counters live on system_clock; convert the epoch through utc→sys in
    // the 100 ns tick unit (NOT system_clock::duration, which is µs on macOS).
    int64_t fromEpoch = !timestamps.enableFromCounter || fromCounter.leap ? fromCounter.epoch
        : sys_tick(date::clock_cast<std::chrono::system_clock>(utc_tick(tick(fromCounter.epoch)))).time_since_epoch().count();

    int64_t toEpoch = action == TS_TO_DATETIME || toCounter.leap ? toCounter.epoch
        : sys_tick(date::clock_cast<std::chrono::system_clock>(utc_tick(tick(toCounter.epoch)))).time_since_epoch().count();

    TimestampsParse pi(*this, action, timestamps.enableTzAndLocale ? timestamps.localeName : "");

    struct Replacement { std::string text; size_t left = 0; bool isLastInRow = false; bool isEndOfLine = false; };
    std::vector<std::vector<Replacement>> replacements;
    bool needsSpace = false;

    rs.natural();
    for (auto row : rs) {
        auto& replaceLine = replacements.emplace_back();
        for (const auto& cell : row) {
            if (!cell.textLength() && cell.isLastInRow()) continue;
            auto& replaceCell = replaceLine.emplace_back();
            replaceCell.isLastInRow = cell.isLastInRow();
            replaceCell.isEndOfLine = cell.isEndOfLine();
            if (timestamps.overwrite) {
                const std::string text(cell.text());
                const size_t n = text.find_last_not_of(' ');
                if (n == std::string::npos) continue;
                replaceCell.text = text.substr(0, n + 1);
            }
            else if (cell.isLastInRow() && !std::string(cell.text()).empty() && std::string(cell.text()).back() != ' ') needsSpace = true;

            const std::string source = cell.trim();
            int64_t counter = 0;
            bool sourceIsCounter = false;
            if (timestamps.enableFromCounter && stringToCounter(source, counter, fromCounter.unit, settings.decimalSeparatorIsComma ? ',' : '.')) {
                counter += fromEpoch; sourceIsCounter = true;
                if (timestamps.fromCounter.type == TimestampSettings::CounterType::Ex00 && counter < -22039776000000000) counter += fromCounter.unit;
            }
            else if (!pi.parseDateText(source, counter, tzFrom)) continue;

            if (action == TS_TO_DATETIME) {
                std::string s = pi.localeWords.formatTimePoint(counter, !sourceIsCounter || fromCounter.leap, tzTo, timestamps.dateFormat,
                                                               timestamps.datePicture.empty() ? std::string() : timestamps.datePicture.back());
                replaceCell.text = s;
            }
            else {
                if (sourceIsCounter ? fromCounter.leap != toCounter.leap : !toCounter.leap) {
                    counter = toCounter.leap
                        ? utc_tick(date::clock_cast<date::utc_clock>(sys_tick(tick(counter)))).time_since_epoch().count()
                        : sys_tick(date::clock_cast<std::chrono::system_clock>(utc_tick(tick(counter)))).time_since_epoch().count();
                }
                if (timestamps.toCounter.type == TimestampSettings::CounterType::Ex00 && counter < -22038912000000000) counter -= toCounter.unit;
                std::string s;
                if (!ratioToDecimal(counter - toEpoch, toCounter.unit, s, settings.decimalSeparatorIsComma ? ',' : '.')) continue;
                replaceCell.text = s;
                size_t n = s.find_first_of(settings.decimalSeparatorIsComma ? ',' : '.');
                replaceCell.left = n == std::string::npos ? s.length() : n;
            }
        }
    }

    struct ColumnWidth { size_t left = 0, right = 0, total = 0; };
    std::vector<ColumnWidth> maxWidth;
    for (const auto& replaceRow : replacements)
        for (size_t i = 0; i < replaceRow.size(); ++i) {
            if (maxWidth.size() <= i) maxWidth.resize(i + 1);
            const auto& repl = replaceRow[i]; auto& cm = maxWidth[i];
            if (repl.left) {
                if (cm.left < repl.left) cm.left = repl.left;
                if (cm.right < repl.text.length() - repl.left) cm.right = repl.text.length() - repl.left;
            }
            if (cm.total < repl.text.length()) cm.total = repl.text.length();
        }
    for (auto& cm : maxWidth) if (cm.total < cm.left + cm.right) cm.total = cm.left + cm.right;

    BeginUndoAction();
    for (auto row : rs) {
        std::string r;
        if (needsSpace) r = " ";
        const auto& replacementRow = replacements[row.index];
        for (size_t column = 0; column < replacementRow.size(); ++column) {
            const auto& repl = replacementRow[column]; const auto& cm = maxWidth[column];
            std::string s = (repl.left && repl.left < cm.left) ? std::string(cm.left - repl.left, ' ') : "";
            s += repl.text;
            if (!repl.isEndOfLine && (!settings.elasticEnabled || repl.isLastInRow) && s.length() < cm.total) s.resize(cm.total, ' ');
            r += s;
            if (!repl.isLastInRow) r += '\t';
        }
        if (!timestamps.overwrite) {
            if (row.isEndOfLine() && r.empty()) continue;
            if (row.vsMax()) {
                SetTargetRange(row.endOfLine(), row.endOfLine());
                SetTargetStartVirtualSpace(row.vsMax());
                SetTargetEndVirtualSpace(row.vsMax());
            }
            else SetTargetRange(row.cpMax(), row.cpMax());
            ReplaceTarget(r);
        }
        else if (r != std::string(row.text())) row.replace(r);
    }
    rs.refit();
    EndUndoAction();
}

void ColumnsPlusPlusData::showTimeFormatsDialog() { runTimeFormatsDialog(*this); }
