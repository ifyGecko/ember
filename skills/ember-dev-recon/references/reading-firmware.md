# Reading firmware: strings, tables and anchors

The tools in this skill say **where** things are: the base, the MMIO pages, the flash
layout. This file is about working out **what things are**: which function is the UART
driver, which chip a block of code was written for, what the firmware waits for before it
prints its banner. The board developer needs exactly that: a page labelled "unknown, 42
reads" is a week of guessing, a page labelled "16550 UART, LSR polled for 0x20 by
`sysSerialPutc`" is an afternoon.

The firmware's authors left the answers in it, as text. Start there.

## The loop

Every finding comes out of the same six steps. When stuck, check which step was skipped.

1. **Lead.** Something readable: a string, a constant, a table, a hot MMIO page.
2. **Locate.** Find the code that uses it: its cross-references.
3. **Read.** Decompile that function. Read its other strings and the functions it calls.
4. **Name.** Rename the function (and its callers, when they become obvious). A named
   function turns every later decompile that calls it into readable text.
5. **Record.** Write the fact in RECON.md with its evidence: the string, its address, the
   function, and a tag (*confirmed / inferred / guess*).
6. **Follow.** The callers say *when* it runs (init order). The callees say *what hardware*
   it touches (registers). Each is the next lead.

Work **from the known to the unknown**. One named function next to an unnamed one is worth
more than ten unnamed ones read cold. Never read a large function with no lead: find a
string or a register in it first.

## Step by step: the string pass

Do this once per code stage, after the base is confirmed (step 2 of the pass) and before
labelling MMIO pages (step 5).

1. **Get the leads.** `recon.sh` has already put them in the draft under *String leads*
   (full output in `.recon/strings.txt`). By hand:
   ```bash
   python3 scripts/strings_scan.py stage.bin --base 0x80001000        # the grouped leads
   python3 scripts/strings_scan.py stage.bin --base 0x80001000 --kind wait,chip --max 0
   python3 scripts/strings_scan.py stage.bin --grep 'uart|baud|console'
   ```
   The second column is the address to give Ghidra. Run it on the **decompressed** stage:
   a compressed member has almost no text. On a PC image run it on the extracted modules
   (`.recon/modules/*.efi`), where the text is UTF-16.
2. **Read the `identity` and `chip` groups first** and write down, as *guess*: RTOS and
   version, vendor, board name, SoC name, every chip named.
3. **Pick ten leads** that cover different hardware: one UART or console string, one
   timer or clock string, one flash string, one Ethernet or PHY string, and the `wait`
   strings that mention hardware (PLL, PHY, flash, calibration, DDR).
4. **For each, find the function** (next section), decompile it, name it, record it.
5. **Find the print function** (below). From then on every caller explains itself.
6. **Match functions to MMIO pages.** A function named from its strings that touches page
   P gives P its label. Put the string in the page's evidence column.

## From a string to the function that uses it

With the Ghidra MCP server:
- `list_strings` with `filter` set to a few words of the string -> its address.
- `get_xrefs_to` that address -> the referencing instructions and their functions.
- `decompile_function_by_address` on each.

Headless (no MCP server):
```bash
scripts/ghidra-query.sh --blob stage.bin strings "lock timeout"     # address, reference count
scripts/ghidra-query.sh --blob stage.bin xrefs 0x802d4a10           # who refers to it
scripts/ghidra-query.sh --blob stage.bin decompile 0x80023f40       # the function
```

### When a string has no references

This is common and each cause has its own fix. Check them in this order.

