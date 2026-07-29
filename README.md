# Columns++ for Nextpad++ — Linux (GTK4) port

Features for working with text and data arranged in columns, inside
Nextpad++ on Linux. Port of the macOS port of Randall Joseph Fellmy's
[Columns++ for Notepad++](https://coises.github.io/ColumnsPlusPlus/help.htm)
(GPL v3+).

## Features (complete set, matching the macOS port)

- **Elastic tabstops** with profiles, per-extension profile mapping,
  monospace optimization, and size/line-count disable thresholds.
- **Align** left / right / numeric / custom (first, last, or regex match).
- **Column sort**: binary / locale / numeric / width keys, ascending or
  descending, whole lines or selected text only, tabbed or regex-defined
  keys with per-key overrides (`2,1d,3an`), locale collation options
  (case, digits-as-numbers, diacritics, symbols).
- **Search in region** — modeless find/replace/count/select-all over a
  rectangular or multi-selection region, with Normal / Extended / Regex
  modes and regex-calculate substitutions.
- **Calculate** — per-row ExprTk expressions with regex capture inputs,
  thousands/decimal/time formatting, aligned column insertion.
- **Timestamps** — counter ⇄ date/time conversion (Unix, FILETIME,
  Excel 1900/1904, custom epochs), time zones (OS tzdb), locale-aware
  formats and custom parse patterns / output pictures.
- **Add / Average numbers** with a live-formatted results dialog.
- **Convert** tabs ⇄ separated values (CSV/TSV/custom) and tabs → spaces.
- **Selection utilities**: extend/enclose rectangular selections in any
  direction, plus optional automatic extension rules (Options).

## How the platform layer maps

The engine — elastic tabstops analysis/layout, the rectangular-selection
model, align/sort/convert, numeric parse/format, Search, Calculate (ExprTk),
Timestamps (Howard Hinnant's date/tz against the OS zoneinfo) — is
**byte-identical to the macOS port** (the `.mm` files were pure C++). The
platform layer maps as:

| macOS | Linux |
|---|---|
| NSRegularExpression adapters (`Regex.h`, `RegularExpression.h`) | `icu::RegexMatcher` — the same ICU engine NSRegularExpression wraps, so pattern syntax and semantics are identical |
| `NSString compare:` locale sort | `icu::Collator` (true ignore-symbols via shifted alternate handling — closer to the Windows original than the macOS width-insensitivity approximation) |
| `NSDateFormatter` / `NSCalendar` / `NSLocale` | `icu::DateFormat` / `GregorianCalendar` / `Locale` |
| AppKit dialogs (11) | GTK4, field-for-field |
| `NSPasteboard` | `GdkClipboard` |

Settings persist to the host's plugin config dir as `ColumnsPlusPlus.data`
in the same text format the Windows/macOS versions use.

## Building

```sh
cmake -B build -S .
cmake --build build -j"$(nproc)"
cmake --install build   # -> ~/.local/share/nextpad++/plugins/ColumnsPlusPlus/
```

Requires `libgtk-4-dev`, `libicu-dev`, and the Nextpad++ GTK4 tree checked
out alongside this folder.

### Tests

```sh
ctest --test-dir build --output-on-failure
```

`loader` checks the dlopen surface, menu shape (38 items, 7 separators) and
the FuncItem ABI. `cpp_test` (43 assertions, no display needed) covers the
Linux-rewritten layers: both ICU regex adapters (groups, named groups,
look-behind, `\p{…}`, UTF-8 byte offsets on multibyte text, the Boost
`format_all` replacement subset) and numeric parse/format round-trips.
The byte-identical engine is exercised through the live host battery
(rect-selection sort, elastic layout, tabs→spaces using elastic widths,
Search-in-region replace, config persistence).

## Linux notes

- No pre-bound keyboard shortcuts (this host's `FuncItem` has no shortcut
  field) — bind them in *Settings ▸ Shortcut Mapper ▸ Plugin commands*.
- "Show Columns++ on the main menu bar" is preserved in the settings file
  but has no effect (menu placement is fixed), as on macOS.
- Deviations kept from the macOS port rather than the Windows original:
  no `\X` grapheme clusters or matching inside invalid UTF-8, and no
  conditional `(?N:a:b)` replacements.

## License

GNU General Public License v3 (or later), as upstream.
