#!/usr/bin/env python3
"""
ppcdis.py - look inside XPatch360 memory dumps (.bin) from the command line.

Small PowerPC (Xbox 360 / Xenon, big endian) disassembler and search tool.
Python 3, no packages needed. Not a full disassembler: vector (VMX128)
instructions show as raw words.

  python ppcdis.py DUMP.bin dis 82A1A200 [COUNT]   instructions from an address
  python ppcdis.py DUMP.bin func 82A1A21C          the whole function around an address
  python ppcdis.py DUMP.bin refs 82A1A200          code that calls / branches to / loads it
  python ppcdis.py DUMP.bin strings density        text in the dump (case-insensitive)
  python ppcdis.py DUMP.bin find "39 60 00 02"     a byte sequence (hex)

The dump's start address is taken from the file name (<TITLEID>_<ADDRESS>.bin)
or is 82000000 (code dumps, <TITLEID>_TU<n>_code.bin); override with --base.
Addresses are hex, with or without 0x.

Part of XPatch360 (bobber360 & Claude), MIT License.
"""
import argparse, os, re, struct, sys

def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

SPR = {8: 'lr', 9: 'ctr', 1: 'xer', 268: 'tb', 269: 'tbu'}

def dis(word, pc):
    op = word >> 26
    rD = (word >> 21) & 31; rA = (word >> 16) & 31; rB = (word >> 11) & 31
    simm = sx(word & 0xFFFF, 16); uimm = word & 0xFFFF
    if word == 0x60000000: return 'nop'
    if op == 18:
        li = sx(word & 0x03FFFFFC, 26); aa = word & 2; lk = word & 1
        t = li if aa else pc + li
        return ('bl' if lk else 'b') + ' 0x%08X' % (t & 0xFFFFFFFF)
    if op == 16:
        bo = rD; bi = rA; bd = sx(word & 0xFFFC, 16); lk = word & 1
        t = pc + bd
        cr = bi >> 2; bit = ['lt', 'gt', 'eq', 'so'][bi & 3]
        if bo & 0x14 == 0x14: m = 'b'
        elif bo & 0x1C == 0x0C: m = 'b' + bit
        elif bo & 0x1C == 0x04: m = 'b' + {'lt': 'ge', 'gt': 'le', 'eq': 'ne', 'so': 'ns'}[bit]
        elif bo & 0x16 == 0x10: m = 'bdnz'
        elif bo & 0x16 == 0x12: m = 'bdz'
        else: m = 'bc%d,%d' % (bo, bi)
        return '%s%s cr%d, 0x%08X' % (m, 'l' if lk else '', cr, t)
    if op == 19:
        xo = (word >> 1) & 0x3FF
        if word == 0x4E800020: return 'blr'
        if word == 0x4E800420: return 'bctr'
        if word == 0x4E800421: return 'bctrl'
        if word == 0x4C00012C: return 'isync'
        if xo == 16: return 'bclr %d,%d' % (rD, rA) if rD != 20 else 'blr'
        if xo == 528: return 'bcctr %d,%d' % (rD, rA)
        return 'cr-op %d' % xo
    if op == 14: return ('li r%d, %d' % (rD, simm)) if rA == 0 else 'addi r%d, r%d, %d' % (rD, rA, simm)
    if op == 15: return ('lis r%d, 0x%X' % (rD, uimm)) if rA == 0 else 'addis r%d, r%d, 0x%X' % (rD, rA, uimm)
    if op == 12: return 'addic r%d, r%d, %d' % (rD, rA, simm)
    if op == 13: return 'addic. r%d, r%d, %d' % (rD, rA, simm)
    if op == 8: return 'subfic r%d, r%d, %d' % (rD, rA, simm)
    if op == 7: return 'mulli r%d, r%d, %d' % (rD, rA, simm)
    if op == 10: return 'cmplwi cr%d, r%d, 0x%X' % (rD >> 2, rA, uimm) if not rD & 1 else 'cmpldi cr%d, r%d, 0x%X' % (rD >> 2, rA, uimm)
    if op == 11: return 'cmpwi cr%d, r%d, %d' % (rD >> 2, rA, simm) if not rD & 1 else 'cmpdi cr%d, r%d, %d' % (rD >> 2, rA, simm)
    if op == 24: return 'ori r%d, r%d, 0x%X' % (rA, rD, uimm)
    if op == 25: return 'oris r%d, r%d, 0x%X' % (rA, rD, uimm)
    if op == 26: return 'xori r%d, r%d, 0x%X' % (rA, rD, uimm)
    if op == 28: return 'andi. r%d, r%d, 0x%X' % (rA, rD, uimm)
    if op == 29: return 'andis. r%d, r%d, 0x%X' % (rA, rD, uimm)
    if op == 21:
        sh = rB; mb = (word >> 6) & 31; me = (word >> 1) & 31
        return 'rlwinm%s r%d, r%d, %d, %d, %d' % ('.' if word & 1 else '', rA, rD, sh, mb, me)
    if op == 20:
        sh = rB; mb = (word >> 6) & 31; me = (word >> 1) & 31
        return 'rlwimi r%d, r%d, %d, %d, %d' % (rA, rD, sh, mb, me)
    loads = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 40: 'lhz', 41: 'lhzu', 42: 'lha', 36: 'stw', 37: 'stwu',
             38: 'stb', 39: 'stbu', 44: 'sth', 45: 'sthu', 48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu', 52: 'stfs',
             53: 'stfsu', 54: 'stfd', 55: 'stfdu', 46: 'lmw', 47: 'stmw'}
    if op in loads:
        r = ('f%d' if op >= 48 else 'r%d') % rD
        return '%s %s, %d(r%d)' % (loads[op], r, simm, rA)
    if op == 58:
        ds = sx(word & 0xFFFC, 16); k = word & 3
        return '%s r%d, %d(r%d)' % (['ld', 'ldu', 'lwa', '?'][k], rD, ds, rA)
    if op == 62:
        ds = sx(word & 0xFFFC, 16); k = word & 3
        return '%s r%d, %d(r%d)' % (['std', 'stdu', '?', '?'][k], rD, ds, rA)
    if op == 30:
        xo = (word >> 2) & 7; sh = rB | ((word >> 1) & 1) << 5; mb = ((word >> 6) & 31) | ((word >> 5) & 1) << 5
        names = {0: 'rldicl', 1: 'rldicr', 2: 'rldic', 3: 'rldimi'}
        return '%s r%d, r%d, %d, %d' % (names.get(xo, 'rld?'), rA, rD, sh, mb)
    if op == 31:
        xo = (word >> 1) & 0x3FF; rc = '.' if word & 1 else ''
        x31 = {266: 'add', 40: 'subf', 8: 'subfc', 136: 'subfe', 138: 'adde', 10: 'addc', 235: 'mullw', 491: 'divw',
               459: 'divwu', 28: 'and', 60: 'andc', 444: 'or', 124: 'nor', 316: 'xor', 24: 'slw', 536: 'srw',
               792: 'sraw', 27: 'sld', 539: 'srd', 794: 'srad', 233: 'mulld', 457: 'divdu', 489: 'divd', 75: 'mulhw',
               11: 'mulhwu', 412: 'orc', 284: 'eqv', 476: 'nand'}
        xoA = (word >> 1) & 0x1FF
        if xo == 444 and rD == rB: return 'mr%s r%d, r%d' % (rc, rA, rD)
        if xoA in (266, 40, 8, 136, 138, 10, 235, 491, 459, 233, 457, 489, 75, 11):
            return '%s%s r%d, r%d, r%d' % (x31[xoA], rc, rD, rA, rB)
        if xoA == 104: return 'neg%s r%d, r%d' % (rc, rD, rA)
        if xoA == 202: return 'addze%s r%d, r%d' % (rc, rD, rA)
        if xoA == 200: return 'subfze%s r%d, r%d' % (rc, rD, rA)
        if xo in (28, 60, 444, 124, 316, 24, 536, 792, 27, 539, 794, 412, 284, 476):
            return '%s%s r%d, r%d, r%d' % (x31[xo], rc, rA, rD, rB)
        if xo == 824: return 'srawi%s r%d, r%d, %d' % (rc, rA, rD, rB)
        if xo in (826, 827): return 'sradi%s r%d, r%d, %d' % (rc, rA, rD, rB | ((word >> 1) & 1) << 5)
        if xo == 26: return 'cntlzw%s r%d, r%d' % (rc, rA, rD)
        if xo == 58: return 'cntlzd%s r%d, r%d' % (rc, rA, rD)
        if xo == 954: return 'extsb%s r%d, r%d' % (rc, rA, rD)
        if xo == 922: return 'extsh%s r%d, r%d' % (rc, rA, rD)
        if xo == 986: return 'extsw%s r%d, r%d' % (rc, rA, rD)
        if xo == 0: return 'cmp%s cr%d, r%d, r%d' % ('d' if rD & 1 else 'w', rD >> 2, rA, rB)
        if xo == 32: return 'cmpl%s cr%d, r%d, r%d' % ('d' if rD & 1 else 'w', rD >> 2, rA, rB)
        if xo == 339:
            spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5); return 'mfspr r%d, %s' % (rD, SPR.get(spr, spr))
        if xo == 467:
            spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5); return 'mtspr %s, r%d' % (SPR.get(spr, spr), rD)
        if xo == 371:
            spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5); return 'mftb r%d, %s' % (rD, SPR.get(spr, spr))
        if xo == 19: return 'mfcr r%d' % rD
        if xo == 598: return 'sync'
        if xo == 854: return 'eieio'
        if xo == 86: return 'dcbf r%d, r%d' % (rA, rB)
        if xo == 54: return 'dcbst r%d, r%d' % (rA, rB)
        if xo == 1014: return 'dcbz r%d, r%d' % (rA, rB)
        if xo == 20: return 'lwarx r%d, r%d, r%d' % (rD, rA, rB)
        if xo == 150: return 'stwcx. r%d, r%d, r%d' % (rD, rA, rB)
        x = {23: 'lwzx', 87: 'lbzx', 279: 'lhzx', 151: 'stwx', 215: 'stbx', 407: 'sthx', 21: 'ldx', 149: 'stdx',
             535: 'lfsx', 599: 'lfdx', 663: 'stfsx', 727: 'stfdx', 343: 'lhax', 341: 'lwax'}
        if xo in x: return '%s %s%d, r%d, r%d' % (x[xo], 'f' if xo >= 535 and xo <= 727 else 'r', rD, rA, rB)
        return 'op31 xo=%d' % xo
    if op == 59 or op == 63:
        xo5 = (word >> 1) & 31; xo = (word >> 1) & 0x3FF; s = 's' if op == 59 else ''
        fC = (word >> 6) & 31
        m5 = {18: 'fdiv', 20: 'fsub', 21: 'fadd', 25: 'fmul', 28: 'fmsub', 29: 'fmadd', 30: 'fnmsub', 31: 'fnmadd',
              22: 'fsqrt', 26: 'frsqrte', 23: 'fsel', 24: 'fres'}
        if xo5 in m5:
            if xo5 == 25: return '%s%s f%d, f%d, f%d' % (m5[xo5], s, rD, rA, fC)
            if xo5 in (28, 29, 30, 31, 23): return '%s%s f%d, f%d, f%d, f%d' % (m5[xo5], s, rD, rA, fC, rB)
            return '%s%s f%d, f%d, f%d' % (m5[xo5], s, rD, rA, rB)
        if op == 63:
            m = {72: 'fmr', 40: 'fneg', 264: 'fabs', 12: 'frsp', 14: 'fctiw', 15: 'fctiwz', 846: 'fcfid', 815: 'fctidz',
                 814: 'fctid'}
            if xo in m: return '%s f%d, f%d' % (m[xo], rD, rB)
            if xo == 0: return 'fcmpu cr%d, f%d, f%d' % (rD >> 2, rA, rB)
            if xo == 32: return 'fcmpo cr%d, f%d, f%d' % (rD >> 2, rA, rB)
        return 'fp op%d xo=%d' % (op, xo)
    if op == 4 or op == 5 or op == 6: return 'vmx .long 0x%08X' % word
    if op == 3: return 'twi'
    if op == 2: return 'tdi'
    return '.long 0x%08X' % word




