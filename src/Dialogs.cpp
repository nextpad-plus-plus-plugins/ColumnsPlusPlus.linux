// Columns++ for Notepad++ — Linux (GTK4) port.
// Dialogs.cpp — programmatic GTK4 reimplementations of the dialogs the ported
// commands use, plus the shared alert helpers. Field-for-field ports of the
// macOS AppKit dialogs (which in turn mirror the Windows resources).
//
// Dialogs: Align, Column Sort, Conversion (CSV), Calculation results, Options,
// Elastic profile, About, the MODELESS Search-in-region panel, Calculate,
// Timestamps and Time formats.
//
// Modal rules (PORTING_NOTES): hide-on-close + every widget read happens
// before gtk_window_destroy (the ModalDlg destructor destroys at scope end,
// after the caller's read-back). The Search panel is modeless: it closes for
// real, syncing settings and clearing its hooks in close-request first.

#include "CppData.h"
#include "RegularExpression.h"

#include <gtk/gtk.h>

#include <unicode/locid.h>
#include <unicode/unistr.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Shared modal framework + widget helpers.
// ---------------------------------------------------------------------------
namespace {

struct ModalDlg {
    GtkWidget* win;
    GtkWidget* vbox;
    GMainLoop* loop = nullptr;
    int result = 0;

    explicit ModalDlg(const char* title) {
        win = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(win), title);
        gtk_window_set_modal(GTK_WINDOW(win), TRUE);
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
        gtk_window_set_hide_on_close(GTK_WINDOW(win), TRUE);
        g_signal_connect(win, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer p) -> gboolean {
            ((ModalDlg*)p)->finish(0);
            return FALSE;   // hide-on-close hides; we destroy after read-back
        }), this);
        GtkEventController* k = gtk_event_controller_key_new();
        g_signal_connect(k, "key-pressed",
            G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer p) -> gboolean {
                if (kv == GDK_KEY_Escape) { ((ModalDlg*)p)->finish(0); return TRUE; }
                return FALSE;
            }), this);
        gtk_widget_add_controller(win, k);

        vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_top(vbox, 14);
        gtk_widget_set_margin_bottom(vbox, 12);
        gtk_widget_set_margin_start(vbox, 16);
        gtk_widget_set_margin_end(vbox, 16);
        gtk_window_set_child(GTK_WINDOW(win), vbox);
    }
    ~ModalDlg() { gtk_window_destroy(GTK_WINDOW(win)); }

    void finish(int r) { result = r; if (loop) g_main_loop_quit(loop); }

    int run() {
        loop = g_main_loop_new(nullptr, FALSE);
        gtk_window_present(GTK_WINDOW(win));
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
        loop = nullptr;
        return result;
    }

    // OK/Cancel row; OK finishes with 1 (and is the default widget).
    void okCancel(const char* okLabel = "OK") {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign(row, GTK_ALIGN_END);
        gtk_widget_set_margin_top(row, 6);
        GtkWidget* ok = gtk_button_new_with_label(okLabel);
        gtk_widget_add_css_class(ok, "suggested-action");
        g_signal_connect(ok, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            ((ModalDlg*)p)->finish(1);
        }), this);
        GtkWidget* cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            ((ModalDlg*)p)->finish(0);
        }), this);
        gtk_box_append(GTK_BOX(row), ok);
        gtk_box_append(GTK_BOX(row), cancel);
        gtk_box_append(GTK_BOX(vbox), row);
        gtk_window_set_default_widget(GTK_WINDOW(win), ok);
    }
};

GtkWidget* mkLabel(const char* s) {
    GtkWidget* l = gtk_label_new(s);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}
GtkWidget* mkCheck(const char* s, bool on) {
    GtkWidget* c = gtk_check_button_new_with_label(s);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), on);
    return c;
}
GtkWidget* mkRadio(const char* s, GtkWidget* group) {
    GtkWidget* r = gtk_check_button_new_with_label(s);
    if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(r), GTK_CHECK_BUTTON(group));
    return r;
}
GtkWidget* mkEntry(const std::string& text, int chars) {
    GtkWidget* e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), text.c_str());
    gtk_editable_set_width_chars(GTK_EDITABLE(e), chars);
    return e;
}
// Editable combo pre-filled from an MRU history (newest last in the vector,
// shown newest first; initial text = newest — matches the macOS combo()).
GtkWidget* mkCombo(const std::vector<std::string>& history) {
    GtkWidget* c = gtk_combo_box_text_new_with_entry();
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), it->c_str());
    if (!history.empty()) {
        GtkWidget* e = gtk_combo_box_get_child(GTK_COMBO_BOX(c));
        gtk_editable_set_text(GTK_EDITABLE(e), history.back().c_str());
    }
    gtk_widget_set_hexpand(c, TRUE);
    return c;
}
// Non-editable popup.
GtkWidget* mkPopup(const std::vector<std::string>& items, int sel) {
    GtkWidget* c = gtk_combo_box_text_new();
    for (const auto& s : items) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), s.c_str());
    if (sel >= 0 && sel < (int)items.size()) gtk_combo_box_set_active(GTK_COMBO_BOX(c), sel);
    return c;
}
GtkWidget* hbox(std::initializer_list<GtkWidget*> ws, int spacing = 8) {
    GtkWidget* b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    for (GtkWidget* w : ws) gtk_box_append(GTK_BOX(b), w);
    return b;
}

bool checkOn(GtkWidget* c) { return gtk_check_button_get_active(GTK_CHECK_BUTTON(c)) == TRUE; }
std::string entryText(GtkWidget* e) {
    const char* t = gtk_editable_get_text(GTK_EDITABLE(e));
    return t ? t : "";
}
int entryInt(GtkWidget* e) { return atoi(entryText(e).c_str()); }
std::string comboText(GtkWidget* c) {
    gchar* t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c));
    std::string s = t ? t : "";
    g_free(t);
    return s;
}
int popupIndex(GtkWidget* c) { return gtk_combo_box_get_active(GTK_COMBO_BOX(c)); }

void pushHistory(std::vector<std::string>& h, const std::string& s) {
    if (s.empty()) return;
    for (auto it = h.begin(); it != h.end(); )
        if (*it == s) it = h.erase(it); else ++it;
    h.push_back(s);
}

