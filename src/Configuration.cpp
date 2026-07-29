// Columns++ for Notepad++ — macOS port.
// Configuration.mm — persist settings + elastic-tabstops profiles + extension map.
//
// Keeps the SAME on-disk format and filename as the Windows build so a config dir
// shared between platforms stays readable: a UTF-8, BOM-prefixed, line-oriented,
// TAB-delimited "sections + key/value" text file named ColumnsPlusPlus.data in
// NPPM_GETPLUGINSCONFIGDIR. (The Windows source parses with std::regex and writes
// with std::ofstream — it is NOT JSON despite the vendored nlohmann/ folder, which
// is unused.) Booleans are bare 0/1; enums are lowercase keywords; history lists
// are backslash-delimited with \r \n \\ escaped, capped at the last 16 entries.
//
// The sections/keys consumed by the feature set are read/written here (LastSettings,
// Sort, Align, Calculate, Search, Timestamps, Profile <name>, Extensions). The Update
// section (Windows auto-updater — not applicable on macOS) is read but ignored.

#include "CppData.h"

#include <glib.h>

#include <fstream>
#include <regex>
#include <sstream>

namespace {

std::string configFilePath(ColumnsPlusPlusData& data) {
    char buf[4096] = {0};
    data.npp(NPPM_GETPLUGINSCONFIGDIR, (uintptr_t)sizeof(buf), (intptr_t)buf);
    std::string dir = buf[0] ? std::string(buf)
        : std::string(g_get_user_data_dir()) + "/nextpad++/plugins/Config";
    g_mkdir_with_parents(dir.c_str(), 0755);
    return dir + "/ColumnsPlusPlus.data";
}

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Encode a string for a history line: wrap in backslashes, escape \r \n \\.
std::string encodeDelimited(const std::string& s) {
    std::string r = "\\";
    for (char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '\r') r += "\\r";
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    r += "\\";
    return r;
}
std::string decodeDelimited(const std::string& s) {
    if (s.size() < 2 || s.front() != '\\' || s.back() != '\\') return "";
    std::string r;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) {
            char n = s[i + 1];
            if (n == '\\') { r += '\\'; ++i; }
            else if (n == 'r') { r += '\r'; ++i; }
            else if (n == 'n') { r += '\n'; ++i; }
            else r += s[i];
        }
        else r += s[i];
    }
    return r;
}

} // namespace

void ColumnsPlusPlusData::initializeBuiltinElasticTabstopsProfiles() {
    auto make = [](int tab, int space, bool leading, bool lineUp, bool eol, bool over) {
        ElasticTabsProfile p;
        p.minimumOrLeadingTabSize = tab;
        p.minimumSpaceBetweenColumns = space;
        p.monospace = ElasticTabsProfile::MonospaceBest;
        p.leadingTabsIndent = leading;
        p.lineUpAll = lineUp;
        p.treatEolAsTab = eol;
        p.overrideTabSize = over;
        p.monospaceNoMnemonics = true;
        return p;
    };
    profiles["Classic"] = make(4, 2, false, false, false, false);
    profiles["General"] = make(4, 2, true , false, false, false);
    profiles["Tabular"] = make(1, 2, false, true , true , true );
}

