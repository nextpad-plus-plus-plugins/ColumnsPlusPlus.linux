// Columns++ for Notepad++ — Linux (GTK4) port.
// Regex.h — a thin ICU RegexMatcher wrapper standing in for the Windows
// plugin's Boost.Regex + Unicode Character Database engine.
//
// The macOS port backs this with NSRegularExpression, which is a wrapper over
// exactly this ICU engine — so pattern syntax and matching semantics are
// identical across the two ports: named groups, look-behind, \p{…} Unicode
// property classes, and code-point (not byte) matching. The public interface
// is unchanged from macOS/Windows, so callers are untouched. (Not brought
// over from Boost/UCD: \X grapheme clusters and matching within invalid
// UTF-8 — same limitation as macOS.)

#pragma once

#include <unicode/regex.h>
#include <unicode/unistr.h>

#include <memory>
#include <string>

class SimpleRegex {
    std::unique_ptr<icu::RegexPattern> _pat;
    std::unique_ptr<icu::RegexMatcher> _m;       // matcher holding the last match
    icu::UnicodeString                 _subject; // subject of the last match (kept alive)
    bool                               _found = false;
    bool                               _ok    = false;
    std::string                        _err;

    // UTF-16 index within _subject → UTF-8 byte offset (matches the Windows
    // std::smatch::position() byte convention, like the macOS byteOffset()).
    intptr_t byteOffset(int32_t loc) const {
        if (loc < 0) return -1;
        if (loc > _subject.length()) loc = _subject.length();
        std::string pre;
        _subject.tempSubString(0, loc).toUTF8String(pre);
        return (intptr_t)pre.size();
    }

public:
    // compile — returns "" on success, or an error message (Windows API: rx.find()
    // returns an error wstring, empty on success).
    std::string compile(const std::string& pattern, bool matchCase) {
        _pat.reset(); _m.reset(); _found = false;
        UErrorCode status = U_ZERO_ERROR;
        UParseError pe = {};
        icu::UnicodeString up = icu::UnicodeString::fromUTF8(pattern);
        uint32_t flags = matchCase ? 0 : UREGEX_CASE_INSENSITIVE;
        _pat.reset(icu::RegexPattern::compile(up, flags, pe, status));
        _ok = U_SUCCESS(status) && _pat != nullptr;
        if (_ok) _err.clear();
        else {
            _pat.reset();
            _err = std::string("Invalid regular expression: ") + u_errorName(status);
        }
        return _err;
    }
    bool can_search() const { return _ok; }

    // search — run against a subject; remember the match for str()/position().
    bool search(const std::string& subject) {
        _m.reset(); _found = false;
        if (!_ok || !_pat) return false;
        _subject = icu::UnicodeString::fromUTF8(subject);
        UErrorCode status = U_ZERO_ERROR;
        _m.reset(_pat->matcher(_subject, status));
        if (U_FAILURE(status) || !_m) { _m.reset(); return false; }
        status = U_ZERO_ERROR;
        _found = _m->find(0, status) && U_SUCCESS(status);
        return _found;
    }
    size_t size() const {
        return _found && _m ? (size_t)_m->groupCount() + 1 : 0;
    }
    size_t mark_count() const {
        if (!_pat) return 0;
        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<icu::RegexMatcher> tmp(_pat->matcher(status));
        return (U_SUCCESS(status) && tmp) ? (size_t)tmp->groupCount() : 0;
    }

    std::string str(int group) const {
        if (!_found || !_m || group < 0 || (size_t)group >= size()) return "";
        UErrorCode status = U_ZERO_ERROR;
        int32_t s = _m->start(group, status);
        int32_t e = _m->end(group, status);
        if (U_FAILURE(status) || s < 0) return "";
        std::string out;
        _subject.tempSubString(s, e - s).toUTF8String(out);
        return out;
    }
    // position of a group within the last searched subject (byte offset), or -1.
    intptr_t position(int group) const {
        if (!_found || !_m || group < 0 || (size_t)group >= size()) return -1;
        UErrorCode status = U_ZERO_ERROR;
        int32_t s = _m->start(group, status);
        if (U_FAILURE(status) || s < 0) return -1;
        return byteOffset(s);
    }
};