// ICU locale list for the Sort dialog: parallel {id, "id  —  display"} vectors.
void icuLocaleList(std::vector<std::string>& ids, std::vector<std::string>& titles) {
    int32_t count = 0;
    const icu::Locale* locs = icu::Locale::getAvailableLocales(count);
    for (int32_t i = 0; i < count; ++i) {
        std::string id = locs[i].getName();
        icu::UnicodeString ud;
        locs[i].getDisplayName(icu::Locale::getDefault(), ud);
        std::string disp;
        ud.toUTF8String(disp);
        ids.push_back(id);
        titles.push_back(disp.empty() ? id : id + "  —  " + disp);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Shared alert helpers (declared in CppData.h). Synchronous, like NSAlert.
// ---------------------------------------------------------------------------
void cppAlert(const std::string& title, const std::string& message) {
    if (!gtk_is_initialized()) return;
    ModalDlg dlg(title.c_str());
    GtkWidget* l = mkLabel(message.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    gtk_box_append(GTK_BOX(dlg.vbox), l);
    GtkWidget* ok = gtk_button_new_with_label("OK");
    gtk_widget_set_halign(ok, GTK_ALIGN_END);
    g_signal_connect(ok, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        ((ModalDlg*)p)->finish(1);
    }), &dlg);
    gtk_box_append(GTK_BOX(dlg.vbox), ok);
    gtk_window_set_default_widget(GTK_WINDOW(dlg.win), ok);
    dlg.run();
}

bool cppConfirm(const std::string& title, const std::string& message) {
    if (!gtk_is_initialized()) return false;
    ModalDlg dlg(title.c_str());
    GtkWidget* l = mkLabel(message.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    gtk_box_append(GTK_BOX(dlg.vbox), l);
    dlg.okCancel();
    return dlg.run() == 1;
}

// ===========================================================================
// ALIGN dialog (IDD_ALIGN)
// ===========================================================================
bool runAlignDialog(ColumnsPlusPlusData& data) {
    ModalDlg dlg("Align");

    GtkWidget* alignBy = mkCombo(data.align.history);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Align by:"), alignBy }));

    GtkWidget* rFirst = mkRadio("First", nullptr);
    GtkWidget* rLast  = mkRadio("Last", rFirst);
    GtkWidget* rRegex = mkRadio("Regular expression", rFirst);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.align.alignOn == AlignSettings::Last ? rLast :
        data.align.alignOn == AlignSettings::Regex ? rRegex : rFirst), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ rFirst, rLast, rRegex }));

    GtkWidget* matchCase = mkCheck("Match case", data.align.matchCase);
    gtk_box_append(GTK_BOX(dlg.vbox), matchCase);

    GtkWidget* margin = mkEntry(std::to_string(data.align.margin), 4);
    GtkWidget* mLeft  = mkRadio("Left", nullptr);
    GtkWidget* mRight = mkRadio("Right", mLeft);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(data.align.marginRight ? mRight : mLeft), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Margin:"), margin, mLeft, mRight }));

    dlg.okCancel();
    if (dlg.run() != 1) return true;   // cancelled

    std::string s = comboText(alignBy);
    if (s.empty()) { cppAlert("Align", "Enter something to align."); return true; }
    data.align.matchCase   = checkOn(matchCase);
    data.align.marginRight = checkOn(mRight);
    data.align.alignOn     = checkOn(rLast) ? AlignSettings::Last
                           : checkOn(rRegex) ? AlignSettings::Regex : AlignSettings::First;
    data.align.margin      = std::max(0, std::min(99, entryInt(margin)));
    pushHistory(data.align.history, s);
    return false;
}

// ===========================================================================
// SORT dialog (IDD_SORT)
// ===========================================================================
bool runSortDialog(ColumnsPlusPlusData& data) {
    ModalDlg dlg("Column Sort");

    GtkWidget* wholeLines = mkRadio("Whole lines", nullptr);
    GtkWidget* withinSel  = mkRadio("Selected text only", wholeLines);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.sort.sortColumnSelectionOnly ? withinSel : wholeLines), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("What to sort:"), wholeLines, withinSel }));

    GtkWidget* asc  = mkRadio("Ascending", nullptr);
    GtkWidget* desc = mkRadio("Descending", asc);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(data.sort.sortDescending ? desc : asc), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Direction:"), asc, desc }));

    GtkWidget* tBin = mkRadio("Binary", nullptr);
    GtkWidget* tLoc = mkRadio("Locale", tBin);
    GtkWidget* tNum = mkRadio("Numeric", tBin);
    GtkWidget* tWid = mkRadio("Width", tBin);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.sort.sortType == SortSettings::Locale ? tLoc :
        data.sort.sortType == SortSettings::Numeric ? tNum :
        data.sort.sortType == SortSettings::Width ? tWid : tBin), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Sort type:"), tBin, tLoc, tNum, tWid }));

    GtkWidget* kEntire = mkRadio("Entire column", nullptr);
    GtkWidget* kIgnore = mkRadio("Ignore blanks/tabs", kEntire);
    GtkWidget* kTabbed = mkRadio("Tabbed", kEntire);
    GtkWidget* kRegex  = mkRadio("Regular expression", kEntire);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.sort.keyType == SortSettings::IgnoreBlanks ? kIgnore :
        data.sort.keyType == SortSettings::Tabbed ? kTabbed :
        data.sort.keyType == SortSettings::Regex ? kRegex : kEntire), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Sort key:"), kEntire, kIgnore }));
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("             "), kTabbed, kRegex }));

    GtkWidget* findWhat = mkCombo(data.sort.regexHistory);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Find what:"), findWhat }));
    GtkWidget* matchCase = mkCheck("Match case", data.sort.regexMatchCase);
    GtkWidget* useKey    = mkCheck("Specify keys using capture groups", data.sort.regexUseKey);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ matchCase, useKey }));
    GtkWidget* keys = mkCombo(data.sort.keygroupHistory);
    gtk_widget_set_tooltip_text(keys, "e.g.: 2,1d,3an  —  a/d = asc/desc, b/l/n/w = type override");
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Keys:"), keys }));

    std::vector<std::string> localeIds, localeTitles;
    icuLocaleList(localeIds, localeTitles);
    std::vector<std::string> localeItems;
    localeItems.push_back("(current locale)");
    localeItems.insert(localeItems.end(), localeTitles.begin(), localeTitles.end());
    int sel = 0;
    if (!data.sort.localeName.empty())
        for (size_t i = 0; i < localeIds.size(); ++i)
            if (localeIds[i] == data.sort.localeName) { sel = (int)i + 1; break; }
    GtkWidget* locale = mkPopup(localeItems, sel);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Locale:"), locale }));

    GtkWidget* caseSensitive = mkCheck("Case sensitive", data.sort.localeCaseSensitive);
    GtkWidget* digitsNum     = mkCheck("Digits as numbers", data.sort.localeDigitsAsNumbers);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ caseSensitive, digitsNum }));
    GtkWidget* ignDia = mkCheck("Ignore diacritics", data.sort.localeIgnoreDiacritics);
    GtkWidget* ignSym = mkCheck("Ignore symbols", data.sort.localeIgnoreSymbols);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ ignDia, ignSym }));

    dlg.okCancel();
    if (dlg.run() != 1) return true;

    data.sort.sortColumnSelectionOnly = checkOn(withinSel);
    data.sort.sortDescending = checkOn(desc);
    data.sort.sortType = checkOn(tLoc) ? SortSettings::Locale
                       : checkOn(tNum) ? SortSettings::Numeric
                       : checkOn(tWid) ? SortSettings::Width : SortSettings::Binary;
    data.sort.keyType = checkOn(kIgnore) ? SortSettings::IgnoreBlanks
                      : checkOn(kTabbed) ? SortSettings::Tabbed
                      : checkOn(kRegex) ? SortSettings::Regex : SortSettings::EntireColumn;
    data.sort.regexMatchCase = checkOn(matchCase);
    data.sort.regexUseKey = checkOn(useKey);
    data.sort.localeCaseSensitive = checkOn(caseSensitive);
    data.sort.localeDigitsAsNumbers = checkOn(digitsNum);
    data.sort.localeIgnoreDiacritics = checkOn(ignDia);
    data.sort.localeIgnoreSymbols = checkOn(ignSym);
    int li = popupIndex(locale);
    data.sort.localeName = (li > 0 && (size_t)li <= localeIds.size()) ? localeIds[li - 1] : "";
    if (data.sort.keyType == SortSettings::Regex) {
        std::string fw = comboText(findWhat);
        if (!fw.empty()) pushHistory(data.sort.regexHistory, fw);
    }
    std::string ks = comboText(keys);
    if ((data.sort.keyType == SortSettings::Tabbed || data.sort.regexUseKey) && !ks.empty())
        pushHistory(data.sort.keygroupHistory, ks);
    return false;
}

