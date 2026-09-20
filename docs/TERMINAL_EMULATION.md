# Terminal Emulation Reference

**Scope:** what `AnsiParser` + `TerminalScreen` (`src/terminal/`) understand, and which bytes
`TerminalWidget` sends for each key. Target: the VT100/xterm subset used by U-Boot, the Linux
console, busybox/ash line editing, ncurses programs (`top`, `vi`, `menuconfig`) and MCU shells.
Not a terminfo-complete xterm; unknown input is consumed silently (logged at debug level on the
`buildai.terminal` category) and **never desynchronises** the output.

The parser is Paul Williams' VT500-series state machine (<https://vt100.net/emu/dec_ansi_parser>)
with two deliberate extensions: colon sub-parameters in CSI (for `SGR 38:2::r:g:b`), and any
non-ASCII printable inside an ESC/CSI sequence aborts the sequence and is printed (real devices
emit garbage between sequences after a reset; swallowing the text that follows is worse).

## 1. Bytes to characters

| Topic | Behaviour |
|---|---|
| Decoding | Stateful `QStringDecoder`, default **UTF-8**. Others offered: `ISO-8859-1`, `GB18030` (when Qt has ICU), `System` (Windows ANSI code page). Unknown names fall back to UTF-8. |
| Split input | Multi-byte characters and escape sequences split across `feed()` calls are handled (decoder state + parser state persist). |
| Invalid UTF-8 | Each invalid byte or truncated sequence decodes to `U+FFFD` (one cell); decoding resumes at the next byte, so a stray `0x9B` prints `�` and does **not** act as CSI. At a chunk boundary only a valid-so-far incomplete sequence is carried over; an invalid lead byte at the end of a chunk becomes `U+FFFD` and the bytes after it (e.g. the ESC of the next sequence) are processed normally. |
| C1 controls | Code points `U+0080..U+009F` (reachable via 8-bit encodings or UTF-8 `C2 9B` etc.) are honoured: `9B` CSI, `9D` OSC, `90` DCS, `98`/`9E`/`9F` SOS/PM/APC, `9C` ST; the rest are ignored. |
| Character width | 2 cells: U+1100-115F, 2E80-303E, 3041-33FF, 3400-4DBF, 4E00-9FFF, A000-A4CF, AC00-D7A3, F900-FAFF, FE30-FE4F, FF00-FF60, FFE0-FFE6, 1F300-1F64F, 1F680-1F6FF, 1F900-1F9FF, 20000-2FFFD, 30000-3FFFD. 0 cells: combining marks (0300-036F, 1AB0-1AFF, 1DC0-1DFF, 20D0-20FF, FE20-FE2F), Hangul Jamo medial/final (1160-11FF), format chars (200B-200F, 2028-202E, 2060-2064), variation selectors (FE00-FE0F, E0100-E01EF), C0/C1 controls. Everything else, including `U+2600-26FF` (misc. symbols) and `U+FFFD`, is 1 cell. |
| Zero-width code points | Ignored (not stored); combining accents are dropped rather than rendered. |
| Wide characters | Occupy a `WideLead` cell plus a `WideTrail` placeholder (`ch == 0`). If only one column is left the line wraps first (autowrap off: placed as a narrow cell in the last column). Overwriting or erasing either half blanks the other half. |
| `DEL` (0x7F) | Ignored everywhere. |

## 2. C0 controls

Executed in every parser state except inside OSC/DCS/SOS/PM/APC strings (where `BEL`/`ESC`
terminate, `CAN`/`SUB` abort, and other C0 are dropped).

| Byte | Name | Action |
|---|---|---|
| `0x07` | BEL | `TerminalScreen::bell()` → `bellRequested()` |
| `0x08` | BS | Cursor left one column; stops at column 0, never wraps, never erases |
| `0x09` | HT | Next tab stop, or the last column |
| `0x0A` `0x0B` `0x0C` | LF VT FF | Line feed. With **implicit CR** (default on) or **LNM** set, a carriage return is performed **first**. At the bottom margin the scroll region scrolls up; when the region is the whole screen and the alternate screen is off, the top line goes to the scrollback |
| `0x0D` | CR | Column 0 |
| `0x18` `0x1A` | CAN SUB | Abort the current sequence, back to ground |
| `0x1B` | ESC | Start an escape sequence (from any state; terminates OSC strings) |
| `0x0E` `0x0F` | SO SI | Ignored (display is always Unicode) |
| others | NUL ENQ XON XOFF … | Ignored |

