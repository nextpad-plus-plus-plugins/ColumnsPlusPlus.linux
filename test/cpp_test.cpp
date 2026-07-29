// Behaviour tests for the Linux-specific layers of Columns++:
//   * SimpleRegex / RegularExpression (the ICU adapters replacing the macOS
//     NSRegularExpression wrappers) — matching, groups, named groups, UTF-8
//     byte offsets on multibyte text, and the format_all replacement subset
//   * formatNumber / parseNumber (NumericFormat.cpp, linked directly)
//
// The engine files (Commands/Search/ElasticTabstops/…) are byte-identical to
// the macOS port, so their coverage comes from the live host battery; these
// tests pin down the parts that were REWRITTEN for Linux.
//
// Usage: cpp_test [ignored]   (no display needed)

#include "Regex.h"
#include "RegularExpression.h"

#include <cstdio>
#include <string>

// NumericFormat.cpp members need the class definition; pull the real header
// and instantiate the god object without any host. Constructing it drags in
// SearchData + one elastic hook, which live in TUs with heavy dependencies —
// stub those symbols here (the test never calls them).
#include "CppData.h"

class RegexCalc {};   // forward-declared in CppData.h; complete for the stubs
SearchData::SearchData() {}
SearchData::~SearchData() {}
void ColumnsPlusPlusData::setTabstops(DocumentData&, SciLine, SciLine, bool) {}

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    ok ? ++g_pass : ++g_fail;
}
static void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    bool ok = got == want;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) printf("         want: '%s'\n         got:  '%s'\n", want.c_str(), got.c_str());
    ok ? ++g_pass : ++g_fail;
}

int main() {
    // ── SimpleRegex ────────────────────────────────────────────────────────
    printf("== SimpleRegex (ICU) ==\n");
    {
        SimpleRegex rx;
        check(rx.compile("(\\d+)-(\\d+)", true).empty(), "compile ok");
        check(rx.search("abc 12-345 xyz"), "search finds match");
        checkEq(rx.str(0), "12-345", "group 0 text");
        checkEq(rx.str(1), "12", "group 1 text");
        checkEq(rx.str(2), "345", "group 2 text");
        check(rx.position(0) == 4, "byte position of match");
        check(rx.size() == 3, "size = groups + 1");
        check(rx.mark_count() == 2, "mark_count = capture groups");

        // Case-insensitive + \p{} + code-point-correct offsets on CJK text.
        check(rx.compile("\\p{L}+", true).empty(), "\\p{L} compiles (ICU)");
        check(rx.search("!!\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E ok"), "matches CJK letters");
        checkEq(rx.str(0), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "CJK match text");
        check(rx.position(0) == 2, "byte offset before multibyte match");

        SimpleRegex bad;
        check(!bad.compile("(unclosed", true).empty(), "compile error reported");
        check(!bad.can_search(), "cannot search after error");

        SimpleRegex ci;
        ci.compile("hello", false);
        check(ci.search("say HELLO"), "case-insensitive match");

        // look-behind (unsupported by std::regex; supported by ICU)
        SimpleRegex lb;
        check(lb.compile("(?<=\\$)\\d+", true).empty(), "look-behind compiles");
        check(lb.search("cost $42 total") && lb.str(0) == "42", "look-behind matches");
    }

    // ── RegularExpression ─────────────────────────────────────────────────
    printf("\n== RegularExpression (ICU) ==\n");
    {
        RegularExpression rx;
        check(rx.find("(?<word>\\w+)@(\\d+)", true).empty(), "compile with named group");
        check(rx.search(std::string_view("  abc@77  ")), "string search");
        checkEq(rx.str(0), "abc@77", "whole match");
        checkEq(rx.str("word"), "abc", "named group lookup");
        check(rx.position(0) == 2 && rx.length(0) == 6, "position+length in bytes");

        // Document-range search through a reader, with absolute positions.
        std::string doc = "line one\nfind 123 here\nlast";
        rx.setReader([&doc](intptr_t a, intptr_t b) {
            return doc.substr((size_t)a, (size_t)(b - a));
        });
        check(rx.find("\\d+", true).empty(), "compile digits");
        check(rx.search((intptr_t)9, (intptr_t)22, (intptr_t)9), "range search via reader");
        check(rx.position(0) == 14, "document-absolute position");
        checkEq(rx.str(0), "123", "range match text");

        // search-from offset within multibyte text: byte offset snaps to lead.
        RegularExpression mb;
        mb.find("\\d", true);
        std::string s = "\xC3\xA9 1 \xC3\xA9 2";   // é 1 é 2
        check(mb.search(std::string_view(s), 4), "search from mid-string byte offset");
        check(mb.position(0) == 8, "second digit found at byte 8");

        // format(): the Boost format_all subset.
        RegularExpression fm;
        fm.find("(?<first>\\w+) (\\w+)", true);
        fm.search(std::string_view("hello world!"));
        checkEq(fm.format("$2-$1"), "world-hello", "$N substitution");
        checkEq(fm.format("${first}!"), "hello!", "${name} substitution");
        checkEq(fm.format("$&|$0"), "hello world|hello world", "$& and $0");
        checkEq(fm.format("[$`][$']"), "[][!]", "$` and $' (before/after)");
        checkEq(fm.format("\\U$1\\E-$2"), "HELLO-world", "\\U..\\E case span");
        checkEq(fm.format("\\L$2\\E"), "world", "\\L case span");
        checkEq(fm.format("\\1+\\2"), "hello+world", "\\N substitution");
        checkEq(fm.format("$$ \\t"), "$ \t", "literal $ and escapes");
    }

    // ── NumericFormat (linked from the real NumericFormat.cpp) ────────────
    printf("\n== formatNumber / parseNumber ==\n");
    {
        static ColumnsPlusPlusData data;   // no host needed for these members

        NumericFormat f;
        f.maxDec = 2; f.minDec = 2;
        checkEq(data.formatNumber(1234.5, f), "1234.50", "fixed 2 decimals");
        f.thousands = ","; f.leftPad = 1;
        checkEq(data.formatNumber(1234567.891, f), "1,234,567.89", "thousands separator");
        f.thousands = ""; f.maxDec = 0; f.minDec = -1;
        checkEq(data.formatNumber(42.4, f), "42", "round to integer");

        data.settings.decimalSeparatorIsComma = false;
        NumericParse p = data.parseNumber("1234.75");
        check(p && (double)p == 1234.75 && p.decimalPlaces == 2, "parse plain decimal");
        p = data.parseNumber("not a number");
        check(!p, "non-number yields NaN");
        data.settings.decimalSeparatorIsComma = true;
        p = data.parseNumber("12,5");
        check(p && (double)p == 12.5, "comma decimal separator honoured");
        data.settings.decimalSeparatorIsComma = false;

        // time parsing (H:M) — recognised per the timeFormatEnable defaults.
        p = data.parseNumber("1:30");
        check(p && p.timeSegments >= 1, "colon time parses");
    }

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
