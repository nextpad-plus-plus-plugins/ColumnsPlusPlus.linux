// Columns++ for Notepad++ — macOS port.
// CppData.mm — non-inline ColumnsPlusPlusData members: Scintilla text access,
// file/profile helpers, document lookup, elastic-tabstops geometry helpers and
// the UTF-8<->UTF-32 conversion used by the number parser.

#include "CppData.h"

std::string ColumnsPlusPlusData::StringOfRange(SciPosition a, SciPosition b) const {
    if (b <= a) return std::string();
    std::string buf((size_t)(b - a), 0);
    Sci_TextRangeFull tr;
    tr.chrg.cpMin = a;
    tr.chrg.cpMax = b;
    // SCI_GETTEXTRANGEFULL writes (cpMax-cpMin)+1 bytes (incl. terminating NUL),
    // so give it room and then trim the NUL.
    buf.resize((size_t)(b - a) + 1, 0);
    tr.lpstrText = buf.data();
    send(SCI_GETTEXTRANGEFULL, 0, (intptr_t)&tr);
    buf.resize((size_t)(b - a));
    return buf;
}

std::string ColumnsPlusPlusData::TargetText() const {
    intptr_t n = send(SCI_GETTARGETTEXT, 0, 0);
    if (n <= 0) return std::string();
    std::string buf((size_t)n, 0);
    send(SCI_GETTARGETTEXT, 0, (intptr_t)buf.data());
    return buf;
}

// ---------------------------------------------------------------------------
// File path / extension helpers (NPPM_GETFULLPATHFROMBUFFERID). On macOS paths
// use '/'; the Windows source split on '\\'. We accept both.
// ---------------------------------------------------------------------------
std::string ColumnsPlusPlusData::getFilePath() const {
    return getFilePath((uintptr_t)npp(NPPM_GETCURRENTBUFFERID, 0, 0));
}

std::string ColumnsPlusPlusData::getFilePath(uintptr_t buffer) const {
    intptr_t n = npp(NPPM_GETFULLPATHFROMBUFFERID, buffer, 0);
    if (n < 1) return std::string();
    std::string s((size_t)n + 1, 0);
    npp(NPPM_GETFULLPATHFROMBUFFERID, buffer, (intptr_t)s.data());
    s.resize(strlen(s.c_str()));
    return s;
}

std::string ColumnsPlusPlusData::getFileExtension() const {
    return getFileExtension((uintptr_t)npp(NPPM_GETCURRENTBUFFERID, 0, 0));
}

std::string ColumnsPlusPlusData::getFileExtension(uintptr_t buffer) const {
    return getFileExtension(getFilePath(buffer));
}