class Dump:
    def __init__(self, path, base):
        self.d = open(path, 'rb').read()
        self.base = base
        self.end = base + len(self.d)
        self.n = len(self.d) // 4
        self._words = None

    def words(self):
        if self._words is None:
            self._words = struct.unpack('>%dI' % self.n, self.d[:self.n * 4])
        return self._words

    def w(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.d):
            raise SystemExit('address %08X is outside the dump (%08X-%08X)' % (a, self.base, self.end))
        return struct.unpack('>I', self.d[o:o + 4])[0]

    def show(self, start, count, mark=()):
        for i in range(count):
            a = start + 4 * i
            if a + 4 > self.end: break
            x = self.w(a)
            print('%s%08X: %08X  %s' % ('>' if a in mark else ' ', a, x, dis(x, a)))

    def func_start(self, a):
        a &= ~3
        for _ in range(0x4000):
            if a - 4 < self.base: return a
            x = self.w(a - 4)
            if x in (0x4E800020, 0) and self.w(a) != 0: return a
            a -= 4
        return a

    def func_end(self, a):
        for _ in range(0x4000):
            if a + 4 > self.end or self.w(a) == 0x4E800020:
                # keep going while the next word is code of the same function (tail after blr)
                if a + 8 <= self.end and self.w(a + 4) not in (0,) and self.w(a + 4) != 0x7D8802A6:
                    a += 4; continue
                return a + 4
            a += 4
        return a

    def refs(self, target):
        W = self.words(); B = self.base; out = []
        for i, x in enumerate(W):
            op = x >> 26
            if op == 18:                                   # b / bl
                li = sx(x & 0x03FFFFFC, 26)
                if (B + 4 * i + li) & 0xFFFFFFFF == target and not x & 2:
                    out.append((B + 4 * i, 'bl' if x & 1 else 'b'))
            elif op == 16:                                 # conditional branch
                bd = sx(x & 0xFFFC, 16)
                if B + 4 * i + bd == target and not x & 2:
                    out.append((B + 4 * i, 'bc'))
        # lis rX,hi + addi/ori/lwz/stw/lfs... rY,lo(rX)
        lo = target & 0xFFFF
        his = {(target >> 16) & 0xFFFF, ((target >> 16) + (1 if lo & 0x8000 else 0)) & 0xFFFF}
        for i, x in enumerate(W):
            if x >> 26 == 15 and (x >> 16) & 31 == 0 and (x & 0xFFFF) in his:
                rd = (x >> 21) & 31
                for k in range(1, 12):
                    if i + k >= len(W): break
                    y = W[i + k]
                    yop = y >> 26
                    if (y & 0xFFFF) == lo and (y >> 16) & 31 == rd and yop in (14, 24, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54):
                        hi = x & 0xFFFF
                        full = ((hi << 16) + (sx(lo, 16) if yop != 24 else lo)) & 0xFFFFFFFF
                        if full == target: out.append((B + 4 * (i + k), 'load/store/addr'))
                        break
                    if (y >> 21) & 31 == rd and yop not in (36, 38, 44, 52, 54): break   # rd overwritten
        p = struct.pack('>I', target); j = self.d.find(p)
        while j >= 0:
            if j % 4 == 0: out.append((B + j, 'pointer in data'))
            j = self.d.find(p, j + 1)
        return sorted(set(out))


