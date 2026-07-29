// Columns++ for Notepad++ — macOS port.
// Commands.mm — the column-editing commands: Align (left/right/numeric/custom),
// Sort (binary/locale/numeric/width + custom), Add/Average numbers, the tab<->space
// and separated-values<->tabs conversions, and the six Select* commands.
//
// Algorithms are ported faithfully from the Windows Align.cpp, Sort.cpp,
// Numeric.cpp, Convert.cpp and Selection.cpp. Platform changes:
//   * Win32 dialogs → the AppKit panels in Dialogs.mm (this file consumes the
//     settings those panels write).
//   * Locale sort: Windows LCMapStringEx(LCMAP_SORTKEY) → Foundation collation
//     (-[NSString compare:options:range:locale:]). macOS ships ICU only as a
//     private library with versioned symbols, so we use the supported Foundation
//     API instead of ucol_getSortKey; the comparison honours the same flags
//     (case / diacritics / numeric digits / symbols) via NSStringCompareOptions.
//   * Regex Align/Sort: std::regex (see Regex.h) instead of Boost+UCD.
//   * "Convert tabs to spaces" with elastic OFF used NPPM_MENUCOMMAND(IDM_EDIT_TAB2SW);
//     that host command exists, but to be self-contained and tab-correct we always
//     expand using SCI_GETCOLUMN/pixel math here.

#include "CppData.h"

#include <unicode/coll.h>
#include <unicode/locid.h>
#include <memory>
#include "Regex.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>   // sort-column spec parser only (ASCII "1a 2d…"); content regex is Regex.h

// Dialog hooks implemented in Dialogs.mm. Each returns true if the user cancelled.
bool runAlignDialog(ColumnsPlusPlusData& data);
bool runSortDialog(ColumnsPlusPlusData& data);
bool runCsvDialog(ColumnsPlusPlusData& data);
// Calculation-results dialog: shows the per-column results, lets the user pick
// thousands/decimals/time/insert; returns false if cancelled. `doInsert` receives
// the user's insert choice; `copyResults` the tab-joined string copied to clipboard.
bool runCalculationResultsDialog(ColumnsPlusPlusData& data,
                                 const std::vector<double>& values,
                                 std::vector<NumericFormat>& autoFormats,
                                 bool isMean, int columns, int numbers, bool hasSpace,
                                 std::string& copyResults);

// ===========================================================================
// ALIGN
// ===========================================================================
void ColumnsPlusPlusData::alignLeft() {
    auto rs = getRectangularSelection();
    if (!rs.size()) return;
    BeginUndoAction();
    for (auto row : rs) {
        std::string r;
        for (const auto& cell : row) {
            r += cell.trim();
            if (!settings.elasticEnabled || (cell.isLastInRow() && !cell.isEndOfLine())) {
                r += std::string(cell.leading() + cell.trailing(), ' ');
            }
            r += cell.terminator();
        }
        if (r != row.text()) row.replace(r);
    }
    EndUndoAction();
    rs.refit();
}

void ColumnsPlusPlusData::alignRight() {
    auto rs = getRectangularSelection();
    if (!rs.size()) return;
    BeginUndoAction();
    for (auto row : rs) {
        std::string r;
        for (const auto& cell : row) {
            if (cell.trimLength() == 0)  r += cell.text() + cell.terminator();
            else if (cell.isLastInRow()) r += std::string(row.vsMax() - row.vsMin() + cell.leading() + cell.trailing(), ' ') + cell.trim();
            else {
                int padWidth = PointXFromPosition(cell.end() + 1) - PointXFromPosition(cell.end());
                padWidth = rs.blankCount(padWidth) - (settings.elasticEnabled ? settings.minimumSpaceBetweenColumns : 1);
                if (padWidth < 0) padWidth = 0;
                r += std::string(padWidth + cell.leading() + cell.trailing(), ' ') + cell.trim() + '\t';
            }
        }
        if (r != row.text()) row.replace(r);
    }
    EndUndoAction();
    rs.refit();
}

