// Columns++ for Notepad++ — macOS port.
//
// Original Windows plugin "Columns++ for Notepad++" Copyright 2023-2026 by
// Randall Joseph Fellmy <software@coises.com>, <http://www.coises.com/software/>.
// Released under the GNU General Public License v3 (or, at your option, any later
// version). See LICENSE.txt.
//
// This macOS port carries over the column-editing feature set faithfully through
// Scintilla's SCI_* message interface. The platform layer (Win32 dialogs/menus,
// HMENU/registry, GetTickCount progress dialogs) is replaced with AppKit and the
// host plugin API; the column-math, elastic-tabstops engine, rectangular-selection
// model, alignment, sort, numeric accumulation, conversion and the number
// parse/format code are ported as directly as the languages allow.
//
// CppData.h — the central data model, Scintilla wrapper and shared types.
// Mirrors the Windows ColumnsPlusPlus.h (RectangularSelection iteration model,
// ElasticTabsProfile, DocumentData, the NumericFormat/NumericParse value types
// and the ColumnsPlusPlusData "god object").

#pragma once

#include "NppPluginInterfaceLinux.h"
#include "Scintilla.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>

// ---------------------------------------------------------------------------
// Scintilla position/line aliases (match the Windows source's Scintilla:: types).
// ---------------------------------------------------------------------------
typedef intptr_t SciPosition;
typedef intptr_t SciLine;

// Scintilla selection-mode constants (SC_SEL_STREAM/RECTANGLE/LINES/THIN) come
// from Scintilla.h (the host header), which is included above.

// STYLE_DEFAULT (Scintilla predefined style index used for tab-width measuring).
#ifndef STYLE_DEFAULT
#define STYLE_DEFAULT 32
#endif

// ---------------------------------------------------------------------------
// Number formatting / parsing value types (verbatim semantics from the Windows
// NumericFormat.cpp). Internally everything is UTF-8 std::string.
// ---------------------------------------------------------------------------
class NumericFormat {
public:
    std::string thousands = "";   // thousands separator bytes (UTF-8), or "" for none
    int leftPad = 1;              // left pad to at least this many digits before the decimal
    int minDec  = -1;            // minimum digits after decimal; -1 = drop separator if none
    int maxDec  = 6;             // round to this many digits after the decimal
    int timeEnable = 1;          // bit mask: 8(4 seg)+4(3 seg)+2(2 seg)+1(1 seg)
};

class NumericParse {
public:
    double value = std::numeric_limits<double>::quiet_NaN();
    int decimalPlaces = 0;
    int timeSegments  = 0;
    operator bool()   const { return std::isfinite(value); }
    operator double() const { return value; }
};

// ---------------------------------------------------------------------------
// Elastic tabstops profile (ported verbatim from ColumnsPlusPlus.h).
// ---------------------------------------------------------------------------
class ElasticTabsProfile {
public:
    int minimumOrLeadingTabSize    = 4;
    int minimumSpaceBetweenColumns = 2;
    enum { MonospaceNever = 0, MonospaceAlways = 1, MonospaceBest = 2 } monospace = MonospaceBest;
    bool leadingTabsIndent    = false;
    bool lineUpAll            = false;
    bool treatEolAsTab        = false;
    bool overrideTabSize      = false;
    bool monospaceNoMnemonics = true;
    bool operator==(const ElasticTabsProfile&) const = default;
};

class DocumentDataSettings : public ElasticTabsProfile {
public:
    std::string profileName;
    bool elasticEnabled          = false;
    bool decimalSeparatorIsComma = false;
};

// A node of the per-document tab layout tree: a vertical block of lines that share
// a tab column, with a computed pixel width and the blocks to its right.
class TabLayoutBlock {
public:
    SciLine firstLine, lastLine;
    int width;
    std::vector<TabLayoutBlock> right;
    TabLayoutBlock(SciLine line, int width = 0) : firstLine(line), lastLine(line), width(width) {}
};

class DocumentData {
public:
    DocumentDataSettings settings;
    std::vector<TabLayoutBlock> tabLayouts;
    uintptr_t buffer = 0;                          // Notepad++ buffer id
    int  width24b = 0;                             // widths of 24 blanks / digits / W's
    int  width24d = 0;                             //   when tabLayouts were last computed;
    int  width24w = 0;                             //   if these change, full re-analysis needed
    int  tabOriginal = 4;                          // tab width to restore when elastic turned off
    bool elasticAnalysisRequired   = false;
    bool deleteWithoutLayoutChange = false;
    bool assumeMonospace           = false;
    SciPosition deleteWithoutLayoutChangePosition = 0;
    SciPosition deleteWithoutLayoutChangeLength   = 0;
};

// ---------------------------------------------------------------------------
// Per-feature persisted settings (ported from ColumnsPlusPlus.h). All subsystems
// are ported, so the Search / Calculate / Timestamps settings are included below.
// ---------------------------------------------------------------------------
class CsvSettings {
public:
    std::string replaceTab = "(TAB)";
    std::string replaceLF  = "(LF)";
    std::string replaceCR  = "(CR)";
    char separator  = ',';
    char escapeChar = '\\';
    char encodeTNR  = '\\';
    char encodeURL  = '%';
    bool quote          = true;
    bool apostrophe     = false;
    bool escape         = false;
    bool preserveQuotes = false;
    bool keepSeparator  = false;
    enum { Replace = 0, TNR = 1, URL = 2 } encodingStyle = Replace;
};