## 3. ESC sequences

| Sequence | Name | Action |
|---|---|---|
| `ESC 7` | DECSC | Save cursor position, attributes, origin mode, autowrap |
| `ESC 8` | DECRC | Restore them (nothing saved: home + default attributes) |
| `ESC D` | IND | Line feed (no CR) |
| `ESC E` | NEL | CR + line feed |
| `ESC H` | HTS | Set a tab stop at the cursor column |
| `ESC M` | RI | Cursor up; at the top margin the region scrolls down |
| `ESC Z` | DECID | Reply as DA: `ESC [ ? 1 ; 2 c` |
| `ESC c` | RIS | Full reset: screen, scrollback, cursor, attributes, modes, tabs, parser modes (LNM, DECCKM, bracketed paste). Window title is kept |
| `ESC # 8` | DECALN | Fill the screen with `E`, reset margins and origin mode, home |
| `ESC # 3/4/5/6` | DECDHL/DECSWL/DECDWL | Ignored |
| `ESC =` `ESC >` | DECKPAM/DECKPNM | Ignored |
| `ESC ( ) * + - . /` *x* | SCS | Character set designation: ignored |
| `ESC N O n o \| } ~` | SS2 SS3 LS2 LS3 LS3R LS2R LS1R | Ignored |
| `ESC SP` *x*, `ESC %` *x* | S7C1T/S8C1T, ESC % G / @ | Ignored |
| `ESC \` | ST | Ends a string; otherwise no effect |
| `ESC [` `ESC ]` `ESC P` `ESC X ^ _` | CSI OSC DCS SOS/PM/APC | Introducers, see below |
| other | | Logged, ignored |

## 4. CSI sequences

Parameters: decimal, `;`-separated, up to 32 (extras ignored), each capped at 65535, empty =
0. `:` introduces sub-parameters (up to 8 per parameter). *Pn* defaults to 1 when 0/missing;
*Ps* defaults to 0. Coordinates are 1-based on the wire, 0-based in `TerminalScreen`.

| Sequence | Name | Action |
|---|---|---|
| `CSI Pn @` | ICH | Insert *Pn* blank cells at the cursor (line shifts right, last cells lost) |
| `CSI Pn A` / `B` / `C` / `D` | CUU CUD CUF CUB | Cursor up/down/right/left, clamped to the screen (to the scroll region when the cursor is inside it) |
| `CSI Pn E` / `F` | CNL CPL | Down/up *Pn* lines to column 0 |
| `CSI Pn G` / `` ` `` | CHA HPA | Cursor to column *Pn* |
| `CSI Pr ; Pc H` / `f` | CUP HVP | Cursor to row *Pr*, column *Pc* (relative to the region top in origin mode) |
| `CSI Pn I` | CHT | Forward *Pn* tab stops |
| `CSI Ps J` | ED | 0 cursor→end, 1 start→cursor, 2 whole screen, 3 scrollback only. Cursor unchanged |
| `CSI Ps K` | EL | 0 cursor→end of line, 1 start→cursor, 2 whole line |
| `CSI Pn L` / `M` | IL DL | Insert/delete *Pn* lines at the cursor row inside the scroll region (ignored outside it); cursor moves to column 0 (DEC behaviour) |
| `CSI Pn P` | DCH | Delete *Pn* cells, rest of the line shifts left |
| `CSI Pn S` / `T` | SU SD | Scroll the region up/down *Pn* lines (never into the scrollback). `T` with more than one parameter (XTHIMOUSE) is ignored |
| `CSI Pn X` | ECH | Erase *Pn* cells from the cursor |
| `CSI Pn Z` | CBT | Back *Pn* tab stops (fixed 8-column stops) |
| `CSI Pn a` / `e` | HPR VPR | Relative column / row move |
| `CSI Pn b` | REP | Repeat the last printed character *Pn* times |
| `CSI Ps c` | DA | *Ps* = 0: reply `ESC [ ? 1 ; 2 c` ("VT100 with AVO") |
| `CSI Pn d` | VPA | Cursor to row *Pn* |
| `CSI Ps g` | TBC | 0 clear the stop at the cursor, 3 clear all |
| `CSI Ps ; … h` / `l` | SM RM | Set/reset ANSI modes (§6) |
| `CSI ? Ps ; … h` / `l` | DECSET DECRST | Set/reset DEC private modes (§6) |
| `CSI Ps ; … m` | SGR | Attributes and colours (§5) |
| `CSI Ps n` | DSR | 5: reply `ESC [ 0 n`. 6: reply `ESC [ row ; col R` (1-based, relative to the region in origin mode) |
| `CSI ? 6 n` | DECXCPR | Reply `ESC [ ? row ; col R` |
| `CSI Pt ; Pb r` | DECSTBM | Scroll region rows *Pt*..*Pb* (defaults 1 and last row); *Pt* ≥ *Pb* ignored; homes the cursor |
| `CSI s` / `u` | SCOSC SCORC | Save / restore cursor (same slot as DECSC/DECRC) |
| `CSI ! p` | DECSTR | Soft reset (§7) |
| `CSI ? Ps J` / `K` | DECSED DECSEL | Treated like ED / EL |
| `CSI Ps SP q` | DECSCUSR | Cursor style: ignored |
| `CSI Ps " q`, `CSI … " p` | DECSCA DECSCL | Ignored |
| `CSI … t` | XTWINOPS | Window operations: ignored |
| `CSI Ps q`, `CSI Ps x` | DECLL DECREQTPARM | Ignored |
| `CSI $ …`, `CSI ' …`, `CSI * …` | DECRQM, rectangular ops … | Ignored |
| `CSI > …`, `CSI = …`, `CSI < …` | XTMODKEYS, secondary/tertiary DA, kitty keyboard … | Ignored |
| other final bytes | | Logged, ignored. Malformed sequences (private marker after digits, digits after an intermediate) are discarded up to their final byte |

