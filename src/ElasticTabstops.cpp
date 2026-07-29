// Columns++ for Notepad++ — macOS port.
// ElasticTabstops.mm — the elastic-tabstops engine and notification handlers.
//
// This is the headline feature. It is ported faithfully from the Windows
// ColumnsPlusPlus.cpp. The mechanism:
//
//   analyzeTabstops(dd)  — scan the whole document, splitting each line on tabs,
//       and build a tree of TabLayoutBlock nodes. Each block spans a run of lines
//       that share a tab column; its `width` is the widest cell in that column
//       (measured in pixels via SCI_TEXTWIDTH / SCI_POINTXFROMPOSITION, the same
//       way Scintilla measures, so it is font- and encoding-correct). Blocks chain
//       to the right for subsequent tab columns.
//
//   setTabstops(dd, first, last) — for each line, walk the layout tree and push
//       explicit per-line tab stops into Scintilla via SCI_CLEARTABSTOPS +
//       SCI_ADDTABSTOP (cumulative pixel x of each tab column). Scintilla then
//       lays the text out with elastic columns. A monospaced-font fast path
//       (assumeMonospace) measures by character count; a second pass widens any
//       column whose measured width turns out larger than the estimate.
//
//   scnModified / scnUpdateUI / scnZoom — react to edits, scrolling and zoom by
//       re-analyzing (when the layout could have changed) and re-applying tab
//       stops to the visible range.
//
// DIFFERENCE FROM WINDOWS (documented):
//   * The Windows build shows a cancellable progress dialog for slow layout work,
//     paced by GetTickCount64. On macOS the work runs synchronously to completion
//     without a progress dialog; elastic tabstops is auto-disabled for files over
//     1000 KB / 5000 lines (the same default guard the Windows build uses), which
//     keeps the per-keystroke cost bounded. The stepwise loop structure is kept
//     (it is the analysis algorithm) but simply iterated to the end.
//   * The Windows build defers re-selecting a mouse-dragged rectangular selection
//     until the left mouse button is released, using GetKeyState(VK_LBUTTON) and a
//     Win32 timer. A plugin on macOS cannot poll the global mouse-button state
//     through the host plugin API, so the rectangular selection is re-fitted
//     directly on SCN_UPDATEUI. The visible effect is identical for keyboard
//     selections; for a live mouse drag the column edges settle on mouse-up
//     (the next UpdateUI) as before.

#include "CppData.h"

// SCN_MODIFIED flags / SCI_SETMODEVENTMASK bits we need.
#ifndef SC_MOD_INSERTTEXT
#define SC_MOD_INSERTTEXT 0x1
#define SC_MOD_DELETETEXT 0x2
#define SC_MOD_CHANGESTYLE 0x4
#define SC_MOD_BEFOREDELETE 0x800
#endif

namespace {

// Carries one analyze/setTabstops pass. Mirrors the Windows ElasticProgressInfo,
// minus the Win32 progress dialog — the loops simply run to completion.
struct ElasticPass {
    ColumnsPlusPlusData& data;
    DocumentData& dd;
    SciLine firstNeeded = 0;
    SciLine lastNeeded  = 0;
    SciLine lastMonospaceFail = -1;
    int  step       = 0;
    bool secondTime = false;

    const int ch1440;
    const int tabGap;
    const int tabInd;
    const int tabMin;

    static constexpr int stepSize = 100;

    std::vector<char>* lineTabsSet;

    ElasticPass(ColumnsPlusPlusData& data, DocumentData& dd)
        : data(data), dd(dd),
          ch1440(data.TextWidth(STYLE_DEFAULT, std::string(1440, '0').c_str())),
          tabGap(data.TextWidth(STYLE_DEFAULT, std::string(dd.settings.minimumSpaceBetweenColumns, ' ').c_str())),
          tabInd(data.TextWidth(STYLE_DEFAULT,
                 std::string(dd.settings.overrideTabSize ? dd.settings.minimumOrLeadingTabSize : data.TabWidth(), ' ').c_str())),
          tabMin(dd.settings.leadingTabsIndent ? 0 : tabInd),
          lineTabsSet(data.getLineTabsSet()) {}

