// Columns++ for Notepad++ — macOS port.
// Rectangular.mm — the RectangularSelection engine (constructor, extend, refit).
//
// Ported from the Windows Rectangular.cpp. The Windows version pops an owner-drawn
// 3x3 "numeric keypad" picker dialog (IDD_CHOOSE_RECTANGULAR_SELECTION*) when the
// current selection is ambiguous, letting the user choose which way to grow it
// into a rectangle. On macOS the picker is reimplemented with an NSAlert offering
// the same logical directions; for the unambiguous / auto-extend cases (governed
// by the extendSingleLine / extendFullLines / extendZeroWidth options) the
// behaviour is identical. The geometry math is ported faithfully.

#include "CppData.h"

#include <gtk/gtk.h>

#include <limits>

// chooseDirection — replacement for the keypad picker. Returns a numpad-style code
// 1..9 (or 0 for cancel). `mask` is a set bit per allowed code (bit n => code n).
// The prompt mirrors the Windows dialog's question text.
static intptr_t chooseDirection(const std::string& prompt, unsigned mask) {
    // GTK modal picker (macOS: NSAlert with one button per direction).
    // Dialog rules: hide-on-close, read state before destroy.
    struct Opt { int code; const char* title; };
    static const Opt all[] = {
        {5, "Enclose (entire region)"},
        {8, "Up"},     {2, "Down"},
        {4, "Left"},   {6, "Right"},
        {7, "Up-left"},{9, "Up-right"},
        {1, "Down-left"},{3, "Down-right"},
    };
    if (!gtk_is_initialized()) return 0;

    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Columns++ rectangular selection");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget* lbl = gtk_label_new(prompt.c_str());
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 46);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), lbl);

    struct PickState { GMainLoop* loop; intptr_t code; };
    static PickState st;   // single-threaded modal use only
    st.loop = g_main_loop_new(nullptr, FALSE);
    st.code = 0;

    for (const auto& o : all) if (mask & (1u << o.code)) {
        GtkWidget* b = gtk_button_new_with_label(o.title);
        g_object_set_data(G_OBJECT(b), "cpp-code", GINT_TO_POINTER(o.code));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer p) {
            PickState* ps = (PickState*)p;
            ps->code = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "cpp-code"));
            g_main_loop_quit(ps->loop);
        }), &st);
        gtk_box_append(GTK_BOX(vbox), b);
    }
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        PickState* ps = (PickState*)p;
        ps->code = 0;
        g_main_loop_quit(ps->loop);
    }), &st);
    gtk_box_append(GTK_BOX(vbox), cancel);

    g_signal_connect(win, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer p) -> gboolean {
        PickState* ps = (PickState*)p;
        ps->code = 0;
        g_main_loop_quit(ps->loop);
        return FALSE;   // hide-on-close hides it
    }), &st);

    gtk_window_present(GTK_WINDOW(win));
    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);

    intptr_t code = st.code;
    gtk_window_destroy(GTK_WINDOW(win));
    return code;
}

RectangularSelection::RectangularSelection(ColumnsPlusPlusData& data)
    : data(data),
      blank1440(data.TextWidth(STYLE_DEFAULT, std::string(1440, ' ').c_str())),
      tabWidth(data.TabWidth()) {
    _mode   = data.SelectionMode();
    _size   = data.Selections();
    _anchor = corner(data.SelectionNAnchor(0), data.SelectionNAnchorVirtualSpace(0));
    _caret  = corner(data.SelectionNCaret(_size - 1), data.SelectionNCaretVirtualSpace(_size - 1));
    _reverse = _anchor.ln > _caret.ln;
    if (_mode != SC_SEL_RECTANGLE && _mode != SC_SEL_THIN) _size = 0;
}