## 5. SGR (`CSI … m`)

An SGR with no parameters is SGR 0. Attributes accumulate until reset.

| Parameter | Effect |
|---|---|
| `0` | Reset everything to default |
| `1` / `2` | Bold / dim (`22` clears both) |
| `3` / `23` | Italic on / off |
| `4`, `4:1`..`4:5`, `21` | Underline on (all styles and double underline render as a single underline); `4:0`, `24` off |
| `5`, `6` / `25` | Blink on / off (rendered as normal text) |
| `7` / `27` | Inverse on / off |
| `8` / `28` | Hidden on / off |
| `9` / `29` | Strikethrough on / off |
| `30`-`37` / `40`-`47` | Foreground / background ANSI colour 0-7 |
| `90`-`97` / `100`-`107` | Bright foreground / background 8-15 |
| `39` / `49` | Default foreground / background |
| `38;5;n`, `48;5;n`, `38:5:n`, `48:5:n` | 256-colour index *n* (0-15 palette, 16-231 6×6×6 cube, 232-255 greys) |
| `38;2;r;g;b`, `48;2;r;g;b`, `38:2::r:g:b`, `48:2::r:g:b`, also the malformed `38:2:r:g:b` | 24-bit colour (components clamped to 0-255) |
| `58…`, `59` | Underline colour: parsed and ignored |
| other | Logged, ignored (the remaining parameters are still applied) |

Rendering note: with an indexed colour 0-7 and **bold** set, the renderer uses the bright
variant 8-15 ("bold is bright", `Palette::resolve()`).

## 6. Modes

