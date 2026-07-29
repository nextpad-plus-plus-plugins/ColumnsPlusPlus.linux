// Columns++ for Notepad++ — macOS port.
// Calculate.mm — the Calculate command (M_CALC). Ported faithfully from the
// Windows Numeric.cpp calculate() + Numeric.h CalculateHistory/ExCol/ExReg/
// ExTab/ExLast. Expressions are evaluated with ExprTk; per-row column / regex /
// tab operands are read from the selection via CalculateHistory. The input
// dialog is runCalculateDialog() (Dialogs.mm); this file owns the engine.
//
// Windows `data.sci.X()` → macOS `data.X()`; `RegularExpression rx(sci)` → the
// NSRegularExpression-backed adapter (RegularExpression.h); parseNumber() yields
// a NumericParse convertible to double.

#include "CppData.h"
#include "RegularExpression.h"

#define exprtk_disable_rtl_io
#define exprtk_disable_rtl_io_file
#define exprtk_disable_rtl_vecops
#include "exprtk/exprtk.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

// Implemented in Dialogs.mm. `validate(formula, regex)` returns "" on success or
// an error message; it compiles the formula into the caller's ExprTk expression
// and checks the regex. Returns true if the user cancelled.
bool runCalculateDialog(ColumnsPlusPlusData& data,
                        const std::function<std::string(const std::string&, const std::string&)>& validate);

namespace {

struct CalculateInfo {
    ColumnsPlusPlusData& data;
    exprtk::symbol_table<double> symbol_table;
    exprtk::expression<double>   expression;
    exprtk::parser<double>       parser;
    CalculateInfo(ColumnsPlusPlusData& data) : data(data) {}
};

class CalculateHistory {
public:

    class LineCache {
    public:
        std::vector<double> col, reg, tab;
        std::string text;
        double result = 0;
        bool colValid = false, regValid = false, tabValid = false, textValid = false;
        void invalidate() { colValid = regValid = tabValid = textValid = false; }
    };

    ColumnsPlusPlusData& data;
    const int origin;
    RegularExpression rx;
    std::vector<LineCache> cache;
    std::vector<double>    results;
    std::vector<size_t>    skipMap;
    std::vector<bool>      skipFlag;
    double lastFiniteResult = 0;
    size_t currentEntry     = 0;

    CalculateHistory(ColumnsPlusPlusData& data)
        : data(data), origin(data.Anchor() > data.CurrentPos() ? data.Selections() - 1 : 0) {}

    size_t previous(size_t n) {
        if (n >= skipMap.size()) return std::string::npos;
        n = results.size() - skipMap[skipMap.size() - n - 1] - 1;
        if (n < cache.size()) return n <= currentEntry ? currentEntry - n : cache.size() + currentEntry - n;
        cache.insert(cache.begin() + currentEntry + 1, n + 1 - cache.size(), LineCache());
        return currentEntry + 1;
    }

    int selectionIndex(size_t cacheIndex) {
        size_t n = cacheIndex <= currentEntry ? currentEntry - cacheIndex : cache.size() + currentEntry - cacheIndex;
        return origin ? origin - static_cast<int>(results.size() - n - 1) : static_cast<int>(results.size() - n - 1);
    }

    const std::string& text(size_t lcn) {
        static const std::string nullString = "";
        if (lcn == std::string::npos) return nullString;
        LineCache& lc = cache[lcn];
        if (!lc.textValid) {
            lc.textValid = true;
            const int index = selectionIndex(lcn);
            const SciPosition cpMin = data.SelectionNStart(index);
            const SciPosition cpMax = data.SelectionNEnd(index);
            if (cpMin == cpMax) lc.text = "";
            else if (!data.settings.elasticEnabled || !data.settings.leadingTabsIndent)
                lc.text = data.StringOfRange(cpMin, cpMax);
            else {
                const SciLine     lineNumber     = data.LineFromPosition(cpMin);
                const SciPosition lineStart      = data.PositionFromLine(lineNumber);
                const std::string line           = data.StringOfRange(lineStart, cpMax);
                const size_t      selectionStart = cpMin - lineStart;
                const size_t      firstNonTab    = line.find_first_not_of('\t');
                if (firstNonTab == std::string::npos) lc.text = "";
                else lc.text = line.substr(std::max(selectionStart, firstNonTab));
            }
        }
        return lc.text;
    }

    void push() {
        if (cache.size() < 3) { cache.emplace_back(); currentEntry = cache.size() - 1; }
        else {
            ++currentEntry;
            if (currentEntry == cache.size()) currentEntry = 0;
            cache[currentEntry].invalidate();
        }
        if (results.size() && std::isfinite(results.back())) lastFiniteResult = results.back();
        skipFlag.push_back(false);
        skipMap.push_back(results.size());
        results.push_back(std::numeric_limits<double>::quiet_NaN());
    }

    void skip() { skipFlag.back() = true; skipMap.pop_back(); }
    bool skipped(size_t n) { return skipFlag[n]; }