RectangularSelection& RectangularSelection::refit(bool addLine) {

    if (!_size) return *this;
    if (data.settings.elasticEnabled) {
        DocumentData* ddp = data.getDocument();
        data.analyzeTabstops(*ddp);
        SciPosition firstVisible = data.FirstVisibleLine();
        SciPosition lastVisible  = firstVisible + data.LinesOnScreen();
        data.setTabstops(*ddp, std::min(std::min(_anchor.ln, _caret.ln), firstVisible),
                               std::max(std::max(_anchor.ln, _caret.ln), lastVisible));
    }
    data.PointXFromPosition(0);  // clears a stale Scintilla x-position cache (see Windows note)

    int pxLeft  = std::numeric_limits<int>::max();
    int pxRight = 0;
    for (int i = 0; i < _size; ++i) {
        SciPosition cpMin = data.SelectionNStart(i);
        SciPosition vsMin = data.SelectionNStartVirtualSpace(i);
        SciPosition cpMax = data.SelectionNEnd(i);
        int px = data.PointXFromPosition(cpMin) + blankWidth(vsMin);
        if (px < pxLeft) pxLeft = px;
        px = data.PointXFromPosition(cpMax);
        if (px > pxRight) pxRight = px;
    }

    if (leftToRight()) { _anchor.px = pxLeft ; _caret.px = pxRight; }
                  else { _anchor.px = pxRight; _caret.px = pxLeft ; }

    if (addLine) {
        (topToBottom() ? _caret.ln : _anchor.ln)++;
        _size++;
    }

    for (Corner* corner : {&_anchor, &_caret}) {
        corner->en = data.LineEndPosition(corner->ln);
        SciPosition pxEnd = data.PointXFromPosition(corner->en);
        corner->cp = corner->px < pxEnd ? data.positionFromLineAndPointX(corner->ln, corner->px) : corner->en;
        corner->vs = corner->px < pxEnd ? 0 : blankCount(corner->px - pxEnd);
        corner->st = data.PositionFromLine(corner->ln);
        corner->sx = data.PointXFromPosition(corner->st);
        corner->px -= corner->sx;
    }
    data.SetRectangularSelectionAnchor            (_anchor.cp);
    data.SetRectangularSelectionAnchorVirtualSpace(_anchor.vs);
    data.SetRectangularSelectionCaret             (_caret.cp );
    data.SetRectangularSelectionCaretVirtualSpace (_caret.vs );
    return *this;
}