void ColumnsPlusPlusData::loadConfiguration() {
    std::ifstream in(configFilePath(*this), std::ios::binary);
    if (!in) return;
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    // Strip UTF-8 BOM.
    if (contents.size() >= 3 && (unsigned char)contents[0] == 0xEF && (unsigned char)contents[1] == 0xBB && (unsigned char)contents[2] == 0xBF)
        contents = contents.substr(3);

    std::istringstream ss(contents);
    std::string line;
    if (!std::getline(ss, line)) return;
    // Header: "Notepad++ Columns++ Configuration <level> <compat>".
    {
        std::istringstream hs(line);
        std::string a, b, c, d;
        hs >> a >> b >> c >> d;   // "Notepad++" "Columns++" "Configuration" then level...
        // We don't strictly validate; if compat>5 the Windows build refuses — we are lenient.
    }

    std::string section;
    std::string profileName;
    static const std::regex kv(R"(\s*(\S+)\s+(.*\S)\s*)");

    auto applyHistory = [](std::vector<std::string>& h, const std::string& val) {
        std::string d = decodeDelimited(val);
        h.push_back(d);
    };

    while (std::getline(ss, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        // Section header lines have no leading key/value tab structure; detect them.
        std::string lt = lower(t);
        if (lt == "lastsettings" || lt == "calculate" || lt == "search" || lt == "sort"
            || lt == "align" || lt == "timestamps" || lt == "update" || lt == "extensions") {
            section = lt;
            continue;
        }
        if (lt.rfind("profile ", 0) == 0) {
            section = "profile";
            profileName = trim(t.substr(8));
            continue;
        }
        std::smatch m;
        if (!std::regex_match(line, m, kv)) continue;
        std::string key = lower(m[1].str());
        std::string val = m[2].str();
        bool bval = val != "0";
        int  ival = atoi(val.c_str());

        if (section == "lastsettings") {
            if      (key == "elasticenabled") settings.elasticEnabled = bval;
            else if (key == "decimalseparatoriscomma") settings.decimalSeparatorIsComma = bval;
            else if (key == "name") settings.profileName = val;
            else if (key == "minimumorleadingtabsize") settings.minimumOrLeadingTabSize = ival;
            else if (key == "minimumspacebetweencolumns") settings.minimumSpaceBetweenColumns = ival;
            else if (key == "leadingtabsindent") settings.leadingTabsIndent = bval;
            else if (key == "lineupall") settings.lineUpAll = bval;
            else if (key == "treateolastab") settings.treatEolAsTab = bval;
            else if (key == "overridetabsize") settings.overrideTabSize = bval;
            else if (key == "monospace") settings.monospace = lower(val) == "yes" ? ElasticTabsProfile::MonospaceAlways
                                                            : lower(val) == "no"  ? ElasticTabsProfile::MonospaceNever
                                                            : ElasticTabsProfile::MonospaceBest;
            else if (key == "monospacenomnemonics") settings.monospaceNoMnemonics = bval;
            else if (key == "disableoversize") disableOverSize = ival;
            else if (key == "disableoverlines") disableOverLines = ival;
            else if (key == "timescalarunit") timeScalarUnit = ival;
            else if (key == "timepartialrule") timePartialRule = ival;
            else if (key == "timeformatenable") timeFormatEnable = ival;
            else if (key == "showonmenubar") showOnMenuBar = bval;
            else if (key == "replacestaysput") replaceStaysPut = bval;
            else if (key == "extendsingleline") extendSingleLine = bval;
            else if (key == "extendfulllines") extendFullLines = bval;
            else if (key == "extendzerowidth") extendZeroWidth = bval;
            else if (key == "csvquote") csv.quote = bval;
            else if (key == "csvapostrophe") csv.apostrophe = bval;
            else if (key == "csvescape") csv.escape = bval;
            else if (key == "csvpreservequotes") csv.preserveQuotes = bval;
            else if (key == "csvkeepseparator") csv.keepSeparator = bval;
            else if (key == "csvseparator") { std::string d = decodeDelimited(val); if (!d.empty()) csv.separator = d[0]; }
            else if (key == "csvescapechar") { std::string d = decodeDelimited(val); if (!d.empty()) csv.escapeChar = d[0]; }
            else if (key == "csvencodetnr") { std::string d = decodeDelimited(val); if (!d.empty()) csv.encodeTNR = d[0]; }
            else if (key == "csvencodeurl") { std::string d = decodeDelimited(val); if (!d.empty()) csv.encodeURL = d[0]; }
            else if (key == "csvreplacetab") csv.replaceTab = decodeDelimited(val);
            else if (key == "csvreplacelf") csv.replaceLF = decodeDelimited(val);
            else if (key == "csvreplacecr") csv.replaceCR = decodeDelimited(val);
            else if (key == "csvencodingstyle") csv.encodingStyle = lower(val) == "backslash" ? CsvSettings::TNR
                                                                  : lower(val) == "url" ? CsvSettings::URL : CsvSettings::Replace;
        }
        else if (section == "calculate") {
            if      (key == "thousands") calc.thousands = lower(val) == "apostrophe" ? CalculateSettings::Apostrophe
                                                        : lower(val) == "blank" ? CalculateSettings::Blank
                                                        : (lower(val) == "comma" || lower(val) == "period") ? CalculateSettings::Comma
                                                        : CalculateSettings::None;
            else if (key == "insert") calc.insert = bval;
            else if (key == "addline") calc.addLine = bval;
            else if (key == "decimalplaces") calc.decimalPlaces = ival;
            else if (key == "decimalsfixed") calc.decimalsFixed = bval;
            else if (key == "timesegments") calc.timeSegments = ival;
            else if (key == "autodecimals") calc.autoDecimals = bval;
            else if (key == "matchcase") calc.matchCase = bval;
            else if (key == "skipunmatched") calc.skipUnmatched = bval;
            else if (key == "formatastime") calc.formatAsTime = bval;
            else if (key == "tabbed") calc.tabbed = bval;
            else if (key == "aligned") calc.aligned = bval;
            else if (key == "left") calc.left = bval;
            else if (key == "formula") applyHistory(calc.formulaHistory, val);
            else if (key == "regex") applyHistory(calc.regexHistory, val);
        }
        else if (section == "search") {
            if      (key == "mode") searchData.mode = lower(val) == "regex" ? SearchData::Regex
                                                    : lower(val) == "extended" ? SearchData::Extended : SearchData::Normal;
            else if (key == "backward")              searchData.backward = bval;
            else if (key == "wholeword")             searchData.wholeWord = bval;
            else if (key == "matchcase")             searchData.matchCase = bval;
            else if (key == "autoclear")             searchData.autoClear = bval;
            else if (key == "autoclearselection")    searchData.autoClearSelection = bval;
            else if (key == "autosetselection")      searchData.autoSetSelection = bval;
            else if (key == "enablecustomindicator") searchData.enableCustomIndicator = bval;
            else if (key == "forceuserindicator")    searchData.forceUserIndicator = bval;
            else if (key == "indicator")             searchData.indicator = ival;
            else if (key == "customalpha")           searchData.customAlpha = ival;
            else if (key == "customcolor")           searchData.customColor = ival;
            else if (key == "customindicator")       searchData.customIndicator = ival;
            else if (key == "userindicator")         searchData.userIndicator = ival;
            else if (key == "replacestaysput")       replaceStaysPut = bval;
            else if (key == "find")    applyHistory(searchData.findHistory, val);
            else if (key == "replace") applyHistory(searchData.replaceHistory, val);
        }
        else if (section == "timestamps") {
            auto ctype = [](const std::string& x) { std::string l = lower(x);
                return l == "file" ? TimestampSettings::CounterType::File : l == "ex00" ? TimestampSettings::CounterType::Ex00
                     : l == "ex04" ? TimestampSettings::CounterType::Ex04 : l == "custom" ? TimestampSettings::CounterType::custom
                                                                                          : TimestampSettings::CounterType::Unix; };
            if      (key == "fromcountertype")  timestamps.fromCounter.type = ctype(val);
            else if (key == "tocountertype")    timestamps.toCounter.type = ctype(val);
            else if (key == "fromcustomepoch")  timestamps.fromCounter.custom.epoch = atoll(val.c_str());
            else if (key == "fromcustomunit")   timestamps.fromCounter.custom.unit = atoll(val.c_str());
            else if (key == "fromcustomleap")   timestamps.fromCounter.custom.leap = bval;
            else if (key == "tocustomepoch")    timestamps.toCounter.custom.epoch = atoll(val.c_str());
            else if (key == "tocustomunit")     timestamps.toCounter.custom.unit = atoll(val.c_str());
            else if (key == "tocustomleap")     timestamps.toCounter.custom.leap = bval;
            else if (key == "localename")       timestamps.localeName = val;
            else if (key == "fromzone")         timestamps.fromZone = val;
            else if (key == "tozone")           timestamps.toZone = val;
            else if (key == "twodigityearlimit") timestamps.twoDigitYearLimit = ival;
            else if (key == "datepriority")     timestamps.datePriority = lower(val) == "mdy" ? TimestampSettings::DatePriority::mdy
                                                                        : lower(val) == "dmy" ? TimestampSettings::DatePriority::dmy
                                                                        : lower(val) == "custom" ? TimestampSettings::DatePriority::custom
                                                                                                 : TimestampSettings::DatePriority::ymd;
            else if (key == "dateformat")       timestamps.dateFormat = lower(val) == "localelong" ? TimestampSettings::DateFormat::localeLong
                                                                      : lower(val) == "localeshort" ? TimestampSettings::DateFormat::localeShort
                                                                      : lower(val) == "custom" ? TimestampSettings::DateFormat::custom
                                                                                               : TimestampSettings::DateFormat::iso8601;
            else if (key == "enablefromcounter")  timestamps.enableFromCounter = bval;
            else if (key == "enablefromdatetime") timestamps.enableFromDatetime = bval;
            else if (key == "enabletzandlocale")  timestamps.enableTzAndLocale = bval;
            else if (key == "overwrite")          timestamps.overwrite = bval;
            else if (key == "dateparse")          applyHistory(timestamps.dateParse, val);
            else if (key == "datepicture")        applyHistory(timestamps.datePicture, val);
            else if (key == "timescalarunit")     timeScalarUnit = ival;
            else if (key == "timepartialrule")    timePartialRule = ival;
            else if (key == "timeformatenable")   timeFormatEnable = ival;
        }
        else if (section == "sort") {
            if      (key == "columnselectiononly") sort.sortColumnSelectionOnly = bval;
            else if (key == "descending") sort.sortDescending = bval;
            else if (key == "regexmatchcase") sort.regexMatchCase = bval;
            else if (key == "regexusekey") sort.regexUseKey = bval;
            else if (key == "localename") sort.localeName = val;
            else if (key == "localecasesensitive") sort.localeCaseSensitive = bval;
            else if (key == "localedigitsasnumbers") sort.localeDigitsAsNumbers = bval;
            else if (key == "localeignorediacritics") sort.localeIgnoreDiacritics = bval;
            else if (key == "localeignoresymbols") sort.localeIgnoreSymbols = bval;
            else if (key == "sorttype") sort.sortType = lower(val) == "locale" ? SortSettings::Locale
                                                      : lower(val) == "numeric" ? SortSettings::Numeric
                                                      : lower(val) == "width" ? SortSettings::Width : SortSettings::Binary;
            else if (key == "keytype") sort.keyType = lower(val) == "ignoreblanks" ? SortSettings::IgnoreBlanks
                                                    : lower(val) == "tabbed" ? SortSettings::Tabbed
                                                    : lower(val) == "regex" ? SortSettings::Regex : SortSettings::EntireColumn;
            else if (key == "regex") applyHistory(sort.regexHistory, val);
            else if (key == "keys") applyHistory(sort.keygroupHistory, val);
        }
        else if (section == "align") {
            if      (key == "alignon") align.alignOn = lower(val) == "last" ? AlignSettings::Last
                                                     : lower(val) == "regex" ? AlignSettings::Regex : AlignSettings::First;
            else if (key == "matchcase") align.matchCase = bval;
            else if (key == "margin") align.margin = ival;
            else if (key == "marginright") align.marginRight = bval;
            else if (key == "history") applyHistory(align.history, val);
        }
        else if (section == "profile" && !profileName.empty()) {
            ElasticTabsProfile& p = profiles[profileName];
            if      (key == "minimumorleadingtabsize") p.minimumOrLeadingTabSize = ival;
            else if (key == "minimumspacebetweencolumns") p.minimumSpaceBetweenColumns = ival;
            else if (key == "leadingtabsindent") p.leadingTabsIndent = bval;
            else if (key == "lineupall") p.lineUpAll = bval;
            else if (key == "treateolastab") p.treatEolAsTab = bval;
            else if (key == "overridetabsize") p.overrideTabSize = bval;
            else if (key == "monospace") p.monospace = lower(val) == "yes" ? ElasticTabsProfile::MonospaceAlways
                                                     : lower(val) == "no"  ? ElasticTabsProfile::MonospaceNever : ElasticTabsProfile::MonospaceBest;
            else if (key == "monospacenomnemonics") p.monospaceNoMnemonics = bval;
        }
        else if (section == "extensions") {
            std::string mapKey;
            if      (key == "new") mapKey = "";
            else if (key == "none") mapKey = ".";
            else if (key == "default") mapKey = "*";
            else { mapKey = key; for (auto& c : mapKey) if (c == '.') c = ' '; }
            extensionToProfile[mapKey] = (lower(val) == "(disable)") ? "" : val;
        }
    }
}

void ColumnsPlusPlusData::saveConfiguration() {
    std::ofstream out(configFilePath(*this), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "\xEF\xBB\xBF";   // UTF-8 BOM
    out << "Notepad++ Columns++ Configuration 5 1\n";

    auto b = [](bool v) { return v ? "1" : "0"; };
    auto writeHistory = [&](const char* key, const std::vector<std::string>& h) {
        size_t start = h.size() > 16 ? h.size() - 16 : 0;
        for (size_t i = start; i < h.size(); ++i) out << key << '\t' << encodeDelimited(h[i]) << '\n';
    };

    const char* monoStr[] = {"no", "yes", "best"};
    auto mono = [&](int m) { return m == ElasticTabsProfile::MonospaceAlways ? "yes" : m == ElasticTabsProfile::MonospaceNever ? "no" : "best"; };

    out << "\nLastSettings\n\n";
    out << "elasticEnabled\t" << b(settings.elasticEnabled) << '\n';
    out << "decimalSeparatorIsComma\t" << b(settings.decimalSeparatorIsComma) << '\n';
    if (!settings.profileName.empty()) out << "name\t" << settings.profileName << '\n';
    out << "minimumOrLeadingTabSize\t" << settings.minimumOrLeadingTabSize << '\n';
    out << "minimumSpaceBetweenColumns\t" << settings.minimumSpaceBetweenColumns << '\n';
    out << "leadingTabsIndent\t" << b(settings.leadingTabsIndent) << '\n';
    out << "lineUpAll\t" << b(settings.lineUpAll) << '\n';
    out << "treatEolAsTab\t" << b(settings.treatEolAsTab) << '\n';
    out << "overrideTabSize\t" << b(settings.overrideTabSize) << '\n';
    out << "monospace\t" << mono(settings.monospace) << '\n';
    out << "monospaceNoMnemonics\t" << b(settings.monospaceNoMnemonics) << '\n';
    out << "disableOverSize\t" << disableOverSize << '\n';
    out << "disableOverLines\t" << disableOverLines << '\n';
    out << "timeScalarUnit\t" << timeScalarUnit << '\n';
    out << "timePartialRule\t" << timePartialRule << '\n';
    out << "timeFormatEnable\t" << timeFormatEnable << '\n';
    out << "showOnMenuBar\t" << b(showOnMenuBar) << '\n';
    out << "replaceStaysPut\t" << b(replaceStaysPut) << '\n';
    out << "extendSingleLine\t" << b(extendSingleLine) << '\n';
    out << "extendFullLines\t" << b(extendFullLines) << '\n';
    out << "extendZeroWidth\t" << b(extendZeroWidth) << '\n';
    out << "csvQuote\t" << b(csv.quote) << '\n';
    out << "csvApostrophe\t" << b(csv.apostrophe) << '\n';
    out << "csvEscape\t" << b(csv.escape) << '\n';
    out << "csvPreserveQuotes\t" << b(csv.preserveQuotes) << '\n';
    out << "csvKeepSeparator\t" << b(csv.keepSeparator) << '\n';
    out << "csvSeparator\t" << encodeDelimited(std::string(1, csv.separator)) << '\n';
    out << "csvEscapeChar\t" << encodeDelimited(std::string(1, csv.escapeChar)) << '\n';
    out << "csvEncodeTNR\t" << encodeDelimited(std::string(1, csv.encodeTNR)) << '\n';
    out << "csvEncodeURL\t" << encodeDelimited(std::string(1, csv.encodeURL)) << '\n';
    out << "csvReplaceTab\t" << encodeDelimited(csv.replaceTab) << '\n';
    out << "csvReplaceLF\t" << encodeDelimited(csv.replaceLF) << '\n';
    out << "csvReplaceCR\t" << encodeDelimited(csv.replaceCR) << '\n';
    out << "csvEncodingStyle\t" << (csv.encodingStyle == CsvSettings::TNR ? "backslash" : csv.encodingStyle == CsvSettings::URL ? "url" : "replace") << '\n';

    out << "\nCalculate\n\n";
    out << "thousands\t" << (calc.thousands == CalculateSettings::Apostrophe ? "apostrophe"
                           : calc.thousands == CalculateSettings::Blank ? "blank"
                           : calc.thousands == CalculateSettings::Comma ? (settings.decimalSeparatorIsComma ? "period" : "comma") : "none") << '\n';
    out << "insert\t" << b(calc.insert) << '\n';
    out << "addLine\t" << b(calc.addLine) << '\n';
    out << "decimalPlaces\t" << calc.decimalPlaces << '\n';
    out << "decimalsFixed\t" << b(calc.decimalsFixed) << '\n';
    out << "timeSegments\t" << calc.timeSegments << '\n';
    out << "autoDecimals\t" << b(calc.autoDecimals) << '\n';
    out << "matchCase\t" << b(calc.matchCase) << '\n';
    out << "skipUnmatched\t" << b(calc.skipUnmatched) << '\n';
    out << "formatAsTime\t" << b(calc.formatAsTime) << '\n';
    out << "tabbed\t" << b(calc.tabbed) << '\n';
    out << "aligned\t" << b(calc.aligned) << '\n';
    out << "left\t" << b(calc.left) << '\n';
    writeHistory("formula", calc.formulaHistory);
    writeHistory("regex", calc.regexHistory);

    out << "\nSearch\n\n";
    out << "mode\t" << (searchData.mode == SearchData::Regex ? "regex" : searchData.mode == SearchData::Extended ? "extended" : "normal") << '\n';
    out << "backward\t" << b(searchData.backward) << '\n';
    out << "wholeWord\t" << b(searchData.wholeWord) << '\n';
    out << "matchCase\t" << b(searchData.matchCase) << '\n';
    out << "autoClear\t" << b(searchData.autoClear) << '\n';
    out << "autoClearSelection\t" << b(searchData.autoClearSelection) << '\n';
    out << "autoSetSelection\t" << b(searchData.autoSetSelection) << '\n';
    out << "enableCustomIndicator\t" << b(searchData.enableCustomIndicator) << '\n';
    out << "forceUserIndicator\t" << b(searchData.forceUserIndicator) << '\n';
    out << "indicator\t" << searchData.indicator << '\n';
    out << "customAlpha\t" << searchData.customAlpha << '\n';
    out << "customColor\t" << searchData.customColor << '\n';
    out << "customIndicator\t" << searchData.customIndicator << '\n';
    out << "userIndicator\t" << searchData.userIndicator << '\n';
    out << "replaceStaysPut\t" << b(replaceStaysPut) << '\n';
    writeHistory("find", searchData.findHistory);
    writeHistory("replace", searchData.replaceHistory);

    out << "\nTimestamps\n\n";
    auto ctypeStr = [](TimestampSettings::CounterType t) {
        return t == TimestampSettings::CounterType::File ? "file" : t == TimestampSettings::CounterType::Ex00 ? "ex00"
             : t == TimestampSettings::CounterType::Ex04 ? "ex04" : t == TimestampSettings::CounterType::custom ? "custom" : "unix"; };
    out << "fromCounterType\t" << ctypeStr(timestamps.fromCounter.type) << '\n';
    out << "toCounterType\t"   << ctypeStr(timestamps.toCounter.type) << '\n';
    out << "fromCustomEpoch\t" << timestamps.fromCounter.custom.epoch << '\n';
    out << "fromCustomUnit\t"  << timestamps.fromCounter.custom.unit  << '\n';
    out << "fromCustomLeap\t"  << b(timestamps.fromCounter.custom.leap) << '\n';
    out << "toCustomEpoch\t"   << timestamps.toCounter.custom.epoch << '\n';
    out << "toCustomUnit\t"    << timestamps.toCounter.custom.unit  << '\n';
    out << "toCustomLeap\t"    << b(timestamps.toCounter.custom.leap) << '\n';
    if (!timestamps.localeName.empty()) out << "localeName\t" << timestamps.localeName << '\n';
    if (!timestamps.fromZone.empty())   out << "fromZone\t"   << timestamps.fromZone << '\n';
    if (!timestamps.toZone.empty())     out << "toZone\t"     << timestamps.toZone << '\n';
    out << "twoDigitYearLimit\t" << timestamps.twoDigitYearLimit << '\n';
    out << "datePriority\t" << (timestamps.datePriority == TimestampSettings::DatePriority::mdy ? "mdy"
                              : timestamps.datePriority == TimestampSettings::DatePriority::dmy ? "dmy"
                              : timestamps.datePriority == TimestampSettings::DatePriority::custom ? "custom" : "ymd") << '\n';
    out << "dateFormat\t" << (timestamps.dateFormat == TimestampSettings::DateFormat::localeLong ? "localelong"
                            : timestamps.dateFormat == TimestampSettings::DateFormat::localeShort ? "localeshort"
                            : timestamps.dateFormat == TimestampSettings::DateFormat::custom ? "custom" : "iso8601") << '\n';
    out << "enableFromCounter\t"  << b(timestamps.enableFromCounter)  << '\n';
    out << "enableFromDatetime\t" << b(timestamps.enableFromDatetime) << '\n';
    out << "enableTzAndLocale\t"  << b(timestamps.enableTzAndLocale)  << '\n';
    out << "overwrite\t"          << b(timestamps.overwrite) << '\n';
    out << "timeScalarUnit\t"     << timeScalarUnit  << '\n';
    out << "timePartialRule\t"    << timePartialRule << '\n';
    out << "timeFormatEnable\t"   << timeFormatEnable << '\n';
    writeHistory("dateParse",   timestamps.dateParse);
    writeHistory("datePicture", timestamps.datePicture);

    out << "\nSort\n\n";
    out << "columnSelectionOnly\t" << b(sort.sortColumnSelectionOnly) << '\n';
    out << "descending\t" << b(sort.sortDescending) << '\n';
    out << "regexMatchCase\t" << b(sort.regexMatchCase) << '\n';
    out << "regexUseKey\t" << b(sort.regexUseKey) << '\n';
    if (!sort.localeName.empty()) out << "localeName\t" << sort.localeName << '\n';
    out << "localeCaseSensitive\t" << b(sort.localeCaseSensitive) << '\n';
    out << "localeDigitsAsNumbers\t" << b(sort.localeDigitsAsNumbers) << '\n';
    out << "localeIgnoreDiacritics\t" << b(sort.localeIgnoreDiacritics) << '\n';
    out << "localeIgnoreSymbols\t" << b(sort.localeIgnoreSymbols) << '\n';
    out << "sortType\t" << (sort.sortType == SortSettings::Locale ? "locale" : sort.sortType == SortSettings::Numeric ? "numeric"
                          : sort.sortType == SortSettings::Width ? "width" : "binary") << '\n';
    out << "keyType\t" << (sort.keyType == SortSettings::IgnoreBlanks ? "ignoreblanks" : sort.keyType == SortSettings::Tabbed ? "tabbed"
                         : sort.keyType == SortSettings::Regex ? "regex" : "entirecolumn") << '\n';
    writeHistory("regex", sort.regexHistory);
    writeHistory("keys", sort.keygroupHistory);

    out << "\nAlign\n\n";
    out << "alignOn\t" << (align.alignOn == AlignSettings::Last ? "last" : align.alignOn == AlignSettings::Regex ? "regex" : "first") << '\n';
    out << "matchCase\t" << b(align.matchCase) << '\n';
    out << "margin\t" << align.margin << '\n';
    out << "marginRight\t" << b(align.marginRight) << '\n';
    writeHistory("history", align.history);

    // User profiles (built-ins Classic/General/Tabular are recreated in code).
    for (auto& kv : profiles) {
        const std::string& name = kv.first;
        if (name == "Classic" || name == "General" || name == "Tabular") continue;
        const ElasticTabsProfile& p = kv.second;
        out << "\nProfile " << name << "\n\n";
        out << "minimumOrLeadingTabSize\t" << p.minimumOrLeadingTabSize << '\n';
        out << "minimumSpaceBetweenColumns\t" << p.minimumSpaceBetweenColumns << '\n';
        out << "leadingTabsIndent\t" << b(p.leadingTabsIndent) << '\n';
        out << "lineUpAll\t" << b(p.lineUpAll) << '\n';
        out << "treatEolAsTab\t" << b(p.treatEolAsTab) << '\n';
        out << "overrideTabSize\t" << b(p.overrideTabSize) << '\n';
        out << "monospace\t" << mono(p.monospace) << '\n';
        out << "monospaceNoMnemonics\t" << b(p.monospaceNoMnemonics) << '\n';
    }
    (void)monoStr;

    out << "\nExtensions\n\n";
    for (auto& kv : extensionToProfile) {
        std::string key = kv.first;
        std::string outKey;
        if      (key == "")  outKey = "new";
        else if (key == ".") outKey = "none";
        else if (key == "*") outKey = "default";
        else { outKey = key; for (auto& c : outKey) if (c == ' ') c = '.'; outKey = std::string(".") + outKey; }
        std::string val = kv.second.empty() ? "(disable)" : kv.second;
        out << outKey << '\t' << val << '\n';
    }

    out.close();
}