class SortSettings {
public:
    std::vector<std::string> regexHistory;
    std::vector<std::string> keygroupHistory;
    std::string localeName;
    enum SortType {Binary, Locale, Numeric, Width           } sortType = Binary;
    enum KeyType  {EntireColumn, IgnoreBlanks, Tabbed, Regex} keyType  = EntireColumn;
    bool sortColumnSelectionOnly = false;
    bool sortDescending          = false;
    bool regexMatchCase          = false;
    bool regexUseKey             = false;
    bool localeCaseSensitive     = false;
    bool localeDigitsAsNumbers   = true;
    bool localeIgnoreDiacritics  = false;
    bool localeIgnoreSymbols     = false;
};

class AlignSettings {
public:
    std::vector<std::string> history;
    enum AlignOn {First, Last, Regex} alignOn = First;
    int  margin      = 0;
    bool marginRight = false;
    bool matchCase   = false;
};

class CalculateSettings {
public:
    std::vector<std::string> formulaHistory;   // Calculate: formula MRU (newest last)
    std::vector<std::string> regexHistory;     // Calculate: regex MRU (newest last)
    enum { None, Comma, Apostrophe, Blank } thousands = None;
    int  decimalPlaces = 2;
    int  timeSegments  = 0;
    bool decimalsFixed = false;
    bool autoDecimals  = true;
    bool insert        = true;
    bool addLine       = false;
    bool matchCase     = false;   // Calculate: regex match case
    bool skipUnmatched = false;   // Calculate: skip lines the regex does not match
    bool formatAsTime  = false;   // Calculate: format results as time
    bool tabbed        = true;    // Calculate: separate the new column with a tab
    bool aligned       = true;    // Calculate: pad the new column to keep numbers aligned
    bool left          = false;   // Calculate: insert the new column at the left
};

class RegexCalc;   // Search regex-calculate substitution engine (defined in Search.mm)

class SearchSettings {
public:
    enum { Normal = 0, Extended = 1, Regex = 2 } mode = Normal;
    bool backward              = false;
    bool wholeWord             = false;
    bool matchCase             = false;
    bool autoClear             = true;
    bool autoClearSelection    = false;
    bool autoSetSelection      = true;
    bool enableCustomIndicator = true;      // assign a custom indicator for column searches
    bool forceUserIndicator    = false;
    int  indicator             = 18;        // indicator used for searches (9..20 = custom)
    int  customAlpha           = 48;
    int  customColor           = 0x007898;
    int  customIndicator       = 18;
    int  userIndicator         = 18;
};

class SearchData : public SearchSettings {
public:
    void*    dialog   = nullptr;   // modeless find/replace panel controller (ObjC), null when closed
    intptr_t findStep = -1;        // start of last stepwise regex Find, or -1 if invalid for Replace
    intptr_t nullAt   = -1;        // avoid re-matching the same zero-length string
    bool     wrap     = false;     // next stepwise Find/Replace starts at document start (or end)
    std::vector<std::string> findHistory;
    std::vector<std::string> replaceHistory;
    std::unique_ptr<RegexCalc> regexCalc;
    std::function<void(const std::string&)> setMessage;      // dialog message sink (set by the panel)
    std::function<void()>                   syncFindButton;  // dialog Find-button label sync
    SearchData();
    ~SearchData();
};

// Timestamp ⇄ datetime conversion settings (Timestamps.mm). Counters are int64
// tick counts (100 ns unit internally); epochs/units per counter type below.
class TimestampSettings {
public:
    enum class CounterType  { custom, Unix, File, Ex00, Ex04 };
    enum class DatePriority { custom, ymd, mdy, dmy };
    enum class DateFormat   { custom, iso8601, localeLong, localeShort };

    struct CounterSpec { int64_t epoch = 0; int64_t unit = 10000000; bool leap = false; };

    struct Counter {
        CounterSpec custom;
        CounterType type = CounterType::Unix;
        CounterSpec spec() const {
            switch (type) {
            case CounterType::Unix: return { 0                  ,     10000000, false };  // 1970-01-01, 1 second
            case CounterType::File: return { -116444736000000000,            1, true  };  // 1601-01-01, 100 ns
            case CounterType::Ex00: return { -22091616000000000 , 864000000000, false };  // 1899-12-30, 1 day
            case CounterType::Ex04: return { -20828448000000000 , 864000000000, false };  // 1904-01-01, 1 day
            default: break;
            }
            return custom;
        }
    };

    Counter      fromCounter;
    Counter      toCounter;
    std::string  localeName;
    std::string  fromZone;
    std::string  toZone;
    std::vector<std::string> dateParse;
    std::vector<std::string> datePicture;
    int          twoDigitYearLimit  = 2049;
    DatePriority datePriority       = DatePriority::ymd;
    DateFormat   dateFormat         = DateFormat::iso8601;
    bool         enableFromCounter  = true;
    bool         enableFromDatetime = true;
    bool         enableTzAndLocale  = false;
    bool         overwrite          = false;
};