void ColumnsPlusPlusData::alignNumeric() {

    struct Item {
        std::string text;
        intptr_t colonExtentLeft;
        intptr_t colonExtentRight;
        intptr_t decimalExtentLeft;
        intptr_t decimalExtentRight;
        bool     align;
    };
    struct Line : std::vector<Item> { std::string line; bool eol; };
    struct Column {
        intptr_t colonExtentLeft = 0, colonExtentRight = 0;
        intptr_t decimalExtentLeft = 0, decimalExtentRight = 0;
        intptr_t width = 0, pad = 0;
    };

    auto rs = getRectangularSelection();
    if (rs.size() < 2) return;

    std::vector<Line> item(rs.size());
    std::vector<Column> column;

    for (auto row : rs) {
        Line& cLine = item[row.index];
        cLine.line  = row.text();
        cLine.eol   = row.isEndOfLine();
        for (const auto& cell : row) {
            cLine.emplace_back();
            Item& cItem = cLine.back();
            if (cLine.size() > column.size()) column.emplace_back();
            Column& cColumn = column[cLine.size() - 1];
            intptr_t width = PointXFromPosition(cell.end()) - PointXFromPosition(cell.start());
            if (cell.isLastInRow()) width += rs.blankWidth(row.vsMax() - row.vsMin());
            if (width > cColumn.width) cColumn.width = width;
            size_t cp = 0, dp = 0;
            if (cell.trimLength() == 0 || !getNumericAlignment(cell.trim(), cp, dp)) {
                cItem.text = cell.text();
                cItem.align = false;
                continue;
            }
            if (cp == std::string::npos) {
                auto dpX = PointXFromPosition(cell.left() + dp);
                cItem.decimalExtentLeft  = dpX - PointXFromPosition(cell.left());
                cItem.decimalExtentRight = PointXFromPosition(cell.right()) - dpX;
                if (cItem.decimalExtentLeft  > cColumn.decimalExtentLeft ) cColumn.decimalExtentLeft  = cItem.decimalExtentLeft;
                if (cItem.decimalExtentRight > cColumn.decimalExtentRight) cColumn.decimalExtentRight = cItem.decimalExtentRight;
                cItem.colonExtentLeft = cItem.colonExtentRight = 0;
            }
            else {
                auto cpX = PointXFromPosition(cell.left() + cp);
                cItem.colonExtentLeft  = cpX - PointXFromPosition(cell.left());
                cItem.colonExtentRight = PointXFromPosition(cell.right()) - cpX;
                if (cItem.colonExtentLeft  > cColumn.colonExtentLeft ) cColumn.colonExtentLeft  = cItem.colonExtentLeft;
                if (cItem.colonExtentRight > cColumn.colonExtentRight) cColumn.colonExtentRight = cItem.colonExtentRight;
                cItem.decimalExtentLeft = cItem.decimalExtentRight = 0;
            }
            cItem.text = cell.trim();
            cItem.align = true;
        }
    }

    intptr_t colonDecimalOffset = (timeScalarUnit - (timePartialRule == 0 ? 0 : timePartialRule == 3 ? 2 : 1)) * TextWidth(STYLE_DEFAULT, ":00");

    for (Column& col : column) {
        if (col.colonExtentRight == 0)
            col.pad = rs.blankCount(col.width - col.decimalExtentLeft - col.decimalExtentRight);
        else if (col.decimalExtentLeft == 0 && col.decimalExtentRight == 0)
            col.pad = rs.blankCount(col.width - col.colonExtentLeft - col.colonExtentRight);
        else
            col.pad = rs.blankCount( col.width - std::max(col.colonExtentLeft , col.decimalExtentLeft  - colonDecimalOffset)
                                               - std::max(col.colonExtentRight, col.decimalExtentRight + colonDecimalOffset) );
    }

    BeginUndoAction();

    for (auto row : rs) {
        const auto& cells = item[row.index];
        bool notEOL = !cells.eol;
        intptr_t lastCell = cells.size() - 1;
        std::string r;
        for (int j = 0;;++j) {
            const Item& cell = cells[j];
            const Column& col = column[j];
            if (cell.align) {
                if (col.pad > 0) r += std::string(col.pad, ' ');
                intptr_t colLeft, colRight, cellLeft, cellRight;
                if (col.colonExtentRight == 0) {
                    colLeft = col.decimalExtentLeft; colRight = col.decimalExtentRight;
                    cellLeft = cell.decimalExtentLeft; cellRight = cell.decimalExtentRight;
                }
                else if (col.decimalExtentLeft == 0 && col.decimalExtentRight == 0) {
                    colLeft = col.colonExtentLeft; colRight = col.colonExtentRight;
                    cellLeft = cell.colonExtentLeft; cellRight = cell.colonExtentRight;
                }
                else {
                    colLeft  = std::max(col.colonExtentLeft , col.decimalExtentLeft  - colonDecimalOffset);
                    colRight = std::max(col.colonExtentRight, col.decimalExtentRight + colonDecimalOffset);
                    if (cell.colonExtentRight == 0) {
                        cellLeft  = cell.decimalExtentLeft  - colonDecimalOffset;
                        cellRight = cell.decimalExtentRight + colonDecimalOffset;
                    }
                    else { cellLeft = cell.colonExtentLeft; cellRight = cell.colonExtentRight; }
                }
                r += std::string(rs.blankCount(colLeft - cellLeft), ' ');
                r += cell.text;
                if (j == lastCell ? notEOL : !settings.elasticEnabled)
                    r += std::string(rs.blankCount(colRight - cellRight), ' ');
            }
            else {
                r += cell.text;
                if (col.pad < 0 && (j == lastCell ? notEOL : !settings.elasticEnabled)) r += std::string(-col.pad, ' ');
            }
            if (j == lastCell) break;
            r += '\t';
        }
        if (r != cells.line) row.replace(r);
    }

    EndUndoAction();
    rs.refit();
}

void ColumnsPlusPlusData::alignCustom() {
    auto rs = getRectangularSelection();
    if (rs.size() < 2) return;

    if (runAlignDialog(*this)) return;   // cancelled

    std::string find = align.history.back();
    SimpleRegex rx;
    if (align.alignOn == AlignSettings::Regex) {
        std::string err = rx.compile(align.history.back(), align.matchCase);
        if (!err.empty()) { cppAlert("Align", err); return; }
    }

    struct Item { std::string text; intptr_t extentLeft; intptr_t extentRight; bool align; };
    struct Line : std::vector<Item> { std::string line; bool eol; };
    struct Column { intptr_t extentLeft = 0, extentRight = 0, width = 0, pad = 0; };

    std::vector<Line> item(rs.size());
    std::vector<Column> column;

    SetSearchFlags(align.matchCase ? 0x4 /* SCFIND_MATCHCASE */ : 0);

    for (auto row : rs) {
        Line& cLine = item[row.index];
        cLine.line  = row.text();
        cLine.eol   = row.isEndOfLine();
        for (const auto& cell : row) {
            cLine.emplace_back();
            Item& cItem = cLine.back();
            if (cLine.size() > column.size()) column.emplace_back();
            Column& cColumn = column[cLine.size() - 1];
            intptr_t width = PointXFromPosition(cell.end()) - PointXFromPosition(cell.start());
            if (cell.isLastInRow()) width += rs.blankWidth(row.vsMax() - row.vsMin());
            if (width > cColumn.width) cColumn.width = width;
            SciPosition found = -1;
            if (cell.trimLength() > 0) {
                if (align.alignOn == AlignSettings::Regex) {
                    std::string t = cell.trim();
                    if (rx.search(t)) found = cell.left() + rx.position(0);
                }
                else {
                    if (align.alignOn == AlignSettings::Last) SetTargetRange(cell.right(), cell.left());
                                                         else SetTargetRange(cell.left(), cell.right());
                    found = SearchInTarget(find);
                }
            }
            if (found < 0) { cItem.text = cell.text(); cItem.align = false; continue; }
            auto cpX = PointXFromPosition(found);
            cItem.extentLeft  = cpX - PointXFromPosition(cell.left());
            cItem.extentRight = PointXFromPosition(cell.right()) - cpX;
            cItem.text = cell.trim();
            if (align.margin > 0) {
                if (align.marginRight) cItem.extentRight += rs.blankWidth(align.margin);
                else { cItem.text.insert(0, std::string(align.margin, ' ')); cItem.extentLeft += rs.blankWidth(align.margin); }
            }
            if (cItem.extentLeft  > cColumn.extentLeft ) cColumn.extentLeft  = cItem.extentLeft;
            if (cItem.extentRight > cColumn.extentRight) cColumn.extentRight = cItem.extentRight;
            cItem.align = true;
        }
    }

    for (Column& col : column) col.pad = rs.blankCount(col.width - col.extentLeft - col.extentRight);

    BeginUndoAction();

    for (auto row : rs) {
        const auto& cells = item[row.index];
        bool notEOL = !cells.eol;
        intptr_t lastCell = cells.size() - 1;
        std::string r;
        for (int j = 0;;++j) {
            const Item& cell = cells[j];
            const Column& col = column[j];
            if (cell.align) {
                if (col.pad > 0 && align.marginRight) r += std::string(col.pad, ' ');
                intptr_t colLeft = col.extentLeft, colRight = col.extentRight, cellLeft = cell.extentLeft, cellRight = cell.extentRight;
                r += std::string(rs.blankCount(colLeft - cellLeft), ' ');
                r += cell.text;
                if (j == lastCell ? notEOL : !settings.elasticEnabled) {
                    intptr_t rightPad = rs.blankCount(colRight - cellRight);
                    if (align.marginRight) rightPad += align.margin;
                    else if (col.pad > 0) rightPad += col.pad;
                    r += std::string(rightPad, ' ');
                }
            }
            else {
                r += cell.text;
                if (col.pad < 0 && (j == lastCell ? notEOL : !settings.elasticEnabled)) r += std::string(-col.pad, ' ');
            }
            if (j == lastCell) break;
            r += '\t';
        }
        if (r != cells.line) row.replace(r);
    }

    EndUndoAction();
    rs.refit();
}

