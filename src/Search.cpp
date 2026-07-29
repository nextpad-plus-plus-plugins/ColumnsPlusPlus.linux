// Columns++ for Notepad++ — macOS port.
// Search.mm — Search in region (M_SEARCH). Faithful port of the Windows
// Search.cpp engine + Search.h RegexCalc glue: find / replace / replace-all /
// count / select within a rectangular or multiple-selection region marked by a
// Scintilla indicator, in Normal / Extended / Regex modes, with regex-calculate
// substitutions ((?=expression) segments evaluated by ExprTk).
//
// Adaptations from Windows:
//   * data.sci.X()  → data.X()  (the sci wrappers live on ColumnsPlusPlusData);
//   * RegularExpression(sci) → the NSRegularExpression adapter + a document reader;
//   * the modeless find/replace panel lives in Dialogs.mm (runSearchPanel); this
//     file drives it through searchData.dialog / setMessage / syncFindButton;
//   * the timer-driven progress dialog is run synchronously (correct results; no
//     cancellable progress bar for very large replace-all — documented);
//   * std::wstring/fromWide dropped — everything is UTF-8 std::string.

#include "CppData.h"
#include "RegularExpression.h"

#define exprtk_disable_rtl_io
#define exprtk_disable_rtl_io_file
#define exprtk_disable_rtl_vecops
#include "exprtk/exprtk.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <regex>
#include <string>
#include <vector>

#include "Scintilla.h"

// The modeless find/replace panel (Dialogs.mm).
void runSearchPanel(ColumnsPlusPlusData& data);
void focusSearchPanel(void* controller);

// ===========================================================================
// RegexCalc — ExprTk substitution engine for (?=expression) replacement segments
// (ported from Search.h + Search.cpp).
// ===========================================================================
class RegexCalcHistory {
public:
    std::vector<std::vector<double>> values;    // regex capture-group values, per match
    std::vector<std::vector<double>> results;   // expression results, per match
    std::vector<double> lastFiniteResult;       // last finite result of each expression
    size_t expressionIndex = 0;
};

namespace {

struct RCReg : public exprtk::ivararg_function<double> {
    RegexCalcHistory& history;
    RCReg(RegexCalcHistory& history) : history(history) {}
    double operator()(const std::vector<double>& a) {
        size_t capture  = a.size() > 0 ? (size_t)a[0] : 0;
        size_t previous = a.size() > 1 ? (size_t)a[1] : 0;
        double n = std::numeric_limits<double>::quiet_NaN();
        if (previous < history.values.size() && capture < history.values[previous].size())
            n = history.values[history.values.size() - previous - 1][capture];
        if (a.size() > 2 && !std::isfinite(n)) n = a[2];
        return n;
    }
};
struct RCSub : public exprtk::ivararg_function<double> {
    RegexCalcHistory& history;
    RCSub(RegexCalcHistory& history) : history(history) {}
    double operator()(const std::vector<double>& a) {
        size_t substitution = a.size() > 0 ? (size_t)a[0] : 0;
        size_t previous     = a.size() > 1 ? (size_t)a[1] : 0;
        double n = std::numeric_limits<double>::quiet_NaN();
        if (previous < history.results.size() && substitution <= history.results[previous].size()) {
            if (substitution == 0) substitution = history.expressionIndex + 1;
            if (a.size() < 2) n = history.lastFiniteResult[substitution - 1];
            else if (previous > 0 || substitution <= history.expressionIndex)
                n = history.results[history.results.size() - previous - 1][substitution - 1];
        }
        if (a.size() > 2 && !std::isfinite(n)) n = a[2];
        return n;
    }
};
struct RCLast : public exprtk::ivararg_function<double> {
    RegexCalcHistory& history;
    RCLast(RegexCalcHistory& history) : history(history) { exprtk::enable_zero_parameters(*this); }
    double operator()(const std::vector<double>& a) {
        if (!a.size()) return history.lastFiniteResult[history.expressionIndex];
        double n = std::numeric_limits<double>::quiet_NaN();
        size_t previous = (size_t)a[0];
        if (previous > 0 && previous < history.results.size())
            n = history.results[history.results.size() - previous - 1][history.expressionIndex];
        if (a.size() > 1 && !std::isfinite(n)) n = a[1];
        return n;
    }
};

} // namespace

