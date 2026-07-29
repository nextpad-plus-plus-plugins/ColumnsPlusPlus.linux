// Columns++ for Notepad++ — Linux (GTK4) port.
// RegularExpression.h — ICU RegexMatcher adapter matching the Windows
// RegularExpression interface (Search + Calculate rely on it). The macOS port
// backs this with NSRegularExpression — a wrapper over exactly this ICU
// engine — so matching semantics are identical across the ports:
//   * named groups, look-behind, \p{…}, code-point correct matching;
//   * document-range search reads the Scintilla range text via a supplied
//     reader (keeps this header self-contained and unit-testable);
//   * format() implements the common Boost format_all replacement vocabulary:
//       $$        literal '$'          $&  / $0     whole match
//       $1..$99   numbered group       ${name}      named group
//       $`        text before match    $'           text after match
//       \1..\9    numbered group       \U \L \E     upper / lower / end case span
//       \n \r \t \\                    (case span applies until \E or end)
//     (Not carried over from format_all: conditional replacements (?N:a:b) and \X
//      grapheme escapes — rare; documented. Same as macOS.)
//
// Byte offsets are UTF-8 (matching the plugin's Scintilla byte convention).

#pragma once

#include <unicode/regex.h>
#include <unicode/unistr.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class RegularExpression {

    std::unique_ptr<icu::RegexPattern> _pat;
    std::unique_ptr<icu::RegexMatcher> _m;       // matcher holding the last match
    icu::UnicodeString                 _subject; // subject of the last match
    intptr_t                           _base  = 0;  // doc-absolute byte offset of _subject[0]
    bool                               _found = false;
    bool                               _ok    = false;
    std::string                        _err;
    std::function<std::string(intptr_t,intptr_t)> _reader;

    // UTF-16 index within _subject → UTF-8 byte offset.
    intptr_t byteOffset(int32_t loc) const {
        if (loc < 0) return -1;
        if (loc > _subject.length()) loc = _subject.length();
        std::string pre;
        _subject.tempSubString(0, loc).toUTF8String(pre);
        return (intptr_t)pre.size();
    }
    // start/end UTF-16 range of group n, or {-1,-1}.
    void rangeAt(int n, int32_t& s, int32_t& e) const {
        s = e = -1;
        if (!_found || !_m || n < 0 || (size_t)n >= size()) return;
        UErrorCode status = U_ZERO_ERROR;
        int32_t rs = _m->start(n, status);
        int32_t re = _m->end(n, status);
        if (U_FAILURE(status)) return;
        s = rs; e = re;
    }
    std::string utf8Of(const icu::UnicodeString& us) const {
        std::string out;
        us.toUTF8String(out);
        return out;
    }