// ===========================================================================
// SORT
// ===========================================================================
namespace {

// One sort key: either a string (numeric==false) or a double (numeric==true).
// localeText (when set) means "compare as a locale string" via Foundation.
struct SortKey {
    std::string string;
    double      number = 0;
    bool        numeric = false;
    bool        locale  = false;
    bool        descending = false;
    SortKey(const std::string& s, bool desc, bool loc) : string(s), numeric(false), locale(loc), descending(desc) {}
    SortKey(double n, bool desc) : number(n), numeric(true), descending(desc) {}
};

struct SortLine {
    // Pointers into the captured text block.
    char* line = nullptr;
    char* left = nullptr;
    char* right = nullptr;
    SciPosition vsLeft = 0, vsRight = 0;
    intptr_t lineLength = 0;
    int selection = 0;
    std::vector<SortKey> keys;
};

// ICU locale comparison honouring the SortSettings flags. Returns <0/0/>0.
// (macOS used NSString compare — a wrapper over this same ICU collation.)
int localeCompare(const std::string& a, const std::string& b, icu::Collator* coll) {
    if (!coll) return a.compare(b);
    UErrorCode status = U_ZERO_ERROR;
    UCollationResult r = coll->compareUTF8(icu::StringPiece(a), icu::StringPiece(b), status);
    if (U_FAILURE(status)) return a.compare(b);
    return r == UCOL_LESS ? -1 : r == UCOL_GREATER ? 1 : 0;
}

// keycap parser — parse "2,1d,3an" style key specs into parallel vectors.
// each key: a 1-based number, optional direction (a/d), optional type (b/l/n/w).
bool parseKeycap(const std::string& spec, const SortSettings& ss,
                 std::vector<int>& capGroup, std::vector<bool>& capDesc, std::vector<SortSettings::SortType>& capType) {
    static const std::regex re(R"(\s*(\d+)\s*([adAD][blnwBLNW]?|[blnwBLNW][adAD]?)?\s*[,; ]?)");
    auto begin = std::sregex_iterator(spec.begin(), spec.end(), re);
    auto end = std::sregex_iterator();
    bool any = false;
    size_t consumed = 0;
    for (auto it = begin; it != end; ++it) {
        std::smatch m = *it;
        if (m.position(0) != (long)consumed) return false;   // gap => invalid
        consumed += m.length(0);
        any = true;
        capGroup.push_back(std::stoi(m[1].str()));
        bool desc = ss.sortDescending;
        SortSettings::SortType type = ss.sortType;
        std::string flags = m[2].str();
        for (char c : flags) {
            char lc = (char)tolower((unsigned char)c);
            if (lc == 'a') desc = false;
            else if (lc == 'd') desc = true;
            else if (lc == 'b') type = SortSettings::Binary;
            else if (lc == 'l') type = SortSettings::Locale;
            else if (lc == 'n') type = SortSettings::Numeric;
            else if (lc == 'w') type = SortSettings::Width;
        }
        capDesc.push_back(desc);
        capType.push_back(type);
    }
    // Trim trailing whitespace from spec when checking full consumption.
    std::string trimmed = spec;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == ',' || trimmed.back() == ';')) trimmed.pop_back();
    if (consumed < trimmed.size()) return false;
    return any;
}