    bool analyzeStep();
    bool setStep(bool stepless = false);
};

bool ElasticPass::analyzeStep() {
    auto& sci = data;
    const SciLine firstToProcess = step * stepSize;
    const SciLine lastToProcess  = std::min(lastNeeded, firstToProcess + stepSize - 1);
    if (!step) dd.tabLayouts.clear();
    for (SciLine lineNum = firstToProcess; lineNum <= lastToProcess; ++lineNum) {
        SciPosition begin = sci.PositionFromLine(lineNum);
        SciPosition end = sci.LineEndPosition(lineNum);
        if (begin == end) continue;
        std::string lineText = sci.StringOfRange(begin, end);
        if (dd.settings.treatEolAsTab) lineText += "\t";
        std::vector<TabLayoutBlock>* layouts = &dd.tabLayouts;
        size_t from = dd.settings.leadingTabsIndent ? lineText.find_first_not_of('\t') : 0;
        if (from == std::string::npos) continue;
        int indentSize = static_cast<int>(from) * tabInd;
        for (size_t tab; tab = lineText.find_first_of('\t', from), tab != std::string::npos;) {
            if (!layouts->size() || (!dd.settings.lineUpAll && layouts->back().lastLine < lineNum - 1)) layouts->emplace_back(lineNum, tabMin);
            TabLayoutBlock& tlb = layouts->back();
            tlb.lastLine = lineNum;
            int width = dd.assumeMonospace ? static_cast<int>((sci.CountCharacters(begin + from, begin + tab) * ch1440 + 720) / 1440)
                                           : data.unwrappedWidth(begin + from, begin + tab);
            width += tabGap + indentSize;
            indentSize = 0;
            if (width > tlb.width) tlb.width = width;
            from = tab + 1;
            layouts = &tlb.right;
        }
    }
    if (lastToProcess < lastNeeded) {
        ++step;
        return true;
    }
    return false;
}

bool ElasticPass::setStep(bool stepless) {
    auto& sci = data;
    SciLine firstToProcess = stepless ? firstNeeded : firstNeeded + step * stepSize;
    SciLine lastToProcess  = stepless ? lastNeeded  : std::min(lastNeeded, firstToProcess + stepSize - 1);
    size_t tlbIndex = 0;
    for (SciLine lineNum = firstToProcess; lineNum <= lastToProcess; ++lineNum) {
        if ((*lineTabsSet)[lineNum]) continue;
        (*lineTabsSet)[lineNum] = 1;
        while (tlbIndex < dd.tabLayouts.size() && dd.tabLayouts[tlbIndex].lastLine < lineNum) ++tlbIndex;
        if (tlbIndex >= dd.tabLayouts.size() || dd.tabLayouts[tlbIndex].firstLine > lineNum) {
            sci.ClearTabStops(lineNum);
            continue;
        }
        SciPosition lineStarts = sci.PositionFromLine(lineNum);
        SciPosition lineEnds   = sci.LineEndPosition(lineNum);
        if (lineStarts == lineEnds) continue;
        std::string line = sci.StringOfRange(lineStarts, lineEnds);
        std::vector<int> tabs;
        std::vector<size_t> tabOffsets;
        if (dd.settings.leadingTabsIndent) for (unsigned int i = 0; i < line.length() && line[i] == '\t'; ++i) {
            tabs.push_back((i + 1) * tabInd);
            tabOffsets.push_back(i);
        }
        size_t leadingTabCount = tabs.size();
        int eolLimit = 0;
        {
            int tabstop = 0;
            const TabLayoutBlock* tlb = &dd.tabLayouts[tlbIndex];
            for (size_t tabchar = leadingTabCount; tabchar = line.find_first_of('\t', tabchar), tabchar != std::string::npos; ++tabchar) {
                tabstop += tlb->width;
                tabs.push_back(tabstop);
                tabOffsets.push_back(tabchar);
                size_t i = 0;
                while (i < tlb->right.size() && tlb->right[i].lastLine < lineNum) ++i;
                if (i >= tlb->right.size() || tlb->right[i].firstLine > lineNum) break;
                tlb = &tlb->right[i];
            }
            if (dd.settings.treatEolAsTab && tlb->right.size()) eolLimit = tabstop + tlb->width;
        }
        if (leadingTabCount == tabs.size()) {
            sci.ClearTabStops(lineNum);
            if (!eolLimit) continue;
        }
        else {
            bool unchanged = true;
            int position = 0;
            for (size_t i = 0; i < tabs.size(); ++i) {
                position = sci.GetNextTabStop(lineNum, position);
                if (position != tabs[i]) {
                    unchanged = false;
                    break;
                }
            }
            if (!unchanged) {
                sci.ClearTabStops(lineNum);
                for (size_t i = 0; i < tabs.size(); ++i) sci.AddTabStop(lineNum, tabs[i]);
            }
        }
        if (!secondTime && dd.assumeMonospace) {
            int pxStart = sci.PointXFromPosition(lineStarts);
            if ( (tabs.size() > leadingTabCount && tabs.back() < sci.PointXFromPosition(lineStarts + tabOffsets.back() + 1) - pxStart)
              || (eolLimit                      && eolLimit < sci.PointXFromPosition(lineEnds) - pxStart + tabGap) ) {
                lastMonospaceFail = lineNum;
                if (eolLimit) tabOffsets.push_back(line.length());
                bool lineTabsInvalidated = false;
                size_t tabIndex = leadingTabCount;
                size_t from = 0;
                for (TabLayoutBlock* tlb = &dd.tabLayouts[tlbIndex]; tabIndex < tabOffsets.size(); ++tabIndex) {
                    int width = data.unwrappedWidth(lineStarts + from, lineStarts + tabOffsets[tabIndex]) + tabGap;
                    if (width > tlb->width) {
                        tlb->width = width;
                        if (!lineTabsInvalidated) {
                            for (SciLine ln = tlb->firstLine; ln <= tlb->lastLine; ++ln) (*lineTabsSet)[ln] = 0;
                            lineTabsInvalidated = true;
                        }
                    }
                    size_t i = 0;
                    if (i < tlb->right.size() && tlb->right[i].lastLine < lineNum) ++i;
                    if (i >= tlb->right.size() || tlb->right[i].firstLine > lineNum) break;
                    tlb = &tlb->right[i];
                    from = tabOffsets[tabIndex] + 1;
                }
            }
        }
    }
    if (lastToProcess < lastNeeded) {
        ++step;
        return true;
    }
    if (secondTime || lastMonospaceFail < 0) return false;
    secondTime = true;
    step = 0;
    if (stepless) return setStep(true);
    return true;
}

} // namespace