// ===========================================================================
// CSV / conversion dialog (IDD_CSV)
// ===========================================================================
bool runCsvDialog(ColumnsPlusPlusData& data) {
    ModalDlg dlg("Conversion settings");

    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Column separator:"));
    GtkWidget* rComma = mkRadio("Comma", nullptr);
    GtkWidget* rSemi  = mkRadio("Semicolon", rComma);
    GtkWidget* rPipe  = mkRadio("Vertical line", rComma);
    GtkWidget* rOther = mkRadio("Other:", rComma);
    GtkWidget* sep    = mkEntry(std::string(1, data.csv.separator), 3);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.csv.separator == ',' ? rComma : data.csv.separator == ';' ? rSemi :
        data.csv.separator == '|' ? rPipe : rOther), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ rComma, rSemi, rPipe }));
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ rOther, sep }));

    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Separated values syntax:"));
    GtkWidget* quote      = mkCheck("Quote", data.csv.quote);
    GtkWidget* apostrophe = mkCheck("Apostrophe", data.csv.apostrophe);
    GtkWidget* escape     = mkCheck("Escape char:", data.csv.escape);
    GtkWidget* escapeChar = mkEntry(std::string(1, data.csv.escapeChar), 3);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ quote, apostrophe, escape, escapeChar }));

    GtkWidget* preserve = mkCheck("Preserve quotes/escapes/blanks when converting to tabbed",
                                  data.csv.preserveQuotes);
    GtkWidget* keepSep  = mkCheck("Keep separator character when converting to tabbed",
                                  data.csv.keepSeparator);
    gtk_box_append(GTK_BOX(dlg.vbox), preserve);
    gtk_box_append(GTK_BOX(dlg.vbox), keepSep);

    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Tab/newline/return handling:"));
    GtkWidget* rTNR = mkRadio("Backslash (\\t \\n \\r):", nullptr);
    GtkWidget* tnr  = mkEntry(std::string(1, data.csv.encodeTNR), 3);
    GtkWidget* rURL = mkRadio("URL (%09 %0A %0D):", rTNR);
    GtkWidget* url  = mkEntry(std::string(1, data.csv.encodeURL), 3);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ rTNR, tnr, rURL, url }));
    GtkWidget* rReplace = mkRadio("Replace:", rTNR);
    GtkWidget* repTab = mkEntry(data.csv.replaceTab, 7);
    GtkWidget* repLF  = mkEntry(data.csv.replaceLF, 7);
    GtkWidget* repCR  = mkEntry(data.csv.replaceCR, 7);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ rReplace, mkLabel("Tab:"), repTab,
                                             mkLabel("LF:"), repLF, mkLabel("CR:"), repCR }));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.csv.encodingStyle == CsvSettings::TNR ? rTNR :
        data.csv.encodingStyle == CsvSettings::URL ? rURL : rReplace), TRUE);

    dlg.okCancel();
    if (dlg.run() != 1) return true;

    std::string s = entryText(sep);
    if (checkOn(rComma)) s = ",";
    else if (checkOn(rSemi)) s = ";";
    else if (checkOn(rPipe)) s = "|";
    if (s.empty() || s[0] == '\n' || s[0] == '\r') {
        cppAlert("Conversion settings", "The column separator must be a single character.");
        return true;
    }
    data.csv.separator = s[0];
    data.csv.quote = checkOn(quote);
    data.csv.apostrophe = checkOn(apostrophe);
    data.csv.escape = checkOn(escape);
    if (data.csv.escape) { std::string e = entryText(escapeChar); if (!e.empty()) data.csv.escapeChar = e[0]; }
    data.csv.preserveQuotes = checkOn(preserve);
    data.csv.keepSeparator = checkOn(keepSep);
    data.csv.encodingStyle = checkOn(rTNR) ? CsvSettings::TNR
                           : checkOn(rURL) ? CsvSettings::URL : CsvSettings::Replace;
    { std::string e = entryText(tnr); if (!e.empty()) data.csv.encodeTNR = e[0]; }
    { std::string e = entryText(url); if (!e.empty()) data.csv.encodeURL = e[0]; }
    data.csv.replaceTab = entryText(repTab);
    data.csv.replaceLF  = entryText(repLF);
    data.csv.replaceCR  = entryText(repCR);
    return false;
}

// ===========================================================================
// CALCULATION RESULTS dialog (IDD_CALCULATION_RESULTS) — Add / Average numbers
// ===========================================================================
namespace {
struct CalcResCtx {
    ColumnsPlusPlusData* data;
    const std::vector<double>* values;
    std::vector<NumericFormat>* autoFormats;
    std::string* copyOut;
    GtkWidget *results, *tNone, *tComma, *tApos, *tBlank, *autoDec, *places, *suppress;

    void render() {
        ColumnsPlusPlusData& d = *data;
        std::string separator = checkOn(tApos)  ? "'"
                              : checkOn(tBlank) ? " "
                              : checkOn(tComma) ? (d.settings.decimalSeparatorIsComma ? "." : ",")
                              : "";
        bool autoDecimals = checkOn(autoDec);
        std::string show, copy;
        for (size_t i = 0; i < values->size(); ++i) {
            NumericFormat format = (*autoFormats)[i];
            format.thousands = separator;
            if (!autoDecimals) {
                format.maxDec = std::max(0, entryInt(places));
                format.minDec = checkOn(suppress) ? -1 : format.maxDec;
            }
            std::string s = d.formatNumber((*values)[i], format);
            if (!show.empty() && !s.empty()) show += "  |  ";
            show += s;
            if (i > 0) copy += '\t';
            copy += s;
        }
        gtk_editable_set_text(GTK_EDITABLE(results), show.c_str());
        *copyOut = copy;
    }
    static void changed(GtkWidget*, gpointer p) { ((CalcResCtx*)p)->render(); }
};
} // namespace

bool runCalculationResultsDialog(ColumnsPlusPlusData& data,
                                 const std::vector<double>& values,
                                 std::vector<NumericFormat>& autoFormats,
                                 bool isMean, int columns, int numbers, bool hasSpace,
                                 std::string& copyResults) {
    ModalDlg dlg("Calculation results");
    CalcResCtx ctx;
    ctx.data = &data; ctx.values = &values; ctx.autoFormats = &autoFormats; ctx.copyOut = &copyResults;

    char msg[160];
    g_snprintf(msg, sizeof(msg), "%s of %d value%s%s%s",
               isMean ? "Mean" : "Sum", numbers, numbers == 1 ? "" : "s",
               columns > 1 ? " in " : "",
               columns > 1 ? (std::to_string(columns) + " columns").c_str() : "");
    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel(msg));

    ctx.results = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(ctx.results), FALSE);
    gtk_widget_set_hexpand(ctx.results, TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), ctx.results);

    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Thousands separator:"));
    ctx.tNone  = mkRadio("None", nullptr);
    ctx.tComma = mkRadio(data.settings.decimalSeparatorIsComma ? "Period" : "Comma", ctx.tNone);
    ctx.tApos  = mkRadio("Apostrophe", ctx.tNone);
    ctx.tBlank = mkRadio("Blank", ctx.tNone);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.calc.thousands == CalculateSettings::Apostrophe ? ctx.tApos :
        data.calc.thousands == CalculateSettings::Blank ? ctx.tBlank :
        data.calc.thousands == CalculateSettings::Comma ? ctx.tComma : ctx.tNone), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ ctx.tNone, ctx.tComma, ctx.tApos, ctx.tBlank }));

    ctx.autoDec = mkCheck("Automatic decimal places", data.calc.autoDecimals);
    gtk_box_append(GTK_BOX(dlg.vbox), ctx.autoDec);
    ctx.places = mkEntry(std::to_string(data.calc.decimalPlaces), 4);
    ctx.suppress = mkCheck("Suppress trailing zeros", !data.calc.decimalsFixed);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Decimal places:"), ctx.places, ctx.suppress }));

    GtkWidget* insert = mkCheck(hasSpace ? "Insert results in the last line"
                                         : "Insert a line and add results",
                                hasSpace ? data.calc.insert : data.calc.addLine);
    gtk_box_append(GTK_BOX(dlg.vbox), insert);

    for (GtkWidget* w : { ctx.tNone, ctx.tComma, ctx.tApos, ctx.tBlank, ctx.autoDec, ctx.suppress })
        g_signal_connect(w, "toggled", G_CALLBACK(CalcResCtx::changed), &ctx);
    g_signal_connect(ctx.places, "changed", G_CALLBACK(CalcResCtx::changed), &ctx);

    dlg.okCancel("Copy && Insert");
    ctx.render();
    int r = dlg.run();

    // Persist the chosen formatting prefs (macOS persists even on cancel).
    data.calc.thousands = checkOn(ctx.tApos) ? CalculateSettings::Apostrophe
                        : checkOn(ctx.tBlank) ? CalculateSettings::Blank
                        : checkOn(ctx.tComma) ? CalculateSettings::Comma : CalculateSettings::None;
    data.calc.autoDecimals = checkOn(ctx.autoDec);
    data.calc.decimalPlaces = std::max(0, std::min(16, entryInt(ctx.places)));
    data.calc.decimalsFixed = !checkOn(ctx.suppress);
    if (hasSpace) data.calc.insert = checkOn(insert);
    else          data.calc.addLine = checkOn(insert);

    if (r != 1) return false;
    // Copy to clipboard (the Windows OK action does this).
    gdk_clipboard_set_text(gtk_widget_get_clipboard(dlg.win), copyResults.c_str());
    return true;
}