void sortCommon(ColumnsPlusPlusData& data, const SortSettings& ss, RectangularSelection& rs) {

    // Build an ICU collator from the SortSettings flags. Strength mapping:
    //   default                          -> TERTIARY (case + diacritics count)
    //   !caseSensitive                   -> SECONDARY (ignore case)
    //   ignoreDiacritics                 -> PRIMARY   (+ CASE_LEVEL when case-sensitive)
    // digitsAsNumbers -> UCOL_NUMERIC_COLLATION; ignoreSymbols -> alternate=shifted
    // (the REAL ignore-symbols — the Windows original's LINGUISTIC_IGNORESYMBOLS;
    // macOS could only approximate it with width-insensitivity).
    UErrorCode collStatus = U_ZERO_ERROR;
    icu::Locale icuLoc = ss.localeName.empty() ? icu::Locale::getDefault()
                                               : icu::Locale(ss.localeName.c_str());
    std::unique_ptr<icu::Collator> coll(icu::Collator::createInstance(icuLoc, collStatus));
    if (U_FAILURE(collStatus)) coll.reset();
    if (coll) {
        if (ss.localeIgnoreDiacritics) {
            coll->setStrength(icu::Collator::PRIMARY);
            if (ss.localeCaseSensitive)
                coll->setAttribute(UCOL_CASE_LEVEL, UCOL_ON, collStatus);
        }
        else coll->setStrength(ss.localeCaseSensitive ? icu::Collator::TERTIARY
                                                      : icu::Collator::SECONDARY);
        if (ss.localeDigitsAsNumbers)
            coll->setAttribute(UCOL_NUMERIC_COLLATION, UCOL_ON, collStatus);
        if (ss.localeIgnoreSymbols)
            coll->setAttribute(UCOL_ALTERNATE_HANDLING, UCOL_SHIFTED, collStatus);
    }

    bool forward = rs.topToBottom();

    std::vector<int> capGroup;
    std::vector<bool> capDesc;
    std::vector<SortSettings::SortType> capType;
    if (ss.keyType == SortSettings::Tabbed || (ss.keyType == SortSettings::Regex && ss.regexUseKey)) {
        if (!ss.keygroupHistory.empty())
            parseKeycap(ss.keygroupHistory.back(), ss, capGroup, capDesc, capType);
    }
    else if (ss.keyType == SortSettings::Regex) {
        capGroup = {0};
        capDesc = {ss.sortDescending};
        capType = {ss.sortType};
    }

    SimpleRegex rx;
    if (ss.keyType == SortSettings::Regex && !ss.regexHistory.empty())
        rx.compile(ss.regexHistory.back(), ss.regexMatchCase);

    int lines = rs.size();
    std::vector<SortLine> sl(lines);
    std::vector<SortLine> unsorted(lines);

    SciLine  textLine  = rs.top().ln;
    SciPosition textStart = rs.top().st;
    SciPosition textEnd   = data.PositionFromLine(textLine + lines);
    std::string text = data.StringOfRange(textStart, textEnd);

    std::string appendedEOL;
    if (textLine + lines == data.LineCount()) {
        int eol = data.EOLMode();   // SC_EOL_CRLF=0, CR=1, LF=2
        appendedEOL = eol == 1 ? "\r" : eol == 2 ? "\n" : "\r\n";
        text += appendedEOL;
    }
    char* textPointer = text.data();

    SciPosition cpNextLine = textEnd;
    for (int n = lines - 1; n >= 0; --n) {
        sl[n].selection = forward ? n : lines - 1 - n;
        auto row = rs[sl[n].selection];
        SciPosition cpLine = data.PositionFromLine(row.line());
        SciPosition cpMin = row.cpMin();
        SciPosition cpMax = row.cpMax();
        sl[n].line   = cpLine - textStart + textPointer;
        sl[n].left   = cpMin  - textStart + textPointer;
        sl[n].right  = cpMax  - textStart + textPointer;
        sl[n].vsLeft = row.vsMin();
        sl[n].vsRight= row.vsMax();

        std::vector<SortKey>& keys = sl[n].keys;

        if (ss.keyType == SortSettings::Regex) {
            std::string rowText(row.text());
            if (rx.search(rowText)) {
                for (size_t i = 0; i < capGroup.size(); ++i) {
                    std::string s = rx.str(capGroup[i]);
                    if (capType[i] == SortSettings::Numeric) keys.emplace_back((double)data.parseNumber(s), capDesc[i]);
                    else if (capType[i] == SortSettings::Width) {
                        if (s.empty()) keys.emplace_back((double)0, capDesc[i]);
                        else {
                            SciPosition start = row.cpMin() + rx.position(capGroup[i]);
                            keys.emplace_back((double)data.unwrappedWidth(start, start + s.length()), capDesc[i]);
                        }
                    }
                    else keys.emplace_back(s, capDesc[i], capType[i] == SortSettings::Locale);
                }
            }
        }
        else if (ss.keyType == SortSettings::Tabbed) {
            std::vector<std::string> cellText;
            std::vector<SciPosition> cellStart, cellEnd;
            cellText.push_back(std::string(row.text()));
            cellStart.push_back(row.cpMin());
            cellEnd.push_back(row.cpMax());
            for (const auto& cell : row) {
                cellText.push_back(cell.text());
                cellStart.push_back(cell.start());
                cellEnd.push_back(cell.end());
            }
            for (size_t i = 0; i < capGroup.size(); ++i) {
                int g = capGroup[i];
                std::string s = (g >= 0 && (size_t)g < cellText.size()) ? cellText[g] : "";
                if (capType[i] == SortSettings::Numeric) keys.emplace_back((double)data.parseNumber(s), capDesc[i]);
                else if (capType[i] == SortSettings::Width) {
                    if (g >= 0 && (size_t)g < cellText.size()) keys.emplace_back((double)data.unwrappedWidth(cellStart[g], cellEnd[g]), capDesc[i]);
                    else keys.emplace_back((double)0, capDesc[i]);
                }
                else keys.emplace_back(s, capDesc[i], capType[i] == SortSettings::Locale);
            }
        }
        else if (ss.sortType == SortSettings::Numeric) {
            for (const auto& cell : row) keys.emplace_back((double)data.parseNumber(cell.trim()), ss.sortDescending);
        }
        else if (ss.sortType == SortSettings::Width) {
            SciPosition start = row.cpMin(), end = row.cpMax();
            if (ss.keyType == SortSettings::IgnoreBlanks) {
                std::string s(row.text());
                size_t i = s.find_first_not_of("\t ");
                if (i == std::string::npos) end = start;
                else { end = start + s.find_last_not_of("\t ") + 1; start += i; }
            }
            keys.emplace_back((double)data.unwrappedWidth(start, end), ss.sortDescending);
        }
        else {
            std::string s(row.text());
            if (ss.keyType == SortSettings::IgnoreBlanks) {
                size_t i = s.find_first_not_of("\t ");
                if (i == std::string::npos) s = "";
                else { size_t j = s.find_last_not_of("\t "); s = s.substr(i, j - i + 1); }
            }
            keys.emplace_back(s, ss.sortDescending, ss.sortType == SortSettings::Locale);
        }

        sl[n].lineLength = cpNextLine - cpLine;
        cpNextLine = cpLine;
        unsorted[n] = sl[n];
        unsorted[n].keys.clear();
    }
    sl[lines - 1].lineLength += appendedEOL.length();

    std::stable_sort(sl.begin(), sl.end(), [&](const SortLine& a, const SortLine& b) {
        size_t m = std::min(a.keys.size(), b.keys.size());
        for (size_t i = 0; i < m; ++i) {
            const SortKey& ka = a.keys[i];
            const SortKey& kb = b.keys[i];
            if (!ka.numeric) {
                if (ka.locale) {
                    int c = localeCompare(ka.string, kb.string, coll.get());
                    if (c == 0) continue;
                    bool lt = (c < 0);
                    return ka.descending ? !lt : lt;
                }
                if (ka.string == kb.string) continue;
                return ka.descending ? ka.string > kb.string : ka.string < kb.string;
            }
            if (std::isnan(ka.number)) { if (std::isnan(kb.number)) continue; else return true; }
            if (std::isnan(kb.number)) return false;
            if (ka.number == kb.number) continue;
            return ka.descending ? ka.number > kb.number : ka.number < kb.number;
        }
        return a.keys.size() < b.keys.size();
    });

    // Write back.
    std::string r;
    if (ss.sortColumnSelectionOnly) {
        for (int i = 0; i < lines; ++i) {
            const SortLine& u = unsorted[i];
            const SortLine& s = sl[i];
            r.append(u.line, u.left - u.line);
            r.append((size_t)u.vsLeft, ' ');
            r.append(s.left, s.right - s.left);
            r.append((size_t)(s.vsRight - s.vsLeft), ' ');
            r.append(u.right, u.lineLength - (u.right - u.line));
        }
    }
    else {
        for (int i = 0; i < lines; ++i) r.append(sl[i].line, sl[i].lineLength);
        if (!appendedEOL.empty() && !r.empty() && (r.back() == '\n' || r.back() == '\r')) {
            r.pop_back();
            if (!r.empty() && appendedEOL.size() == 2 && r.back() == '\r') r.pop_back();
        }
    }

    // Determine new corners (first/last sorted line's left/right) before replacing.
    bool tlbr = rs.topToBottom();
    SciPosition firstLeftOffset  = sl[0].left  - textPointer;
    SciPosition firstRightOffset = sl[0].right - textPointer;
    SciPosition lastLeftOffset   = sl[lines-1].left  - textPointer;
    SciPosition lastRightOffset  = sl[lines-1].right - textPointer;
    SciPosition firstVsLeft = sl[0].vsLeft, firstVsRight = sl[0].vsRight;
    SciPosition lastVsLeft  = sl[lines-1].vsLeft, lastVsRight = sl[lines-1].vsRight;

    data.BeginUndoAction();
    data.SetTargetRange(textStart, textEnd);
    data.ReplaceTarget(r);
    data.EndUndoAction();

    // Re-establish a rectangular selection over the sorted region.
    SciPosition top    = textStart + firstLeftOffset;
    SciPosition topR   = textStart + firstRightOffset;
    SciPosition bottom = textStart + lastLeftOffset;
    SciPosition botR   = textStart + lastRightOffset;
    if (ss.sortColumnSelectionOnly) {
        // After column-only replace, offsets shift; fall back to a simple reselect
        // of the original geometry via line bounds.
        data.SetSelectionMode(SC_SEL_RECTANGLE);
        SciPosition a = data.PositionFromLine(textLine);
        data.SetRectangularSelectionAnchor(a);
        data.SetRectangularSelectionCaret(a);
        data.ChooseCaretX();
        return;
    }
    data.SetSelectionMode(SC_SEL_RECTANGLE);
    if (tlbr) {
        data.SetRectangularSelectionAnchor(rs.leftToRight() ? top : topR);
        data.SetRectangularSelectionAnchorVirtualSpace(rs.leftToRight() ? firstVsLeft : firstVsRight);
        data.SetRectangularSelectionCaret(rs.leftToRight() ? botR : bottom);
        data.SetRectangularSelectionCaretVirtualSpace(rs.leftToRight() ? lastVsRight : lastVsLeft);
    }
    else {
        data.SetRectangularSelectionAnchor(rs.leftToRight() ? bottom : botR);
        data.SetRectangularSelectionAnchorVirtualSpace(rs.leftToRight() ? lastVsLeft : lastVsRight);
        data.SetRectangularSelectionCaret(rs.leftToRight() ? topR : top);
        data.SetRectangularSelectionCaretVirtualSpace(rs.leftToRight() ? firstVsRight : firstVsLeft);
    }
    data.ChooseCaretX();
}