class RectangularSelection;

// ---------------------------------------------------------------------------
// The plugin "god object" — owns Scintilla access, per-document state and all
// command implementations. One static instance lives in ColumnsPlusPlus.mm.
// ---------------------------------------------------------------------------
class ColumnsPlusPlusData {
public:

    NppData   nppData;            // host handles (set in setInfo)
    NppHandle activeScintilla = 0;

    int aboutMenuItem            = -1;
    int decimalSeparatorMenuItem = -1;
    int elasticEnabledMenuItem   = -1;
    bool sentADDSCNMODIFIEDFLAGS = false;

    std::map<void*, DocumentData>            documents;
    std::map<std::string, ElasticTabsProfile> profiles;
    std::map<std::string, std::string>        extensionToProfile = { {"", "*"}, {"*", "*"} };

    std::vector<char> view1TabsSet;
    std::vector<char> view2TabsSet;

    DocumentDataSettings settings;   // settings for last active document, or initial defaults
    CsvSettings       csv;
    SortSettings      sort;
    AlignSettings     align;
    CalculateSettings calc;
    SearchData        searchData;
    TimestampSettings timestamps;

    int  disableOverSize  = 1000;    // KB; if <=0, inactive (and -value remembers last setting)
    int  disableOverLines = 5000;    // lines; same convention
    int  timeScalarUnit   = 3;       // 0=days 1=hours 2=minutes 3=seconds
    int  timePartialRule  = 3;       // interpretation of 2/3-segment times
    int  timeFormatEnable = 15;      // bit mask 8+4+2+1
    bool showOnMenuBar    = false;   // persisted (no-op on macOS, documented)
    bool replaceStaysPut  = false;
    bool extendSingleLine = false;
    bool extendFullLines  = false;
    bool extendZeroWidth  = false;

    // -- Scintilla access -----------------------------------------------------
    // sci(msg, w, l) sends to the active Scintilla. The typed helpers below mirror
    // the Windows ScintillaCall wrapper used throughout the source.
    intptr_t send(uint32_t msg, uintptr_t w = 0, intptr_t l = 0) const {
        return nppData._sendMessage(activeScintilla, msg, w, l);
    }
    intptr_t npp(uint32_t msg, uintptr_t w = 0, intptr_t l = 0) const {
        return nppData._sendMessage(nppData._nppHandle, msg, w, l);
    }

    void updateActiveScintilla() {
        int which = -1;
        npp(NPPM_GETCURRENTSCINTILLA, 0, (intptr_t)&which);
        activeScintilla = which == 0 ? nppData._scintillaMainHandle
                        : which == 1 ? nppData._scintillaSecondHandle
                        : nppData._scintillaMainHandle;
    }

    // Typed Scintilla helpers (only those the port uses).
    SciPosition Length()                 const { return send(SCI_GETLENGTH); }
    SciLine     LineCount()              const { return send(SCI_GETLINECOUNT); }
    SciLine     LinesOnScreen()          const { return send(SCI_LINESONSCREEN); }
    SciLine     FirstVisibleLine()       const { return send(SCI_GETFIRSTVISIBLELINE); }
    SciPosition PositionFromLine(SciLine l)  const { return send(SCI_POSITIONFROMLINE, (uintptr_t)l); }
    SciPosition LineEndPosition(SciLine l)   const { return send(SCI_GETLINEENDPOSITION, (uintptr_t)l); }
    SciLine     LineFromPosition(SciPosition p) const { return send(SCI_LINEFROMPOSITION, (uintptr_t)p); }
    SciPosition PositionAfter(SciPosition p)  const { return send(SCI_POSITIONAFTER, (uintptr_t)p); }
    int         CharacterAt(SciPosition p)    const { return (int)send(SCI_GETCHARAT, (uintptr_t)p); }
    int         StyleIndexAt(SciPosition p)   const { return (int)send(SCI_GETSTYLEAT, (uintptr_t)p); }
    SciPosition CountCharacters(SciPosition a, SciPosition b) const { return send(SCI_COUNTCHARACTERS, (uintptr_t)a, (intptr_t)b); }
    int         PointXFromPosition(SciPosition p) const { return (int)send(SCI_POINTXFROMPOSITION, 0, (intptr_t)p); }
    int         PointYFromPosition(SciPosition p) const { return (int)send(SCI_POINTYFROMPOSITION, 0, (intptr_t)p); }
    int         TabWidth()               const { return (int)send(SCI_GETTABWIDTH); }
    void        SetTabWidth(int w)             { send(SCI_SETTABWIDTH, (uintptr_t)w); }
    void        SetTabIndents(bool on)         { send(SCI_SETTABINDENTS, on ? 1 : 0); }
    int         GetNextTabStop(SciLine line, int x) const { return (int)send(SCI_GETNEXTTABSTOP, (uintptr_t)line, (intptr_t)x); }
    void        ClearTabStops(SciLine line)    { send(SCI_CLEARTABSTOPS, (uintptr_t)line); }
    void        AddTabStop(SciLine line, int x){ send(SCI_ADDTABSTOP, (uintptr_t)line, (intptr_t)x); }
    int         ControlCharSymbol()      const { return (int)send(SCI_GETCONTROLCHARSYMBOL); }
    void        SetControlCharSymbol(int c)    { send(SCI_SETCONTROLCHARSYMBOL, (uintptr_t)c); }
    int         CodePage()               const { return (int)send(SCI_GETCODEPAGE); }
    int         EOLMode()                const { return (int)send(SCI_GETEOLMODE); }
    void        BeginUndoAction()              { send(SCI_BEGINUNDOACTION); }
    void        EndUndoAction()                { send(SCI_ENDUNDOACTION); }
    void        ChooseCaretX()                 { send(SCI_CHOOSECARETX); }
    void*       DocPointer()             const { return (void*)send(SCI_GETDOCPOINTER); }
    int         Selections()             const { return (int)send(SCI_GETSELECTIONS); }
    int         SelectionMode()          const { return (int)send(SCI_GETSELECTIONMODE); }
    bool        SelectionEmpty()         const { return send(SCI_GETSELECTIONEMPTY) != 0; }
    void        SetSelectionMode(int m)        { send(SCI_SETSELECTIONMODE, (uintptr_t)m); }
    SciPosition CurrentPos()             const { return send(SCI_GETCURRENTPOS); }
    SciPosition Anchor()                 const { return send(SCI_GETANCHOR); }
    void        SetSel(SciPosition a, SciPosition c) { send(SCI_SETSEL, (uintptr_t)a, (intptr_t)c); }