void ColumnsPlusPlusData::analyzeTabstops(DocumentData& dd) {
    ElasticPass epi(*this, dd);
    if (!epi.lineTabsSet) return;
    dd.elasticAnalysisRequired   = false;
    dd.deleteWithoutLayoutChange = false;
    dd.width24b = TextWidth(STYLE_DEFAULT, "                        ");
    dd.width24d = TextWidth(STYLE_DEFAULT, "123456789012345678901234");
    dd.width24w = TextWidth(STYLE_DEFAULT, "WWWWWWWWWWWWWWWWWWWWWWWW");
    dd.assumeMonospace = dd.settings.monospace == ElasticTabsProfile::MonospaceBest ? guessMonospaced()
                       : dd.settings.monospace == ElasticTabsProfile::MonospaceAlways;
    const int ccsym = settings.monospaceNoMnemonics && dd.assumeMonospace ? '!' : 0;
    const SciLine lineCount = LineCount();
    if (bothViewsShowSameDocument()) {
        view1TabsSet.assign(lineCount, 0);
        view2TabsSet.assign(lineCount, 0);
        nppData._sendMessage(nppData._scintillaMainHandle,   SCI_SETCONTROLCHARSYMBOL, (uintptr_t)ccsym, 0);
        nppData._sendMessage(nppData._scintillaSecondHandle, SCI_SETCONTROLCHARSYMBOL, (uintptr_t)ccsym, 0);
    }
    else {
        SetControlCharSymbol(ccsym);
        epi.lineTabsSet->assign(lineCount, 0);
    }
    epi.firstNeeded = 0;
    epi.lastNeeded  = lineCount - 1;
    while (epi.analyzeStep()) { /* run to completion */ }
}

void ColumnsPlusPlusData::setTabstops(DocumentData& dd, SciLine firstNeeded, SciLine lastNeeded, bool skipChooseCaretX) {
    ElasticPass epi(*this, dd);
    if (!epi.lineTabsSet) return;
    const SciLine lineCount     = LineCount();
    const SciLine linesOnScreen = LinesOnScreen();
    if (epi.lineTabsSet->size() != static_cast<size_t>(lineCount)) epi.lineTabsSet->assign(lineCount, 0);
    if (firstNeeded == -1) {
        epi.firstNeeded = FirstVisibleLine();
        epi.lastNeeded  = std::min(epi.firstNeeded + linesOnScreen, lineCount - 1);
        epi.setStep(true);
    }
    else {
        epi.firstNeeded = firstNeeded;
        epi.lastNeeded  = (lastNeeded < 0 || lastNeeded >= lineCount) ? lineCount - 1 : lastNeeded;
        while (epi.setStep()) { /* run to completion */ }
    }
    if (!skipChooseCaretX) {
        PointXFromPosition(0);  // clears Scintilla's stale ChooseCaretX cache (Windows note)
        ChooseCaretX();
    }
}