void sortStandard(ColumnsPlusPlusData& data, SortSettings::SortType sortType, bool descending) {
    auto rs = data.getRectangularSelection();
    if (rs.size() < 2) return;
    SortSettings s;
    s.sortType = sortType;
    s.sortDescending = descending;
    sortCommon(data, s, rs);
}

} // namespace

void ColumnsPlusPlusData::sortAscendingBinary  () { sortStandard(*this, SortSettings::Binary , false); }
void ColumnsPlusPlusData::sortDescendingBinary () { sortStandard(*this, SortSettings::Binary , true ); }
void ColumnsPlusPlusData::sortAscendingLocale  () { sortStandard(*this, SortSettings::Locale , false); }
void ColumnsPlusPlusData::sortDescendingLocale () { sortStandard(*this, SortSettings::Locale , true ); }
void ColumnsPlusPlusData::sortAscendingNumeric () { sortStandard(*this, SortSettings::Numeric, false); }
void ColumnsPlusPlusData::sortDescendingNumeric() { sortStandard(*this, SortSettings::Numeric, true ); }

void ColumnsPlusPlusData::sortCustom() {
    auto rs = getRectangularSelection();
    if (rs.size() < 2) return;
    if (runSortDialog(*this)) return;   // cancelled
    sortCommon(*this, sort, rs);
}

// ===========================================================================
// ADD / AVERAGE NUMBERS
// ===========================================================================
void ColumnsPlusPlusData::accumulate(bool isMean) {
    struct Column { double sum = 0; int decimalPlaces = 0; int timeSegments = 0; int count = 0; };

    auto rs = getRectangularSelection();
    if (!rs.size()) return;

    std::vector<Column> column;
    Column total;
    int indexOfLastRowWithValues = -1;

    for (auto row : rs) {
        size_t columnNumber = 0;
        for (const auto& cell : row) {
            if (columnNumber >= column.size()) column.emplace_back();
            Column& col = column[columnNumber]; ++columnNumber;
            NumericParse np = parseNumber(cell.trim());
            if (!std::isfinite(np.value)) {
                if (cell.trim().find_first_of("0123456789") != std::string::npos) {
                    SetSel(cell.left(), cell.right());
                    return;
                }
                continue;
            }
            col.sum += np.value; ++col.count;
            col.timeSegments  = std::max(col.timeSegments, np.timeSegments);
            col.decimalPlaces = std::max(col.decimalPlaces, np.decimalPlaces);
            total.sum += np.value; ++total.count;
            total.timeSegments  = std::max(total.timeSegments, np.timeSegments);
            total.decimalPlaces = std::max(total.decimalPlaces, np.decimalPlaces);
            indexOfLastRowWithValues = row.index;
        }
    }
    if (!total.count) { cppAlert(isMean ? "Average numbers" : "Add numbers", "No numbers found in the selection."); return; }

    std::vector<double> values;
    std::vector<NumericFormat> autoFormats;
    int columns = 0, numbers = 0;
    for (Column& col : column) {
        NumericFormat format;
        if (col.count) {
            format.maxDec = col.decimalPlaces;
            if (col.timeSegments) format.timeEnable = timeFormatEnable;
            if (isMean) {
                format.maxDec += col.count == 1 ? 0
                               : (col.count == 2 || col.count == 5 || col.count == 10) ? 1
                               : (col.count == 20 || col.count == 25 || col.count == 50 || col.count == 100) ? 2 : 3;
                values.push_back(col.sum / col.count);
            }
            else values.push_back(col.sum);
            ++columns; numbers += col.count;
        }
        else values.push_back(std::numeric_limits<double>::quiet_NaN());
        autoFormats.push_back(format);
    }

    bool hasSpace = std::string(rs.back().text()).find_first_not_of("\t ") == std::string::npos;
    std::string copyResults;
    if (!runCalculationResultsDialog(*this, values, autoFormats, isMean, columns, numbers, hasSpace, copyResults))
        return;

    if (hasSpace ? !calc.insert : !calc.addLine) return;

    BeginUndoAction();
    if (!hasSpace) {
        int eol = EOLMode();
        std::string EOL = eol == 1 ? "\r" : eol == 2 ? "\n" : "\r\n";
        InsertText(rs.bottom().en, EOL.c_str());
        rs.refit(true);
        if (!rs.topToBottom()) indexOfLastRowWithValues++;
    }
    (void)indexOfLastRowWithValues;   // used only by the deferred elastic leading-tab placement
    auto answerRow = rs.back();
    const std::string& answer = copyResults;

    if (answerRow.vsMin() == 0 || !settings.elasticEnabled) {
        if (column.size() == 1) {
            size_t space = answerRow.cpMax() - answerRow.cpMin() + answerRow.vsMax();
            answerRow.replace(space > answer.length() ? std::string(space - answer.length(), ' ') + answer : answer);
        }
        else answerRow.replace(std::string(answerRow.vsMin(), ' ') + answer);
    }
    else {
        // Elastic + leading virtual space: keep the simple placement (port note —
        // the Windows leading-tab indentation mirror is an elastic-only refinement).
        answerRow.replace(std::string(answerRow.vsMin(), ' ') + answer);
    }
    rs.refit();
    EndUndoAction();
}