    int TextWidth(int style, const char* text) const { return (int)send(SCI_TEXTWIDTH, (uintptr_t)style, (intptr_t)text); }

    SciPosition SelectionNAnchor(int i)              const { return send(SCI_GETSELECTIONNANCHOR, (uintptr_t)i); }
    SciPosition SelectionNCaret(int i)               const { return send(SCI_GETSELECTIONNCARET, (uintptr_t)i); }
    SciPosition SelectionNAnchorVirtualSpace(int i)  const { return send(SCI_GETSELECTIONNANCHORVIRTUALSPACE, (uintptr_t)i); }
    SciPosition SelectionNCaretVirtualSpace(int i)   const { return send(SCI_GETSELECTIONNCARETVIRTUALSPACE, (uintptr_t)i); }
    SciPosition SelectionNStart(int i)               const { return send(SCI_GETSELECTIONNSTART, (uintptr_t)i); }
    SciPosition SelectionNEnd(int i)                 const { return send(SCI_GETSELECTIONNEND, (uintptr_t)i); }
    SciPosition SelectionNStartVirtualSpace(int i)   const { return send(SCI_GETSELECTIONNSTARTVIRTUALSPACE, (uintptr_t)i); }
    void SetSelectionNAnchor(int i, SciPosition p)             { send(SCI_SETSELECTIONNANCHOR, (uintptr_t)i, (intptr_t)p); }
    void SetSelectionNCaret(int i, SciPosition p)              { send(SCI_SETSELECTIONNCARET, (uintptr_t)i, (intptr_t)p); }
    void SetSelectionNAnchorVirtualSpace(int i, SciPosition v) { send(SCI_SETSELECTIONNANCHORVIRTUALSPACE, (uintptr_t)i, (intptr_t)v); }
    void SetSelectionNCaretVirtualSpace(int i, SciPosition v)  { send(SCI_SETSELECTIONNCARETVIRTUALSPACE, (uintptr_t)i, (intptr_t)v); }

    SciPosition RectangularSelectionAnchor()             const { return send(SCI_GETRECTANGULARSELECTIONANCHOR); }
    SciPosition RectangularSelectionCaret()              const { return send(SCI_GETRECTANGULARSELECTIONCARET); }
    SciPosition RectangularSelectionAnchorVirtualSpace() const { return send(SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE); }
    SciPosition RectangularSelectionCaretVirtualSpace()  const { return send(SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE); }
    void SetRectangularSelectionAnchor(SciPosition p)             { send(SCI_SETRECTANGULARSELECTIONANCHOR, 0, (intptr_t)p); }
    void SetRectangularSelectionCaret(SciPosition p)              { send(SCI_SETRECTANGULARSELECTIONCARET, 0, (intptr_t)p); }
    void SetRectangularSelectionAnchorVirtualSpace(SciPosition v) { send(SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE, 0, (intptr_t)v); }
    void SetRectangularSelectionCaretVirtualSpace(SciPosition v)  { send(SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE, 0, (intptr_t)v); }

    void SetTargetRange(SciPosition a, SciPosition b) { send(SCI_SETTARGETRANGE, (uintptr_t)a, (intptr_t)b); }
    void SetTargetStartVirtualSpace(SciPosition v)    { send(SCI_SETTARGETSTARTVIRTUALSPACE, (uintptr_t)v); }
    void SetTargetEndVirtualSpace(SciPosition v)      { send(SCI_SETTARGETENDVIRTUALSPACE, (uintptr_t)v); }
    void TargetWholeDocument()                        { send(SCI_TARGETWHOLEDOCUMENT); }
    void SetSearchFlags(int f)                        { send(SCI_SETSEARCHFLAGS, (uintptr_t)f); }
    SciPosition SearchInTarget(const std::string& s)  { return send(SCI_SEARCHINTARGET, (uintptr_t)s.length(), (intptr_t)s.data()); }
    SciPosition ReplaceTarget(const std::string& s)   { return send(SCI_REPLACETARGET, (uintptr_t)s.length(), (intptr_t)s.data()); }
    void InsertText(SciPosition p, const char* s)     { send(SCI_INSERTTEXT, (uintptr_t)p, (intptr_t)s); }
    void CopyText(const std::string& s)               { send(SCI_COPYTEXT, (uintptr_t)s.length(), (intptr_t)s.data()); }