| Mode | Name | Default | Action |
|---|---|---|---|
| `4` | IRM | reset | Insert mode: printed characters shift the rest of the line right |
| `20` | LNM | reset | LF/VT/FF also perform CR (same effect as the *implicit CR* option) |
| `2`, `12` | KAM, SRM | | Ignored |
| `?1` | DECCKM | reset | Cursor keys send `ESC O A..D` instead of `ESC [ A..D`; `AnsiParser::cursorKeyModeChanged(bool)` |
| `?6` | DECOM | reset | Origin mode: rows relative to the scroll region, cursor confined to it; homes the cursor |
| `?7` | DECAWM | **set** | Autowrap (see §7 pending wrap) |
| `?25` | DECTCEM | set | Cursor visible |
| `?47`, `?1047` | | reset | Alternate screen (blank, no scrollback while active); primary contents restored on reset |
| `?1048` | | | Save (set) / restore (reset) cursor |
| `?1049` | | reset | `?1048` + `?47`: save cursor, switch to a blank alternate screen; reset restores screen and cursor |
| `?2004` | | reset | Bracketed paste: `AnsiParser::bracketedPasteChanged(bool)`; pastes are wrapped in `ESC [ 200 ~` … `ESC [ 201 ~` |
| `?3 ?4 ?5 ?8 ?9 ?12 ?40 ?45 ?1000-1007 ?1015 ?1016 ?1034 ?1036 ?2026 ?7727 ?8452` | DECCOLM, smooth scroll, reverse video, autorepeat, mouse tracking, focus events, cursor blink, … | | Consumed and ignored |
| other | | | Logged, ignored |

## 7. Screen model semantics (`TerminalScreen`)

- **Pending wrap (xterm):** writing in the last column leaves the cursor on that column with a
  pending-wrap flag; the next printable character wraps (the line is marked `wrapped`, so copy &
  paste joins it with the next line without a newline). CR, LF, BS, HT, any cursor movement,
  erase or edit clears the flag. Autowrap off: characters overwrite the last column.
- **Scrolling:** LF/IND/NEL at the bottom margin scroll the region. Only a full-screen scroll on
  the primary screen pushes the top line into the scrollback (bounded by *scrollback lines*,
  oldest dropped, `wrapped` flag preserved). IL/DL/SU/SD/RI never touch the scrollback. LF below
  the region moves down to the last row and stops.
- **Erase** (ED/EL/ECH/DCH/ICH/IL/DL/scroll fill) writes blank cells carrying the current
  **background colour** only (`bce`), no other attributes.
- **Alternate screen:** separate blank grid, no scrollback pushes, its own DECSC/DECRC slot.
  Leaving it restores the primary grid (fitted to the current size).
- **Resize:** columns truncate/pad lines (no reflow; a wide character cut in half is blanked).
  Fewer rows: lines above the cursor move into the scrollback first so the cursor stays visible,
  then blank/bottom lines are dropped. More rows: lines are pulled back from the scrollback,
  otherwise blank lines are appended at the bottom. The scroll region resets to the full screen;
  default tab stops are added in new columns.
- **Tabs:** every 8 columns after RIS/construction; HTS/TBC edit them; HT stops at the last column.
- **DECSTR (`CSI ! p`)** resets: attributes, cursor visible, IRM off, DECOM off, DECAWM on,
  scroll region full, LNM off, DECCKM off, bracketed paste off. The cursor position and the
  screen contents are kept.
- **RIS (`ESC c`)** additionally clears the screen and scrollback, homes the cursor, leaves the
  alternate screen, forgets saved cursors and restores the default tab stops.
- **Signals:** every mutating call emits `contentChanged()` exactly once (`putText()` once per
  run of printable characters, never per cell); rows are tracked in `dirtyRows()`, whole-screen
  changes (scroll, resize, clear, alternate screen) set `allDirty()`.

## 8. OSC and other strings