// ===========================================================================
// CONVERSION
// ===========================================================================
static bool isValidCsvField(const std::string& s, const CsvSettings& csv, bool strict) {
    size_t p = s.find_first_not_of(' ');
    if (p == std::string::npos) return true;
    size_t q = s.find_last_not_of(' ');
    if ((s[p] == '"' && csv.quote) || (s[p] == '\'' && csv.apostrophe)) {
        if (p == q || s[p] != s[q]) return false;
        std::string stop = csv.escape ? s.substr(p, 1) + std::string(1, csv.escapeChar) : s.substr(p, 1);
        for (size_t i = p + 1; (i = s.find_first_of(stop, i)) < q; i += 2)
            if (i == q - 1 || (s[i] == s[p] && s[i + 1] != s[p])) return false;
        return true;
    }
    std::string stop = std::string("\r\n") + csv.separator;
    if (strict && csv.quote)      stop += '"';
    if (strict && csv.apostrophe) stop += '\'';
    if (csv.escape)               stop += csv.escapeChar;
    for (size_t i = p + 1; i < q;) {
        size_t j = s.find_first_of(stop, i);
        if (j == std::string::npos) return true;
        if (s[j] != csv.escapeChar) return false;
        if (j == s.length() - 1) return false;
        else i = j + 2;
    }
    return true;
}

void ColumnsPlusPlusData::separatedValuesToTabs() {
    if (runCsvDialog(*this)) return;

    const char activeEncode = csv.encodingStyle == CsvSettings::TNR ? csv.encodeTNR
                            : csv.encodingStyle == CsvSettings::URL ? csv.encodeURL : 0;
    std::string commonStop = "\t\n\r";
    if (csv.escape) commonStop += csv.escapeChar;
    if (activeEncode && (!csv.escape || activeEncode != csv.escapeChar)) commonStop += activeEncode;
    const std::string stopUnquoted = commonStop + csv.separator;
    const std::string subTab = csv.encodingStyle == CsvSettings::TNR ? std::string(1, csv.encodeTNR) + "t"
                             : csv.encodingStyle == CsvSettings::URL ? std::string(1, csv.encodeURL) + "09" : csv.replaceTab;
    const std::string subLF  = csv.encodingStyle == CsvSettings::TNR ? std::string(1, csv.encodeTNR) + "n"
                             : csv.encodingStyle == CsvSettings::URL ? std::string(1, csv.encodeURL) + "0A" : csv.replaceLF;
    const std::string subCR  = csv.encodingStyle == CsvSettings::TNR ? std::string(1, csv.encodeTNR) + "r"
                             : csv.encodingStyle == CsvSettings::URL ? std::string(1, csv.encodeURL) + "0D" : csv.replaceCR;
    std::string subEnc;
    if (csv.encodingStyle == CsvSettings::TNR) subEnc = std::string(2, csv.encodeTNR);
    else if (csv.encodingStyle == CsvSettings::URL) {
        char c = csv.encodeURL;
        subEnc += csv.encodeURL;
        subEnc += "0123456789ABCDEF"[(c >> 4) & 15];
        subEnc += "0123456789ABCDEF"[c & 15];
    }
    const std::string subEsc = (csv.encodingStyle == CsvSettings::TNR && csv.encodeTNR == csv.escapeChar)
                            || (csv.encodingStyle == CsvSettings::URL && csv.encodeURL == csv.escapeChar) ? subEnc : std::string(1, csv.escapeChar);
    const std::string subSep = (csv.encodingStyle == CsvSettings::TNR && csv.encodeTNR == csv.separator)
                            || (csv.encodingStyle == CsvSettings::URL && csv.encodeURL == csv.separator) ? subEnc : std::string(1, csv.separator);

    BeginUndoAction();
    int selectionCount = Selections();
    for (int selectionNumber = 0; selectionNumber < selectionCount; ++selectionNumber) {
        SciPosition start = SelectionNStart(selectionNumber);
        SciPosition end = SelectionNEnd(selectionNumber);
        if (start != end) SetTargetRange(start, end);
        else if (selectionCount == 1) TargetWholeDocument();
        else continue;

        const std::string s = TargetText();
        std::string t;
        for (size_t start_of_field = 0; start_of_field < s.length();) {
            size_t p = s.find_first_not_of(' ', start_of_field);
            if (p == std::string::npos) {
                if (csv.preserveQuotes) t.append(s.length() - start_of_field, ' ');
                break;
            }
            if (csv.preserveQuotes) t.append(p - start_of_field, ' ');

            if ((s[p] == '"' && csv.quote) || (s[p] == '\'' && csv.apostrophe)) {
                char quote = s[p];
                const std::string stop = commonStop + quote;
                if (csv.preserveQuotes) t += quote;
                ++p;
                for (;;) {
                    size_t q = s.find_first_of(stop, p);
                    if (q == std::string::npos) { t += s.substr(p); p = s.length(); break; }
                    t += s.substr(p, q - p);
                    p = q + 1;
                    if      (s[q] == '\t') t += subTab;
                    else if (s[q] == '\n') t += subLF;
                    else if (s[q] == '\r') t += subCR;
                    else if (s[q] == csv.escapeChar && csv.escape) {
                        if (p == s.length()) { t += subEsc; break; }
                        else {
                            if (csv.preserveQuotes) t += subEsc;
                            if (s[p] == csv.escapeChar) { t += subEsc; ++p; }
                            else if (s[p] == quote) { t += quote; ++p; }
                        }
                    }
                    else if (s[q] == quote) {
                        if (csv.preserveQuotes) t += quote;
                        if (p < s.length() && s[p] == quote) { t += quote; ++p; }
                        else break;
                    }
                    else if (csv.encodingStyle == CsvSettings::TNR) t += subEnc;
                    else
                        t += (p < s.length() - 1 && (unsigned char)s[p] <= 'f' && (unsigned char)s[p + 1] <= 'f'
                              && std::isxdigit((unsigned char)s[p]) && std::isxdigit((unsigned char)s[p + 1])) ? subEnc : s.substr(q, 1);
                }
            }

            for (;;) {
                size_t q = s.find_first_of(stopUnquoted, p);
                if (q == std::string::npos || s[q] == csv.separator || s[q] == '\n' || s[q] == '\r') {
                    if (csv.preserveQuotes) t += s.substr(p, q - p);
                    else if (q > p) {
                        size_t u = s.find_last_not_of(' ', q - 1);
                        if (u != std::string::npos && u >= p) t += s.substr(p, u - p + 1);
                    }
                    if (q == std::string::npos) start_of_field = std::string::npos;
                    else {
                        if (s[q] == csv.separator && csv.keepSeparator) t += s[q];
                        t += s[q] == csv.separator ? '\t' : s[q];
                        start_of_field = q + 1;
                    }
                    break;
                }
                t += s.substr(p, q - p);
                p = q + 1;
                if (s[q] == '\t') t += subTab;
                else if (s[q] == csv.escapeChar && csv.escape) {
                    if (p == s.length()) { t += subEsc; start_of_field = std::string::npos; break; }
                    if (csv.preserveQuotes) t += subEsc;
                    if      (s[p] == csv.escapeChar) { t += subEsc; ++p; }
                    else if (s[p] == csv.separator ) { t += subSep; ++p; }
                    else if (s[p] == '\n'          ) { t += subLF ; ++p; }
                    else if (s[p] == '\r'          ) { t += subCR ; ++p; }
                }
                else if (csv.encodingStyle == CsvSettings::TNR) t += subEnc;
                else
                    t += (p < s.length() - 1 && (unsigned char)s[p] <= 'f' && (unsigned char)s[p + 1] <= 'f'
                          && std::isxdigit((unsigned char)s[p]) && std::isxdigit((unsigned char)s[p + 1])) ? subEnc : s.substr(q, 1);
            }
        }
        ReplaceTarget(t);
    }
    EndUndoAction();
    if (settings.elasticEnabled) { DocumentData& dd = *getDocument(); analyzeTabstops(dd); setTabstops(dd); }
}