    // -- indicators / target-end / viewport / call-tip / selection (Search) ---
    void        SetIndicatorCurrent(int i)                          { send(SCI_SETINDICATORCURRENT, (uintptr_t)i); }
    void        SetIndicatorValue(int v)                            { send(SCI_SETINDICATORVALUE, (uintptr_t)v); }
    void        IndicatorFillRange(SciPosition p, SciPosition len)  { send(SCI_INDICATORFILLRANGE, (uintptr_t)p, (intptr_t)len); }
    void        IndicatorClearRange(SciPosition p, SciPosition len) { send(SCI_INDICATORCLEARRANGE, (uintptr_t)p, (intptr_t)len); }
    int         IndicatorValueAt(int i, SciPosition p)  const { return (int)send(SCI_INDICATORVALUEAT, (uintptr_t)i, (intptr_t)p); }
    SciPosition IndicatorStart(int i, SciPosition p)    const { return send(SCI_INDICATORSTART, (uintptr_t)i, (intptr_t)p); }
    SciPosition IndicatorEnd(int i, SciPosition p)      const { return send(SCI_INDICATOREND, (uintptr_t)i, (intptr_t)p); }
    void        IndicSetStyle(int i, int s)                { send(SCI_INDICSETSTYLE, (uintptr_t)i, (intptr_t)s); }
    void        IndicSetFore(int i, int c)                 { send(SCI_INDICSETFORE, (uintptr_t)i, (intptr_t)c); }
    void        IndicSetAlpha(int i, int a)                { send(SCI_INDICSETALPHA, (uintptr_t)i, (intptr_t)a); }
    void        IndicSetUnder(int i, bool u)               { send(SCI_INDICSETUNDER, (uintptr_t)i, u ? 1 : 0); }
    SciPosition TargetEnd()                             const { return send(SCI_GETTARGETEND); }
    void        CallTipCancel()                            { send(SCI_CALLTIPCANCEL); }
    void        CallTipShow(SciPosition p, const char* s)  { send(SCI_CALLTIPSHOW, (uintptr_t)p, (intptr_t)s); }
    void        SetVisiblePolicy(int policy, int slop)     { send(SCI_SETVISIBLEPOLICY, (uintptr_t)policy, (intptr_t)slop); }
    void        EnsureVisibleEnforcePolicy(SciLine l)      { send(SCI_ENSUREVISIBLEENFORCEPOLICY, (uintptr_t)l); }
    int         XOffset()                               const { return (int)send(SCI_GETXOFFSET); }
    void        SetXOffset(int x)                          { send(SCI_SETXOFFSET, (uintptr_t)x); }
    void        ScrollRange(SciPosition a, SciPosition b)  { send(SCI_SCROLLRANGE, (uintptr_t)a, (intptr_t)b); }
    void        SetMainSelection(int i)                    { send(SCI_SETMAINSELECTION, (uintptr_t)i); }
    void        AddSelection(SciPosition caret, SciPosition anchor) { send(SCI_ADDSELECTION, (uintptr_t)caret, (intptr_t)anchor); }
    void        SetEmptySelection(SciPosition p)           { send(SCI_SETEMPTYSELECTION, (uintptr_t)p); }
    SciPosition SelectionStart()                        const { return send(SCI_GETSELECTIONSTART); }
    SciPosition SelectionEnd()                          const { return send(SCI_GETSELECTIONEND); }
    int         MarginRight()                           const { return (int)send(SCI_GETMARGINRIGHT); }

    // StringOfRange — SCI_GETTEXTRANGEFULL byte range [a,b).
    std::string StringOfRange(SciPosition a, SciPosition b) const;
    std::string TargetText() const;

    // -- file/profile helpers -------------------------------------------------
    std::string getFilePath() const;
    std::string getFilePath(uintptr_t buffer) const;
    std::string getFileExtension() const;
    std::string getFileExtension(uintptr_t buffer) const;
    std::string getFileExtension(const std::string& filepath) const;

    DocumentData* getDocument();                 // by current doc pointer (current view)
    DocumentData* getDocument(NppHandle scintilla);

    std::vector<char>* getLineTabsSet() {
        if (activeScintilla == nppData._scintillaMainHandle  ) return &view1TabsSet;
        if (activeScintilla == nppData._scintillaSecondHandle) return &view2TabsSet;
        return nullptr;
    }

    bool bothViewsShowSameDocument();

    RectangularSelection getRectangularSelection();

    // -- elastic tabstops geometry helpers (verbatim from ColumnsPlusPlus.h) ---
    SciPosition positionFromLineAndPointX(SciLine line, int px);
    int unwrappedWidth(SciPosition from, SciPosition to);