| cause | how to tell | what to do |
|---|---|---|
| **The base is wrong** | nearly every string has 0 references (`ghidra-query.sh strings` prints the count) | go back to step 2 of the pass: a right base makes most strings referenced |
| **The reference is a data word** (a table of names, messages or commands) | `strings_scan.py --ptrs --grep 'TEXT'` lists a word holding the address | read the table (*Tables*, below); the code refers to the table, so xref the table's start |
| **Code not disassembled** | the bytes around the expected user are undefined data in Ghidra | disassemble there (`ghidra-query.sh decompile ADDR` makes a temporary function); look for what calls it through a pointer |
| **gp- or base-register-relative** (MIPS `gp`, PowerPC `r2`/`r13`, RISC-V `gp`) | the string sits in small data, close to other short strings; the init code loads the register with a constant | take the register's value from the entry code, set it in Ghidra, re-analyse; or search for the offset `string - gp` |
| **Harvard part** (AVR, PIC, 8051) or an 8-bit CPU | Ghidra has no reference into program memory | search the bytes of the instruction that loads the address: 8051 `90 hh ll` (`mov dptr,#`), Z80 `21 ll hh` (`ld hl,`), 6502 `a9 ll` with `a2 hh` or `a0 hh` (`lda #lo`, `ldx`/`ldy #hi`), AVR an `ldi r30`/`ldi r31` pair with the low and high byte |
| **Reference to the middle** | the text is the tail of a longer string, or follows another without a terminator | xref the start of the longer string; try the addresses just before |
| **Indexed message table** | many similar messages in a row, none referenced, one pointer array next to them | the index is an error or state number: xref the array, the function is the "print error N" helper |
| **Dead text** | none of the above | a library linked in but not used. Say so; do not build a device from it |

### The neighbours of a string

The linker stores a source file's strings together, in order. So the strings just before
and after a lead come from the same file, usually the same function or the next one:
```bash
python3 scripts/strings_scan.py stage.bin --base 0x80001000 --near 0x802ca574
```
A board name surrounded by `"Speeds(Hz): CPU %d, DDR %d, AHB %d"` and `"BSP Version: %s"`
is the BSP's `sysLib.c`: the functions that use those three strings are the clock, memory
and hardware init the board has to satisfy. One readable string names its whole
neighbourhood.

## What each kind of string gives you

| kind (`strings_scan.py` group) | example | what it tells you | what the board developer does with it |
|---|---|---|---|
| version banner (`identity`) | `VxWorks 5.5.1`, `U-Boot 1.1.4`, `ESP-IDF v4.4` | RTOS and SDK, so the boot order and the names of its standard functions | knows which subsystems start (shell, network), and the banner is the first progress marker |
| board or BSP name (`identity`, `chip`) | `AR9330: AP121 Board`, `.../config/ar9331/sysLib.c` | the SoC, hence the public register map and the vendor's reference source | reads the datasheet for the pages in the MMIO table instead of reverse engineering each |
| chip name (`chip`) | `W25Q64`, `88E6060`, `DS1307`, `KSZ8041` | an external part and its bus (SPI flash, MDIO PHY or switch, I2C RTC) | a device model with that part's ID and command set |
| source file (`source`) | `Assertion failed: ..., file ag7240End.c, line 631` | every function using it is from that driver; the path often names the SoC directory | names a group of functions at once; the file name finds the open-source driver to read alongside |
| function-name prefix (`func`) | `sysUartInit: baud %d`, `%s: timeout` | the function's real name (with `%s:` the name is passed in, often as a second string nearby) | rename; the MMIO page it touches gets the device's name |
| wait or probe failure (`wait`) | `PLL lock timeout`, `PHY not found`, `flash not responding` | a loop or check that needs an answer from hardware | a register value the board must return; goes under *Polling loops* with the value that ends it |
| self-check failure (`error`) | `bad image checksum`, `invalid magic`, `NVRAM CRC error` | data the firmware validates | the emulated flash must hold a valid image or partition, or the default path must be acceptable |
| register dump format (`device`) | `GPIO_OE = %x`, `ISR=%08x IMR=%08x` | the register names, straight from the vendor's header | names for offsets in the MMIO table |
| clock or size report (`device`) | `CPU %d MHz`, `Memory: %dMB` | where the code computes clock and RAM size (straps, PLL registers, a RAM probe) | the strap and PLL values to return; the RAM size the board needs |
| command, help, prompt (`shell`) | `help`, `Usage: tftp ...`, `-> `, `ap121> ` | the console exists and what it accepts; commands sit in a table | the prompt is the "it booted" marker; a command is a way to reach code later (and a fuzz entry) |
| key=value, paths (`config`) | `bootdelay=3`, `lan_ipaddr=192.168.1.1`, `/dev/mtd3` | defaults of a config partition, and which devices and files are opened | content to synthesize for partitions the image lacks; flash partitions by number |
| task or thread name | `tNetTask`, `tShell`, `wdog` | the tasks that run; a task table gives each one's entry | which subsystems will run after init, hence which devices get used |

## Find the print function first

Almost every string is an argument to one or two print functions (`printf`, a log
function with a level, a panic). Name them and half the image explains itself.