class RegexCalc {
public:
    class Formula {
    public:
        exprtk::expression<double> expression;
        NumericFormat format;
    };
    RegexCalcHistory history;
    exprtk::symbol_table<double> symbol_table;
    exprtk::parser<double>       parser;
    std::vector<Formula>         formula;
    std::vector<std::string>     replacement;
    double rcMatch = 0, rcLine = 0, rcThis = 0;
    RCReg rcReg; RCSub rcSub; RCLast rcLast;
    RegexCalc() : rcReg(history), rcSub(history), rcLast(history) {
        symbol_table.add_variable("match", rcMatch);
        symbol_table.add_variable("line" , rcLine );
        symbol_table.add_variable("this" , rcThis );
        symbol_table.add_function("reg"  , rcReg  );
        symbol_table.add_function("sub"  , rcSub  );
        symbol_table.add_function("last" , rcLast );
    }
    void clear() {
        formula.clear(); replacement.clear();
        history.values.clear(); history.results.clear(); history.lastFiniteResult.clear();
        rcMatch = 0;
    }
};

SearchData::SearchData() { regexCalc = std::make_unique<RegexCalc>(); }
SearchData::~SearchData() {}

// ===========================================================================
// Engine helpers
// ===========================================================================
namespace {

void setSearchMessage(const ColumnsPlusPlusData& data, const std::string& text) {
    if (data.searchData.setMessage) data.searchData.setMessage(text);
}

void showSearchError(ColumnsPlusPlusData& data, SciPosition found) {
    if (found > -2) return;
    if (found < -2) { setSearchMessage(data, "An unidentified error occurred (SearchInTarget returned "
                                            + std::to_string((long long)found) + ")."); return; }
    setSearchMessage(data, "Invalid regular expression.");
}

// append the UTF-8 encoding of code point c
void appendUTF8(std::string& r, int c) {
    if (c < 0x80) r += (char)c;
    else if (c < 0x800) { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { r += (char)(0xE0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
    else { r += (char)(0xF0 | (c >> 18)); r += (char)(0x80 | ((c >> 12) & 0x3F)); r += (char)(0x80 | ((c >> 6) & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
}

// Notepad++ Extended-mode escapes: \0 \n \r \t \\ \b(bin) \o(oct) \d(dec) \xHH \uHHHH \UHHHHHH.
// `utf8` true when the document is UTF-8 (bytes 0x80-0xFF from \b\o\d\x are re-encoded as UTF-8).
std::string expandExtendedSearchString(const std::string& s, bool utf8) {
    auto digitOK = [](char ch, int base) -> bool {
        if (base <= 10) return ch >= '0' && ch < '0' + base;
        return std::isdigit((unsigned char)ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    };
    std::string r;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '\\') { r += s[i++]; continue; }
        if (i + 1 >= s.size()) { r += '\\'; break; }
        char e = s[i + 1];
        switch (e) {
        case '\\': r += '\\'; i += 2; break;
        case '0' : r += '\0'; i += 2; break;
        case 'n' : r += '\n'; i += 2; break;
        case 'r' : r += '\r'; i += 2; break;
        case 't' : r += '\t'; i += 2; break;
        case 'b': case 'd': case 'o': case 'x': {
            int base   = e == 'b' ? 2 : e == 'd' ? 10 : e == 'o' ? 8 : 16;
            int maxdig = e == 'b' ? 8 : e == 'd' ? 3  : e == 'o' ? 3 : 2;
            size_t j = i + 2; std::string num;
            while (j < s.size() && (int)num.size() < maxdig && digitOK(s[j], base)) num += s[j++];
            if (num.empty()) { r += '\\'; ++i; }
            else {
                int c = (int)std::stol(num, nullptr, base);
                if (utf8 && c > 0x7F) { r += (char)(c < 0xC0 ? 0xC2 : 0xC3); r += (char)(0x80 | (0x3F & c)); }
                else r += (char)c;
                i = j;
            }
            break;
        }
        case 'u': case 'U': {
            int maxdig = e == 'u' ? 4 : 6;
            size_t j = i + 2; std::string num;
            while (j < s.size() && (int)num.size() < maxdig && digitOK(s[j], 16)) num += s[j++];
            if (num.empty()) { r += '\\'; ++i; }
            else {
                int c = (int)std::stol(num, nullptr, 16);
                if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { r += '\\'; r += e; r += num; }
                else appendUTF8(r, c);
                i = j;
            }
            break;
        }
        default: r += '\\'; ++i; break;   // unknown escape → literal backslash, reprocess
        }
    }
    return r;
}

bool doesRegexUseK(const std::string& r) { return r.find("\\K") != std::string::npos; }

std::string prepareFind(ColumnsPlusPlusData& data) {
    if (data.searchData.mode != SearchData::Regex) {
        int flags = 0;
        if (data.searchData.wholeWord) flags |= SCFIND_WHOLEWORD;
        if (data.searchData.matchCase) flags |= SCFIND_MATCHCASE;
        data.SetSearchFlags(flags);
    }
    const std::string& f = data.searchData.findHistory.back();
    return data.searchData.mode == SearchData::Extended
        ? expandExtendedSearchString(f, data.CodePage() == SC_CP_UTF8) : f;
}

// For regex replace, split the replacement on (?=…) groups into alternating
// literal / expression segments (odd indices are expressions). Byte-scan is safe:
// the delimiters ' ( ) \ ? = are ASCII, never UTF-8 continuation bytes.
std::vector<std::string> prepareReplace(ColumnsPlusPlusData& data) {
    const std::string& r = data.searchData.replaceHistory.back();
    if (data.searchData.mode != SearchData::Regex)
        return { data.searchData.mode == SearchData::Extended
                 ? expandExtendedSearchString(r, data.CodePage() == SC_CP_UTF8) : r };
    std::vector<std::string> v;
    bool insideQuotes = false;
    int  depth = 0;
    v.emplace_back();
    for (size_t i = 0; i < r.length(); ++i) {
        if (insideQuotes) {
            if (r[i] == '\'') insideQuotes = false;
            v.back() += r[i];
        }
        else if (depth) {
            switch (r[i]) {
            case '\'': insideQuotes = true; break;
            case '(' : ++depth; break;
            case ')' : if (!--depth) v.emplace_back(); break;
            }
            v.back() += r[i];
        }
        else {
            v.back() += r[i];
            switch (r[i]) {
            case '(':
                if (i < r.length() - 2 && r.substr(i, 3) == "(?=") { i += 2; v.emplace_back(); depth = 1; }
                break;
            case '\\':
                if (i < r.length() - 1) v.back() += r[++i];
                break;
            }
        }
    }
    return v;
}

// Parse the optional leading format spec on an expression segment, compile each
// expression, and prime the RegexCalc. Returns false (with a message) on a bad formula.
bool prepareSubstitutions(ColumnsPlusPlusData& data, const std::vector<std::string>& sciRepl) {
    auto& rc = *data.searchData.regexCalc;
    if (sciRepl == rc.replacement) return true;
    rc.clear();
    if (sciRepl.size() == 1) return true;
    for (size_t i = 1; i < sciRepl.size(); i += 2) {
        auto& formula = rc.formula.emplace_back();
        formula.expression.register_symbol_table(rc.symbol_table);
        std::string s = sciRepl[i];
        // format spec:  [leftpad][t]  [.,][[min]-]max  :  expression
        static const std::regex rxFormat(
            R"(\s*(\d{1,2}[t]?|t)?(?:([.,])(?:((\d{1,2})?-)?(\d{1,2}))?)?\s*:(.*))");
        std::smatch m;
        if (std::regex_match(s, m, rxFormat)) {
            if (m[1].matched) {
                if (std::isdigit((unsigned char)m[1].str()[0])) formula.format.leftPad = std::stoi(m[1]);
                if (!std::isdigit((unsigned char)m[1].str().back())) formula.format.timeEnable = data.timeFormatEnable;
                formula.format.minDec = -1; formula.format.maxDec = 0;
            }
            if (m[2].matched) {
                formula.format.maxDec = 6;
                if (m[5].matched) {
                    formula.format.minDec = formula.format.maxDec = std::stoi(m[5]);
                    if (m[3].matched) formula.format.minDec = m[4].matched ? std::stoi(m[4]) : -1;
                }
            }
            s = m[6];
        }
        if (!rc.parser.compile(s, formula.expression)) {
            setSearchMessage(data, "Formula " + std::to_string((i + 1) / 2) + ": " + rc.parser.error());
            return false;
        }
    }
    rc.history.lastFiniteResult.insert(rc.history.lastFiniteResult.end(), rc.formula.size(), 0);
    rc.replacement = sciRepl;
    return true;
}

std::string calculateSubstitutions(ColumnsPlusPlusData& data, const RegularExpression& rx, SciPosition found) {
    std::string r;
    auto& rc = *data.searchData.regexCalc;
    ++rc.rcMatch;
    rc.rcLine = (double)(data.LineFromPosition(found) + 1);
    auto& values  = rc.history.values.emplace_back();
    auto& results = rc.history.results.emplace_back();
    for (int i = 0; i < (int)rx.size(); ++i) values.push_back((double)data.parseNumber(rx.str(i)));
    rc.rcThis = values[0];
    for (size_t i = 0; i < rc.replacement.size(); ++i) {
        if (i & 1) {
            rc.history.expressionIndex = (i - 1) / 2;
            auto& formula = rc.formula[(i - 1) / 2];
            double result = formula.expression.value();
            results.push_back(result);
            if (std::isfinite(result)) r += data.formatNumber(result, formula.format);
            else if (formula.expression.results().count()) {
                const auto& exResults = formula.expression.results();
                bool lastWasNumeric = false;
                for (size_t j = 0; j < exResults.count(); ++j) {
                    auto& rj = exResults[j];
                    if (rj.type == rj.e_string) {
                        exprtk::igeneric_function<double>::generic_type::string_view sv(rj);
                        r += std::string(std::string_view(sv.data_, sv.size()));
                        lastWasNumeric = false;
                    }
                    else if (rj.type == exprtk::type_store<double>::store_type::e_scalar) {
                        if (lastWasNumeric) r += ' ';
                        exprtk::igeneric_function<double>::generic_type::scalar_view sv(rj);
                        r += data.formatNumber(sv(), formula.format);
                        lastWasNumeric = true;
                    }
                }
            }
        }
        else r += rc.replacement[i];
    }
    for (size_t i = 0; i < results.size(); ++i) if (std::isfinite(results[i])) rc.history.lastFiniteResult[i] = results[i];
    return r;
}

bool convertSelectionToSearchRegion(ColumnsPlusPlusData& data) {
    int n = data.Selections();
    if (data.searchData.autoSetSelection && n < 2 && data.SelectionEmpty()) {
        data.SetIndicatorCurrent(data.searchData.indicator);
        data.SetIndicatorValue(1);
        data.IndicatorFillRange(0, data.Length());
        return true;
    }
    RectangularSelection rs(data);
    if (rs.size() || (n < 2 && rs.anchor().ln == rs.caret().ln)) {
        rs.extend();
        if (!rs.size()) return false;
        n = data.Selections();
    }
    for (int i = 0;; ++i) {
        if (i >= n) { setSearchMessage(data, "Could not construct a search region from this selection."); return false; }
        if (data.SelectionNStart(i) < data.SelectionNEnd(i)) break;
    }
    data.SetIndicatorCurrent(data.searchData.indicator);
    data.IndicatorClearRange(0, data.Length());
    data.SetIndicatorValue(1);
    for (int i = 0; i < n; ++i) data.IndicatorFillRange(data.SelectionNStart(i), data.SelectionNEnd(i) - data.SelectionNStart(i));
    if (data.searchData.autoClearSelection) data.SetEmptySelection(data.CurrentPos());
    return true;
}

void showRange(ColumnsPlusPlusData& data, SciPosition foundStart, SciPosition foundEnd) {
    SciLine foundLine = data.LineFromPosition(foundStart);
    data.SetVisiblePolicy(VISIBLE_SLOP, 3 * (int)data.LinesOnScreen() / 4);
    data.EnsureVisibleEnforcePolicy(foundLine);
    data.ScrollRange(foundStart, foundEnd);   // bring the match horizontally + vertically into view
    data.SetSel(foundStart, foundEnd);
    data.ChooseCaretX();
}

// ---------------------------------------------------------------------------
// SearchProgressInfo — count / replace over the region (run synchronously).
// ---------------------------------------------------------------------------
struct SearchProgressInfo {
    ColumnsPlusPlusData&     data;
    std::string              find;
    std::string              message;
    std::vector<std::string> replace;
    RegularExpression        rx;
    bool     selecting = false;
    bool     usesK     = false;
    SciPosition partialStart = 0, partialEnd = 0, position = 0;
    SciPosition nullAt = -1;
    intptr_t    count  = 0;
    bool (SearchProgressInfo::* task)() = nullptr;

    SearchProgressInfo(ColumnsPlusPlusData& d) : data(d) {
        rx.setReader([&d](intptr_t a, intptr_t b) { return d.StringOfRange(a, b); });
    }
    bool scCounting();
    bool rxCounting();
    bool scReplacing();
    bool rxReplacing();
    void searchMultiple(bool replace, bool partial, bool before);
};

void SearchProgressInfo::searchMultiple(bool replacing, bool partial, bool before) {
    auto& sci = data;
    ColumnsPlusPlusData::SearchRegionStatus srs = data.getSearchRegionStatus();
    if (srs != ColumnsPlusPlusData::SearchRegionReady) {
        if (!convertSelectionToSearchRegion(data)) return;
        data.searchData.wrap = srs == ColumnsPlusPlusData::SearchRegionNotReady;
    }
    sci.CallTipCancel();
    partialStart = partial && !before ? sci.SelectionEnd()   : 0;
    partialEnd   = partial &&  before ? sci.SelectionStart() : sci.Length();
    position = partialStart;
    message = replacing ? "Matches replaced" : selecting ? "Matches selected" : "Matches found";
    if (data.searchData.mode == SearchData::Regex) {
        task  = replacing ? &SearchProgressInfo::rxReplacing : &SearchProgressInfo::rxCounting;
        usesK = doesRegexUseK(data.searchData.findHistory.back());
        rx.find(data.searchData.findHistory.back(), data.searchData.matchCase);
    }
    else {
        task = replacing ? &SearchProgressInfo::scReplacing : &SearchProgressInfo::scCounting;
        find = prepareFind(data);
    }
    if (replacing) sci.BeginUndoAction();
    while ((this->*task)()) { }        // synchronous (no cancellable progress dialog)
    if (replacing) sci.EndUndoAction();
}

bool SearchProgressInfo::scCounting() {
    auto& sci = data; auto& searchData = data.searchData;
    if (!sci.IndicatorValueAt(searchData.indicator, position)) {
        position = sci.IndicatorEnd(searchData.indicator, position);
        if (position >= partialEnd) return false;
    }
    SciPosition indicatorEnd = sci.IndicatorEnd(searchData.indicator, position);
    sci.SetTargetRange(position, indicatorEnd);
    SciPosition found = sci.SearchInTarget(find);
    if (found >= 0) {
        position = sci.TargetEnd();
        if (position > partialEnd) return false;
        ++count;
        if (selecting) { if (count == 1) sci.SetSel(found, position); else sci.AddSelection(position, found); }
    }
    else if (found < -1) { showSearchError(data, found); return false; }
    else position = indicatorEnd;
    return position < partialEnd;
}

bool SearchProgressInfo::rxCounting() {
    auto& sci = data; auto& searchData = data.searchData;
    SciPosition start;
    if (sci.IndicatorValueAt(searchData.indicator, position)) start = sci.IndicatorStart(searchData.indicator, position);
    else { position = start = sci.IndicatorEnd(searchData.indicator, position); if (position >= partialEnd) return false; }
    SciPosition indicatorEnd = sci.IndicatorEnd(searchData.indicator, position);
    rx.invalidate();
    if (rx.search(position, indicatorEnd, start)) {
        SciPosition found  = rx.position(0);
        SciPosition length = rx.length();
        if (length == 0) {
            if (usesK) { if (found == nullAt) { position = found + 1; return position < partialEnd; } nullAt = position = found; }
            else position = found + 1;
        }
        else position = found + length;
        if (found + length > partialEnd) return false;
        ++count;
        if (selecting) { if (count == 1) sci.SetSel(found, found + length); else sci.AddSelection(found + length, found); }
    }
    else position = indicatorEnd;
    return position < partialEnd;
}

bool SearchProgressInfo::scReplacing() {
    auto& sci = data; auto& searchData = data.searchData;
    if (!sci.IndicatorValueAt(searchData.indicator, position)) {
        position = sci.IndicatorEnd(searchData.indicator, position);
        if (position >= partialEnd) return false;
    }
    SciPosition indicatorEnd = sci.IndicatorEnd(searchData.indicator, position);
    sci.SetTargetRange(position, indicatorEnd);
    SciPosition found = sci.SearchInTarget(find);
    if (found >= 0) {
        position = sci.TargetEnd();
        if (position > partialEnd) return false;
        ++count;
        SciPosition oldLength = position - found;
        SciPosition newLength = sci.ReplaceTarget(replace[0]);
        position += newLength - oldLength; indicatorEnd += newLength - oldLength; partialEnd += newLength - oldLength;
        sci.SetIndicatorCurrent(searchData.indicator);
        sci.SetIndicatorValue(1);
        sci.IndicatorFillRange(found, newLength);
    }
    else if (found < -1) { showSearchError(data, found); return false; }
    else position = indicatorEnd;
    return position < partialEnd;
}

bool SearchProgressInfo::rxReplacing() {
    auto& sci = data; auto& searchData = data.searchData;
    SciPosition start;
    if (sci.IndicatorValueAt(searchData.indicator, position)) start = sci.IndicatorStart(searchData.indicator, position);
    else { position = start = sci.IndicatorEnd(searchData.indicator, position); if (position >= partialEnd) return false; }
    SciPosition indicatorEnd = sci.IndicatorEnd(searchData.indicator, position);
    rx.invalidate();
    if (rx.search(position, indicatorEnd, start)) {
        SciPosition found  = rx.position(0);
        SciPosition length = rx.length();
        if (length == 0) {
            if (usesK) { if (found == nullAt) { position = found + 1; return position < partialEnd; } nullAt = position = found; }
            else position = found + 1;
        }
        else position = found + length;
        if (found + length > partialEnd) return false;
        ++count;
        std::string r = rx.format(replace.size() == 1 ? replace[0] : calculateSubstitutions(data, rx, found));
        sci.SetTargetRange(found, found + length);
        sci.ReplaceTarget(r);
        position += (SciPosition)r.length() - length; indicatorEnd += (SciPosition)r.length() - length; partialEnd += (SciPosition)r.length() - length;
        sci.SetIndicatorCurrent(searchData.indicator);
        sci.SetIndicatorValue(1);
        sci.IndicatorFillRange(found, r.length());
    }
    else position = indicatorEnd;
    return position < partialEnd;
}

} // namespace

// Mark / extend / un-mark the search region from the current selection (the
// panel's Set / Add / Remove region buttons). Ported from Search.cpp.
void updateSearchRegion(ColumnsPlusPlusData& data, bool modify, bool remove) {
    int n = data.Selections();
    data.SetIndicatorCurrent(data.searchData.indicator);
    data.SetIndicatorValue(1);
    if (n < 2 && data.SelectionEmpty() && !modify) {
        data.IndicatorFillRange(0, data.Length());
        return;
    }
    bool postClear = data.searchData.autoClearSelection
                  || ( modify
                    && data.searchData.autoSetSelection
                    && ( data.SelectionMode() != SC_SEL_STREAM || n > 1 )
                    && ( data.IndicatorValueAt(data.searchData.indicator, 0)
                      || ( data.IndicatorEnd(data.searchData.indicator, 0) != 0
                        && data.IndicatorEnd(data.searchData.indicator, 0) != data.Length() ) ) );
    if (!modify) data.IndicatorClearRange(0, data.Length());
    if (remove)
         for (int i = 0; i < n; ++i) data.IndicatorClearRange(data.SelectionNStart(i), data.SelectionNEnd(i) - data.SelectionNStart(i));
    else for (int i = 0; i < n; ++i) data.IndicatorFillRange (data.SelectionNStart(i), data.SelectionNEnd(i) - data.SelectionNStart(i));
    if (postClear) data.SetEmptySelection(data.CurrentPos());
}

// ===========================================================================
// Region status + commands (ColumnsPlusPlusData methods)
// ===========================================================================
ColumnsPlusPlusData::SearchRegionStatus ColumnsPlusPlusData::getSearchRegionStatus() {
    if (searchData.autoSetSelection) {
        if (SelectionMode() != SC_SEL_STREAM) return SearchRegionNotReady;
        if (Selections() > 1) return SearchRegionNotReady;
    }
    if (IndicatorValueAt(searchData.indicator, 0)) return SearchRegionReady;
    SciPosition ie = IndicatorEnd(searchData.indicator, 0);
    if (ie != 0 && ie != Length()) return SearchRegionReady;
    return searchData.autoSetSelection && SelectionEmpty() ? SearchRegionImpliedAll : SearchRegionNotReady;
}

static std::string matchCountMessage(intptr_t count, const char* verb) {
    return count == 0 ? std::string("No matches ") + verb + "."
         : count == 1 ? std::string("One match ")  + verb + "."
                      : std::to_string((long long)count) + " matches " + verb + ".";
}

void ColumnsPlusPlusData::searchCount(bool select, bool partial, bool before) {
    SearchProgressInfo spi(*this);
    spi.selecting = select;
    spi.searchMultiple(false, partial, before);
    if (select && spi.count) SetMainSelection(0);
    setSearchMessage(*this, matchCountMessage(spi.count, select ? "selected" : "found"));
}

void ColumnsPlusPlusData::searchFind(bool postReplace) {
    auto& sci = *this;
    bool fullSearch = searchData.wrap;
    SciPosition documentLength = sci.Length();
    SearchRegionStatus srs = getSearchRegionStatus();
    if (srs != SearchRegionReady) {
        if (!convertSelectionToSearchRegion(*this)) return;
        fullSearch = srs == SearchRegionNotReady;
    }
    searchData.wrap = false;
    if (fullSearch) searchData.nullAt = -1;
    bool backward = searchData.mode != SearchData::Regex && searchData.backward;
    sci.CallTipCancel();
    SciPosition cpFrom = fullSearch ? (backward ? documentLength : 0)
                       : backward   ? std::min(sci.Anchor(), sci.CurrentPos())
                                    : std::max(sci.Anchor(), sci.CurrentPos());
    SciPosition cpTo;
    if (searchData.mode == SearchData::Regex) {
        RegularExpression rx; rx.setReader([this](intptr_t a, intptr_t b) { return StringOfRange(a, b); });
        rx.find(searchData.findHistory.back(), searchData.matchCase);
        for (;;) {
            cpTo = sci.IndicatorEnd(searchData.indicator, cpFrom);
            if (sci.IndicatorValueAt(searchData.indicator, cpFrom)) {
                SciPosition start = sci.IndicatorStart(searchData.indicator, cpFrom);
                while (rx.search(cpFrom, cpTo, start)) {
                    SciPosition found  = rx.position();
                    SciPosition length = rx.length(0);
                    if (length == 0 && found == searchData.nullAt) {
                        if (cpFrom >= cpTo) break;
                        ++cpFrom; continue;
                    }
                    showRange(*this, found, found + length);
                    if (length == 0) { searchData.nullAt = found; sci.CallTipShow(found, "^ zero length match"); }
                    setSearchMessage(*this, !postReplace ? "" : "Match replaced; next match found.");
                    searchData.findStep = cpFrom;
                    return;
                }
            }
            if (cpTo == documentLength) break;
            cpFrom = cpTo;
            rx.invalidate();
        }
        searchData.findStep = -1;
    }
    else {
        std::string sciFind = prepareFind(*this);
        for (;;) {
            cpTo = backward ? sci.IndicatorStart(searchData.indicator, cpFrom - 1) : sci.IndicatorEnd(searchData.indicator, cpFrom);
            if (sci.IndicatorValueAt(searchData.indicator, backward ? cpTo : cpFrom)) {
                sci.SetTargetRange(cpFrom, cpTo);
                SciPosition found = sci.SearchInTarget(sciFind);
                if (found >= 0) {
                    showRange(*this, found, sci.TargetEnd());
                    setSearchMessage(*this, !postReplace ? "" : backward ? "Match replaced; previous match found."
                                                                         : "Match replaced; next match found.");
                    return;
                }
                if (found < -1) return showSearchError(*this, found);
            }
            if (backward ? cpTo == 0 : cpTo == documentLength) break;
            cpFrom = cpTo;
        }
    }
    setSearchMessage(*this, postReplace ? "Match replaced; no more matches found."
                          : fullSearch  ? "No matches found." : "No more matches found.");
    searchData.wrap = true;
}

void ColumnsPlusPlusData::searchReplace() {
    auto& sci = *this;
    std::vector<std::string> sciRepl = prepareReplace(*this);
    if (!prepareSubstitutions(*this, sciRepl)) return;
    if (getSearchRegionStatus() != SearchRegionReady) return searchFind();
    SciPosition anchor = sci.Anchor();
    SciPosition caret  = sci.CurrentPos();
    SciPosition start  = std::min(anchor, caret);
    SciPosition end    = std::max(anchor, caret);
    if (!sci.IndicatorValueAt(searchData.indicator, start)) return searchFind();
    if (replaceStaysPut && anchor == caret && searchData.findStep == -1 && searchData.nullAt == caret) return searchFind();
    sci.CallTipCancel();
    if (searchData.mode == SearchData::Regex) {
        SciPosition regionStart = sci.IndicatorStart(searchData.indicator, start);
        SciPosition regionEnd   = sci.IndicatorEnd(searchData.indicator, start);
        SciPosition cpMin       = searchData.findStep < 0 ? start : searchData.findStep;
        RegularExpression rx; rx.setReader([this](intptr_t a, intptr_t b) { return StringOfRange(a, b); });
        rx.find(searchData.findHistory.back(), searchData.matchCase);
        if (rx.search(cpMin, regionEnd, regionStart) && rx.position() == start && rx.length(0) == end - start) {
            std::string r = rx.format(sciRepl.size() == 1 ? sciRepl[0] : calculateSubstitutions(*this, rx, start));
            sci.SetTargetRange(start, end);
            sci.ReplaceTarget(r);
            sci.SetIndicatorCurrent(searchData.indicator);
            sci.SetIndicatorValue(1);
            sci.IndicatorFillRange(start, r.length());
            caret = start + (SciPosition)r.length();
            searchData.nullAt = start == end ? caret : -1;
            showRange(*this, caret, caret);
            setSearchMessage(*this, "Match replaced.");
            searchData.findStep = -1;
            searchData.wrap     = false;
            if (!replaceStaysPut) searchFind(true);
            return;
        }
        else return searchFind();
    }
    else {
        std::string sciFind = prepareFind(*this);
        sci.SetTargetRange(start, sci.IndicatorEnd(searchData.indicator, start));
        SciPosition found = sci.SearchInTarget(sciFind);
        if (found == start && sci.TargetEnd() == end) {
            std::string r = sciRepl[0];
            SciPosition replacementLength = sci.ReplaceTarget(r);
            sci.SetIndicatorCurrent(searchData.indicator);
            sci.SetIndicatorValue(1);
            sci.IndicatorFillRange(start, replacementLength);
            caret = searchData.backward ? start : start + replacementLength;
            showRange(*this, caret, caret);
            setSearchMessage(*this, "Match replaced.");
            searchData.wrap = false;
            if (!replaceStaysPut) searchFind(true);
            return;
        }
        if (found >= -1) return searchFind();
        showSearchError(*this, found);
    }
}

void ColumnsPlusPlusData::searchReplaceAll(bool partial, bool before) {
    SearchProgressInfo spi(*this);
    spi.replace = prepareReplace(*this);
    if (!prepareSubstitutions(*this, spi.replace)) return;
    spi.selecting = false;
    spi.searchMultiple(true, partial, before);
    searchData.regexCalc->clear();
    if (spi.count > 0 && settings.elasticEnabled) {
        DocumentData& dd = *getDocument();
        analyzeTabstops(dd);
        setTabstops(dd);
    }
    setSearchMessage(*this,
        spi.count == 0 ? std::string("No matches found.")
      : spi.count == 1 ? std::string("One replacement made.")
                       : std::to_string((long long)spi.count) + " replacements made.");
}

void ColumnsPlusPlusData::showSearchDialog() {
    if (searchData.dialog) { focusSearchPanel(searchData.dialog); return; }
    if (searchData.enableCustomIndicator) {
        IndicSetStyle(searchData.customIndicator, INDIC_FULLBOX);
        IndicSetFore (searchData.customIndicator, searchData.customColor);
        IndicSetAlpha(searchData.customIndicator, searchData.customAlpha);
        IndicSetUnder(searchData.customIndicator, true);
    }
    searchData.regexCalc->clear();
    runSearchPanel(*this);
}