void ColumnsPlusPlusData::tabsToSeparatedValues() {
    if (runCsvDialog(*this)) return;
    const std::string separator(1, csv.separator);
    const std::string escapeChar(1, csv.escapeChar);
    BeginUndoAction();
    int selectionCount = Selections();
    for (int selectionNumber = 0; selectionNumber < selectionCount; ++selectionNumber) {
        SciPosition start = SelectionNStart(selectionNumber);
        SciPosition end = SelectionNEnd(selectionNumber);
        if (start != end) SetTargetRange(start, end);
        else if (selectionCount == 1) TargetWholeDocument();
        else continue;
        const std::string s = TargetText();
        std::string t;
        for (size_t p = 0;;) {
            size_t q = s.find_first_of("\t\n\r", p);
            std::string field = s.substr(p,
                csv.keepSeparator && q != std::string::npos && q != 0 && s[q] == '\t' && s[q - 1] == csv.separator ? q - p - 1 : q - p);
            if (!field.empty()) {
                if (csv.encodingStyle == CsvSettings::TNR) {
                    for (size_t i = 0; (i = field.find_first_of(csv.encodeTNR, i)) < field.length() - 1; ++i) {
                        switch (field[i + 1]) {
                        case 't': field.replace(i, 2, "\t"); break;
                        case 'n': field.replace(i, 2, "\n"); break;
                        case 'r': field.replace(i, 2, "\r"); break;
                        default : if (field[i + 1] == csv.encodeTNR) field.erase(i, 1);
                        }
                    }
                }
                else if (csv.encodingStyle == CsvSettings::URL) {
                    for (size_t i = 0; (i = field.find_first_of(csv.encodeURL, i)) < field.length() - 2; ++i) {
                        if ((unsigned char)field[i + 1] > 'f' || !std::isxdigit((unsigned char)field[i + 1])) continue;
                        if ((unsigned char)field[i + 2] > 'f' || !std::isxdigit((unsigned char)field[i + 2])) continue;
                        field[i] = (char)stoi(field.substr(i + 1, 2), 0, 16);
                        field.erase(i + 1, 2);
                    }
                }
                bool reduces = field[0] == ' ' || field.back() == ' '
                            || (field[0] == '"' && csv.quote) || (field[0] == '\'' && csv.apostrophe);
                if ((csv.preserveQuotes || !reduces) && isValidCsvField(field, csv, !csv.preserveQuotes)) t += field;
                else if (csv.quote || csv.apostrophe) {
                    const char quote = !csv.quote ? '\''
                                     : !csv.apostrophe ? '"'
                                     : std::count(field.begin(), field.end(), '"') > std::count(field.begin(), field.end(), '\'') ? '\'' : '"';
                    t += quote;
                    for (size_t i = 0; i < field.length(); ++i) {
                        if (field[i] == quote || (field[i] == csv.escapeChar && csv.escape)) t += field[i];
                        t += field[i];
                    }
                    t += quote;
                }
                else if (csv.escape) {
                    size_t firstNonBlank = field.find_first_not_of(' ');
                    if (firstNonBlank == std::string::npos)
                        for (size_t i = 0; i < field.length(); ++i) { t += escapeChar; t += ' '; }
                    else {
                        size_t lastNonBlank = field.find_last_not_of(' ');
                        for (size_t i = 0; i < firstNonBlank; ++i) { t += escapeChar; t += ' '; }
                        for (size_t i = firstNonBlank; i <= lastNonBlank; ++i) {
                            if (field[i] == csv.separator || field[i] == csv.escapeChar || field[i] == '\n' || field[i] == '\r') t += csv.escapeChar;
                            t += field[i];
                        }
                        for (size_t i = lastNonBlank + 1; i < field.length(); ++i) { t += escapeChar; t += ' '; }
                    }
                }
                else {
                    EndUndoAction();
                    cppAlert("Convert tabs to separated values",
                             "The selection cannot be converted without quotes or escape characters, but none are enabled.");
                    return;
                }
            }
            if (q == std::string::npos) break;
            if (s[q] == '\t') t += separator;
                         else t += s[q];
            p = q + 1;
        }
        ReplaceTarget(t);
    }
    EndUndoAction();
    if (settings.elasticEnabled) { DocumentData& dd = *getDocument(); analyzeTabstops(dd); setTabstops(dd); }
}