RectangularSelection& RectangularSelection::extend() {

    if (_mode == SC_SEL_RECTANGLE || _mode == SC_SEL_THIN) {

        if (_size == 0) {
            cppAlert("Columns++ rectangular selection", "This command requires a rectangular selection.");
            return *this;
        }

        bool canExtendRight = false;
        int maxExtent = 0;
        for (int i = 0; i < _size; ++i) {
            SciPosition ap = data.SelectionNAnchor(i);
            if (_mode == SC_SEL_RECTANGLE) {
                SciPosition cp = data.SelectionNCaret(i);
                if (ap != cp) return *this;
                SciPosition av = data.SelectionNAnchorVirtualSpace(i);
                SciPosition cv = data.SelectionNCaretVirtualSpace(i);
                if (av != cv) return *this;
            }
            SciPosition lep = data.LineEndPosition(data.LineFromPosition(ap));
            if (ap < lep) canExtendRight = true;
            int extent = data.PointXFromPosition(lep);
            if (extent > maxExtent) maxExtent = extent;
        }

        intptr_t choice;
        if (data.extendZeroWidth && canExtendRight) choice = 6;
        else if (_caret.cp == _caret.st && _caret.vs == 0) {
            if (!canExtendRight) {
                cppAlert("Columns++ rectangular selection", "This command requires a rectangular selection.");
                _size = 0;
                return *this;
            }
            if (!cppConfirm("Columns++ rectangular selection",
                "This command requires a non-zero-width rectangular selection.\n\n"
                "Extend selection to enclose the selected lines?")) {
                _size = 0;
                return *this;
            }
            choice = 6;
        }
        else if (!canExtendRight) {
            if (!cppConfirm("Columns++ rectangular selection",
                "This command requires a non-zero-width rectangular selection.\n\n"
                "Extend selection to enclose the selected lines?")) {
                _size = 0;
                return *this;
            }
            choice = 4;
        }
        else {
            choice = chooseDirection("Extend the rectangular selection which way?", (1u<<4)|(1u<<6));
            if (!choice) { _size = 0; return *this; }
        }

        if (choice == 4) {
            _anchor.cp = _anchor.st;
            _anchor.vs = 0;
            _anchor.px = 0;
        }
        else {
            _anchor.cp    = _anchor.en;
            int pxEOL     = data.PointXFromPosition(_anchor.en);
            int pxVirtual = maxExtent - pxEOL;
            _anchor.vs    = blankCount(pxVirtual);
            _anchor.px    = pxEOL - _anchor.sx + blankWidth(_anchor.vs);
            if (choice == 5) {
                _caret.cp = _caret.st;
                _caret.vs = 0;
                _caret.px = 0;
            }
        }
        data.SetRectangularSelectionAnchor            (_anchor.cp);
        data.SetRectangularSelectionAnchorVirtualSpace(_anchor.vs);
        data.SetRectangularSelectionCaret             (_caret.cp );
        data.SetRectangularSelectionCaretVirtualSpace (_caret.vs );
        _mode = SC_SEL_RECTANGLE;
        return *this;
    }

    if (data.Selections() != 1) {
        if (!cppConfirm("Columns++ rectangular selection",
            "This command requires a rectangular selection.\n\n"
            "Create a rectangular selection that encloses the existing multiple selections?")) {
            _size = 0;
            return *this;
        }
        _size = data.Selections();
        SciLine first = std::numeric_limits<SciLine>::max();
        SciLine last  = 0;
        int left  = std::numeric_limits<int>::max();
        int right = std::numeric_limits<int>::min();
        DocumentData* ddp = data.settings.elasticEnabled ? data.getDocument() : nullptr;
        for (int i = 0; i < _size; ++i) {
            SciPosition p = data.SelectionNStart(i);
            SciPosition q = data.SelectionNEnd(i);
            SciLine pLine = data.LineFromPosition(p);
            SciLine qLine = data.LineFromPosition(q);
            first = std::min(first, pLine);
            last  = std::max(last , qLine);
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
        SciPosition anchor = data.positionFromLineAndPointX(first, left );
        SciPosition caret  = data.positionFromLineAndPointX(last , right);
        int caretVS = blankCount(right - data.PointXFromPosition(caret));
        _anchor = corner(anchor, 0);
        _caret  = corner(caret, caretVS);
        if (ddp) data.setTabstops(*ddp, first, last);
        data.SetRectangularSelectionAnchor(_anchor.cp);
        data.SetRectangularSelectionAnchorVirtualSpace(_anchor.vs);
        data.SetRectangularSelectionCaret(_caret.cp);
        data.SetRectangularSelectionCaretVirtualSpace(_caret.vs);
        _mode = SC_SEL_RECTANGLE;
        _reverse = false;
        return *this;
    }

    Corner topLeft = corner(0, 0);
    Corner bottomLeft = corner(data.PositionFromLine(data.LineCount() - 1), 0);
    if (bottomLeft.st == bottomLeft.en && bottomLeft.ln > 0)
        bottomLeft = corner(data.PositionFromLine(bottomLeft.ln - 1), 0);
    Corner& start  = _anchor.cp < _caret.cp ? _anchor : _caret ;
    Corner& end    = _anchor.cp < _caret.cp ? _caret  : _anchor;
    bool hasWidth  = _anchor.cp != _caret.cp || _anchor.vs != _caret.vs;
    bool hasHeight = _anchor.ln != _caret.ln;

    if (!hasWidth && !hasHeight) {

        intptr_t choice;
        if (_caret.px == 0 && (_caret.ln == 0 || _caret.ln >= bottomLeft.ln)) {
            if (!cppConfirm("Columns++ rectangular selection",
                "This command requires a rectangular selection.\n\nEnclose the entire document in a rectangular selection?")) {
                _size = 0;
                return *this;
            }
            choice = 5;
        }
        else {
            // Offer the full set of directions reachable from the caret.
            unsigned mask = (1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9);
            choice = chooseDirection("Make a rectangular selection based on the cursor position?", mask);
            if (!choice) { _size = 0; return *this; }
        }

        if (data.settings.elasticEnabled) data.setTabstops(*data.getDocument(), 0, -1);
        int maxExtentAll = 0;
        for (SciLine i = 0; i <= bottomLeft.ln; ++i) {
            int extent = data.PointXFromPosition(data.LineEndPosition(i));
            if (extent > maxExtentAll) maxExtentAll = extent;
        }
        Corner topRight    = corner(topLeft   .en, blankCount(maxExtentAll - data.PointXFromPosition(topLeft   .en)));
        Corner bottomRight = corner(bottomLeft.en, blankCount(maxExtentAll - data.PointXFromPosition(bottomLeft.en)));

        Corner anchor, caret;

        switch (choice) {
        case 1: anchor = bottomLeft ; caret = _caret ; break;
        case 3: anchor = bottomRight; caret = _caret ; break;
        case 5: anchor = bottomRight; caret = topLeft; break;
        case 7: anchor = topLeft    ; caret = _caret ; break;
        case 9: anchor = topRight   ; caret = _caret ; break;
        case 2:
        case 8:
            anchor = choice == 2 ? bottomRight : topRight;
            caret.cp = _caret.st;
            caret.vs = 0;
            caret.ln = _caret.ln;
            caret.st = _caret.st;
            caret.en = _caret.en;
            caret.sx = _caret.sx;
            caret.px = _caret.sx;
            break;
        case 4:
        case 6:
            anchor = choice == 4 ? bottomLeft : bottomRight;
            caret.ln = topLeft.ln;
            caret.st = topLeft.st;
            caret.en = topLeft.en;
            caret.sx = topLeft.sx;
            int px = _caret.px + _caret.sx;
            int ex = data.PointXFromPosition(caret.en);
            if (px > ex) {
                caret.cp = caret.en;
                caret.vs = blankCount(px - ex);
            }
            else {
                caret.cp = data.positionFromLineAndPointX(caret.ln, px);
                caret.vs = 0;
            }
            caret.px = ex - caret.sx + blankWidth(caret.vs);
            break;
        }

        SciPosition size = std::max(anchor.ln, caret.ln) - std::min(anchor.ln, caret.ln) + 1;
        if (size > std::numeric_limits<int>::max()) { _size = 0; return *this; }
        _size    = static_cast<int>(size);
        _mode    = SC_SEL_RECTANGLE;
        _anchor  = anchor;
        _caret   = caret;
        _reverse = _anchor.ln > _caret.ln;
        data.SetRectangularSelectionAnchor(_anchor.cp);
        data.SetRectangularSelectionAnchorVirtualSpace(_anchor.vs);
        data.SetRectangularSelectionCaret(_caret.cp);
        data.SetRectangularSelectionCaretVirtualSpace(_caret.vs);
        return *this;
    }

    if (hasWidth && !hasHeight) {
        intptr_t choice;
        if (data.extendSingleLine && _caret.ln < bottomLeft.ln) choice = 2;
        else if (_caret.ln == 0) {
            if (bottomLeft.ln == 0) {
                cppAlert("Columns++ rectangular selection", "This command requires a rectangular selection.");
                _size = 0;
                return *this;
            }
            if (!cppConfirm("Columns++ rectangular selection",
                "This command requires a rectangular selection.\n\nExtend selection to the last line of the document?")) {
                _size = 0;
                return *this;
            }
            choice = 2;
        }
        else if (_caret.ln >= bottomLeft.ln) {
            if (!cppConfirm("Columns++ rectangular selection",
                "This command requires a rectangular selection.\n\nExtend selection to the first line of the document?")) {
                _size = 0;
                return *this;
            }
            choice = 8;
        }
        else {
            choice = chooseDirection("Extend the selection up or down?", (1u<<8)|(1u<<2)|(1u<<5));
            if (!choice) { _size = 0; return *this; }
        }
        if (data.settings.elasticEnabled) data.setTabstops(*data.getDocument(), choice == 2 ? _caret.ln : 0, choice == 8 ? _caret.ln : -1);
        Corner top = _caret;
        Corner bottom = _caret;
        if (choice == 8) {
            top.st = 0;
            top.ln = 0;
            top.en = data.LineEndPosition(0);
            int endX = data.PointXFromPosition(top.en);
            int anchorX = _anchor.px + _anchor.sx;
            if (anchorX > endX) {
                top.cp = top.en;
                top.vs = blankCount(anchorX - endX);
            }
            else {
                top.cp = data.positionFromLineAndPointX(top.ln, anchorX);
                top.vs = 0;
            }
            top.sx = data.PointXFromPosition(top.st);
            top.px = anchorX - top.sx + blankWidth(top.vs);
        }
        else {
            if (choice == 5) {
                top.st = 0;
                top.ln = 0;
                top.en = data.LineEndPosition(0);
            }
            bottom.en = data.Length();
            bottom.ln = data.LineFromPosition(bottom.en);
            bottom.st = data.PositionFromLine(bottom.ln);
            if (bottom.st == bottom.en && bottom.ln > 0) {
                bottom.ln--;
                bottom.st = data.PositionFromLine(bottom.ln);
                bottom.en = data.LineEndPosition(bottom.ln);
            }
            if (bottom.ln == top.ln) { _size = 0; return *this; }
            if (choice == 5) {
                int endX = data.PointXFromPosition(top.en);
                int caretX = _caret.px + _caret.sx;
                if (caretX > endX) {
                    top.cp = top.en;
                    top.vs = blankCount(caretX - endX);
                }
                else {
                    top.cp = data.positionFromLineAndPointX(top.ln, caretX);
                    top.vs = 0;
                }
                top.sx = data.PointXFromPosition(top.st);
                top.px = caretX - top.sx + blankWidth(top.vs);
            }
            int endX = data.PointXFromPosition(bottom.en);
            int anchorX = _anchor.px + _anchor.sx;
            if (anchorX > endX) {
                bottom.cp = bottom.en;
                bottom.vs = blankCount(anchorX - endX);
            }
            else {
                bottom.cp = data.positionFromLineAndPointX(bottom.ln, anchorX);
                bottom.vs = 0;
            }
            bottom.sx = data.PointXFromPosition(bottom.st);
            bottom.px = anchorX - bottom.sx + blankWidth(bottom.vs);
        }
        SciPosition size = bottom.ln - top.ln + 1;
        if (size > std::numeric_limits<int>::max()) { _size = 0; return *this; }
        _size    = static_cast<int>(size);
        _mode    = SC_SEL_RECTANGLE;
        if (choice == 5) _caret = top;
        _anchor  = choice == 8 ? top : bottom;
        _reverse = choice == 8 ? false : true;
        data.SetRectangularSelectionAnchor            (_anchor.cp);
        data.SetRectangularSelectionAnchorVirtualSpace(_anchor.vs);
        data.SetRectangularSelectionCaret             (_caret.cp );
        data.SetRectangularSelectionCaretVirtualSpace (_caret.vs );
        return *this;
    }

    if (hasWidth && hasHeight && start.cp == start.st && (end.cp == end.st || end.cp == end.en)) {
        if (!data.extendFullLines) {
            if (!cppConfirm("Columns++ rectangular selection",
                start.cp == 0 && end.ln >= bottomLeft.ln
                ? "This command requires a rectangular selection.\n\nConvert to a rectangular selection enclosing the entire document?"
                : "This command requires a rectangular selection.\n\nConvert to a rectangular selection enclosing the selected lines?")) {
                _size = 0;
                return *this;
            }
        }
    }
    else {
        cppAlert("Columns++ rectangular selection", "This command requires a rectangular selection.");
        _size = 0;
        return *this;
    }

    if (end.cp == end.st) --end.ln;

    if (data.settings.elasticEnabled) data.setTabstops(*data.getDocument(), start.ln, end.ln);

    SciPosition size = end.ln - start.ln + 1;
    if (size > std::numeric_limits<int>::max()) { _size = 0; return *this; }

    int maxRight = 0;
    for (SciLine i = start.ln; i <= end.ln; ++i) {
        int right = data.PointXFromPosition(data.LineEndPosition(i));
        if (right > maxRight) maxRight = right;
    }

    if (end.cp != end.st) {
        int pxEOL = data.PointXFromPosition(end.en);
        int pxVirtual = maxRight - pxEOL;
        end.vs = blankCount(pxVirtual);
        end.px = pxEOL - end.sx + blankWidth(end.vs);
    }
    else if (_anchor.cp < _caret.cp) {
        _anchor.cp = _anchor.en;
        int pxEOL = data.PointXFromPosition(_anchor.en);
        int pxVirtual = maxRight - pxEOL;
        _anchor.vs = blankCount(pxVirtual);
        _anchor.px = pxEOL - _anchor.sx + blankWidth(_anchor.vs);
        _caret .st = data.PositionFromLine  (_caret.ln);
        _caret .sx = data.PointXFromPosition(_caret.st);
        _caret .en = data.LineEndPosition   (_caret.ln);
        _caret .cp = _caret.st;
        _caret .vs = 0;
        _caret .px = 0;
    }
    else {
        _anchor.st = data.PositionFromLine  (_anchor.ln);
        _anchor.sx = data.PointXFromPosition(_anchor.st);
        _anchor.en = data.LineEndPosition   (_anchor.ln);
        _anchor.cp = _anchor.en;
        int pxEOL = data.PointXFromPosition(_anchor.en);
        int pxVirtual = maxRight - pxEOL;
        _anchor.vs = blankCount(pxVirtual);
        _anchor.px = pxEOL - _anchor.sx + blankWidth(_anchor.vs);
    }

    data.SetRectangularSelectionAnchor            (_anchor.cp);
    data.SetRectangularSelectionAnchorVirtualSpace(_anchor.vs);
    data.SetRectangularSelectionCaret             (_caret.cp );
    data.SetRectangularSelectionCaretVirtualSpace (_caret.vs );
    _mode = SC_SEL_RECTANGLE;
    _size = static_cast<int>(size);
    _reverse = _anchor.ln > _caret.ln;
    return *this;
}