def hexaddr(s):
    return int(s.replace('0x', '').replace('0X', ''), 16)


def main():
    ap = argparse.ArgumentParser(description='Look inside XPatch360 memory dumps (.bin).',
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('dump', help='the .bin file from Hdd:\\Patches\\XPatch360\\')
    ap.add_argument('--base', help='start address of the dump (hex), if not in the file name')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('dis', help='instructions from an address'); p.add_argument('addr'); p.add_argument('count', nargs='?', default='32')
    p = sub.add_parser('func', help='the whole function around an address'); p.add_argument('addr')
    p = sub.add_parser('refs', help='code/data that refers to an address'); p.add_argument('addr')
    p = sub.add_parser('strings', help='text in the dump'); p.add_argument('text'); p.add_argument('--refs', action='store_true', help='also list references to each string')
    p = sub.add_parser('find', help='a byte sequence (hex)'); p.add_argument('hex')
    a = ap.parse_args()

    if a.base:
        base = hexaddr(a.base)
    else:
        m = re.match(r'^.+_([0-9A-Fa-f]{8})\.bin$', os.path.basename(a.dump))
        base = int(m.group(1), 16) if m else 0x82000000
    d = Dump(a.dump, base)
    print('# %s: %08X-%08X' % (os.path.basename(a.dump), d.base, d.end))

    if a.cmd == 'dis':
        d.show(hexaddr(a.addr) & ~3, int(a.count, 0))
    elif a.cmd == 'func':
        t = hexaddr(a.addr) & ~3
        s = d.func_start(t); e = d.func_end(t)
        r = d.refs(s)
        print('# function %08X-%08X, referenced from: %s' % (s, e, ', '.join('%08X (%s)' % (x, k) for x, k in r[:12]) or 'nothing found'))
        d.show(s, min((e - s) // 4, 2000), mark=(t,))
    elif a.cmd == 'refs':
        t = hexaddr(a.addr)
        r = d.refs(t)
        if not r: print('nothing refers to %08X directly' % t)
        for addr, kind in r:
            fs = d.func_start(addr) if kind != 'pointer in data' else None
            print('%08X  %-16s%s' % (addr, kind, ('  (function %08X)' % fs) if fs else ''))
    elif a.cmd == 'strings':
        rx = re.compile(rb'[\x20-\x7e]{4,}')
        key = a.text.lower().encode()
        for m in rx.finditer(d.d):
            if key in m.group().lower():
                addr = d.base + m.start()
                line = '%08X  %s' % (addr, m.group().decode())
                if a.refs:
                    r = d.refs(addr)
                    line += '   <- ' + (', '.join('%08X' % x for x, k in r[:6]) if r else 'no direct refs')
                print(line)
    elif a.cmd == 'find':
        pat = bytes.fromhex(a.hex.replace(' ', '').replace('0x', ''))
        j = d.d.find(pat); n = 0
        while j >= 0 and n < 200:
            print('%08X' % (d.base + j)); n += 1
            j = d.d.find(pat, j + 1)
        if n == 0: print('not found')


if __name__ == '__main__':
    main()
