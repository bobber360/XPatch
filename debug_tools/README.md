# XPatch360 debug tools

For people who want to see what a patch changes, or make new ones for a real Xbox 360.

## 1. Make a dump on the console

XPatch360 keeps its own files in `Hdd:\Patches\XPatch360\`: the log, the settings
(`XPatch360.ini`), the hash cache, `XPatch360.dump` and the dumps (`.bin`). The patch files
(`.toml`) stay in `Hdd:\Patches\`.

1. Aurora → **System → Scripts → XPatch360 → [ Settings ]** → **Debug mode: ON**.
2. Either
   - open a game's patch list → **[ Debug: dump this game's code on next start ]**, or
   - in Settings → **[ Debug: dump the next game I start ]** (for games that have no patch file yet).
3. Start the game. After a few seconds: `XPatch360: dump done (17 MB)`.
4. Copy the dump from `Hdd:\Patches\XPatch360\` to the PC (FTP / Xbox 360 Neighborhood), e.g.
   `545407F2_TU8_code.bin`.

The dump is the game's code and data as loaded (title update included), with XPatch360's own
patches shown as the original bytes. Turn Debug mode off again when you are done.

### Advanced: XPatch360.dump

The menu entries write lines into `Hdd:\Patches\XPatch360\XPatch360.dump`; you can also write them by hand
(one per line, hex numbers, `#` = comment):

| Line | What it does |
|---|---|
| `545407F2 code` | whole game image → `545407F2_TU8_code.bin` (done once, then the line is removed) |
| `next code` | same, for the next game you start |
| `545407F2 code 60` | the same 60 seconds after the game started |
| `545407F2 82AA9E30 10` | 0x10 bytes into the log, right after patching (original bytes, `*` = changed by a patch) |
| `545407F2 82AA9E30 10 60` | the same 60 s after the start, live values |
| `545407F2 *831C22A4+40A0 10 60` | read the pointer at 831C22A4, add 40A0, dump there |
| `545407F2 82000000 1400000` | a range bigger than 0x1000 → `545407F2_82000000.bin` |

Address lines stay in the file and run at every start of that game; delete them when done.

## 2. Read the dump

The `.bin` is a raw memory image: big-endian PowerPC. Byte 0 of the file is the start address
(`82000000` for code dumps, or the address in the file name):

    address = start address + offset in the file

### ppcdis.py (this folder)

Needs only Python 3 (python.org), nothing to install.

```
python ppcdis.py 545407F2_TU8_code.bin dis 82A1A200 16      instructions from an address
python ppcdis.py 545407F2_TU8_code.bin func 82A1A21C        the whole function around an address
python ppcdis.py 545407F2_TU8_code.bin refs 82A1A200        what calls / branches to / loads it
python ppcdis.py 545407F2_TU8_code.bin strings density --refs   text in the dump, and where it is used
python ppcdis.py 545407F2_TU8_code.bin find "39 60 00 02"   a byte sequence
```

Example: what does a patch line `address = 0x82a1a21c` change?

```
> python ppcdis.py 545407F2_TU8_code.bin dis 82A1A218 3
 82A1A218: 2F03FFFF  cmpwi cr6, r3, -1
 82A1A21C: 419A0014  beq cr6, 0x82A1A230
 82A1A220: 786B0020  rldicl r11, r3, 0, 32
```

Each line: address, the 4 bytes (what goes into a patch's `value`), the instruction.
It is a small tool, not a full disassembler: vector (VMX128) instructions show as raw words.

### Ghidra / IDA

- **Ghidra** (free): *File → Import File* → format **Raw Binary**, language **PowerPC, big endian,
  64-bit** (Xenon), *Options → Base Address* = the start address. Then *Analyze*.
- **IDA**: processor *PowerPC big-endian*, loading address = the start address.
- A **hex editor** (e.g. HxD) is enough to search for text or values.

## 3. Make a patch

A patch is a block in the game's `.toml` file in `Hdd:\Patches\`, next to the Xenia ones:

```toml
[[patch]]
    name = "My Patch (console)"
    desc = "What it does on a real Xbox 360."
    author = "you"
    is_enabled = false

    [[patch.be32]]
        address = 0x82a1a21c
        value = 0x48000048
```

Types: `be8`, `be16`, `be32`, `be64`, `f32`, `f64`, `string`, `u16string`, `array`.
XPatch360 applies the file only if its `hash` line matches the running game (the log,
`Hdd:\Patches\XPatch360\XPatch360.log`, shows the game's hash), so the addresses always belong to the right game version.

Found something that works (or a Xenia patch that doesn't)? Please report it in
[Discussions](https://github.com/bobber360/XPatch360/discussions/1).