// ===========================================================================
// OPTIONS dialog (IDD_OPTIONS)
// ===========================================================================
void ColumnsPlusPlusData::showOptionsDialog() {
    ModalDlg dlg("Columns++ Options");

    GtkWidget* menubar = mkCheck(
        "Show Columns++ on the main menu bar (preserved; menu placement is fixed here)",
        showOnMenuBar);
    gtk_widget_set_sensitive(menubar, FALSE);
    gtk_box_append(GTK_BOX(dlg.vbox), menubar);
    GtkWidget* replaceStays = mkCheck("Replace: don't move to the following occurrence", replaceStaysPut);
    gtk_box_append(GTK_BOX(dlg.vbox), replaceStays);
    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Automatically extend selections to form rectangles:"));
    GtkWidget* extSingle = mkCheck("Selections on one line extend downward to the last line", extendSingleLine);
    GtkWidget* extRows   = mkCheck("Full row selections are replaced by the enclosing rectangle", extendFullLines);
    GtkWidget* extZero   = mkCheck("Zero-width selections extend right to the end of the longest line", extendZeroWidth);
    for (GtkWidget* w : { extSingle, extRows, extZero }) {
        gtk_widget_set_margin_start(w, 14);
        gtk_box_append(GTK_BOX(dlg.vbox), w);
    }
    GtkWidget* decComma = mkCheck("Decimal separator is comma", settings.decimalSeparatorIsComma);
    gtk_box_append(GTK_BOX(dlg.vbox), decComma);

    dlg.okCancel();
    if (dlg.run() != 1) return;

    replaceStaysPut  = checkOn(replaceStays);
    extendSingleLine = checkOn(extSingle);
    extendFullLines  = checkOn(extRows);
    extendZeroWidth  = checkOn(extZero);
    bool wantComma = checkOn(decComma);
    if (wantComma != settings.decimalSeparatorIsComma) {
        updateActiveScintilla();
        DocumentData* ddp = getDocument();
        ddp->settings.decimalSeparatorIsComma = settings.decimalSeparatorIsComma = wantComma;
        npp(NPPM_SETMENUITEMCHECK, (uintptr_t)decimalSeparatorMenuItem, settings.decimalSeparatorIsComma);
    }
    saveConfiguration();
}

// ===========================================================================
// ELASTIC PROFILE dialog (IDD_ELASTIC_TABSTOPS_PROFILE)
// ===========================================================================
namespace {
struct ProfileCtx {
    ColumnsPlusPlusData* data;
    GtkWidget *profileSel, *leading, *lineUp, *eolTab, *overrideTab, *tabSize, *minSpace;
    GtkWidget *mYes, *mNo, *mBest, *noMnemonics;
    std::vector<std::string> names;   // popup order (index 0 = "(current settings)")

    static void loadProfile(GtkWidget*, gpointer p) {
        ProfileCtx* c = (ProfileCtx*)p;
        int idx = popupIndex(c->profileSel);
        if (idx <= 0 || (size_t)idx >= c->names.size()) return;
        auto it = c->data->profiles.find(c->names[idx]);
        if (it == c->data->profiles.end()) return;
        const ElasticTabsProfile& pr = it->second;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->leading), pr.leadingTabsIndent);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->lineUp),  pr.lineUpAll);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->eolTab),  pr.treatEolAsTab);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->overrideTab), pr.overrideTabSize);
        gtk_editable_set_text(GTK_EDITABLE(c->tabSize), std::to_string(pr.minimumOrLeadingTabSize).c_str());
        gtk_editable_set_text(GTK_EDITABLE(c->minSpace), std::to_string(pr.minimumSpaceBetweenColumns).c_str());
        gtk_check_button_set_active(GTK_CHECK_BUTTON(
            pr.monospace == ElasticTabsProfile::MonospaceAlways ? c->mYes :
            pr.monospace == ElasticTabsProfile::MonospaceNever ? c->mNo : c->mBest), TRUE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c->noMnemonics), pr.monospaceNoMnemonics);
    }
};
} // namespace