| String | Action |
|---|---|
| `OSC 0 ; text ST` / `OSC 2 ; text ST` | Set window title (`titleChanged`). Terminator: `BEL` (0x07), `ESC \`, or C1 `ST` (0x9C). An `ESC` followed by anything else also ends the string and starts a new sequence |
| `OSC Ps ; …` (other *Ps*: 1, 4, 8, 10-19, 52, 133, 1337 …) | Consumed, ignored |
| `DCS … ST`, `SOS … ST`, `PM … ST`, `APC … ST` | Consumed up to ST, ignored (`BEL` does not terminate them) |

OSC strings are capped at 4096 characters (excess dropped).

## 9. Replies sent to the device (`AnsiParser::responseRequested`)

| Query | Reply |
|---|---|
| `CSI c`, `CSI 0 c`, `ESC Z` | `ESC [ ? 1 ; 2 c` |
| `CSI 5 n` | `ESC [ 0 n` |
| `CSI 6 n` | `ESC [ <row> ; <col> R` (1-based; row relative to the scroll region in origin mode) |
| `CSI ? 6 n` | `ESC [ ? <row> ; <col> R` |

## 10. Options

| Option | Default | Effect |
|---|---|---|
| Implicit CR on LF (`AnsiParser::setImplicitCr`) | on | LF/VT/FF also return to column 0. Needed for MCU firmware printing bare `\n`; harmless for `\r\n` consoles |
| Encoding (`AnsiParser::setEncoding`) | UTF-8 | Decoder for received bytes; the same name is used by `TerminalWidget` to encode typed text |
| Scrollback lines (`TerminalScreen::setScrollbackMax`) | 10 000 | 0 disables the scrollback |

## 11. Key mapping (`TerminalWidget`, bytes sent)

- Enter/Return → `LineEnding::bytes(enterSends())` (default CR)
- Backspace → `0x7F` or `0x08` (`backspaceSendsDelete()`); Shift+Backspace sends the other one
- Tab → `0x09` (the widget keeps Tab: it overrides `focusNextPrevChild` / `event()`)
- Esc → `0x1B`; Delete → `ESC [ 3 ~`; Insert → `ESC [ 2 ~`; Home → `ESC [ H`; End → `ESC [ F`
- Arrows → `ESC [ A/B/C/D`, or `ESC O A/B/C/D` in DECCKM application mode
- PgUp/PgDn → `ESC [ 5 ~` / `ESC [ 6 ~`; Shift+PgUp/PgDn scroll the view instead
- F1-F4 → `ESC O P/Q/R/S`; F5..F12 → `ESC [ 15~ 17~ 18~ 19~ 20~ 21~ 23~ 24~`
- Ctrl+A..Z → `0x01..0x1A`; Ctrl+[ `0x1B`; Ctrl+\ `0x1C`; Ctrl+] `0x1D`; Ctrl+Space `0x00`
- Alt+<key> → `ESC` + key bytes
- Ctrl+Shift+C / Ctrl+Insert → copy selection; Ctrl+Shift+V / Shift+Insert → paste
- Ctrl+Shift+<letter> is left to the main window's actions (Hex View, Find, Send File, Clear,
  Replay Log, Quit, ...) even while connected; one that no action uses is sent as the
  Ctrl+<letter> control byte. Ctrl+T / Ctrl+W / Ctrl+Tab / Ctrl+, / F2 / F3 / F5 pass through too
- Ctrl+C with an active selection → copy (and clear the selection); without → `0x03`
- Ctrl+wheel / Ctrl+'+' / Ctrl+'-' / Ctrl+0 → zoom (font size); emits `fontZoomed()`
- Text (incl. IME commit) → encoded with the current encoding (`QStringEncoder`)
- Middle click → paste selection/clipboard; right click → context menu
  (Copy, Paste, Select All, Clear Scrollback, Reset Terminal, Sync Terminal Size, Find...)
- Paste: newlines are converted to the Enter bytes; in bracketed paste mode wrapped in
  `ESC [ 200 ~` … `ESC [ 201 ~`. Large pastes are sent in one write (pacing is the port's job)
- Drag & drop: a dropped file emits `fileDropped(path)`; dropped text is pasted

## 12. Known limitations

- No left/right margins (DECLRMM), no rectangular-area operations, no 132-column mode.
- Combining characters are dropped; emoji coverage is the listed blocks only; `U+2600-26FF` is
  always narrow.
- CBT uses fixed 8-column stops, ignoring HTS/TBC changes.
- Blink and underline styles are not animated/differentiated by the renderer.
- No mouse reporting, focus events or Sixel/ReGIS graphics; the corresponding modes and strings
  are accepted and ignored so applications keep working.