    bool fontSpacingChange(DocumentData& dd) {
        if (dd.width24b != TextWidth(STYLE_DEFAULT, "                        ")) return true;
        if (dd.width24d != TextWidth(STYLE_DEFAULT, "123456789012345678901234")) return true;
        if (dd.width24w != TextWidth(STYLE_DEFAULT, "WWWWWWWWWWWWWWWWWWWWWWWW")) return true;
        return false;
    }
    bool guessMonospaced();

    void reselectRectangularSelection(DocumentData& dd);

    // -- elastic tabstops engine (ElasticTabstops.mm) -------------------------
    void analyzeTabstops(DocumentData& dd);
    bool findTabLayoutBlock(DocumentData& dd, SciPosition position, SciPosition length, TabLayoutBlock*& tlb, int& width);
    void setTabstops(DocumentData& dd, SciLine firstNeeded = -1, SciLine lastNeeded = -1, bool skipChooseCaretX = false);

    // -- notification handlers (ElasticTabstops.mm) ---------------------------
    void bufferActivated();
    void scnModified(const SCNotification* scnp);
    void scnUpdateUI(const SCNotification* scnp);
    void scnZoom(const SCNotification* scnp);
    void fileSaved();

    void toggleElasticEnabled();
    void toggleDecimalSeparator();

    // -- configuration (Configuration.mm) -------------------------------------
    void initializeBuiltinElasticTabstopsProfiles();
    void loadConfiguration();
    void saveConfiguration();

    // -- number parse/format (NumericFormat.mm) -------------------------------
    std::string  formatNumber(double value, const NumericFormat& format) const;
    NumericParse parseNumber(const std::string& text);
    bool         getNumericAlignment(const std::string& text, size_t& colonPosition, size_t& decimalPosition);

    // -- align (Commands.mm) --------------------------------------------------
    void alignLeft();
    void alignRight();
    void alignNumeric();
    void alignCustom();

    // -- sort (Commands.mm) ---------------------------------------------------
    void sortAscendingBinary();
    void sortDescendingBinary();
    void sortAscendingLocale();
    void sortDescendingLocale();
    void sortAscendingNumeric();
    void sortDescendingNumeric();
    void sortCustom();

    // -- numeric (Commands.mm) ------------------------------------------------
    void accumulate(bool isMean);
    void addNumbers()     { accumulate(false); }
    void averageNumbers() { accumulate(true ); }

    // -- conversion (Commands.mm) ---------------------------------------------
    void separatedValuesToTabs();
    void tabsToSeparatedValues();
    void tabsToSpaces();

    // -- selection (Commands.mm) ----------------------------------------------
    void selectUp();
    void selectDown();
    void selectLeft();
    void selectRight();
    void selectEnclose();
    void selectExtend();

    // -- dialogs (Dialogs.mm) -------------------------------------------------
    void showOptionsDialog();
    void showElasticProfile();
    void showAboutDialog();

    // Search in region (Search.mm) — modeless find/replace over a rectangular or
    // multiple-selection region, with regex + regex-calculate substitutions.
    enum SearchRegionStatus { SearchRegionNotReady, SearchRegionReady, SearchRegionImpliedAll };
    SearchRegionStatus getSearchRegionStatus();
    void showSearchDialog();
    void searchCount(bool select, bool partial, bool before);
    void searchFind(bool postReplace = false);
    void searchReplace();
    void searchReplaceAll(bool partial, bool before);

    void calculate();               // Calculate (Calculate.mm)

    // Deferred (advanced) commands: explain once via NSAlert (Dialogs.mm).
    void convertTimestamps();
    void showTimeFormatsDialog();
};

// ---------------------------------------------------------------------------
// Codepage <-> wide helpers. Notepad++ on macOS works in UTF-8 (CodePage 65001);
// for the ported algorithms we keep a wide form for the few places the Windows
// source used std::wstring (number parsing's currency/sign tables). On macOS
// "wide" == char32_t code points.
// ---------------------------------------------------------------------------
std::u32string toWide32(const std::string& utf8);
std::string    fromWide32(const std::u32string& w);

// ---------------------------------------------------------------------------
// RectangularSelection — presents a rectangular/multiple selection as a container
// of rows, each a container of tab-delimited cells. Ported from ColumnsPlusPlus.h
// (Rectangular.mm holds the non-inline members).
// ---------------------------------------------------------------------------
class RectangularSelection_Row;
class RectangularSelection_Row_Iterator;
class RectangularSelection_Cell;
class RectangularSelection_Cell_Iterator;

class RectangularSelection {
    friend class RectangularSelection_Row;
    friend class RectangularSelection_Row_Iterator;
    friend class RectangularSelection_Cell;
    friend class RectangularSelection_Cell_Iterator;

public:
    struct Corner {
        SciPosition cp, vs, st, en;
        SciLine ln;
        int sx, px;
    };

private:
    Corner _anchor, _caret;
    int    _size;
    bool   _reverse;
    int    _mode;

public:
    ColumnsPlusPlusData& data;
    const int blank1440;
    const int tabWidth;

    RectangularSelection(ColumnsPlusPlusData& data);

    int  mode()        const { return _mode; }
    int  size()        const { return _size; }
    bool leftToRight() const { return _anchor.px <= _caret.px; }
    bool topToBottom() const { return _anchor.ln <= _caret.ln; }