void ColumnsPlusPlusData::showElasticProfile() {
    updateActiveScintilla();
    ModalDlg dlg("Elastic tabstops profile");
    ProfileCtx ctx;
    ctx.data = this;

    ctx.names.push_back("(current settings)");
    for (auto& kv : profiles) ctx.names.push_back(kv.first);
    ctx.profileSel = mkPopup(ctx.names, 0);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Profile:"), ctx.profileSel }));

    ctx.leading = mkCheck("Use leading tabs for indentation only (don't make them elastic)", settings.leadingTabsIndent);
    ctx.lineUp  = mkCheck("Line up elastic tabstops throughout the entire document", settings.lineUpAll);
    ctx.eolTab  = mkCheck("Don't allow text following the last tab on a line to span columns", settings.treatEolAsTab);
    gtk_box_append(GTK_BOX(dlg.vbox), ctx.leading);
    gtk_box_append(GTK_BOX(dlg.vbox), ctx.lineUp);
    gtk_box_append(GTK_BOX(dlg.vbox), ctx.eolTab);
    ctx.overrideTab = mkCheck("Override default/language tab size:", settings.overrideTabSize);
    ctx.tabSize = mkEntry(std::to_string(settings.minimumOrLeadingTabSize), 4);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ ctx.overrideTab, ctx.tabSize }));
    ctx.minSpace = mkEntry(std::to_string(settings.minimumSpaceBetweenColumns), 4);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Minimum space between elastic columns:"), ctx.minSpace }));

    ctx.mYes = mkRadio("Yes", nullptr);
    ctx.mNo  = mkRadio("No", ctx.mYes);
    ctx.mBest = mkRadio("Best estimate", ctx.mYes);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        settings.monospace == ElasticTabsProfile::MonospaceAlways ? ctx.mYes :
        settings.monospace == ElasticTabsProfile::MonospaceNever ? ctx.mNo : ctx.mBest), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Monospaced font optimization:"), ctx.mYes, ctx.mNo, ctx.mBest }));
    ctx.noMnemonics = mkCheck("Don't expand mnemonics for non-printing characters when monospaced", settings.monospaceNoMnemonics);
    gtk_box_append(GTK_BOX(dlg.vbox), ctx.noMnemonics);

    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Disable elastic tabstops (all profiles):"));
    GtkWidget* disSize = mkCheck("when opening files over", disableOverSize > 0);
    GtkWidget* disSizeVal = mkEntry(std::to_string(abs(disableOverSize)), 6);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ disSize, disSizeVal, mkLabel("KB") }));
    GtkWidget* disLines = mkCheck("when opening files over", disableOverLines > 0);
    GtkWidget* disLinesVal = mkEntry(std::to_string(abs(disableOverLines)), 6);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ disLines, disLinesVal, mkLabel("lines") }));

    g_signal_connect(ctx.profileSel, "changed", G_CALLBACK(ProfileCtx::loadProfile), &ctx);

    dlg.okCancel();
    if (dlg.run() != 1) return;

    settings.leadingTabsIndent = checkOn(ctx.leading);
    settings.lineUpAll = checkOn(ctx.lineUp);
    settings.treatEolAsTab = checkOn(ctx.eolTab);
    settings.overrideTabSize = checkOn(ctx.overrideTab);
    settings.minimumOrLeadingTabSize = std::max(1, std::min(999, entryInt(ctx.tabSize)));
    settings.minimumSpaceBetweenColumns = std::max(1, std::min(999, entryInt(ctx.minSpace)));
    settings.monospace = checkOn(ctx.mYes) ? ElasticTabsProfile::MonospaceAlways
                       : checkOn(ctx.mNo) ? ElasticTabsProfile::MonospaceNever
                       : ElasticTabsProfile::MonospaceBest;
    settings.monospaceNoMnemonics = checkOn(ctx.noMnemonics);
    int sizeVal = std::max(1, entryInt(disSizeVal));
    int lineVal = std::max(1, entryInt(disLinesVal));
    disableOverSize  = checkOn(disSize) ? sizeVal : -sizeVal;
    disableOverLines = checkOn(disLines) ? lineVal : -lineVal;

    DocumentData* ddp = getDocument();
    ddp->settings = settings;
    if (settings.elasticEnabled) {
        if (settings.overrideTabSize) SetTabWidth(settings.minimumOrLeadingTabSize);
        analyzeTabstops(*ddp);
        setTabstops(*ddp);
    }
    saveConfiguration();
}

// ===========================================================================
// ABOUT dialog (IDD_ABOUT)
// ===========================================================================
void ColumnsPlusPlusData::showAboutDialog() {
    ModalDlg dlg("Columns++ for Notepad++ (Linux port)");
    GtkWidget* l = mkLabel(
        "Columns++ offers features for working with text and data arranged in\n"
        "columns: elastic tabstops, column alignment, sorting, search and replace\n"
        "within a region, per-row expression Calculate, timestamp \xE2\x87\x84 date/time\n"
        "conversion, Add/Average numbers and separated-values conversion.\n\n"
        "Original Windows plugin \xC2\xA9 2023\xE2\x80\x932026 Randall Joseph Fellmy (coises.com),\n"
        "GNU GPL v3+.\n\n"
        "Linux port by Andrew Letov \xE2\x80\x94 the complete feature set, built on the\n"
        "Scintilla SCI_* interface with ICU-backed regex, collation and dates.");
    gtk_box_append(GTK_BOX(dlg.vbox), l);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget* ok   = gtk_button_new_with_label("OK");
    GtkWidget* help = gtk_button_new_with_label("Help (web)");
    GtkWidget* repo = gtk_button_new_with_label("Nextpad++ Linux");
    g_signal_connect(ok, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        ((ModalDlg*)p)->finish(1);
    }), &dlg);
    g_signal_connect(help, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        gtk_show_uri(GTK_WINDOW(((ModalDlg*)p)->win),
                     "https://coises.github.io/ColumnsPlusPlus/help.htm", GDK_CURRENT_TIME);
    }), &dlg);
    g_signal_connect(repo, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        gtk_show_uri(GTK_WINDOW(((ModalDlg*)p)->win),
                     "https://github.com/nextpad-plus-plus/nextpad-plus-plus-linux", GDK_CURRENT_TIME);
    }), &dlg);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(row), help);
    gtk_box_append(GTK_BOX(row), repo);
    gtk_box_append(GTK_BOX(dlg.vbox), row);
    gtk_window_set_default_widget(GTK_WINDOW(dlg.win), ok);
    dlg.run();
}

// ===========================================================================
// SEARCH — modeless find/replace panel (IDD_SEARCH). showSearchDialog() lives
// in Search.cpp; this panel drives the engine (searchFind/Replace/ReplaceAll/
// Count) and is wired back through searchData.setMessage / syncFindButton.
// ===========================================================================
void updateSearchRegion(ColumnsPlusPlusData& data, bool modify, bool remove);   // Search.cpp

namespace {

struct SearchPanel {
    ColumnsPlusPlusData* data = nullptr;
    GtkWidget *win, *findCombo, *replaceCombo;
    GtkWidget *rNormal, *rExtended, *rRegex;
    GtkWidget *matchCase, *wholeWord, *backward;
    GtkWidget *autoSet, *autoClearSel, *autoClearInd;
    GtkWidget *findButton, *message;

    std::string findText()    const {
        gchar* t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(findCombo));
        std::string s = t ? t : ""; g_free(t); return s;
    }
    std::string replaceText() const {
        gchar* t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(replaceCombo));
        std::string s = t ? t : ""; g_free(t); return s;
    }
    void setMsg(const std::string& s) { gtk_label_set_text(GTK_LABEL(message), s.c_str()); }

    void syncSettings() {
        ColumnsPlusPlusData& d = *data;
        d.searchData.mode = checkOn(rRegex) ? SearchData::Regex
                          : checkOn(rExtended) ? SearchData::Extended : SearchData::Normal;
        d.searchData.backward           = checkOn(backward);
        d.searchData.wholeWord          = checkOn(wholeWord);
        d.searchData.matchCase          = checkOn(matchCase);
        d.searchData.autoSetSelection   = checkOn(autoSet);
        d.searchData.autoClearSelection = checkOn(autoClearSel);
        d.searchData.autoClear          = checkOn(autoClearInd);
    }

    bool validateFind() {
        std::string f = findText();
        if (f.empty()) { setMsg("Enter something to find."); return false; }
        if (data->searchData.mode == SearchData::Regex) {
            RegularExpression rx;
            std::string e = rx.find(f, data->searchData.matchCase);
            if (!e.empty()) { setMsg(e); return false; }
        }
        return true;
    }

    void refreshCombo(GtkWidget* c, const std::vector<std::string>& h) {
        GtkWidget* e = gtk_combo_box_get_child(GTK_COMBO_BOX(c));
        std::string cur = gtk_editable_get_text(GTK_EDITABLE(e));
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(c));
        for (auto it = h.rbegin(); it != h.rend(); ++it)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), it->c_str());
        gtk_editable_set_text(GTK_EDITABLE(e), cur.c_str());
    }
    void pushFind()    { pushHistory(data->searchData.findHistory, findText());
                         refreshCombo(findCombo, data->searchData.findHistory); }
    void pushReplace() { pushHistory(data->searchData.replaceHistory, replaceText());
                         refreshCombo(replaceCombo, data->searchData.replaceHistory); }

    void syncFindButton() {
        ColumnsPlusPlusData& d = *data;
        bool bw = d.searchData.mode != SearchData::Regex && d.searchData.backward;
        const char* title;
        if (d.getSearchRegionStatus() == ColumnsPlusPlusData::SearchRegionNotReady || d.searchData.wrap)
             title = bw ? "Find Last"     : "Find First";
        else title = bw ? "Find Previous" : "Find Next";
        gtk_button_set_label(GTK_BUTTON(findButton), title);
    }
};