public:

    bool can_search() const { return _ok; }
    size_t size()       const { return _found && _m ? (size_t)_m->groupCount() + 1 : 0; }
    size_t mark_count() const {
        if (!_pat) return 0;
        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<icu::RegexMatcher> tmp(_pat->matcher(status));
        return (U_SUCCESS(status) && tmp) ? (size_t)tmp->groupCount() : 0;
    }
    void invalidate() { _m.reset(); _found = false; }

    // find — compile. Returns "" on success or an error message (Windows parity).
    std::string find(const std::string& pattern, bool caseSensitive) {
        _pat.reset(); _m.reset(); _found = false;
        UErrorCode status = U_ZERO_ERROR;
        UParseError pe = {};
        icu::UnicodeString up = icu::UnicodeString::fromUTF8(pattern);
        uint32_t flags = caseSensitive ? 0 : UREGEX_CASE_INSENSITIVE;
        _pat.reset(icu::RegexPattern::compile(up, flags, pe, status));
        _ok = U_SUCCESS(status) && _pat != nullptr;
        if (_ok) _err.clear();
        else {
            _pat.reset();
            _err = std::string("Invalid regular expression: ") + u_errorName(status);
        }
        return _err;
    }

    // search a string starting at byte offset `from`.
    bool search(std::string_view s, size_t from = 0) {
        _m.reset(); _found = false; _base = 0;
        if (!_ok || !_pat) return false;
        _subject = icu::UnicodeString::fromUTF8(
            icu::StringPiece(s.data(), (int32_t)s.size()));
        // byte offset `from` → UTF-16 index (snap back to a UTF-8 lead byte first,
        // matching the macOS adapter).
        size_t fb = from;
        while (fb > 0 && fb < s.size() && ((unsigned char)s[fb] & 0xC0) == 0x80) --fb;
        int32_t u16 = 0;
        if (fb > 0) {
            icu::UnicodeString pre = icu::UnicodeString::fromUTF8(
                icu::StringPiece(s.data(), (int32_t)fb));
            u16 = pre.length();
        }
        if (u16 > _subject.length()) u16 = _subject.length();
        UErrorCode status = U_ZERO_ERROR;
        _m.reset(_pat->matcher(_subject, status));
        if (U_FAILURE(status) || !_m) { _m.reset(); return false; }
        status = U_ZERO_ERROR;
        _found = _m->find(u16, status) && U_SUCCESS(status);
        return _found;
    }

    // search a Scintilla document range [from,to), beginning at byte `start`.
    // `reader(a,b)` returns the UTF-8 text of document bytes [a,b).
    bool search(const std::function<std::string(intptr_t,intptr_t)>& reader,
                intptr_t from, intptr_t to, intptr_t start) {
        std::string text = reader(from, to);
        size_t off = (start > from) ? (size_t)(start - from) : 0;
        if (off > text.size()) off = text.size();
        if (!search(text, off)) return false;
        _base = from;   // positions become document-absolute
        return true;
    }

    // Set a persistent document reader so search(from,to,start) can be used — matches
    // the Windows RegularExpression(sci) ctor, which reads the document directly.
    void setReader(std::function<std::string(intptr_t,intptr_t)> r) { _reader = std::move(r); }

    bool search(intptr_t from, intptr_t to, intptr_t start) {
        return _reader ? search(_reader, from, to, start) : false;
    }

    // byte position of group n (document-absolute after a range search), or -1.
    intptr_t position(int n = 0) const {
        int32_t s, e;
        rangeAt(n, s, e);
        if (s < 0) return -1;
        return _base + byteOffset(s);
    }
    // byte length of group n.
    intptr_t length(int n = 0) const {
        int32_t s, e;
        rangeAt(n, s, e);
        if (s < 0) return 0;
        return byteOffset(e) - byteOffset(s);
    }
    std::string str(int n = 0) const {
        int32_t s, e;
        rangeAt(n, s, e);
        if (s < 0) return "";
        return utf8Of(_subject.tempSubString(s, e - s));
    }
    std::string str(std::string_view name) const {
        if (!_found || !_m || !_pat) return "";
        UErrorCode status = U_ZERO_ERROR;
        icu::UnicodeString un = icu::UnicodeString::fromUTF8(
            icu::StringPiece(name.data(), (int32_t)name.size()));
        int32_t n = _pat->groupNumberFromName(un, status);
        if (U_FAILURE(status) || n < 0) return "";
        return str((int)n);
    }

    // format — expand a replacement template against the last match (format_all subset).
    std::string format(const std::string& replacement) const {
        if (!_found || !_m) return replacement;
        std::string out;
        // 0 = none, 1 = upper (\U), 2 = lower (\L); applies until \E.
        int caseMode = 0;
        auto append = [&](const std::string& piece) {
            if (caseMode == 0) { out += piece; return; }
            icu::UnicodeString us = icu::UnicodeString::fromUTF8(piece);
            if (caseMode == 1) us.toUpper(); else us.toLower();
            std::string conv;
            us.toUTF8String(conv);
            out += conv;
        };
        auto groupText = [&](int n) -> std::string { return str(n); };
        int32_t wholeS, wholeE;
        rangeAt(0, wholeS, wholeE);
        const std::string& r = replacement;
        for (size_t i = 0; i < r.size(); ) {
            char c = r[i];
            if (c == '$' && i + 1 < r.size()) {
                char d = r[i + 1];
                if (d == '$') { append("$"); i += 2; continue; }
                if (d == '&' || d == '0') { append(groupText(0)); i += 2; continue; }
                if (d == '`') { // text before match
                    append(utf8Of(_subject.tempSubString(0, wholeS))); i += 2; continue;
                }
                if (d == '\'') { // text after match
                    append(utf8Of(_subject.tempSubString(wholeE))); i += 2; continue;
                }
                if (d == '{') { // ${name}
                    size_t e = r.find('}', i + 2);
                    if (e != std::string::npos) {
                        std::string name = r.substr(i + 2, e - (i + 2));
                        append(str(std::string_view(name))); i = e + 1; continue;
                    }
                }
                if (d >= '0' && d <= '9') { // $N or $NN
                    int n = d - '0'; size_t j = i + 2;
                    if (j < r.size() && r[j] >= '0' && r[j] <= '9') { int n2 = n * 10 + (r[j] - '0');
                        if ((size_t)n2 < size()) { n = n2; ++j; } }
                    append(groupText(n)); i = j; continue;
                }
                append("$"); ++i; continue;
            }
            if (c == '\\' && i + 1 < r.size()) {
                char d = r[i + 1];
                if (d == 'U') { caseMode = 1; i += 2; continue; }
                if (d == 'L') { caseMode = 2; i += 2; continue; }
                if (d == 'E') { caseMode = 0; i += 2; continue; }
                if (d == 'n') { append("\n"); i += 2; continue; }
                if (d == 'r') { append("\r"); i += 2; continue; }
                if (d == 't') { append("\t"); i += 2; continue; }
                if (d == '\\') { append("\\"); i += 2; continue; }
                if (d >= '0' && d <= '9') { append(groupText(d - '0')); i += 2; continue; }
                append(std::string(1, d)); i += 2; continue;
            }
            append(std::string(1, c)); ++i;
        }
        return out;
    }
};