    int blankCount(intptr_t n) const { return static_cast<int>(n >= 0 ? (n * 2880 + blank1440) / (2 * blank1440) : (n * 2880 - blank1440) / (2 * blank1440)); }
    int blankWidth(intptr_t n) const { return static_cast<int>(n >= 0 ? (n * blank1440 + 720) / 1440             : (n * blank1440 - 720) / 1440            ); }

    RectangularSelection& natural()         { _reverse = false;                return *this; }
    RectangularSelection& reverse()         { _reverse = !_reverse;            return *this; }
    RectangularSelection& reverse(bool yes) { _reverse = yes == topToBottom(); return *this; }

    RectangularSelection& extend();
    RectangularSelection& refit(bool addLine = false);

    const Corner& anchor() const { return _anchor; }
    const Corner& caret () const { return _caret;  }
    const Corner& top   () const { return topToBottom() ? _anchor : _caret; }
    const Corner& bottom() const { return topToBottom() ? _caret : _anchor; }
    const Corner& left  () const { return leftToRight() ? _anchor : _caret; }
    const Corner& right () const { return leftToRight() ? _caret : _anchor; }

    RectangularSelection_Row_Iterator begin() const;
    RectangularSelection_Row_Iterator end  () const;
    RectangularSelection_Row operator[](int index) const;
    RectangularSelection_Row front() const;
    RectangularSelection_Row back () const;

    Corner corner(SciPosition cp, SciPosition vs) const {
        Corner c;
        c.cp = cp;
        c.vs = vs;
        c.ln = data.LineFromPosition(c.cp);
        c.st = data.PositionFromLine(c.ln);
        c.en = data.LineEndPosition(c.ln);
        c.sx = data.PointXFromPosition(c.st);
        c.px = data.PointXFromPosition(c.cp) - c.sx + blankWidth(c.vs);
        return c;
    }
};

class RectangularSelection_Row {
    friend class RectangularSelection;
    friend class RectangularSelection_Row_Iterator;
    friend class RectangularSelection_Cell;
    friend class RectangularSelection_Cell_Iterator;
    SciPosition _cpAnchor, _cpCaret, _vsAnchor, _vsCaret, _endOfLine;
    SciLine     _line;
    size_t      _offset;
    std::string _text;
    RectangularSelection_Row(const RectangularSelection& rs, int index) : rs(rs), index(index) {
        _cpAnchor  = rs.data.SelectionNAnchor(index);
        _cpCaret   = rs.data.SelectionNCaret(index);
        _vsAnchor  = rs.data.SelectionNAnchorVirtualSpace(index);
        _vsCaret   = rs.data.SelectionNCaretVirtualSpace(index);
        _line      = rs.data.LineFromPosition(_cpAnchor);
        _endOfLine = rs.data.LineEndPosition(_line);
        if (rs.data.settings.elasticEnabled && rs.data.settings.leadingTabsIndent) {
            SciPosition startOfLine = rs.data.PositionFromLine(_line);
            _text   = rs.data.StringOfRange(startOfLine, cpMax());
            _offset = cpMin() - startOfLine;
        }
        else {
            _text   = rs.data.StringOfRange(cpMin(), cpMax());
            _offset = 0;
        }
    }
public:
    const RectangularSelection& rs;
    const int index;
    SciPosition cpAnchor   () const { return _cpAnchor; }
    SciPosition cpCaret    () const { return _cpCaret ; }
    SciPosition vsAnchor   () const { return _vsAnchor; }
    SciPosition vsCaret    () const { return _vsCaret ; }
    SciPosition cpMin      () const { return std::min(_cpAnchor, _cpCaret); }
    SciPosition cpMax      () const { return std::max(_cpAnchor, _cpCaret); }
    SciPosition vsMin      () const { return std::min(_vsAnchor, _vsCaret); }
    SciPosition vsMax      () const { return std::max(_vsAnchor, _vsCaret); }
    SciLine     line       () const { return _line; }
    SciPosition endOfLine  () const { return _endOfLine; }
    std::string_view text  () const { return std::string_view(_text).substr(_offset); }
    bool        isEndOfLine() const { return cpMax() == _endOfLine; }
    RectangularSelection_Cell_Iterator begin() const;
    RectangularSelection_Cell_Iterator end() const;
    void replace(const std::string& r) {
        rs.data.SetTargetRange(cpMin(), cpMax());
        rs.data.ReplaceTarget(r);
        if (vsMin()) {
            rs.data.SetSelectionNAnchor            (index, cpMin() + r.length());
            rs.data.SetSelectionNCaret             (index, cpMin() + r.length());
            rs.data.SetSelectionNAnchorVirtualSpace(index, 0);
            rs.data.SetSelectionNCaretVirtualSpace (index, 0);
        }
        else if (rs.leftToRight()) {
            rs.data.SetSelectionNCaret(index, cpMin() + r.length());
            rs.data.SetSelectionNCaretVirtualSpace(index, 0);
        }
        else {
            rs.data.SetSelectionNAnchor(index, cpMin() + r.length());
            rs.data.SetSelectionNAnchorVirtualSpace(index, 0);
        }
    }
};