GtkWidget* panelBtn(const char* title, GCallback cb, gpointer ud) {
    GtkWidget* b = gtk_button_new_with_label(title);
    g_signal_connect(b, "clicked", cb, ud);
    return b;
}

} // namespace

void runSearchPanel(ColumnsPlusPlusData& data) {
    SearchPanel* p = new SearchPanel;
    p->data = &data;

    p->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(p->win), "Columns++ \xE2\x80\x94 Search in region");
    gtk_window_set_resizable(GTK_WINDOW(p->win), FALSE);
    // Modeless: closing really closes; settings are synced in close-request
    // BEFORE the default handler destroys the tree.

    GtkWidget* grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 10);
    gtk_widget_set_margin_start(grid, 14);
    gtk_widget_set_margin_end(grid, 14);
    gtk_window_set_child(GTK_WINDOW(p->win), grid);

    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(grid), left);
    gtk_box_append(GTK_BOX(grid), right);

    p->findCombo = mkCombo(data.searchData.findHistory);
    gtk_box_append(GTK_BOX(left), hbox({ mkLabel("Find what:"), p->findCombo }));
    p->replaceCombo = mkCombo(data.searchData.replaceHistory);
    gtk_box_append(GTK_BOX(left), hbox({ mkLabel("Replace with:"), p->replaceCombo }));

    p->rNormal   = mkRadio("Normal", nullptr);
    p->rExtended = mkRadio("Extended", p->rNormal);
    p->rRegex    = mkRadio("Regular expression", p->rNormal);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.searchData.mode == SearchData::Regex ? p->rRegex :
        data.searchData.mode == SearchData::Extended ? p->rExtended : p->rNormal), TRUE);
    gtk_box_append(GTK_BOX(left), hbox({ p->rNormal, p->rExtended, p->rRegex }));

    p->matchCase = mkCheck("Match case", data.searchData.matchCase);
    p->wholeWord = mkCheck("Whole word", data.searchData.wholeWord);
    p->backward  = mkCheck("Backward", data.searchData.backward);
    gtk_box_append(GTK_BOX(left), hbox({ p->matchCase, p->wholeWord, p->backward }));

    GtkWidget* regionRow = hbox({
        mkLabel("Region:"),
        panelBtn("Set", G_CALLBACK(+[](GtkButton*, gpointer u) {
            SearchPanel* sp = (SearchPanel*)u;
            sp->syncSettings(); updateSearchRegion(*sp->data, false, false); sp->syncFindButton();
        }), p),
        panelBtn("Add", G_CALLBACK(+[](GtkButton*, gpointer u) {
            SearchPanel* sp = (SearchPanel*)u;
            sp->syncSettings(); updateSearchRegion(*sp->data, true, false); sp->syncFindButton();
        }), p),
        panelBtn("Remove", G_CALLBACK(+[](GtkButton*, gpointer u) {
            SearchPanel* sp = (SearchPanel*)u;
            sp->syncSettings(); updateSearchRegion(*sp->data, true, true); sp->syncFindButton();
        }), p),
        panelBtn("Clear ind.", G_CALLBACK(+[](GtkButton*, gpointer u) {
            SearchPanel* sp = (SearchPanel*)u;
            sp->data->SetIndicatorCurrent(sp->data->searchData.indicator);
            sp->data->IndicatorClearRange(0, sp->data->Length());
            sp->syncFindButton();
        }), p),
    });
    gtk_box_append(GTK_BOX(left), regionRow);

    p->autoSet      = mkCheck("Auto-set region from selection", data.searchData.autoSetSelection);
    p->autoClearSel = mkCheck("Auto-clear selection after marking", data.searchData.autoClearSelection);
    p->autoClearInd = mkCheck("Auto-clear indicator on new search", data.searchData.autoClear);
    gtk_box_append(GTK_BOX(left), p->autoSet);
    gtk_box_append(GTK_BOX(left), p->autoClearSel);
    gtk_box_append(GTK_BOX(left), p->autoClearInd);

    p->message = mkLabel("");
    gtk_label_set_wrap(GTK_LABEL(p->message), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(p->message), 48);
    gtk_box_append(GTK_BOX(left), p->message);

    // Mode/option toggles re-sync the Find button label.
    for (GtkWidget* w : { p->rNormal, p->rExtended, p->rRegex, p->matchCase, p->wholeWord,
                          p->backward, p->autoSet, p->autoClearSel, p->autoClearInd })
        g_signal_connect(w, "toggled", G_CALLBACK(+[](GtkWidget*, gpointer u) {
            SearchPanel* sp = (SearchPanel*)u;
            sp->syncSettings(); sp->syncFindButton();
        }), p);

    p->findButton = panelBtn("Find First", G_CALLBACK(+[](GtkButton*, gpointer u) {
        SearchPanel* sp = (SearchPanel*)u;
        sp->syncSettings(); if (!sp->validateFind()) return;
        sp->pushFind(); sp->data->searchFind(); sp->syncFindButton();
    }), p);
    gtk_widget_add_css_class(p->findButton, "suggested-action");
    gtk_box_append(GTK_BOX(right), p->findButton);
    gtk_box_append(GTK_BOX(right), panelBtn("Count", G_CALLBACK(+[](GtkButton*, gpointer u) {
        SearchPanel* sp = (SearchPanel*)u;
        sp->syncSettings(); if (!sp->validateFind()) return;
        sp->pushFind(); sp->data->searchCount(false, false, false); sp->syncFindButton();
    }), p));
    gtk_box_append(GTK_BOX(right), panelBtn("Select All", G_CALLBACK(+[](GtkButton*, gpointer u) {
        SearchPanel* sp = (SearchPanel*)u;
        sp->syncSettings(); if (!sp->validateFind()) return;
        sp->pushFind(); sp->data->searchCount(true, false, false); sp->syncFindButton();
    }), p));
    gtk_box_append(GTK_BOX(right), panelBtn("Replace", G_CALLBACK(+[](GtkButton*, gpointer u) {
        SearchPanel* sp = (SearchPanel*)u;
        sp->syncSettings(); if (!sp->validateFind()) return;
        sp->pushFind(); sp->pushReplace(); sp->data->searchReplace(); sp->syncFindButton();
    }), p));
    gtk_box_append(GTK_BOX(right), panelBtn("Replace All", G_CALLBACK(+[](GtkButton*, gpointer u) {
        SearchPanel* sp = (SearchPanel*)u;
        sp->syncSettings(); if (!sp->validateFind()) return;
        sp->pushFind(); sp->pushReplace(); sp->data->searchReplaceAll(false, false); sp->syncFindButton();
    }), p));
    gtk_box_append(GTK_BOX(right), panelBtn("Close", G_CALLBACK(+[](GtkButton*, gpointer u) {
        gtk_window_close(GTK_WINDOW(((SearchPanel*)u)->win));
    }), p));
    gtk_window_set_default_widget(GTK_WINDOW(p->win), p->findButton);

    // Escape closes the panel.
    GtkEventController* k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer u) -> gboolean {
            if (kv == GDK_KEY_Escape) { gtk_window_close(GTK_WINDOW(((SearchPanel*)u)->win)); return TRUE; }
            return FALSE;
        }), p);
    gtk_widget_add_controller(p->win, k);

    // Close: sync settings and clear the hooks BEFORE the widget tree dies
    // (macOS windowWillClose:), then let the default handler destroy.
    g_signal_connect(p->win, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer u) -> gboolean {
        SearchPanel* sp = (SearchPanel*)u;
        ColumnsPlusPlusData* d = sp->data;
        if (d) {
            sp->syncSettings();
            d->searchData.dialog         = nullptr;
            d->searchData.setMessage     = nullptr;
            d->searchData.syncFindButton = nullptr;
        }
        return FALSE;
    }), p);
    g_signal_connect(p->win, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer u) {
        delete (SearchPanel*)u;
    }), p);

    data.searchData.setMessage     = [p](const std::string& s) { p->setMsg(s); };
    data.searchData.syncFindButton = [p]() { p->syncFindButton(); };
    data.searchData.dialog = p;

    p->syncFindButton();
    gtk_window_present(GTK_WINDOW(p->win));
}