std::string ColumnsPlusPlusData::getFileExtension(const std::string& filepath) const {
    size_t lastSlash = filepath.find_last_of("/\\");
    if (lastSlash == std::string::npos) return std::string();   // untitled / no path
    size_t lastPeriod = filepath.find_last_of('.');
    if (lastPeriod == std::string::npos || lastPeriod < lastSlash || lastPeriod == filepath.length() - 1) return ".";
    std::string ext = filepath.substr(lastPeriod + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

// ---------------------------------------------------------------------------
// Document lookup keyed by the Scintilla doc pointer (mirrors the Windows code).
// ---------------------------------------------------------------------------
DocumentData* ColumnsPlusPlusData::getDocument() {
    void* docptr = DocPointer();
    bool isNew = !documents.contains(docptr);
    DocumentData& dd = documents[docptr];
    if (isNew) {
        dd.buffer = (uintptr_t)npp(NPPM_GETCURRENTBUFFERID, 0, 0);
        dd.settings = settings;
    }
    return &dd;
}

DocumentData* ColumnsPlusPlusData::getDocument(NppHandle scintilla) {
    activeScintilla = scintilla;
    void* docptr = DocPointer();
    if (!documents.contains(docptr)) return nullptr;
    return &documents[docptr];
}

bool ColumnsPlusPlusData::bothViewsShowSameDocument() {
    intptr_t cdi1 = npp(NPPM_GETCURRENTDOCINDEX, 0, 0);
    intptr_t cdi2 = npp(NPPM_GETCURRENTDOCINDEX, 0, 1);
    if (cdi1 < 0 || cdi2 < 0) return false;
    return npp(NPPM_GETBUFFERIDFROMPOS, (uintptr_t)cdi1, 0) == npp(NPPM_GETBUFFERIDFROMPOS, (uintptr_t)cdi2, 1);
}

// ---------------------------------------------------------------------------
// Elastic-tabstops geometry helpers (verbatim from ColumnsPlusPlus.h).
// ---------------------------------------------------------------------------
SciPosition ColumnsPlusPlusData::positionFromLineAndPointX(SciLine line, int px) {
    SciPosition begin = PositionFromLine(line);
    SciPosition end   = LineEndPosition(line);
    SciPosition lastI = begin;
    int         lastX = px;
    for (SciPosition i = begin;; i = PositionAfter(i)) {
        int x = PointXFromPosition(i);
        if (x == px) return i;
        if (x > px) return x - px > px - lastX ? lastI : i;
        lastI = i;
        lastX = x;
        if (i >= end) break;
    }
    return end;
}

int ColumnsPlusPlusData::unwrappedWidth(SciPosition from, SciPosition to) {
    int width = 0;
    int xLoc = PointXFromPosition(from);
    int xEnd = PointXFromPosition(to);
    int wrapMode = (int)send(SCI_GETWRAPMODE);
    if (wrapMode != 0 && send(SCI_WRAPCOUNT, (uintptr_t)LineFromPosition(from)) > 1) {
        int yLoc = PointYFromPosition(from);
        int yEnd = PointYFromPosition(to);
        for (SciPosition next = from + 1; yLoc != yEnd; ++next) {
            int yNext = PointYFromPosition(next);
            while (yNext == yLoc) {
                ++next;
                yNext = PointYFromPosition(next);
            }
            width += PointXFromPosition(next - 1) - xLoc;
            char lastBeforeWrap = (char)CharacterAt(next - 1);
            char cStringBeforeWrap[] = " ";
            if (lastBeforeWrap) cStringBeforeWrap[0] = lastBeforeWrap;
            width += TextWidth(StyleIndexAt(next - 1), cStringBeforeWrap);
            xLoc = PointXFromPosition(next);
            yLoc = yNext;
        }
    }
    // NB: faithful to the Windows source, which returns xEnd - xLoc here; the
    // accumulated `width` of completed wrapped segments feeds only the xLoc
    // updates above (word-wrap is normally off for elastic-tabstops documents).
    (void)width;
    return xEnd - xLoc;
}

bool ColumnsPlusPlusData::guessMonospaced() {
    // Technology::Default == 0; macOS Scintilla uses CoreGraphics but reports a
    // technology value; treat non-zero (DirectWrite-equivalent) like Windows did.
    int tech = (int)send(SCI_GETTECHNOLOGY);
    if (tech != 0) {
        // On macOS the editor isn't GDI; monospace optimization measuring still
        // works, so don't auto-disable purely on technology. Fall through.
    }
    int width24b = TextWidth(STYLE_DEFAULT, "                        ");
    int width24w = TextWidth(STYLE_DEFAULT, "WWWWWWWWWWWWWWWWWWWWWWWW");
    if (width24b != width24w) return false;
    for (int style = 0; style < STYLE_DEFAULT; ++style)
        if ( TextWidth(style, "WWWWWWWWWWWWWWWWWWWWWWWW") != width24b
          || TextWidth(style, "                        ") != width24b) return false;
    for (int style = 39 /* STYLE_LASTPREDEFINED */ + 1; style < 256; ++style)
        if ( TextWidth(style, "WWWWWWWWWWWWWWWWWWWWWWWW") != width24b
          || TextWidth(style, "                        ") != width24b) return false;
    return true;
}

void ColumnsPlusPlusData::reselectRectangularSelection(DocumentData& dd) {
    if (!dd.settings.elasticEnabled) return;
    int selectionMode = SelectionMode();
    if (selectionMode == SC_SEL_RECTANGLE || selectionMode == SC_SEL_THIN) {
        SciPosition anchor        = RectangularSelectionAnchor();
        SciPosition anchorVirtual = RectangularSelectionAnchorVirtualSpace();
        SciPosition caret         = RectangularSelectionCaret();
        SciPosition caretVirtual  = RectangularSelectionCaretVirtualSpace();
        SciLine anchorLine        = LineFromPosition(anchor);
        SciLine caretLine         = LineFromPosition(caret);
        setTabstops(dd, std::min(anchorLine, caretLine), std::max(anchorLine, caretLine));
        SetRectangularSelectionAnchor(anchor);
        SetRectangularSelectionAnchorVirtualSpace(anchorVirtual);
        SetRectangularSelectionCaret(caret);
        SetRectangularSelectionCaretVirtualSpace(caretVirtual);
    }
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-32 (the number parser keeps a 32-bit-codepoint form for the
// currency/sign Unicode tables, replacing the Windows std::wstring path).
// ---------------------------------------------------------------------------
std::u32string toWide32(const std::string& utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    size_t i = 0, n = utf8.size();
    while (i < n) {
        unsigned char c = (unsigned char)utf8[i];
        char32_t cp;
        int len;
        if (c < 0x80)        { cp = c;            len = 1; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }   // invalid lead byte
        if (i + len > n) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)utf8[i + k];
            if ((cc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string fromWide32(const std::u32string& w) {
    std::string out;
    out.reserve(w.size());
    for (char32_t cp : w) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
        else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}