// Given a Scintilla position and length, find the tab layout block within which the
// characters occur. Verbatim from ColumnsPlusPlus.cpp.
bool ColumnsPlusPlusData::findTabLayoutBlock(DocumentData& dd, SciPosition position, SciPosition length, TabLayoutBlock*& tlb, int& width) {
    SciLine lineNum = LineFromPosition(position);
    SciPosition endLine = LineEndPosition(lineNum);
    if (position + length > endLine) {
        tlb = nullptr;
        width = -1;
        return false;
    }
    SciPosition beginLine = PositionFromLine(lineNum);
    std::string line_buffer = StringOfRange(beginLine, endLine);
    intptr_t lineLength = line_buffer.length();
    char* line = line_buffer.data();
    char* positionInString = line + position - beginLine;
    char* tabAfter = (char*)memchr(positionInString, '\t', lineLength - (positionInString - line));
    if (!tabAfter) {
        if (dd.settings.treatEolAsTab) {
            if (!dd.settings.lineUpAll && length == lineLength) {
                tlb = nullptr;
                width = -4;
                return false;
            }
            tabAfter = line + lineLength;
        }
        else {
            tlb = nullptr;
            width = 0;
            return true;
        }
    }
    if (tabAfter - positionInString < length) {
        tlb = nullptr;
        width = -2;
        return false;
    }
    int tabCount = 0;
    char* p = line;
    char* tabStopBefore = line;
    if (dd.settings.leadingTabsIndent) {
        while (p < positionInString && *p == '\t') ++p;
        if (positionInString == p && tabAfter - positionInString == length) {
            tlb = nullptr;
            width = -3;
            return false;
        }
    }
    while (p = (char*)memchr(p, '\t', positionInString - p), p) {
        ++tabCount;
        ++p;
        tabStopBefore = p;
    }
    width = PointXFromPosition(beginLine + (tabAfter - line)) - PointXFromPosition(beginLine + (tabStopBefore - line));
    std::vector<TabLayoutBlock>* layouts = &dd.tabLayouts;
    for (int n = 0; n <= tabCount; ++n) {
        for (size_t i = 0;; ++i) {
            if (i >= layouts->size()) {
                tlb = nullptr;
                width = -999;
                return false;
            }
            tlb = &(*layouts)[i];
            if (lineNum >= tlb->firstLine && lineNum <= tlb->lastLine) {
                layouts = &tlb->right;
                break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Notification handlers
// ---------------------------------------------------------------------------
void ColumnsPlusPlusData::scnModified(const SCNotification* scnp) {
    int mt = scnp->modificationType;
    if (!(mt & (SC_MOD_INSERTTEXT | SC_MOD_BEFOREDELETE | SC_MOD_DELETETEXT | SC_MOD_CHANGESTYLE))) return;
    DocumentData* ddp = getDocument((NppHandle)scnp->nmhdr.hwndFrom);
    if (!ddp) return;
    DocumentData& ctd = *ddp;
    if (!ctd.settings.elasticEnabled || ctd.elasticAnalysisRequired) return;
    if (mt & SC_MOD_INSERTTEXT) {
        ctd.deleteWithoutLayoutChange = false;
        if (scnp->linesAdded == 0) {
            TabLayoutBlock* tlb;
            int width;
            if (findTabLayoutBlock(ctd, scnp->position, scnp->length, tlb, width)) {
                if (tlb) {
                    width += TextWidth(STYLE_DEFAULT, std::string(ctd.settings.minimumSpaceBetweenColumns, ' ').c_str());
                    if (width > tlb->width) {
                        tlb->width = width;
                        auto lineTabsSet = getLineTabsSet();
                        if (lineTabsSet) {
                            if (bothViewsShowSameDocument())
                                 for (SciLine ln = tlb->firstLine; ln <= tlb->lastLine; ++ln) view1TabsSet[ln] = view2TabsSet[ln] = 0;
                            else for (SciLine ln = tlb->firstLine; ln <= tlb->lastLine; ++ln) (*lineTabsSet)[ln] = 0;
                        }
                    }
                }
                return;
            }
        }
    }
    else if (mt & SC_MOD_BEFOREDELETE) {
        TabLayoutBlock* tlb;
        int width;
        if (findTabLayoutBlock(ctd, scnp->position, scnp->length, tlb, width)) {
            width += TextWidth(STYLE_DEFAULT, std::string(ctd.settings.minimumSpaceBetweenColumns, ' ').c_str());
            if (!tlb || width < tlb->width - 1) {
                ctd.deleteWithoutLayoutChange         = true;
                ctd.deleteWithoutLayoutChangePosition = scnp->position;
                ctd.deleteWithoutLayoutChangeLength   = scnp->length;
                return;
            }
        }
        ctd.deleteWithoutLayoutChange = false;
        return;
    }
    else if (mt & SC_MOD_DELETETEXT) {
        if (ctd.deleteWithoutLayoutChange && scnp->position == ctd.deleteWithoutLayoutChangePosition && scnp->length == ctd.deleteWithoutLayoutChangeLength) {
            ctd.deleteWithoutLayoutChange = false;
            return;
        }
    }
    ctd.elasticAnalysisRequired = true;
}

void ColumnsPlusPlusData::scnUpdateUI(const SCNotification* scnp) {
    // SC_UPDATE_CONTENT 0x1, SC_UPDATE_SELECTION 0x2, SC_UPDATE_V_SCROLL 0x8
    int updated = scnp->updated;
    if (!(updated & (0x1 | 0x2 | 0x8))) return;
    DocumentData* ddp = getDocument((NppHandle)scnp->nmhdr.hwndFrom);
    if (!ddp) return;
    if (!ddp->settings.elasticEnabled) return;
    ddp->deleteWithoutLayoutChange = false;
    const bool fullAnalysis = (ddp->elasticAnalysisRequired || fontSpacingChange(*ddp));
    if (fullAnalysis) analyzeTabstops(*ddp);
    if (updated & 0x2) {
        int selectionMode = SelectionMode();
        if (selectionMode == SC_SEL_RECTANGLE || selectionMode == SC_SEL_THIN)
            reselectRectangularSelection(*ddp);
    }
    setTabstops(*ddp, -1, -1, !fullAnalysis);
}

void ColumnsPlusPlusData::scnZoom(const SCNotification* scnp) {
    DocumentData* ddp = getDocument((NppHandle)scnp->nmhdr.hwndFrom);
    if (!ddp || !ddp->settings.elasticEnabled) return;
    ddp->deleteWithoutLayoutChange = false;
    analyzeTabstops(*ddp);
    setTabstops(*ddp);
}

void ColumnsPlusPlusData::fileSaved() {
    // NPPN_FILESAVED — re-establish tab layout if elastic is on (the saved buffer
    // is the active one on macOS, which is all a plugin can reach).
    updateActiveScintilla();
    DocumentData* ddp = getDocument();
    if (!ddp || !ddp->settings.elasticEnabled) return;
    analyzeTabstops(*ddp);
    setTabstops(*ddp);
}

void ColumnsPlusPlusData::bufferActivated() {
    void* docptr = DocPointer();
    bool isNewDocument = !documents.contains(docptr);
    DocumentData& dd = documents[docptr];
    uintptr_t nppBufferId = (uintptr_t)npp(NPPM_GETCURRENTBUFFERID, 0, 0);
    if (nppBufferId != dd.buffer) isNewDocument = true;
    if (isNewDocument) {
        dd.buffer = nppBufferId;
        std::string extension = getFileExtension(dd.buffer);
        std::string profileName = extensionToProfile.contains(extension) ? extensionToProfile[extension]
            : extension.length() && extensionToProfile.contains("*") ? extensionToProfile["*"]
            : "*";
        if (profileName == "") settings.elasticEnabled = false;
        else if (profileName != "*" && profiles.contains(profileName)) {
            settings.profileName = profileName;
            static_cast<ElasticTabsProfile&>(settings) = profiles[profileName];
            settings.elasticEnabled = true;
        }
        if ( extension.length() && settings.elasticEnabled
          && ( (disableOverSize > 0 && (intptr_t)disableOverSize * 1024 < Length())
            || (disableOverLines > 0 && disableOverLines < LineCount()) ) )
            settings.elasticEnabled = false;
        dd.settings = settings;
        if (settings.elasticEnabled && !sentADDSCNMODIFIEDFLAGS) {
            sentADDSCNMODIFIEDFLAGS = true;
            npp(NPPM_ADDSCNMODIFIEDFLAGS, 0,
                SC_MOD_INSERTTEXT | SC_MOD_BEFOREDELETE | SC_MOD_DELETETEXT | SC_MOD_CHANGESTYLE);
        }
    }
    else settings = dd.settings;
    npp(NPPM_SETMENUITEMCHECK, (uintptr_t)elasticEnabledMenuItem, settings.elasticEnabled);
    npp(NPPM_SETMENUITEMCHECK, (uintptr_t)decimalSeparatorMenuItem, settings.decimalSeparatorIsComma);
    dd.tabOriginal = TabWidth();
    SetTabIndents(!settings.elasticEnabled);
    if (!settings.elasticEnabled) {
        SetControlCharSymbol(0);
        return;
    }
    if (settings.overrideTabSize) SetTabWidth(settings.minimumOrLeadingTabSize);
    if (isNewDocument) {
        analyzeTabstops(dd);
        setTabstops(dd);
    }
    else {
        int ccsym = settings.monospaceNoMnemonics && dd.assumeMonospace ? '!' : 0;
        if (ControlCharSymbol() != ccsym) SetControlCharSymbol(ccsym);
        if (dd.tabLayouts.empty()) return;
        auto lineTabsSet = getLineTabsSet();
        if (!lineTabsSet) return;
        SciLine lines = LineCount();
        for (SciLine i = 0; i < lines; ++i) {
            int tab = GetNextTabStop(i, 0);
            if (tab != 0) return;
        }
        lineTabsSet->assign(lines, 0);
        reselectRectangularSelection(dd);
    }
}

void ColumnsPlusPlusData::toggleElasticEnabled() {
    DocumentData* ddp = getDocument();
    ddp->settings.elasticEnabled = settings.elasticEnabled ^= true;
    npp(NPPM_SETMENUITEMCHECK, (uintptr_t)elasticEnabledMenuItem, settings.elasticEnabled);
    if (settings.elasticEnabled) {
        if (settings.overrideTabSize) {
            ddp->tabOriginal = TabWidth();
            SetTabWidth(settings.minimumOrLeadingTabSize);
        }
        SetTabIndents(false);
        analyzeTabstops(*ddp);
        setTabstops(*ddp);
        if (!sentADDSCNMODIFIEDFLAGS) {
            sentADDSCNMODIFIEDFLAGS = true;
            npp(NPPM_ADDSCNMODIFIEDFLAGS, 0,
                SC_MOD_INSERTTEXT | SC_MOD_BEFOREDELETE | SC_MOD_DELETETEXT | SC_MOD_CHANGESTYLE);
        }
    }
    else {
        if (settings.overrideTabSize) SetTabWidth(ddp->tabOriginal);
        SetTabIndents(true);
        if (ControlCharSymbol()) SetControlCharSymbol(0);
        SciLine lineCount = LineCount();
        for (SciLine lineNum = 0; lineNum < lineCount; ++lineNum) ClearTabStops(lineNum);
    }
    if (bothViewsShowSameDocument()) {
        activeScintilla = activeScintilla == nppData._scintillaMainHandle ? nppData._scintillaSecondHandle : nppData._scintillaMainHandle;
        if (settings.elasticEnabled) {
            if (settings.overrideTabSize) SetTabWidth(settings.minimumOrLeadingTabSize);
            SetTabIndents(false);
            setTabstops(*ddp);
        }
        else {
            if (settings.overrideTabSize) SetTabWidth(ddp->tabOriginal);
            SetTabIndents(true);
            if (ControlCharSymbol()) SetControlCharSymbol(0);
            SciLine lineCount = LineCount();
            for (SciLine lineNum = 0; lineNum < lineCount; ++lineNum) ClearTabStops(lineNum);
        }
        activeScintilla = activeScintilla == nppData._scintillaMainHandle ? nppData._scintillaSecondHandle : nppData._scintillaMainHandle;
    }
}

void ColumnsPlusPlusData::toggleDecimalSeparator() {
    DocumentData* ddp = getDocument();
    ddp->settings.decimalSeparatorIsComma = settings.decimalSeparatorIsComma ^= true;
    npp(NPPM_SETMENUITEMCHECK, (uintptr_t)decimalSeparatorMenuItem, settings.decimalSeparatorIsComma);
}