void focusSearchPanel(void* controller) {
    if (!controller) return;
    gtk_window_present(GTK_WINDOW(((SearchPanel*)controller)->win));
}

// ===========================================================================
// CALCULATE dialog (IDD_CALCULATE) — validate loop like macOS.
// ===========================================================================
bool runCalculateDialog(ColumnsPlusPlusData& data,
                        const std::function<std::string(const std::string&, const std::string&)>& validate) {
    ModalDlg dlg("Calculate");

    GtkWidget* formula = mkCombo(data.calc.formulaHistory);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Formula:"), formula }));
    GtkWidget* regex = mkCombo(data.calc.regexHistory);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Regex (optional):"), regex }));
    GtkWidget* matchCase = mkCheck("Match case", data.calc.matchCase);
    GtkWidget* skipUnmatched = mkCheck("Skip unmatched lines", data.calc.skipUnmatched);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ matchCase, skipUnmatched }));

    GtkWidget* thNone  = mkRadio("None", nullptr);
    GtkWidget* thComma = mkRadio(data.settings.decimalSeparatorIsComma ? "Period" : "Comma", thNone);
    GtkWidget* thApos  = mkRadio("Apostrophe", thNone);
    GtkWidget* thBlank = mkRadio("Blank", thNone);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        data.calc.thousands == CalculateSettings::Comma ? thComma :
        data.calc.thousands == CalculateSettings::Apostrophe ? thApos :
        data.calc.thousands == CalculateSettings::Blank ? thBlank : thNone), TRUE);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Thousands:"), thNone, thComma, thApos, thBlank }));

    GtkWidget* places = mkEntry(std::to_string(data.calc.decimalPlaces), 4);
    GtkWidget* suppress = mkCheck("Suppress trailing zeros", !data.calc.decimalsFixed);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Decimal places:"), places, suppress }));

    GtkWidget* formatAsTime = mkCheck("Format as time", data.calc.formatAsTime);
    GtkWidget* tfBtn = gtk_button_new_with_label("Time formats\xE2\x80\xA6");
    g_signal_connect(tfBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer u) {
        ((ColumnsPlusPlusData*)u)->showTimeFormatsDialog();
    }), &data);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ formatAsTime, tfBtn }));

    GtkWidget* tabbed  = mkCheck("Tabbed", data.calc.tabbed);
    GtkWidget* aligned = mkCheck("Aligned", data.calc.aligned);
    GtkWidget* left    = mkCheck("Insert at left", data.calc.left);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ tabbed, aligned, left }));

    dlg.okCancel();

    while (true) {
        if (dlg.run() != 1) return true;   // cancelled
        std::string f = comboText(formula);
        std::string r = comboText(regex);
        data.calc.matchCase = checkOn(matchCase);
        std::string err = validate(f, r);
        if (!err.empty()) { cppAlert("Calculate", err); continue; }
        pushHistory(data.calc.formulaHistory, f);
        pushHistory(data.calc.regexHistory, r);
        data.calc.skipUnmatched = checkOn(skipUnmatched);
        data.calc.thousands = checkOn(thComma) ? CalculateSettings::Comma
                            : checkOn(thApos)  ? CalculateSettings::Apostrophe
                            : checkOn(thBlank) ? CalculateSettings::Blank
                                               : CalculateSettings::None;
        data.calc.decimalPlaces = std::max(0, std::min(16, entryInt(places)));
        data.calc.decimalsFixed = !checkOn(suppress);
        data.calc.formatAsTime  = checkOn(formatAsTime);
        data.calc.tabbed        = checkOn(tabbed);
        data.calc.aligned       = checkOn(aligned);
        data.calc.left          = checkOn(left);
        return false;
    }
}

// ===========================================================================
// TIMESTAMPS dialogs (IDD_TIMESTAMP / IDD_TIME_FORMATS)
// ===========================================================================
enum { TS_CANCEL = 0, TS_TO_DATETIME = 1, TS_TO_COUNTER = 2 };

namespace {
int counterIdx(TimestampSettings::CounterType t) {
    switch (t) {
    case TimestampSettings::CounterType::Unix: return 0;
    case TimestampSettings::CounterType::File: return 1;
    case TimestampSettings::CounterType::Ex00: return 2;
    case TimestampSettings::CounterType::Ex04: return 3;
    default: return 4;
    }
}
TimestampSettings::CounterType counterTypeAt(int i) {
    switch (i) {
    case 0: return TimestampSettings::CounterType::Unix;
    case 1: return TimestampSettings::CounterType::File;
    case 2: return TimestampSettings::CounterType::Ex00;
    case 3: return TimestampSettings::CounterType::Ex04;
    default: return TimestampSettings::CounterType::custom;
    }
}
int priorityIdx(TimestampSettings::DatePriority p) {
    switch (p) {
    case TimestampSettings::DatePriority::ymd: return 0;
    case TimestampSettings::DatePriority::mdy: return 1;
    case TimestampSettings::DatePriority::dmy: return 2;
    default: return 3;
    }
}
TimestampSettings::DatePriority priorityTypeAt(int i) {
    switch (i) {
    case 0: return TimestampSettings::DatePriority::ymd;
    case 1: return TimestampSettings::DatePriority::mdy;
    case 2: return TimestampSettings::DatePriority::dmy;
    default: return TimestampSettings::DatePriority::custom;
    }
}
int formatIdx(TimestampSettings::DateFormat f) {
    switch (f) {
    case TimestampSettings::DateFormat::iso8601: return 0;
    case TimestampSettings::DateFormat::localeShort: return 1;
    case TimestampSettings::DateFormat::localeLong: return 2;
    default: return 3;
    }
}
TimestampSettings::DateFormat formatTypeAt(int i) {
    switch (i) {
    case 0: return TimestampSettings::DateFormat::iso8601;
    case 1: return TimestampSettings::DateFormat::localeShort;
    case 2: return TimestampSettings::DateFormat::localeLong;
    default: return TimestampSettings::DateFormat::custom;
    }
}
GtkWidget* zoneCombo(const std::vector<std::string>& zones, const std::string& sel) {
    GtkWidget* c = gtk_combo_box_text_new_with_entry();
    for (const auto& z : zones) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), z.c_str());
    GtkWidget* e = gtk_combo_box_get_child(GTK_COMBO_BOX(c));
    gtk_editable_set_text(GTK_EDITABLE(e), sel.c_str());
    gtk_widget_set_hexpand(c, TRUE);
    return c;
}
} // namespace