1. Take five format strings from different parts of the image (they contain `%`).
2. Xref and decompile the function using each.
3. In each, look at which callee receives the string as an argument. The callee that shows
   up in most of them is the **print function**. A callee with an extra small integer
   before the format (`log(3, "...")`) is a **levelled log**; one whose callers never
   continue afterwards is **panic/assert**.
4. Rename them. Record their addresses in RECON.md.

Then follow print **downwards**. This is the most valuable chain in the image, because
the console is the first device every board needs:

```
printf -> vsprintf/format -> output hook -> putc -> waits on a status bit -> writes a data register
```

The last function is the UART transmit routine. Record:
- the **base address** and the **data and status offsets** it uses, and the offset spacing
  (1 or 4 bytes between registers);
- the **bit it waits for** before writing (a 16550: line status at register 5, bit 0x20;
  the board must return it or every character hangs);
- the UART type, from the init function next to it: a 16550 writes 0x80 to register 3,
  then a divisor to registers 0 and 1, where divisor = clock / (16 x baud). The constants
  115200, 57600, 38400 or 9600 nearby confirm it and give the UART clock.

If print goes through a function pointer or a device table (`write(fd, ...)`), find where
the pointer is set: that init function names the console driver.

The same downward walk works from other anchors:

| anchor (how to find it) | walk | what you learn |
|---|---|---|
| reset vector / entry | forward, in order | the init sequence: the order devices are needed, which is the build phase's bring-up order |
| exception or interrupt vector | to the dispatcher, then its table of handlers | the interrupt controller's status and mask registers, and which line each device uses |
| the tick handler (increments a counter, calls the scheduler) | up to who installs it | the system timer device and its rate |
| a delay function (a loop on a counter or timer register; called with constants like 1000) | its callers | where the code waits on hardware; a calibrated delay needs a timer that advances |
| panic/assert | its callers' strings | every self-check in the image |
| `memcpy`/`memset`/decompress, called from the entry | their arguments | where stages are copied, BSS bounds, RAM size |

## Tables

Firmware describes itself in tables. A table is an array of fixed-size records in data
where the same field in every record is a pointer to a string and another is a pointer to
code. `strings_scan.py --ptrs` finds them from the string side:

```bash
python3 scripts/strings_scan.py stage.bin --base 0x80001000 --ptrs --grep '^(help|reboot)$'
#   string 0x802d21cc <- word at offset 0x0035fc14 (0x80360c14)  next word 0x802d21b0 is inside the image ...
```
Then read the memory around that word in Ghidra, work out the record size (the distance
to the next record's name pointer) and list every record.

| table | record looks like | gives |
|---|---|---|
| shell commands | {name, help text, handler} | a name for each handler; `reboot`, `flash`, `mem`, `mii`, `gpio` handlers lead straight to those devices |
| device or driver list | {name, base address, irq, init function} | the memory map and interrupt routing, as the authors wrote it |
| interrupt table | {cause bit or vector, handler, argument} | the handler per interrupt line |
| task list | {name, entry, priority, stack size} | what runs after init |
| init calls | an array of function pointers called in a loop | the init order, when there is no long init function to read |
| flash partitions | {name, offset, size} or bare offsets | the flash layout (`flashmap_scan.py`, pitfall 5) |
| chip support | {chip ID, name, geometry or ops} | every part the code supports: the board picks **one** and returns its ID |
| symbol table | {name pointer, address, type}, thousands of records, sorted or hashed | a name for every function (below) |

## Names somebody else already wrote

Before naming functions by hand, check whether the image or its surroundings carry names.

- **ELF symbols**: an ELF that is not stripped gives Ghidra its names on import
  (`ghidra-query.sh ... functions` lists them; `FUN_` names everywhere mean stripped).
- **A VxWorks symbol table**: thousands of function names as strings (`sysHwInit`,
  `usrRoot`, `taskSpawn`) and, elsewhere, an array of records each holding a pointer to
  one of them and a pointer into code (VxWorks 5.x commonly: 16-byte records, name at +4,
  address at +8). `strings_scan.py --ptrs --grep '^sysHwInit$'` finds a record. Apply the
  whole table as function names (a short Ghidra script, or by hand for the functions you
  need). Some vendors ship the table as a separate member of the upgrade file: check the
  container listing for a small member full of names.
- **Linux**: the kernel's `kallsyms` table; device-tree blobs (magic `d0 0d fe ed`),
  which list every device with its `compatible` name, base, size and interrupt: the
  memory map in one file. `compatible` strings such as `ns16550a` name the driver.
- **A root filesystem** in the image: `/etc` scripts, kernel module names, web pages and
  config defaults name the board and its chips.
- **Vendor source**: GPL releases, SDKs and BSPs for the named SoC, U-Boot board files,
  a sibling product's unstripped image. A header file with the register names turns the
  MMIO table into a labelled one. Tag what comes from outside the image *guess* until the
  image's own code agrees (same offsets, same bit tests, same strings).

## Identifying hardware when no string names it

Code that talks to a part has to use that part's numbers. Search for the constant
(Ghidra scalar search, or the compares in `mmio_scan.py` output), then read the function.

| what you see in the code | it is | board consequence |
|---|---|---|
| byte 0x9F sent, three bytes read back and compared | SPI flash JEDEC ID read. First byte is the maker: EF Winbond, C2 Macronix, 01 Spansion, 20 Micron/ST, 1F Atmel/Adesto, BF SST, C8 GigaDevice, 9D ISSI, 1C EON | the flash model must answer 0x9F with an ID from the code's chip table |
| 0x98 written to offset 0x55 (or 0xAA), then `QRY` read at 0x10; or 0xAA/0x55/0x90 written to 0x555/0x2AA/0x555 | parallel NOR flash: CFI query, or the autoselect (ID) sequence | a NOR flash model in ROMD mode (ember-dev-build has `examples/nor_flash.c`) |
| reads of registers 2 and 3 through an MDIO helper, compared with constants | Ethernet PHY ID. Register 2: 0x0141 Marvell, 0x0022 Micrel, 0x004D Atheros, 0x001C Realtek, 0x0007 SMSC, 0x2000 TI/National, 0x0020 / 0x0143 / 0x0362 Broadcom | the MDIO registers must return an accepted ID and, in register 1, link up (bit 2) and autonegotiation complete (bit 5) |
| a 7-bit bus address constant: 0x50-0x57; 0x68; 0x51; 0x48-0x4F | I2C EEPROM (or memory SPD); RTC (DS1307/DS3231 class); RTC (PCF8563 class); temperature sensor | an I2C controller model with that slave answering |
| divisor = clock / (16 x baud), 0x80 written to register 3, status read at register 5 | 16550-class UART | ember-dev-build's `examples/soc_uart.c` |
| a 256-entry table starting `00000000 77073096`, or the constant 0xEDB88320 / 0x04C11DB7 | CRC-32 | the function validates something: find what (image, config, NVRAM) and whether a failure stops the boot |
| 0x67452301 0xEFCDAB89 ...; 0x6A09E667; a 256-byte table starting `63 7c 77 7b` | MD5/SHA-1 init; SHA-256 init; AES S-box | a signature or encrypted section check: note it as a risk |
| `1f 8b 08`; `5d 00 00`; `27 05 19 56`; `hsqs`/`sqsh`; `19 85`; `HDR0` | gzip; LZMA; U-Boot image header; squashfs; JFFS2; TRX | container members (step 1 of the pass) |
| a round constant such as 25000000, 40000000, 48000000 | a reference clock in Hz | the PLL and strap values the clock code expects |
| a register compared with several constants, each choosing a branch | chip ID, revision or strap register | *Identity / config registers the code checks*: list every constant and its branch (pitfall 8) |

A name gives the datasheet; the datasheet gives every register's meaning. **The firmware
stays the authority**: implement what this code reads and writes, not the whole part.

## From a name to what the board must implement

For every chip or block identified, write this much in RECON.md (MMIO table row, plus
*Suggested bring-up order*):

1. **Name and evidence**: the string and its address, or the ID compare and its function.
2. **Where**: the page or bus address, and the bus it hangs off (memory-mapped, SPI chip
   select N, MDIO address N, I2C address N).
3. **Is it on the boot path?** Does init call the driver before the banner or prompt, or
   only on a command or a later event? Only the first kind blocks bring-up.
4. **What must answer**: the reads the code tests (ID, ready bit, link bit) and the value
   that lets it continue. Everything else can read 0 at first.
5. **Does ember-dev-build have it?** UART, timer, interrupt controller, reset block, NOR
   flash and the PC chipset parts have tested examples
   (`../ember-dev-build/references/examples/`). Say so in the row.

## The expected console output

The strings the init sequence prints, in call order, are the boot log the firmware will
produce when the board is right. Collect them while walking the init sequence and put
them in RECON.md (*Suggested bring-up order*, as "expected console output"):

```
1. "\n\nU-Boot 1.1.4 (..."            from board_init_f     needs: UART tx
2. "DRAM:  %d MB"                      from board_init_f     needs: memory controller straps
3. "Flash: %d MB"                      from flash_init       needs: SPI flash ID
4. "Hit any key to stop autoboot"      from main_loop        needs: timer; UART rx to stop it
```

The build phase uses this as its map: the last line a run printed and the next expected
one bracket the stall, and the function between them is the one to read (its `-t console`
trace stamps each line with the pc that printed it). A line that reports a **wrong value**
(`DRAM: 0 MB`) points at the register behind that value. A prompt is the end marker and
the place scripted console input waits for.

## When strings mislead

- **Text for hardware that is not there.** One image often serves several boards and
  carries drivers and ID tables for many chips. A chip is *confirmed* only when a code path
  on the boot path selects it: an ID compare, a strap read, a board-type variable. Find
  that selection and record which value this board gives (often unknown from the image
  alone: then say which values the code accepts, and let the build phase pick).
- **Library and protocol text.** `timeout` in a TCP stack is not a hardware wait. Prefer
  strings stored near driver strings (`--near`) and those whose function touches MMIO.
- **Copied names.** A BSP forked from another SoC's keeps old file and function names
  (`ar531xPlus/sysLib.c` in an AR9331 image). The newest, most specific name wins, and
  the ID compares decide.
- **Version strings of components.** A zlib or OpenSSL version is not the firmware's.
- **A string proves that the text exists**, not that the code runs. Reachability comes
  from callers (static) or from a run (`dynamic-recon.md`: `--log-pc` on the function).

## When there are no strings

Small microcontroller images and stripped bootloaders may print nothing.

1. **Is it packed?** `strings_scan.py` finding under twenty strings in a large file means
   compressed or encrypted: unpack the member first (step 1 of the pass, or let its loader
   unpack it: `dynamic-recon.md`).
2. **Start from the vectors.** Reset and interrupt vectors give the entry and one handler
   per peripheral in use; the vector number names the peripheral once the part is known
   (`arch-cheatsheet.md`).
3. **Name the part from its registers.** The set of SFR or peripheral addresses the code
   touches is a fingerprint: compare with candidate parts' register lists.
4. **Recognise library code by shape**: the startup's copy and clear loops, software
   multiply and divide helpers, `memcpy`. Name them and set them aside; what is left is
   the application.
5. **Use constants** (the table above): baud divisors, timer reload values, I2C addresses.
6. **Run it** (`dynamic-recon.md`): `-t mmio-summary` shows which registers the code
   really uses and in what order, with no reading at all.

## If you are stuck

Go down this list; stop at the first that gives a new lead.

1. No idea what a function does -> list the strings it references and the strings of its
   callers and callees. Still nothing -> its MMIO pages, then its constants.
2. No idea what an MMIO page is -> its accessing functions' strings (step 5 of the pass),
   then `--grep` for the register names a datasheet for the suspected SoC uses.
3. A string with no references -> the table under *When a string has no references*.
4. Too many leads -> only those on the boot path matter now: walk the init sequence from
   the entry and take them in call order.
5. Static reading has taken more than a few functions for one fact -> run it
   (`dynamic-recon.md`).
6. Two readings fit -> write both under *Open questions* with what would decide between
   them. Do not pick one silently.

## Before handing RECON.md over: can you answer these?

Each "no" is an item for *Open questions / risks*, not a reason to guess.

- Which function prints to the console, through which UART (base, type, the status bit it
  waits for)?
- What is the first line the firmware prints, and what must work before it appears?
- What is the last line of a successful boot (a banner, a prompt)?
- Which SoC or microcontroller is this, and what is the evidence besides one string?
- Which external chips does the boot path talk to (flash, PHY or switch, EEPROM, RTC),
  and what ID does each have to return?
- What is the system tick source, and which interrupt line does it use?
- What does the firmware validate at boot (image checksum, config magic, calibration
  data), and what happens when the check fails?
- Which waits does the boot path contain, and what ends each one?
- Which named functions would the build phase want as breakpoints (entry of each stage,
  hardware init, console init, the print function, panic)? List them with addresses.