class RectangularSelection_Cell {
    friend class RectangularSelection;
    friend class RectangularSelection_Row;
    friend class RectangularSelection_Row_Iterator;
    friend class RectangularSelection_Cell_Iterator;
    size_t _start;
    size_t _left;
    size_t _right;
    size_t _end;
    size_t _pastLeadingTabs;
    RectangularSelection_Cell(const RectangularSelection_Row& row, size_t start, size_t& next) : row(row), _start(start) {
        if (_start >= row._text.length()) {
            _start = _left = _right = _end = _pastLeadingTabs = row._text.length();
            next = std::string::npos;
            return;
        }
        _pastLeadingTabs = _start;
        if (_start == row._offset && row.rs.data.settings.elasticEnabled && row.rs.data.settings.leadingTabsIndent) {
            size_t firstNonTab = row._text.find_first_not_of('\t');
            if (firstNonTab > _start) _pastLeadingTabs = firstNonTab == std::string::npos ? row._text.length() : firstNonTab;
        }
        _end = row._text.find_first_of('\t', _pastLeadingTabs);
        if (_end == std::string::npos) {
            _end = row._text.length();
            next = std::string::npos;
        }
        else next = _end + 1;
        if (_end == _pastLeadingTabs) {
            _left = _right = _end;
            return;
        }
        _left = row._text.find_first_not_of(' ', _pastLeadingTabs);
        if (_left >= _end) {
            _left = _right = _end;
            return;
        }
        _right = row._text.find_last_not_of(' ', _end - 1) + 1;
    }
public:
    const RectangularSelection_Row& row;
    std::string text       () const { return row._text.substr(_start, _end   - _start); }
    std::string trim       () const { return row._text.substr(_left , _right - _left ); }
    std::string terminator () const { return isLastInRow() ? "" : "\t"; }
    intptr_t    textLength () const { return _end   - _start; }
    intptr_t    trimLength () const { return _right - _left ; }
    intptr_t    leading    () const { return (_pastLeadingTabs - _start) * row.rs.tabWidth + _left - _pastLeadingTabs; }
    intptr_t    trailing   () const { return _end - _right; }
    SciPosition start      () const { return row.cpMin() - row._offset + _start; }
    SciPosition left       () const { return row.cpMin() - row._offset + _left;  }
    SciPosition right      () const { return row.cpMin() - row._offset + _right; }
    SciPosition end        () const { return row.cpMin() - row._offset + _end;   }
    bool        isLastInRow() const { return _end == row._text.length(); }
    bool        isEndOfLine() const { return end() == row._endOfLine; }
};

class RectangularSelection_Row_Iterator {
    friend class RectangularSelection;
    friend class RectangularSelection_Row;
    const RectangularSelection& rs;
    int index;
    RectangularSelection_Row_Iterator(const RectangularSelection& rs, int index) : rs(rs), index(index) {}
public:
    bool operator!=(const RectangularSelection_Row_Iterator& rsri) { return index != rsri.index; }
    RectangularSelection_Row_Iterator& operator++() { rs._reverse ? --index : ++index; return *this; }
    RectangularSelection_Row operator*() { return RectangularSelection_Row(rs, index); }
};

class RectangularSelection_Cell_Iterator {
    friend class RectangularSelection;
    friend class RectangularSelection_Row;
    const RectangularSelection_Row& row;
    size_t start, next;
    RectangularSelection_Cell_Iterator(const RectangularSelection_Row& row, size_t start) : row(row), start(start), next(start) {}
public:
    bool operator!=(const RectangularSelection_Cell_Iterator& rsci) { return start != rsci.start; }
    RectangularSelection_Cell_Iterator& operator++() {
        if (next == start) RectangularSelection_Cell(row, start, next);
        start = next;
        return *this;
    }
    RectangularSelection_Cell operator*() { return RectangularSelection_Cell(row, start, next); }
};

inline RectangularSelection_Row_Iterator RectangularSelection::begin() const {
    return RectangularSelection_Row_Iterator(*this, _reverse ? _size - 1 : 0);
}
inline RectangularSelection_Row_Iterator RectangularSelection::end() const {
    return RectangularSelection_Row_Iterator(*this, _reverse ? -1 : _size);
}
inline RectangularSelection_Row RectangularSelection::operator[](int index) const {
    return RectangularSelection_Row(*this, index);
}
inline RectangularSelection_Row RectangularSelection::front() const { return (*this)[_reverse ? _size - 1 : 0]; }
inline RectangularSelection_Row RectangularSelection::back () const { return (*this)[_reverse ? 0 : _size - 1]; }

inline RectangularSelection_Cell_Iterator RectangularSelection_Row::begin() const {
    return RectangularSelection_Cell_Iterator(*this, _offset);
}
inline RectangularSelection_Cell_Iterator RectangularSelection_Row::end() const {
    return RectangularSelection_Cell_Iterator(*this, std::string::npos);
}

inline RectangularSelection ColumnsPlusPlusData::getRectangularSelection() {
    return RectangularSelection(*this).extend();
}

// Shared NSAlert helpers (defined in Dialogs.mm).
void cppAlert(const std::string& title, const std::string& message);
bool cppConfirm(const std::string& title, const std::string& message);   // OK/Cancel → true on OK