int runTimestampsDialog(ColumnsPlusPlusData& data,
                        const std::vector<std::string>& locales, const std::vector<std::string>& zones) {
    TimestampSettings& ts = data.timestamps;
    ModalDlg dlg("Columns++ \xE2\x80\x94 Timestamps");
    const std::vector<std::string> counters =
        { "Unix (seconds, 1970)", "Windows FILETIME (1601)", "Excel 1900", "Excel 1904", "Custom" };

    GtkWidget* fromCounterEnable = mkCheck("Source may be a counter:", ts.enableFromCounter);
    GtkWidget* fromCounterType = mkPopup(counters, counterIdx(ts.fromCounter.type));
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ fromCounterEnable, fromCounterType }));

    GtkWidget* fromDatetimeEnable = mkCheck("Source may be a date/time:", ts.enableFromDatetime);
    GtkWidget* datePriority = mkPopup(
        { "Year-Month-Day", "Month-Day-Year", "Day-Month-Year", "Custom pattern" },
        priorityIdx(ts.datePriority));
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ fromDatetimeEnable, datePriority }));
    GtkWidget* yearLimit = mkEntry(std::to_string(ts.twoDigitYearLimit), 6);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("2-digit year \xE2\x89\xA4"), yearLimit }));
    gtk_box_append(GTK_BOX(dlg.vbox),
        mkLabel("Custom parse pattern (regex, named groups y M d H m s t Z):"));
    GtkWidget* parsePattern = mkEntry(ts.dateParse.empty() ? "" : ts.dateParse.back(), 56);
    gtk_box_append(GTK_BOX(dlg.vbox), parsePattern);

    GtkWidget* tzEnable = mkCheck("Apply time zone & locale:", ts.enableTzAndLocale);
    gtk_box_append(GTK_BOX(dlg.vbox), tzEnable);
    GtkWidget* locale = mkCombo(locales);
    {
        GtkWidget* e = gtk_combo_box_get_child(GTK_COMBO_BOX(locale));
        gtk_editable_set_text(GTK_EDITABLE(e), ts.localeName.c_str());
    }
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("Locale:"), locale }));
    GtkWidget* fromZone = zoneCombo(zones, ts.fromZone);
    GtkWidget* toZone   = zoneCombo(zones, ts.toZone);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("From zone:"), fromZone,
                                             mkLabel("To zone:"), toZone }));

    GtkWidget* toCounterType = mkPopup(counters, counterIdx(ts.toCounter.type));
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("To counter type:"), toCounterType }));

    GtkWidget* dateFormat = mkPopup(
        { "ISO 8601", "Locale short", "Locale long", "Custom picture" }, formatIdx(ts.dateFormat));
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ mkLabel("To date/time format:"), dateFormat }));
    gtk_box_append(GTK_BOX(dlg.vbox),
        mkLabel("Custom picture (yyyy MM NN dd DD HH hh mm ss T t Z; 'literal'):"));
    GtkWidget* picture = mkEntry(
        ts.datePicture.empty() ? "yyyy-MM-dd'T'HH:mm:ss.sssZ" : ts.datePicture.back(), 56);
    gtk_box_append(GTK_BOX(dlg.vbox), picture);

    GtkWidget* overwrite = mkCheck(
        "Overwrite source (replace in place instead of adding a column)", ts.overwrite);
    gtk_box_append(GTK_BOX(dlg.vbox), overwrite);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* toDt = gtk_button_new_with_label("To Date/Time");
    GtkWidget* toCt = gtk_button_new_with_label("To Counter");
    GtkWidget* tf   = gtk_button_new_with_label("Time formats\xE2\x80\xA6");
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(toDt, "suggested-action");
    g_signal_connect(toDt, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        ((ModalDlg*)p)->finish(TS_TO_DATETIME);
    }), &dlg);
    g_signal_connect(toCt, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        ((ModalDlg*)p)->finish(TS_TO_COUNTER);
    }), &dlg);
    g_signal_connect(tf, "clicked", G_CALLBACK(+[](GtkButton*, gpointer u) {
        ((ColumnsPlusPlusData*)u)->showTimeFormatsDialog();
    }), &data);
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        ((ModalDlg*)p)->finish(TS_CANCEL);
    }), &dlg);
    gtk_box_append(GTK_BOX(row), toDt);
    gtk_box_append(GTK_BOX(row), toCt);
    gtk_box_append(GTK_BOX(row), tf);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(dlg.vbox), row);
    gtk_window_set_default_widget(GTK_WINDOW(dlg.win), toDt);

    int action = dlg.run();
    if (action == TS_CANCEL) return 0;

    ts.enableFromCounter  = checkOn(fromCounterEnable);
    ts.fromCounter.type   = counterTypeAt(popupIndex(fromCounterType));
    ts.enableFromDatetime = checkOn(fromDatetimeEnable);
    ts.datePriority       = priorityTypeAt(popupIndex(datePriority));
    ts.twoDigitYearLimit  = std::max(0, std::min(9999, entryInt(yearLimit)));
    ts.enableTzAndLocale  = checkOn(tzEnable);
    {
        std::string loc = comboText(locale);
        size_t dash = loc.find("  -  ");
        if (dash == std::string::npos) dash = loc.find("  \xE2\x80\x94  ");
        if (dash != std::string::npos) loc = loc.substr(0, dash);
        ts.localeName = loc;
    }
    ts.fromZone = comboText(fromZone);
    ts.toZone   = comboText(toZone);
    ts.toCounter.type = counterTypeAt(popupIndex(toCounterType));
    ts.dateFormat = formatTypeAt(popupIndex(dateFormat));
    if (ts.datePriority == TimestampSettings::DatePriority::custom) {
        std::string pp = entryText(parsePattern);
        if (!pp.empty()) pushHistory(ts.dateParse, pp);
    }
    if (ts.dateFormat == TimestampSettings::DateFormat::custom) {
        std::string pp = entryText(picture);
        if (!pp.empty()) pushHistory(ts.datePicture, pp);
    }
    ts.overwrite = checkOn(overwrite);
    return action;
}

void runTimeFormatsDialog(ColumnsPlusPlusData& data) {
    ModalDlg dlg("Columns++ \xE2\x80\x94 Time formats");

    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("A scalar (unformatted) time is measured in:"));
    GtkWidget* scalar = mkPopup({ "days", "hours", "minutes", "seconds" }, data.timeScalarUnit);
    gtk_box_append(GTK_BOX(dlg.vbox), scalar);
    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("A time with one colon is interpreted as:"));
    GtkWidget* partial = mkPopup(
        { "days:hours (\xE2\x80\xA6:minutes for two colons)", "hours:minutes (\xE2\x80\xA6:minutes for two)",
          "hours:minutes (\xE2\x80\xA6:seconds for two)", "minutes:seconds (hours:\xE2\x80\xA6 for two)" },
        data.timePartialRule);
    gtk_box_append(GTK_BOX(dlg.vbox), partial);
    gtk_box_append(GTK_BOX(dlg.vbox), mkLabel("Recognise these colon-separated forms:"));
    GtkWidget* e0 = mkCheck("1 segment",  data.timeFormatEnable & 1);
    GtkWidget* e1 = mkCheck("2 segments", data.timeFormatEnable & 2);
    GtkWidget* e2 = mkCheck("3 segments", data.timeFormatEnable & 4);
    GtkWidget* e3 = mkCheck("4 segments", data.timeFormatEnable & 8);
    gtk_box_append(GTK_BOX(dlg.vbox), hbox({ e0, e1, e2 }));
    gtk_box_append(GTK_BOX(dlg.vbox), e3);

    dlg.okCancel();
    if (dlg.run() != 1) return;
    data.timeScalarUnit  = popupIndex(scalar);
    data.timePartialRule = popupIndex(partial);
    data.timeFormatEnable = (checkOn(e0) ? 1 : 0) | (checkOn(e1) ? 2 : 0)
                          | (checkOn(e2) ? 4 : 0) | (checkOn(e3) ? 8 : 0);
}