    double col(size_t n, size_t i) {
        const size_t lcn = previous(n);
        if (lcn == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
        LineCache& lc = cache[lcn];
        if (!lc.colValid) {
            lc.colValid = true;
            const std::string& s = text(lcn);
            lc.col.clear();
            lc.col.push_back((double)data.parseNumber(s));
            for (size_t j = 0; j = s.find_first_not_of("\t ", j), j != std::string::npos;) {
                size_t k = s.find_first_of("\t ", j);
                lc.col.push_back((double)data.parseNumber(s.substr(j, k - j)));
                if (k == std::string::npos) break;
                j = k + 1;
            }
        }
        if (i >= lc.col.size()) return std::numeric_limits<double>::quiet_NaN();
        return lc.col[i];
    }

    double reg(size_t n, size_t i, bool* response = 0) {
        if (response) *response = false;
        if (!rx.can_search() || i > rx.mark_count()) return std::numeric_limits<double>::quiet_NaN();
        const size_t lcn = previous(n);
        if (lcn == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
        LineCache& lc = cache[lcn];
        if (!lc.regValid) {
            lc.regValid = true;
            const std::string& s = text(lcn);
            lc.reg.clear();
            if (rx.search(s)) {
                for (int j = 0; j < static_cast<int>(rx.size()); ++j) lc.reg.push_back((double)data.parseNumber(rx.str(j)));
                if (response) *response = true;
            }
        }
        if (i >= lc.reg.size()) return std::numeric_limits<double>::quiet_NaN();
        return lc.reg[i];
    }

    double tab(size_t n, size_t i) {
        const size_t lcn = previous(n);
        if (lcn == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
        LineCache& lc = cache[lcn];
        if (!lc.tabValid) {
            lc.tabValid = true;
            const std::string& s = text(lcn);
            lc.tab.clear();
            lc.tab.push_back((double)data.parseNumber(s));
            for (size_t j = 0; j < s.length();) {
                size_t k = s.find_first_of("\t", j);
                lc.tab.push_back((double)data.parseNumber(s.substr(j, k - j)));
                if (k == std::string::npos) break;
                j = k + 1;
            }
        }
        if (i >= lc.tab.size()) return std::numeric_limits<double>::quiet_NaN();
        return lc.tab[i];
    }
};

struct ExCol : public exprtk::ivararg_function<double> {
    CalculateHistory& history;
    ExCol(CalculateHistory& history) : history(history) {}
    double operator()(const std::vector<double>& a) {
        double n = history.col(a.size() > 1 ? (size_t)a[1] : 0, a.size() > 0 ? (size_t)a[0] : 0);
        if (a.size() > 2 && !std::isfinite(n)) n = a[2];
        return n;
    }
};
struct ExReg : public exprtk::ivararg_function<double> {
    CalculateHistory& history;
    ExReg(CalculateHistory& history) : history(history) {}
    double operator()(const std::vector<double>& a) {
        double n = history.reg(a.size() > 1 ? (size_t)a[1] : 0, a.size() > 0 ? (size_t)a[0] : 0);
        if (a.size() > 2 && !std::isfinite(n)) n = a[2];
        return n;
    }
};
struct ExTab : public exprtk::ivararg_function<double> {
    CalculateHistory& history;
    ExTab(CalculateHistory& history) : history(history) {}
    double operator()(const std::vector<double>& a) {
        double n = history.tab(a.size() > 1 ? (size_t)a[1] : 0, a.size() > 0 ? (size_t)a[0] : 0);
        if (a.size() > 2 && !std::isfinite(n)) n = a[2];
        return n;
    }
};
struct ExLast : public exprtk::ivararg_function<double> {
    CalculateHistory& history;
    ExLast(CalculateHistory& history) : history(history) { exprtk::enable_zero_parameters(*this); }
    double operator()(const std::vector<double>& a) {
        if (!a.size()) return history.lastFiniteResult;
        double n = std::numeric_limits<double>::quiet_NaN();
        size_t i = (size_t)a[0];
        if (i > 0 && i < history.results.size()) n = history.results[history.results.size() - i - 1];
        if (a.size() > 1 && !std::isfinite(n)) n = a[1];
        return n;
    }
};

} // namespace

void ColumnsPlusPlusData::calculate() {

    auto rs = getRectangularSelection();
    if (!rs.size()) return;

    CalculateInfo ci(*this);
    CalculateHistory history(*this);

    double exCount = rs.size();
    double exIndex = 0, exMatch = 0, exLine = 0, exThis = 0;

    ExCol exCol(history); ExReg exReg(history); ExTab exTab(history); ExLast exLast(history);

    ci.symbol_table.add_variable("count", exCount);
    ci.symbol_table.add_variable("index", exIndex);
    ci.symbol_table.add_variable("match", exMatch);
    ci.symbol_table.add_variable("line" , exLine );
    ci.symbol_table.add_variable("this" , exThis );
    ci.symbol_table.add_function("col"  , exCol );
    ci.symbol_table.add_function("reg"  , exReg );
    ci.symbol_table.add_function("tab"  , exTab );
    ci.symbol_table.add_function("last" , exLast);
    ci.expression.register_symbol_table(ci.symbol_table);

    // Dialog: validate compiles the formula into ci.expression and checks the regex.
    bool cancelled = runCalculateDialog(*this,
        [&](const std::string& formula, const std::string& regex) -> std::string {
            if (!ci.parser.compile(formula, ci.expression)) return ci.parser.error();
            if (!regex.empty()) {
                RegularExpression probe;
                std::string e = probe.find(regex, calc.matchCase);
                if (!e.empty()) return e;
            }
            return "";
        });
    if (cancelled) return;

    const std::string thousandsSeparator = calc.thousands == CalculateSettings::None ? ""
                                         : calc.thousands == CalculateSettings::Apostrophe ? "\'"
                                         : calc.thousands == CalculateSettings::Blank ? " "
                                         : settings.decimalSeparatorIsComma ? "." : ",";

    if (calc.regexHistory.size() && !calc.regexHistory.back().empty())
        history.rx.find(calc.regexHistory.back(), calc.matchCase);

    size_t   matches   = 0;
    intptr_t maxLeft   = 0, maxRight = 0, maxString = 0;

    struct Item { std::string text; intptr_t left = 0; bool align = false; };
    std::vector<Item> items;

    history.results.reserve(rs.size());
    history.skipMap.reserve(rs.size());
    history.skipFlag.reserve(rs.size());
    items.reserve(rs.size());

    NumericFormat format;
    intptr_t      colonDecimalOffset = 0;
    format.maxDec    = calc.decimalPlaces;
    format.thousands = thousandsSeparator;
    if (calc.decimalsFixed) format.minDec = calc.decimalPlaces;
    if (calc.formatAsTime) {
        format.timeEnable  = timeFormatEnable;
        colonDecimalOffset = (timeScalarUnit - (timePartialRule == 0 ? 0 : timePartialRule == 3 ? 2 : 1)) * 3;
    }

    for (auto row : rs) {
        ++exIndex;
        exLine = static_cast<double>(row.line() + 1);
        history.push();
        items.emplace_back();
        if (history.rx.can_search()) {
            bool found;
            exThis = history.reg(0, 0, &found);
            if (found) exMatch = static_cast<double>(++matches);
            else {
                if (calc.skipUnmatched) { history.skip(); continue; }
                exMatch = 0;
            }
        }
        else exThis = history.col(0, 0);
        double result = ci.expression.value();
        history.results.back() = result;
        Item& item = items.back();

        if (std::isfinite(result)) {
            item.text = formatNumber(result, format);
            intptr_t textLength = item.text.length();
            if (calc.aligned) {
                size_t cp = 0, dp = 0;
                getNumericAlignment(item.text, cp, dp);
                item.left = cp == std::string::npos ? static_cast<intptr_t>(dp) - colonDecimalOffset : (intptr_t)cp;
                if (item.left > maxLeft) maxLeft = item.left;
                if (textLength - item.left > maxRight) maxRight = textLength - item.left;
                item.align = true;
            }
            else if (textLength > maxString) maxString = textLength;
        }
        else if (ci.expression.results().count()) {
            const auto& results = ci.expression.results();
            std::string s;
            bool lastWasNumeric = false;
            for (size_t i = 0; i < results.count(); ++i) {
                auto r = results[i];
                if (r.type == r.e_string) {
                    exprtk::igeneric_function<double>::generic_type::string_view sv(r);
                    s += std::string(std::string_view(sv.data_, sv.size()));
                    lastWasNumeric = false;
                }
                else if (r.type == exprtk::type_store<double>::store_type::e_scalar) {
                    if (lastWasNumeric) s += calc.tabbed ? '\t' : ' ';
                    exprtk::igeneric_function<double>::generic_type::scalar_view sv(r);
                    if (std::isfinite(sv())) s += formatNumber(sv(), format);
                    lastWasNumeric = true;
                }
            }
            item.text = s;
            if (maxString < static_cast<intptr_t>(s.length())) maxString = s.length();
        }
    }

    if (maxString > maxLeft + maxRight) maxLeft = maxString - maxRight;
    int resultsIndex = 0;
    BeginUndoAction();
    for (auto row : rs) {
        if (!history.skipped(resultsIndex)) {
            Item& item = items[resultsIndex];
            std::string s = item.text;
            std::string rowText(row.text());
            if (calc.aligned && item.align) s = std::string(maxLeft - item.left, ' ') + s;
            if (!settings.elasticEnabled || !calc.tabbed) s += std::string(maxLeft + maxRight - (intptr_t)s.length(), ' ');
            if (calc.left) {
                s += calc.tabbed ? '\t' : ' ';
                s += rowText;
                row.replace(s);
            }
            else {
                if (calc.tabbed) s = !rowText.empty() && rowText.back() == '\t' ? s + '\t' : '\t' + s;
                else s = ' ' + s;
                s.insert(0, rowText);
                row.replace(s);
            }
        }
        ++resultsIndex;
    }
    rs.refit();
    EndUndoAction();
}