void ColumnsPlusPlusData::tabsToSpaces() {
    DocumentData& dd = *getDocument();

    std::vector<std::pair<SciPosition, SciPosition>> selections;
    if (SelectionEmpty()) selections.emplace_back(0, Length());
    else {
        int n = Selections();
        for (int i = 0; i < n; ++i) selections.emplace_back(SelectionNStart(i), SelectionNEnd(i));
        std::sort(selections.begin(), selections.end(),
            [](const std::pair<SciPosition,SciPosition>& x, const std::pair<SciPosition,SciPosition>& y) { return x.first > y.first; });
    }

    if (settings.elasticEnabled) {
        SciLine firstSelectedLine = LineFromPosition(selections.back().first);
        SciLine lastSelectedLine  = LineFromPosition(selections.front().second);
        setTabstops(dd, firstSelectedLine, lastSelectedLine);
    }

    const int blank1440 = TextWidth(STYLE_DEFAULT, std::string(1440, ' ').c_str());
    BeginUndoAction();

    for (auto& sel : selections) {
        SetTargetRange(sel.first, sel.second);
        std::string text = TargetText();
        std::string repl;
        for (size_t i = 0;;) {
            size_t j = text.find_first_of('\t', i);
            if (j == std::string::npos) { repl += text.substr(i); break; }
            repl += text.substr(i, j - i);
            int width = PointXFromPosition(sel.first + j + 1) - PointXFromPosition(sel.first + j);
            int count = (width * 2880 + blank1440) / (2 * blank1440);
            repl += std::string(count, ' ');
            i = j + 1;
        }
        ReplaceTarget(repl);
    }

    EndUndoAction();
    if (settings.elasticEnabled) { analyzeTabstops(dd); setTabstops(dd); }
}

// ===========================================================================
// SELECTION
// ===========================================================================
namespace {

struct SelectionInformation {
    ColumnsPlusPlusData& data;
    DocumentData* const  ddp;
    const int            blank1440;
    const int            count;
    const int            mode;
    SciLine top, bottom;
    int     left, right;

    int blankCount(intptr_t n) const { return static_cast<int>(n >= 0 ? (n * 2880 + blank1440) / (2 * blank1440) : (n * 2880 - blank1440) / (2 * blank1440)); }
    int blankWidth(intptr_t n) const { return static_cast<int>(n >= 0 ? (n * blank1440 + 720) / 1440 : (n * blank1440 - 720) / 1440); }

    SelectionInformation(ColumnsPlusPlusData& data) : data(data),
        ddp(data.settings.elasticEnabled ? data.getDocument() : nullptr),
        blank1440(data.TextWidth(STYLE_DEFAULT, std::string(1440, ' ').c_str())),
        count(data.Selections()), mode(data.SelectionMode()) {
        if (mode == SC_SEL_THIN || mode == SC_SEL_RECTANGLE) {
            SciPosition anchor   = data.SelectionNAnchor(0);
            SciPosition anchorVs = data.SelectionNAnchorVirtualSpace(0);
            SciPosition caret    = data.SelectionNCaret(count - 1);
            SciPosition caretVs  = data.SelectionNCaretVirtualSpace(count - 1);
            SciLine lna = data.LineFromPosition(anchor);
            SciLine lnc = data.LineFromPosition(caret);
            int pxa = data.PointXFromPosition(anchor) + blankWidth(anchorVs);
            int pxc = data.PointXFromPosition(caret ) + blankWidth(caretVs );
            top    = std::min(lna, lnc);
            bottom = std::max(lna, lnc);
            left   = std::min(pxa, pxc);
            right  = std::max(pxa, pxc);
        }
        else {
            top    = std::numeric_limits<SciLine>::max();
            bottom = 0;
            left   = std::numeric_limits<int>::max();
            right  = std::numeric_limits<int>::min();
            for (int i = 0; i < count; ++i) {
                SciPosition p = data.SelectionNStart(i);
                SciPosition q = data.SelectionNEnd(i);
                SciLine pLine = data.LineFromPosition(p);
                SciLine qLine = data.LineFromPosition(q);
                top    = std::min(top, pLine);
                bottom = std::max(bottom, qLine);
                if (ddp) data.setTabstops(*ddp, pLine, qLine);
                if (pLine == qLine) {
                    left  = std::min(left , data.PointXFromPosition(p));
                    right = std::max(right, data.PointXFromPosition(q));
                }
                else {
                    left = data.PointXFromPosition(0);
                    for (SciLine j = pLine; j < qLine; ++j) right = std::max(right, data.PointXFromPosition(data.LineEndPosition(j)));
                    right = std::max(right, data.PointXFromPosition(q));
                }
            }
        }
    }

    void select(SciLine anchorLine, int anchorPx, SciLine caretLine, int caretPx) {
        if (ddp) data.setTabstops(*ddp, std::min(anchorLine, caretLine), std::max(anchorLine, caretLine));
        SciPosition anchor = data.positionFromLineAndPointX(anchorLine, anchorPx);
        SciPosition caret  = data.positionFromLineAndPointX(caretLine , caretPx );
        data.SetSelectionMode(SC_SEL_RECTANGLE);
        data.SetRectangularSelectionAnchor(anchor);
        data.SetRectangularSelectionAnchorVirtualSpace(blankCount(anchorPx - data.PointXFromPosition(anchor)));
        data.SetRectangularSelectionCaret(caret);
        data.SetRectangularSelectionCaretVirtualSpace(blankCount(caretPx - data.PointXFromPosition(caret)));
    }
};

} // namespace

void ColumnsPlusPlusData::selectDown() {
    SelectionInformation si(*this);
    SciLine lastLine = LineCount() - 1;
    if (PositionFromLine(lastLine) == LineEndPosition(lastLine) && lastLine > 0) --lastLine;
    si.select(lastLine, si.left, si.top, si.right);
}

void ColumnsPlusPlusData::selectEnclose() {
    SelectionInformation si(*this);
    si.select(si.top, si.left, si.bottom, si.right);
}

void ColumnsPlusPlusData::selectExtend() {
    SelectionInformation si(*this);
    if (si.top == si.bottom) {
        SciLine lastLine = LineCount() - 1;
        if (PositionFromLine(lastLine) == LineEndPosition(lastLine) && lastLine > 0) --lastLine;
        si.select(lastLine, si.left, 0, si.right);
    }
    else if (SelectionEmpty()) {
        if (si.ddp) setTabstops(*si.ddp, si.top, si.bottom);
        int right = si.left;
        for (SciLine i = si.top; i <= si.bottom; ++i) right = std::max(right, PointXFromPosition(LineEndPosition(i)));
        si.select(si.bottom, right, si.top, PointXFromPosition(0));
    }
    else si.select(si.top, si.left, si.bottom, si.right);
}

void ColumnsPlusPlusData::selectLeft() {
    SelectionInformation si(*this);
    si.select(si.top, PointXFromPosition(0), si.bottom, si.right);
}

void ColumnsPlusPlusData::selectRight() {
    SelectionInformation si(*this);
    if (si.ddp) setTabstops(*si.ddp, si.top, si.bottom);
    int right = si.left;
    for (SciLine i = si.top; i <= si.bottom; ++i) right = std::max(right, PointXFromPosition(LineEndPosition(i)));
    si.select(si.top, right, si.bottom, si.left);
}

void ColumnsPlusPlusData::selectUp() {
    SelectionInformation si(*this);
    si.select(0, si.left, si.bottom, si.right);
}
